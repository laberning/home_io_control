/// @file radio_sx1262.cpp
/// @brief SX1262 radio driver implementation for IO-Homecontrol.
///
/// Unlike the SX1276, the SX1262 does not provide Semtech's IoHomeOn mode for
/// IO-Homecontrol. On SX1276 that mode handles key protocol details in hardware:
/// CRC generation and checking, packet boundary handling, and delivery of
/// already-decoded protocol bytes in the FIFO. On SX1262 those pieces have to
/// be reproduced in software on top of generic GFSK support.
///
/// Concretely, this driver has to:
/// - append and verify the IO-Homecontrol CRC in software,
/// - UART-pack frames for TX and recover UART-packed on-air bytes on RX,
/// - detect plausible frame boundaries before handing bytes to the parser,
/// - preserve raw capture data and metadata for debugging against the SX1276
///   baseline capture path.
///
/// The chip interface itself is also different: SX1262 uses opcode-based SPI
/// instead of the SX1276 register model, and every transaction must respect the
/// BUSY line. For experiment builds, the RX path prioritizes preserving the
/// chip-reported bytes and metadata verbatim.

// The SX1262 path is intentionally low-level: opcode payloads, line-coding widths, and recovery
// thresholds are written in the same shape as the chip protocol and on-air framing.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

#include "radio_sx1262.h"
#include "log_frame.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.sx1262";
static const uint8_t SX1262_SYNC_WORD_PARAM_24_BITS = 0x18;
// Fixed probe length chosen from captures of 23-25 byte protocol frames after UART packing
// and CRC appending. 32 bytes is large enough to preserve complete traffic without relying on
// the chip's variable-length engine, which consistently truncated the useful payload.
static const uint8_t SX1262_RX_PROBE_PACKET_LEN = 32;
/// Maximum bit offset to search for valid UART decode start position.
/// The UART frame is 10 bits (start + 8 data). If the sync word is not aligned,
/// we probe up to 10 bits offset to recover the correct framing.
static const uint8_t UART_PROBE_MAX_BIT_OFFSET = 10;

/// Extract a single bit (MSB‑first) from a byte buffer.
/// Used by UART decoding to scan raw radio samples.
/// @param data Input byte buffer.
/// @param bit_pos Global bit index within buffer.
/// @return The bit value (0 or 1).
static uint8_t get_bit_msb(const uint8_t *data, uint16_t bit_pos) {
  return (data[bit_pos / 8] >> (7 - (bit_pos % 8))) & 0x01;
}

/// Decode a raw UART‑encoded bitstream into bytes.
/// IO‑Homecontrol uses a UART‑like encoding over the air: each byte is represented
/// by a 10‑bit sequence (start bit 0, 8 data bits LSB‑first, stop bit 1). This
/// function slides a window across the raw bitstream and attempts to recover the
/// original bytes. It stops when the sync pattern (0 followed by 1) is not found.
/// @param raw Raw bytes from the radio buffer.
/// @param raw_len Number of raw bytes available.
/// @param bit_offset Initial bit position to start decoding (probe offset).
/// @param decoded Output buffer for decoded bytes.
/// @param decoded_max_len Capacity of decoded buffer.
/// @return Number of bytes successfully decoded.
static uint8_t decode_uart_probe(const uint8_t *raw, uint8_t raw_len, uint8_t bit_offset, uint8_t *decoded,
                                 uint8_t decoded_max_len) {
  // Bit numbering: we read MSB-first across byte boundaries. The UART frame structure
  // within the bitstream is: start(0), data0, data1, ..., data7, stop(1). Byte values
  // are LSB-first within the 8 data bits (bit 0 arrives first after start).
  // We verify the start bit is 0 and stop bit is 1; if not, the probe offset is wrong.
  uint16_t bit_pos = bit_offset;
  uint16_t const total_bits = raw_len * 8;
  uint8_t decoded_len = 0;

  while (bit_pos + 10 <= total_bits && decoded_len < decoded_max_len) {
    if (get_bit_msb(raw, bit_pos) != 0 || get_bit_msb(raw, bit_pos + 9) != 1)
      break;

    uint8_t value = 0;
    for (uint8_t index = 0; index < 8; index++)
      value |= get_bit_msb(raw, bit_pos + 1 + index) << index;

    decoded[decoded_len++] = value;
    bit_pos += 10;
  }

  return decoded_len;
}

/// @brief Result of the UART probe: best candidate frame within a raw capture.
struct UartProbeResult {
  bool valid{false};                            ///< A plausible frame was found.
  uint8_t bit_offset{0};                        ///< Bit offset where the best decode started.
  uint8_t decoded_len{0};                       ///< Total number of bytes decoded at that offset.
  uint8_t frame_start{0};                       ///< Index into decoded buffer where the frame begins.
  uint8_t frame_len{0};                         ///< Length of the candidate IoFrame (decoded bytes).
  uint8_t decoded[RADIO_PACKET_BUFFER_SIZE]{};  ///< Full decoded UART stream at the chosen offset.
};

