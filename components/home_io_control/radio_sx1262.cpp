/// @file radio_sx1262.cpp
/// @brief SX1262 radio driver implementation for IO-Homecontrol.
/// @ingroup hioc_radio
///
/// Unlike the SX1276, the SX1262 does not provide Semtech's IoHomeOn mode for
/// IO-Homecontrol. On SX1276 that mode handles key protocol details in hardware:
/// CRC generation and checking, packet boundary handling, and delivery of
/// already-decoded protocol bytes in the FIFO. On SX1262 those pieces are reproduced
/// in software by SoftPhyDriverBase (shared with RadioLR1121) on top of generic GFSK
/// support; this file supplies the SPI transport and every register/opcode encoding
/// SoftPhyDriverBase's shared RX/TX orchestration calls through virtual primitives.
///
/// The chip interface itself is opcode-based SPI instead of the SX1276 register model, and
/// every transaction must respect the BUSY line. For experiment builds, the RX path
/// prioritizes preserving the chip-reported bytes and metadata verbatim.

// The SX1262 path is intentionally low-level: opcode payloads, line-coding widths, and recovery
// thresholds are written in the same shape as the chip protocol and on-air framing.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

#include "radio_sx1262.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.sx1262";
static const uint8_t SX1262_SYNC_WORD_PARAM_24_BITS = 0x18;

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