/// @brief Check if a command ID is one of the known IO‑Homecontrol commands.
/// @param cmd Command byte.
/// @return true if cmd matches a known command constant.
static bool is_known_io_command(uint8_t cmd) {
  switch (cmd) {
    case CMD_EXECUTE:
    case CMD_PRIVATE:
    case CMD_PRIVATE_RESP:
    case CMD_DISCOVER_REQ:
    case CMD_DISCOVER_RESP:
    case CMD_DISCOVER_SPE_REQ:
    case CMD_DISCOVER_SPE_RESP:
    case CMD_DISCOVER_CONFIRM:
    case CMD_DISCOVER_CONFIRM_ACK:
    case CMD_KEY_INIT:
    case CMD_KEY_TRANSFER:
    case CMD_KEY_CONFIRM:
    case CMD_CHALLENGE_REQ:
    case CMD_CHALLENGE_RESP:
    case CMD_GET_NAME:
    case CMD_GET_NAME_RESP:
    case CMD_GET_INFO2:
    case CMD_GET_INFO2_RESP:
    case CMD_SET_CONFIG1:
    case CMD_SET_CONFIG1_RESP:
    case CMD_STATUS_UPDATE:
    case CMD_STATUS_UPDATE_RESP:
    case CMD_ERROR_RESP:
      return true;
    default:
      return false;
  }
}

static bool is_plausible_uart_frame(const IoFrame &frame, uint8_t candidate_len) {
  if (candidate_len < 15)
    return false;
  if (is_known_io_command(frame.cmd))
    return true;
  return (frame.ctrl0 & CTRL0_PROTOCOL_1W) != 0;
}

/// @brief Search a raw capture for the most plausible IoFrame using UART decoding.
/// Probes multiple bit offsets and candidate lengths to find a valid parse that
/// looks like a real IO‑Homecontrol frame.
/// @param raw Pointer to raw radio buffer bytes.
/// @param raw_len Number of bytes in raw.
/// @return UartProbeResult with best candidate (may have valid=false if none found).
static UartProbeResult find_uart_probe(const uint8_t *raw, uint8_t raw_len) {
  // The SX1262 RX buffer contains the raw GFSK‑demodulated bits packed as bytes.
  // Due to unknown bit alignment, we probe up to UART_PROBE_MAX_BIT_OFFSET (10) different
  // starting positions. For each offset we attempt UART decoding; if decoding yields
  // a plausible frame length (>= minimum) and contains a known command ID or indicates
  // a 1W frame, we keep it as a candidate. The best (longest valid) candidate wins.
  // This approach tolerates the SX1262's lack of IoHomeOn framing assistance.
  UartProbeResult best{};

  // Current captures consistently decode at bit_offset=0, but keeping a short probe window makes
  // the recovery path robust against future boards or slightly different front-end timing.
  for (uint8_t bit_offset = 0; bit_offset < UART_PROBE_MAX_BIT_OFFSET; bit_offset++) {
    uint8_t decoded[RADIO_PACKET_BUFFER_SIZE] = {0};
    uint8_t const decoded_len = decode_uart_probe(raw, raw_len, bit_offset, decoded, sizeof(decoded));
    if (decoded_len == 0)
      continue;

    if (decoded_len > best.decoded_len) {
      best.bit_offset = bit_offset;
      best.decoded_len = decoded_len;
      memcpy(best.decoded, decoded, decoded_len);
    }

    for (uint8_t start = 0; start < decoded_len; start++) {
      uint8_t const max_candidate_len = std::min<uint8_t>(decoded_len - start, FRAME_MAX_SIZE);
      for (int candidate_len = max_candidate_len; candidate_len >= FRAME_MIN_SIZE; candidate_len--) {
        IoFrame frame;
        if (!parse(decoded + start, candidate_len, frame))
          continue;
        if (!is_plausible_uart_frame(frame, candidate_len))
          continue;

        best.valid = true;
        best.bit_offset = bit_offset;
        best.decoded_len = decoded_len;
        best.frame_start = start;
        best.frame_len = candidate_len;
        memcpy(best.decoded, decoded, decoded_len);
        return best;
      }
    }
  }

  return best;
}

// === Software CRC-CCITT ===

uint8_t RadioSX1262::uart_encode_packet(const uint8_t *data, uint8_t len, uint8_t *encoded, uint8_t encoded_max_len) {
  if (len == 0 || encoded_max_len == 0)
    return 0;

  memset(encoded, 0, encoded_max_len);
  uint16_t bit_pos = 0;
  const uint16_t total_bits = len * 10;
  if (((total_bits + 7) / 8) > encoded_max_len)
    return 0;

  auto write_bit = [encoded](uint16_t pos, uint8_t bit) {
    if (bit != 0)
      encoded[pos / 8] |= 1U << (7 - (pos % 8));
  };

  for (uint8_t byte_index = 0; byte_index < len; byte_index++) {
    const uint8_t value = data[byte_index];

    write_bit(bit_pos++, 0);  // UART start bit
    for (uint8_t bit_index = 0; bit_index < 8; bit_index++)
      write_bit(bit_pos++, (value >> bit_index) & 0x01);
    write_bit(bit_pos++, 1);  // UART stop bit
  }

  return (total_bits + 7) / 8;
}

// === SPI Communication (opcode-based) ===

void RadioSX1262::wait_busy_() {
  uint32_t const start = millis();
  while (this->busy_pin_->digital_read()) {
    if (millis() - start > 10) {
      ESP_LOGE(TAG, "BUSY timeout");
      this->failed_ = true;
      return;
    }
    App.feed_wdt();
  }
}

void RadioSX1262::write_opcode_(uint8_t opcode, const uint8_t *params, uint8_t len) {
  this->wait_busy_();
  this->spi_->spi_enable();
  this->spi_->spi_transfer(opcode);
  for (uint8_t i = 0; i < len; i++)
    this->spi_->spi_transfer(params[i]);
  this->spi_->spi_disable();
}

void RadioSX1262::read_opcode_(uint8_t opcode, uint8_t *data, uint8_t len) {
  this->wait_busy_();
  this->spi_->spi_enable();
  this->spi_->spi_transfer(opcode);
  this->spi_->spi_transfer(0x00);  // NOP — status byte
  for (uint8_t i = 0; i < len; i++)
    data[i] = this->spi_->spi_transfer(0x00);
  this->spi_->spi_disable();
}

uint16_t RadioSX1262::read_irq_status_raw() {
  uint8_t irq_raw[2] = {0};
  this->read_opcode_(SX1262_GET_IRQ_STATUS, irq_raw, 2);
  return (uint16_t) (((uint16_t) irq_raw[0] << 8) | irq_raw[1]);
}

// === wait_for_packet static helpers ===

/// Poll for first radio activity (DIO1 interrupt or any IRQ status) within timeout.
///
/// Checks the DIO1 pin latch and the raw IRQ status register repeatedly until
/// either activity is detected or the timeout expires. On timeout, clears the
/// DIO latch and resets the RX state machine for the next receive cycle.
bool RadioSX1262::poll_until_activity_(uint32_t start, uint32_t timeout_ms, bool &saw_dio1, uint16_t &irq) {
  while (true) {
    if (this->is_dio_fired()) {
      saw_dio1 = true;
      return true;
    }
    irq = this->read_irq_status_raw();
    if (irq != 0)
      return true;
    if (millis() - start > timeout_ms) {
      this->clear_dio_fired();
      this->reset_rx_state_();
      return false;
    }
    App.feed_wdt();
    delay(1);
  }
}

/// Resolve the SYNC_WORD_VALID → RX_DONE race condition.
///
/// On SX1262 the SYNC_WORD_VALID IRQ can assert before the packet is fully
/// received. If we observe SYNC without RX_DONE, clear the sticky SYNC flag
/// and spin until RX_DONE arrives or the remaining timeout elapses.
bool RadioSX1262::resolve_sync_race_(uint32_t start, uint32_t timeout_ms, uint16_t &irq) {
  // If RX_DONE already set or SYNC not set, nothing to resolve.
  if ((irq & SX1262_IRQ_SYNC_WORD_VALID) == 0 || (irq & SX1262_IRQ_RX_DONE) != 0) {
    return true;
  }
  // SYNC seen without RX_DONE — clear sticky SYNC and wait for RX_DONE.
  this->clear_irq_status_(SX1262_IRQ_SYNC_WORD_VALID);
  while (millis() - start <= timeout_ms) {
    if (!this->is_dio_fired()) {
      irq = this->read_irq_status_raw();
      if ((irq & SX1262_IRQ_RX_DONE) != 0)
        return true;
      App.feed_wdt();
      delay(1);
      continue;
    }
    this->clear_dio_fired();
    irq = this->read_irq_status_raw();
    if ((irq & SX1262_IRQ_RX_DONE) != 0)
      return true;
    if (irq != 0)
      this->clear_irq_status_(irq);
  }
  return false;  // timeout
}

/// Finalize receive: read the packet if RX_DONE is set, otherwise record failure.
bool RadioSX1262::finalize_receive_(RadioRxPacket &packet, uint16_t irq) {
  if ((irq & SX1262_IRQ_RX_DONE) == 0) {
    this->fill_capture_info_(true, irq, 0, 0, nullptr, 0, nullptr, 0);
    this->reset_rx_state_();
    return false;
  }
  return this->read_rx_packet(packet, true, irq);
}

void RadioSX1262::write_register_(uint16_t addr, const uint8_t *data, uint8_t len) {
  this->wait_busy_();
  this->spi_->spi_enable();
  this->spi_->spi_transfer(SX1262_WRITE_REGISTER);
  this->spi_->spi_transfer((addr >> 8) & 0xFF);  // Address MSB
  this->spi_->spi_transfer(addr & 0xFF);         // Address LSB
  for (uint8_t i = 0; i < len; i++)
    this->spi_->spi_transfer(data[i]);
  this->spi_->spi_disable();
}