uint32_t RadioSX1262::read_irq_status_raw() {
  uint8_t irq_raw[2] = {0};
  this->read_opcode_(SX1262_GET_IRQ_STATUS, irq_raw, 2);
  return (static_cast<uint32_t>(irq_raw[0]) << 8) | static_cast<uint32_t>(irq_raw[1]);
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
  // preamble_len arrives in bytes, matching every other layer in this codebase (LONG_PREAMBLE,
  // SHORT_PREAMBLE, the tuning defaults) and the SX1276's byte-wide RegPreambleMsb/Lsb. This
  // chip's SetPacketParams field is bit-denominated instead — Semtech's own struct names it
  // sx126x_pkt_params_gfsk_t.preamble_len_in_bits — so convert at this last step, right before
  // the value leaves for the wire.
  const uint16_t preamble_bits = preamble_len * 8;
  uint8_t params[9] = {
      (uint8_t) (preamble_bits >> 8),  // Preamble length MSB
      (uint8_t) preamble_bits,         // Preamble length LSB
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

void RadioSX1262::set_rx_packet_params() {
  // The variable-size packet engine recovers a stable 15-byte boundary, but that boundary is
  // too short for a full software UART decode and buffer reads past it are not trustworthy.
  // Probe again with a fixed raw packet size that matches typical UART-packed 23-25 byte
  // IO-homecontrol frames (SOFT_PHY_RX_PROBE_PACKET_LEN, shared with LR1121 — see its doc
  // comment in radio_soft_phy_driver_base.h).
  this->set_packet_params_(8, SOFT_PHY_RX_PROBE_PACKET_LEN, SX1262_GFSK_PACKET_TYPE_KNOWN_LENGTH, SX1262_GFSK_CRC_OFF);
}

void RadioSX1262::write_modulation_params_() {
  // SX1262 GFSK modulation parameters for the IO-Homecontrol 868 MHz waveform.
  //
  // BitRate = 32 * Fxosc / BR_reg → BR_reg = 32 * 32MHz / 38400 = 26667 = 0x00682B
  // Pulse shape: Gaussian BT=1.0 (0x0B) — reduces TX spectral occupation.
  // Bandwidth: the register byte is runtime-tunable via rx_bandwidth_. The default
  //   117.3 kHz (0x0B) is wider than the Carson-rule minimum (77 kHz) but needed
  //   to tolerate the SX1262 LO frequency offset after the TX→RX transition. At
  //   58.6 kHz the demodulator produced ~50% bit errors on post-TX frames; 117.3 kHz
  //   gives a clean decode (confirmed via loopback turnaround test).
  // Fdev = fdev_hz * 2^25 / 32e6 → 19200 * 2^25 / 32e6 = 20133 = 0x004EA5
  uint8_t mod_params[8] = {
      0x00,
      0x68,
      0x2B,                                       // Bitrate: 38400 bps
      0x0B,                                       // Pulse shape: Gaussian BT=1.0
      static_cast<uint8_t>(this->rx_bandwidth_),  // Bandwidth: runtime-tunable
      0x00,
      0x4E,
      0xA5,  // Fdev: 19200 Hz
  };
  this->write_opcode_(SX1262_SET_MODULATION_PARAMS, mod_params, sizeof(mod_params));
}

void RadioSX1262::clear_irq_status(uint32_t irq_mask) {
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

void RadioSX1262::configure_buffer_base() {
  uint8_t buf_base[2] = {0x00, 0x80};
  this->write_opcode_(SX1262_SET_BUFFER_BASE_ADDRESS, buf_base, sizeof(buf_base));
}

void RadioSX1262::fill_capture_info(bool blocking_wait, uint32_t irq_status, uint8_t rx_offset, uint8_t reported_len,
                                    const uint8_t *raw, uint8_t raw_len, const uint8_t *frame, uint8_t frame_len) {
  uint8_t packet_status[3] = {0};
  this->read_opcode_(SX1262_GET_PACKET_STATUS, packet_status, sizeof(packet_status));

  this->populate_capture_base_(blocking_wait, this->current_freq_, -(int16_t) packet_status[1] / 2, raw, raw_len, frame,
                               frame_len);
  this->last_capture_.rx_done = (irq_status & SX1262_IRQ_RX_DONE) != 0;
  this->last_capture_.crc_error = (irq_status & SX1262_IRQ_CRC_ERR) != 0;
  this->last_capture_.irq_status = static_cast<uint16_t>(irq_status);
  this->last_capture_.packet_status = packet_status[0];
  this->last_capture_.rx_offset = rx_offset;
  this->last_capture_.reported_len = reported_len;
}

uint8_t RadioSX1262::read_rssi_raw_byte() {
  uint8_t raw = 0;
  this->read_opcode_(SX1262_GET_RSSI_INST, &raw, 1);
  return raw;
}

void RadioSX1262::get_rx_buffer_status(uint8_t &reported_len, uint8_t &rx_offset) {
  uint8_t rx_status[2] = {0};
  this->read_opcode_(SX1262_GET_RX_BUFFER_STATUS, rx_status, sizeof(rx_status));
  reported_len = rx_status[0];
  rx_offset = rx_status[1];
}

void RadioSX1262::start_tx() {
  // Start TX with 4s timeout (256000 ticks at 15.625us/tick = 0x03E800).
  uint8_t tx_timeout[3] = {0x03, 0xE8, 0x00};
  this->write_opcode_(SX1262_SET_TX, tx_timeout, sizeof(tx_timeout));
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
  this->set_frequency_register(FREQ_CH2);

  // 9. Calibrate image for 863-870 MHz band
  uint8_t cal_img[2] = {0xD7, 0xDB};
  this->write_opcode_(SX1262_CALIBRATE_IMAGE, cal_img, sizeof(cal_img));

  // 9b. Use boosted RX gain for maximum sensitivity.
  uint8_t const rx_gain = 0x96;
  this->write_register_(SX1262_REG_RX_GAIN, &rx_gain, 1);

  // 10. Apply GFSK modulation parameters (detailed values live in write_modulation_params_()).
  this->write_modulation_params_();

  // 11. Default RX packet params: variable-size GFSK with hardware CCITT CRC validation.
  this->set_rx_packet_params();

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
  this->configure_buffer_base();

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
  this->clear_irq_status(0xFFFF);
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

void RadioSX1262::set_frequency_register(uint32_t freq_hz) {
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

void RadioSX1262::set_rx_bandwidth_(SX1262RxBandwidth bandwidth) {
  this->rx_bandwidth_ = bandwidth;
  this->write_modulation_params_();
}

}  // namespace home_io_control
}  // namespace esphome

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