void RadioSX1262::read_register_(uint16_t addr, uint8_t *data, uint8_t len) {
  this->wait_busy_();
  this->spi_->spi_enable();
  this->spi_->spi_transfer(SX1262_READ_REGISTER);
  this->spi_->spi_transfer((addr >> 8) & 0xFF);  // Address MSB
  this->spi_->spi_transfer(addr & 0xFF);         // Address LSB
  this->spi_->spi_transfer(0x00);                // NOP — status byte
  for (uint8_t i = 0; i < len; i++)
    data[i] = this->spi_->spi_transfer(0x00);
  this->spi_->spi_disable();
}

void RadioSX1262::write_buffer_(uint8_t offset, const uint8_t *data, uint8_t len) {
  this->wait_busy_();
  this->spi_->spi_enable();
  this->spi_->spi_transfer(SX1262_WRITE_BUFFER);
  this->spi_->spi_transfer(offset);
  for (uint8_t i = 0; i < len; i++)
    this->spi_->spi_transfer(data[i]);
  this->spi_->spi_disable();
}

void RadioSX1262::read_buffer_(uint8_t offset, uint8_t *data, uint8_t len) {
  this->wait_busy_();
  this->spi_->spi_enable();
  this->spi_->spi_transfer(SX1262_READ_BUFFER);
  this->spi_->spi_transfer(offset);
  this->spi_->spi_transfer(0x00);  // NOP — status byte
  for (uint8_t i = 0; i < len; i++)
    data[i] = this->spi_->spi_transfer(0x00);
  this->spi_->spi_disable();
}

// === Packet params helper ===

void RadioSX1262::set_packet_params_(uint16_t preamble_len, uint8_t payload_len, uint8_t packet_type,
                                     uint8_t crc_type) {
  uint8_t params[9] = {
      (uint8_t) (preamble_len >> 8),   // Preamble length MSB
      (uint8_t) (preamble_len),        // Preamble length LSB
      0x04,                            // Preamble detector: 8 bits (1 byte)
      SX1262_SYNC_WORD_PARAM_24_BITS,  // Sync word length: 24 bits (3 bytes)
      0x00,                            // Address comparison: off
      packet_type,                     // GFSK packet type: known length or variable size
      payload_len,                     // Configured payload length
      crc_type,                        // CRC type: SX1262 GFSK CRC mode
      0x00,                            // Whitening: off
  };
  this->write_opcode_(SX1262_SET_PACKET_PARAMS, params, sizeof(params));
}

void RadioSX1262::set_rx_packet_params_() {
  // The variable-size packet engine recovers a stable 15-byte boundary, but that boundary is
  // too short for a full software UART decode and buffer reads past it are not trustworthy.
  // Probe again with a fixed raw packet size that matches typical UART-packed 23-25 byte
  // IO-homecontrol frames: ceil(25 * 10 / 8) = 32 bytes.
  this->set_packet_params_(8, SX1262_RX_PROBE_PACKET_LEN, SX1262_GFSK_PACKET_TYPE_KNOWN_LENGTH, SX1262_GFSK_CRC_OFF);
}

void RadioSX1262::clear_irq_status_(uint16_t irq_mask) {
  uint8_t clear_irq[2] = {
      (uint8_t) ((irq_mask >> 8) & 0xFF),
      (uint8_t) (irq_mask & 0xFF),
  };
  this->write_opcode_(SX1262_CLEAR_IRQ_STATUS, clear_irq, sizeof(clear_irq));
}

uint16_t RadioSX1262::get_device_errors_() {
  uint8_t errors_raw[2] = {0};
  this->read_opcode_(SX1262_GET_DEVICE_ERRORS, errors_raw, sizeof(errors_raw));
  return (uint16_t) (((uint16_t) errors_raw[0] << 8) | errors_raw[1]);
}

void RadioSX1262::clear_device_errors_() {
  uint8_t clear_errors[2] = {0x00, 0x00};
  this->write_opcode_(SX1262_CLEAR_DEVICE_ERRORS, clear_errors, sizeof(clear_errors));
}

void RadioSX1262::reset_rx_state_(bool force_standby) {
  uint8_t buf_base[2] = {0x00, 0x80};
  if (force_standby)
    this->set_mode_standby();
  this->clear_irq_status_(0xFFFF);
  this->write_opcode_(SX1262_SET_BUFFER_BASE_ADDRESS, buf_base, sizeof(buf_base));
  this->set_rx_packet_params_();
  this->set_mode_rx();
}

void RadioSX1262::fill_capture_info_(bool blocking_wait, uint16_t irq_status, uint8_t rx_offset, uint8_t reported_len,
                                     const uint8_t *raw, uint8_t raw_len, const uint8_t *frame, uint8_t frame_len) {
  uint8_t packet_status[3] = {0};
  this->read_opcode_(SX1262_GET_PACKET_STATUS, packet_status, sizeof(packet_status));

  this->populate_capture_base_(blocking_wait, this->current_freq_, -(int16_t) packet_status[1] / 2, raw, raw_len, frame,
                               frame_len);
  this->last_capture_.rx_done = (irq_status & SX1262_IRQ_RX_DONE) != 0;
  this->last_capture_.crc_error = (irq_status & SX1262_IRQ_CRC_ERR) != 0;
  this->last_capture_.irq_status = irq_status;
  this->last_capture_.packet_status = packet_status[0];
  this->last_capture_.rx_offset = rx_offset;
  this->last_capture_.reported_len = reported_len;
}

// === ISR ===

void IRAM_ATTR RadioSX1262::gpio_intr(RadioSX1262 *arg) { arg->mark_dio_fired_from_isr(); }

// === Initialization ===

bool RadioSX1262::init() {
  // --- Pin setup ---
  this->rst_pin_->setup();
  this->dio1_pin_->setup();
  this->busy_pin_->setup();

  // Front-end module pins (e.g., Heltec V4)
  if (this->fem_en_pin_ != nullptr) {
    this->fem_en_pin_->setup();
    this->fem_en_pin_->digital_write(true);
  }
  if (this->vfem_pin_ != nullptr) {
    this->vfem_pin_->setup();
    this->vfem_pin_->digital_write(true);
  }
  if (this->fem_pa_pin_ != nullptr) {
    this->fem_pa_pin_->setup();
    this->fem_pa_pin_->digital_write(true);
  }

  // --- Hardware reset ---
  this->reset_hardware_();
  this->wait_busy_();
  if (this->failed_)
    return false;

  this->configure_radio_();
  if (this->failed_)
    return false;

  ESP_LOGI(TAG, "SX1262 initialized");
  return true;
}

void RadioSX1262::dump_debug() {
  this->wait_busy_();
  this->spi_->spi_enable();
  uint8_t const chip_status = this->spi_->spi_transfer(SX1262_GET_STATUS);
  this->spi_->spi_transfer(0x00);
  this->spi_->spi_disable();

  uint8_t const chip_mode = (chip_status >> 4) & 0x07;
  uint8_t const cmd_status = (chip_status >> 1) & 0x07;
  const char *mode_str = "?";
  switch (chip_mode) {
    case 2:
      mode_str = "STDBY_RC";
      break;
    case 3:
      mode_str = "STDBY_XOSC";
      break;
    case 4:
      mode_str = "FS";
      break;
    case 5:
      mode_str = "RX";
      break;
    case 6:
      mode_str = "TX";
      break;
    default:
      break;
  }

  uint8_t sync[3];
  this->read_register_(SX1262_REG_SYNC_WORD, sync, 3);

  uint8_t irq_raw[2];
  this->read_opcode_(SX1262_GET_IRQ_STATUS, irq_raw, 2);
  uint16_t const irq = ((uint16_t) irq_raw[0] << 8) | irq_raw[1];
  uint16_t const errors = this->get_device_errors_();

  ESP_LOGCONFIG(TAG, "  SX1262 Diagnostic:");
  ESP_LOGCONFIG(TAG, "    Chip status: 0x%02X (mode=%s, cmd=%u)", chip_status, mode_str, cmd_status);
  ESP_LOGCONFIG(TAG, "    BUSY=%d DIO1=%d", this->busy_pin_->digital_read(), this->dio1_pin_->digital_read());
  ESP_LOGCONFIG(TAG, "    Sync word: %02X %02X %02X (expect 57 FD 99)", sync[0], sync[1], sync[2]);
  ESP_LOGCONFIG(TAG, "    IRQ status: 0x%04X", irq);
  ESP_LOGCONFIG(TAG, "    Device errors: 0x%04X", errors);
}

void RadioSX1262::configure_radio_() {
  // 1. Standby on RC oscillator (safe starting point)
  uint8_t const stdby_rc = 0x00;
  this->write_opcode_(SX1262_SET_STANDBY, &stdby_rc, 1);

  // 2. Configure TCXO via DIO3 — voltage + 5ms timeout (320 ticks at 15.625us/tick)
  uint8_t tcxo_params[4] = {this->tcxo_voltage_, 0x00, 0x01, 0x40};
  this->write_opcode_(SX1262_SET_DIO3_AS_TCXO_CTRL, tcxo_params, sizeof(tcxo_params));

  // 3. Calibrate all blocks
  uint8_t const cal = 0x7F;
  this->write_opcode_(SX1262_CALIBRATE, &cal, 1);
  delay(5);  // Wait for calibration to complete

  // 4. Standby on XOSC (TCXO now running)
  uint8_t const stdby_xosc = 0x01;
  this->write_opcode_(SX1262_SET_STANDBY, &stdby_xosc, 1);

  // 5. Use DC-DC regulator for better efficiency
  uint8_t const reg_mode = 0x01;
  this->write_opcode_(SX1262_SET_REGULATOR_MODE, &reg_mode, 1);

  // 6. DIO2 as RF switch control (for boards with integrated RF switch)
  uint8_t const dio2_rf = 0x01;
  this->write_opcode_(SX1262_SET_DIO2_AS_RF_SWITCH_CTRL, &dio2_rf, 1);

  // 6b. Keep the crystal path alive after RX/TX completion instead of relying on the chip default.
  uint8_t const fallback_mode = SX1262_FALLBACK_STDBY_XOSC;
  this->write_opcode_(SX1262_SET_RX_TX_FALLBACK_MODE, &fallback_mode, 1);

  // 7. FSK packet type
  uint8_t const pkt_type = 0x00;
  this->write_opcode_(SX1262_SET_PACKET_TYPE, &pkt_type, 1);

  // 8. Set frequency to channel 2 (868.95 MHz)
  this->set_frequency_register_(FREQ_CH2);

  // 9. Calibrate image for 863-870 MHz band
  uint8_t cal_img[2] = {0xD7, 0xDB};
  this->write_opcode_(SX1262_CALIBRATE_IMAGE, cal_img, sizeof(cal_img));

  // 9b. Use boosted RX gain for maximum sensitivity.
  uint8_t const rx_gain = 0x96;
  this->write_register_(SX1262_REG_RX_GAIN, &rx_gain, 1);

  // 10. FSK modulation params:
  //     BitRate = 32 * Fxosc / BR_reg → BR_reg = 32 * 32MHz / 38400 = 26667 = 0x00682B
  //     Pulse shape: no shaping (0x00)
  //     Bandwidth: 312.0 kHz (0x19) — wider than needed but safe margin
  //     Fdev = fdev_hz * 2^25 / 32e6 → 19200 * 2^25 / 32e6 = 20133 = 0x004EA5
  uint8_t mod_params[8] = {
      0x00, 0x68, 0x2B,  // Bitrate: 38400 bps
      0x00,              // Pulse shape: no shaping
      0x19,              // Bandwidth: 312.0 kHz
      0x00, 0x4E, 0xA5,  // Fdev: 19200 Hz
  };
  this->write_opcode_(SX1262_SET_MODULATION_PARAMS, mod_params, sizeof(mod_params));

  // 11. Default RX packet params: variable-size GFSK with hardware CCITT CRC validation.
  this->set_rx_packet_params_();

  // 12. Sync word: 0x57 0xFD 0x99 (24-bit UART-derived IO-homecontrol hypothesis)
  uint8_t sync_word[8] = {0x57, 0xFD, 0x99, 0x00, 0x00, 0x00, 0x00, 0x00};
  this->write_register_(SX1262_REG_SYNC_WORD, sync_word, sizeof(sync_word));

  // 12b. CRC registers are configured for potential future hardware-CRC use, but RX uses
  // CRC_OFF because the UART encoding makes hardware CRC checking impossible — the chip
  // sees UART-packed bits, not raw protocol bytes.
  uint8_t crc_init[2] = {0x1D, 0x0F};
  this->write_register_(0x06BC, crc_init, 2);
  uint8_t crc_poly[2] = {0x10, 0x21};
  this->write_register_(0x06BE, crc_poly, 2);

  // 13. Buffer base addresses: TX at 0x00, RX at 0x80
  uint8_t buf_base[2] = {0x00, 0x80};
  this->write_opcode_(SX1262_SET_BUFFER_BASE_ADDRESS, buf_base, sizeof(buf_base));

  // 14. PA config: SX1262 high power PA (paDutyCycle=0x04, hpMax=0x07, deviceSel=0x00=SX1262, paLut=0x01)
  uint8_t pa_config[4] = {0x04, 0x07, 0x00, 0x01};
  this->write_opcode_(SX1262_SET_PA_CONFIG, pa_config, sizeof(pa_config));

  // 14b. Apply Semtech's SX1262 clamp workaround for better tolerance of RF mismatch.
  uint8_t tx_clamp = 0;
  this->read_register_(SX1262_REG_TX_CLAMP_CONFIG, &tx_clamp, 1);
  tx_clamp |= 0x1E;
  this->write_register_(SX1262_REG_TX_CLAMP_CONFIG, &tx_clamp, 1);

  // 15. TX params: power in dBm (SX1262 accepts -9 to +22 directly), ramp 200us (0x04)
  int8_t const power = std::max((int8_t) -9, std::min((int8_t) 22, (int8_t) this->tx_power_));
  uint8_t tx_params[2] = {(uint8_t) power, 0x04};
  this->write_opcode_(SX1262_SET_TX_PARAMS, tx_params, sizeof(tx_params));

  // 16. IRQ config: map TxDone + RxDone + SyncWordValid + CrcErr to DIO1
  uint8_t irq_params[8] = {
      0x00, 0x4B,  // irqMask: TxDone(0x0001) | RxDone(0x0002) | SyncWordValid(0x0008) | CrcErr(0x0040)
      0x00, 0x4B,  // dio1Mask: same
      0x00, 0x00,  // dio2Mask: none
      0x00, 0x00,  // dio3Mask: none
  };
  this->write_opcode_(SX1262_SET_DIO_IRQ_PARAMS, irq_params, sizeof(irq_params));

  // 17. Attach DIO1 interrupt
  this->dio1_pin_->attach_interrupt(&RadioSX1262::gpio_intr, this, gpio::INTERRUPT_RISING_EDGE);

  // 18. Clear any pending IRQs
  this->clear_irq_status_(0xFFFF);
  this->clear_device_errors_();

  // 19. Enter continuous receive
  uint8_t rx_continuous[3] = {0xFF, 0xFF, 0xFF};  // 0xFFFFFF = continuous
  this->write_opcode_(SX1262_SET_RX, rx_continuous, sizeof(rx_continuous));
}

// === Mode control ===

void RadioSX1262::set_mode_standby() {
  uint8_t const stdby = 0x01;  // STDBY_XOSC
  this->write_opcode_(SX1262_SET_STANDBY, &stdby, 1);
}

void RadioSX1262::set_mode_rx() {
  uint8_t rx_continuous[3] = {0xFF, 0xFF, 0xFF};
  this->write_opcode_(SX1262_SET_RX, rx_continuous, sizeof(rx_continuous));
}

// === Frequency control ===

void RadioSX1262::set_frequency_register_(uint32_t freq_hz) {
  auto freq_reg = (uint32_t) ((double) freq_hz * (1 << 25) / 32e6);
  uint8_t params[4] = {
      (uint8_t) (freq_reg >> 24),
      (uint8_t) (freq_reg >> 16),
      (uint8_t) (freq_reg >> 8),
      (uint8_t) freq_reg,
  };
  this->write_opcode_(SX1262_SET_RF_FREQUENCY, params, sizeof(params));
  this->current_freq_ = freq_hz;
}

void RadioSX1262::change_frequency(uint32_t freq_hz) {
  this->set_mode_standby();
  this->set_frequency_register_(freq_hz);
  this->set_mode_rx();
}

int16_t RadioSX1262::read_rssi() {
  uint8_t raw = 0;
  this->read_opcode_(SX1262_GET_RSSI_INST, &raw, 1);
  return -(int16_t) raw / 2;
}

// === Packet TX ===

bool RadioSX1262::send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) {
  if (len == 0)
    return false;

#ifdef IOHOME_FRAME_LOG
  log_frame("TX", data, len, tx_config.freq_hz, tx_config.preamble_len);
#endif

  this->set_mode_standby();

  // Set frequency (already in standby, no need for full change_frequency cycle)
  this->set_frequency_register_(tx_config.freq_hz);

  uint8_t frame_with_crc[FRAME_MAX_SIZE + 2] = {0};
  uint8_t tx_buf[RADIO_PACKET_BUFFER_SIZE];
  if ((uint16_t) len + 2 > (uint16_t) sizeof(frame_with_crc))
    return false;

  memcpy(frame_with_crc, data, len);
  const uint16_t crc = crc_ccitt(data, len);
  frame_with_crc[len] = crc & 0xFF;
  frame_with_crc[len + 1] = (crc >> 8) & 0xFF;

  const uint8_t encoded_len = uart_encode_packet(frame_with_crc, len + 2, tx_buf, sizeof(tx_buf));
  if (encoded_len == 0)
    return false;

  this->set_packet_params_(tx_config.preamble_len, encoded_len, SX1262_GFSK_PACKET_TYPE_KNOWN_LENGTH,
                           SX1262_GFSK_CRC_OFF);

  // Clear IRQs and write to TX buffer at offset 0
  this->clear_irq_status_(0xFFFF);
  this->write_buffer_(0x00, tx_buf, encoded_len);

  // Start TX with 4s timeout (256000 ticks at 15.625us/tick = 0x03E800)
  this->clear_dio_fired();
  uint8_t tx_timeout[3] = {0x03, 0xE8, 0x00};
  this->write_opcode_(SX1262_SET_TX, tx_timeout, sizeof(tx_timeout));

  auto read_irq_status = [this]() {
    uint8_t irq_raw[2] = {0};
    this->read_opcode_(SX1262_GET_IRQ_STATUS, irq_raw, sizeof(irq_raw));
    return (uint16_t) (((uint16_t) irq_raw[0] << 8) | irq_raw[1]);
  };

  // Wait for an actual TxDone IRQ. DIO1 is shared with RX-related events, so
  // a stale or unrelated interrupt must not be treated as TX completion.
  uint32_t const start = millis();
  uint16_t tx_irq = 0;
  while (true) {
    if (!this->is_dio_fired()) {
      if (millis() - start > 4000) {
        ESP_LOGE(TAG, "TX timeout — DIO1 never fired");
        this->set_mode_standby();
        return false;
      }
      App.feed_wdt();
      delayMicroseconds(100);
      continue;
    }

    this->clear_dio_fired();
    tx_irq = read_irq_status();
    if ((tx_irq & SX1262_IRQ_TX_DONE) != 0)
      break;

    if (tx_irq != 0) {
      this->clear_irq_status_(tx_irq);
    }

    if (millis() - start > 4000) {
      ESP_LOGE(TAG, "TX timeout — no TX_DONE IRQ (last_irq=0x%04X)", tx_irq);
      this->set_mode_standby();
      return false;
    }
  }
  // TxDone used the same DIO1 latch as RX. Clear the local latch before
  // re-arming RX so an immediate reply remains visible to wait_for_packet().
  this->clear_dio_fired();

  // Clear IRQs, restore default packet params, return to RX. TxDone already
  // left the radio in fallback standby mode, so avoid an extra explicit
  // standby command here.
  this->clear_irq_status_(0xFFFF);
  this->reset_rx_state_(false);
  return true;
}

// === Packet RX (blocking) ===

bool RadioSX1262::wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) {
  // Blocking receive with timeout. Returns true if a packet was successfully received.
  // This orchestrator decomposes the state machine into three low‑complexity helpers.
  this->prepare_blocking_receive_(packet);

  uint32_t const start = millis();
  bool saw_dio1 = false;
  uint16_t irq = 0;

  // Phase 1: Wait for first activity (DIO interrupt or any IRQ status change).
  if (!this->poll_until_activity_(start, timeout_ms, saw_dio1, irq)) {
    return false;
  }

  // If DIO fired, refresh IRQ status to capture the reason bits.
  if (saw_dio1) {
    this->clear_dio_fired();
    irq = this->read_irq_status_raw();
  }

  // Phase 2: Resolve the SYNC_WORD_VALID → RX_DONE race condition.
  if (!this->resolve_sync_race_(start, timeout_ms, irq)) {
    return false;
  }

  // Phase 3: Finalize — either read the packet or treat as a failure.
  return this->finalize_receive_(packet, irq);
}

// === Shared RX helper ===

bool RadioSX1262::read_rx_packet(RadioRxPacket &packet, bool blocking_wait, uint16_t irq_status) {
  uint8_t rx_status[2] = {0};
  uint8_t rx_buf[RADIO_PACKET_BUFFER_SIZE] = {0};
  uint8_t recovered_buf[RADIO_PACKET_BUFFER_SIZE] = {0};

  this->read_opcode_(SX1262_GET_RX_BUFFER_STATUS, rx_status, sizeof(rx_status));
  uint8_t const reported_len = std::min(rx_status[0], (uint8_t) sizeof(rx_buf));
  uint8_t const rx_offset = rx_status[1];
  uint8_t raw_probe_len = reported_len;
  if (reported_len > 0 && reported_len < 32) {
    // When the SX1262 reports a short packet length, still pull the full raw window. Earlier
    // bring-up on this chip showed that trimming this probe too aggressively makes the recovered
    // post-auth response less reliable because the useful UART-packed tail may sit past the
    // chip-reported boundary.
    raw_probe_len = sizeof(rx_buf);
  }
  if (reported_len == SX1262_RX_PROBE_PACKET_LEN)
    raw_probe_len = SX1262_RX_PROBE_PACKET_LEN;
  if (raw_probe_len > 0)
    this->read_buffer_(rx_offset, rx_buf, raw_probe_len);

  // SX1262 does not expose the already-decoded IO-homecontrol frame the way SX1276 does. We first
  // capture the raw bytes exactly as reported by the chip, then recover the UART-packed protocol
  // stream in software and only pass a plausible frame up to the parser. This software recovery
  // path is the SX1262-specific adaptation to the same protocol.
  UartProbeResult probe = find_uart_probe(rx_buf, raw_probe_len);
  if (probe.valid) {
    memcpy(recovered_buf, probe.decoded + probe.frame_start, probe.frame_len);
    memcpy(packet.data, recovered_buf, probe.frame_len);
    packet.len = probe.frame_len;
  } else {
    uint8_t const copy_len = std::min(reported_len, FRAME_MAX_SIZE);
    if (copy_len > 0)
      memcpy(packet.data, rx_buf, copy_len);
    packet.len = copy_len;
  }
  packet.freq_hz = this->current_freq_;
  this->fill_capture_info_(blocking_wait, irq_status, rx_offset, reported_len, rx_buf, raw_probe_len, packet.data,
                           packet.len);

#ifdef IOHOME_FRAME_LOG
  if (packet.len > 0)
    log_frame("RX", packet.data, packet.len, this->current_freq_);
#endif
  this->reset_rx_state_();
  return packet.len > 0;
}

// === Packet RX (non-blocking) ===

bool RadioSX1262::check_for_packet(RadioRxPacket &packet) {
  if (!this->is_dio_fired())
    return false;
  this->prepare_nonblocking_receive_(packet);

  uint8_t irq_raw[2];
  this->read_opcode_(SX1262_GET_IRQ_STATUS, irq_raw, 2);
  uint16_t const irq = ((uint16_t) irq_raw[0] << 8) | irq_raw[1];

  if ((irq & SX1262_IRQ_SYNC_WORD_VALID) != 0 && (irq & SX1262_IRQ_RX_DONE) == 0) {
    this->clear_irq_status_(SX1262_IRQ_SYNC_WORD_VALID);
    return false;
  }

  if ((irq & SX1262_IRQ_RX_DONE) != 0) {
    return this->read_rx_packet(packet, false, irq);
  }

  this->fill_capture_info_(false, irq, 0, 0, nullptr, 0, nullptr, 0);
  this->reset_rx_state_();
  return false;
}

}  // namespace home_io_control
}  // namespace esphome

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
