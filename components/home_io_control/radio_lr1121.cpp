/// @file radio_lr1121.cpp
/// @brief LR1121 radio driver implementation for IO-Homecontrol.
/// @ingroup hioc_radio
///
/// See radio_lr1121.h for the architectural context: this driver is a re-plumbed
/// RadioSX1262 over a different SPI command set. Everything about the software UART
/// PHY (TX bit-encoding, RX probe/CRC recovery) is identical to the SX1262 and lives
/// in radio_soft_phy.{h,cpp}; this file only has to reproduce the LR1121-specific
/// transport (16-bit opcodes, two-transaction command/response, 32-bit IRQ word) and
/// the chip's own GFSK/RF-switch/TCXO/PA configuration.

// Opcode payloads, line-coding widths, and recovery thresholds are written in the same shape
// as the chip protocol and on-air framing they reproduce.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

#include "radio_lr1121.h"
#include "log_frame.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include <cinttypes>

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.lr1121";

// Fixed probe length — identical reasoning to SX1262 (SX1262_RX_PROBE_PACKET_LEN): chosen from
// captures of 23-25 byte protocol frames after UART packing and CRC appending. This is a
// protocol-frame-size property, not a chip quirk, so the same value applies here unchanged.
static const uint8_t LR1121_RX_PROBE_PACKET_LEN = 48;

// === SPI Communication (16-bit opcode, two-transaction) ===

void RadioLR1121::wait_busy_() {
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

void RadioLR1121::write_command_(uint16_t opcode, const uint8_t *params, uint8_t len) {
  // Write-only phase: MISO is don't-care while we send the opcode + params, so spi_write()
  // (MOSI only) is used rather than spi_transfer() — the two are equivalent on real hardware,
  // but only spi_write() correctly signals "no response byte here" to the SpiAccess contract.
  this->wait_busy_();
  this->spi_->spi_enable();
  this->spi_->spi_write((opcode >> 8) & 0xFF);
  this->spi_->spi_write(opcode & 0xFF);
  for (uint8_t i = 0; i < len; i++)
    this->spi_->spi_write(params[i]);
  this->spi_->spi_disable();
}

void RadioLR1121::read_command_(uint16_t opcode, const uint8_t *params, uint8_t params_len, uint8_t *out,
                                uint8_t out_len) {
  // First transaction: send opcode + request params, NSS up. The chip raises BUSY while it
  // prepares the response.
  this->write_command_(opcode, params, params_len);
  // Second transaction: wait BUSY low again, then clock out Stat1 followed by the response
  // (MOSI don't-care here, so spi_read() rather than spi_transfer(0x00)).
  this->wait_busy_();
  this->spi_->spi_enable();
  this->last_stat1_ = this->spi_->spi_read();  // Stat1 — command status byte.
  for (uint8_t i = 0; i < out_len; i++)
    out[i] = this->spi_->spi_read();
  this->spi_->spi_disable();
  this->log_command_status_(opcode);
}

void RadioLR1121::log_command_status_(uint16_t opcode) {
  // TODO(hw-verify): the Stat1 byte's command-status bit field was not cross-checked against
  // the UM this session (see file-level @todo in radio_lr1121.h), so this only surfaces the raw
  // byte for diagnostics rather than decoding a specific error enum. Step 6 hardware bring-up
  // should confirm the encoding and tighten this into a real error check if warranted.
#ifdef IOHOME_FRAME_LOG
  ESP_LOGV(TAG, "cmd 0x%04X stat1=0x%02X", opcode, this->last_stat1_);
#endif
}

void RadioLR1121::write_buffer_(const uint8_t *data, uint8_t len) {
  this->write_command_(LR1121_CMD_WRITE_BUFFER, data, len);
}

void RadioLR1121::read_buffer_(uint8_t offset, uint8_t len, uint8_t *data) {
  uint8_t params[2] = {offset, len};
  this->read_command_(LR1121_CMD_READ_BUFFER, params, sizeof(params), data, len);
}

uint32_t RadioLR1121::read_irq_status_raw() {
  // GetStatus returns Stat1, Stat2, then the 32-bit IRQ status word (design §3.2:
  // "read IRQ status non-destructively via GetStatus"). TODO(hw-verify): the exact response
  // byte count/ordering (Stat1+Stat2+4 IRQ bytes here) was not cross-checked against the UM.
  uint8_t resp[6] = {0};
  this->read_command_(LR1121_CMD_GET_STATUS, nullptr, 0, resp, sizeof(resp));
  return (static_cast<uint32_t>(resp[2]) << 24) | (static_cast<uint32_t>(resp[3]) << 16) |
         (static_cast<uint32_t>(resp[4]) << 8) | static_cast<uint32_t>(resp[5]);
}

// === wait_for_packet static helpers ===

/// Poll for first radio activity (IRQ pin or any IRQ status) within timeout.
/// Mirrors RadioSX1262::poll_until_activity_ exactly, over the LR1121's wider IRQ word.
bool RadioLR1121::poll_until_activity_(uint32_t start, uint32_t timeout_ms, bool &saw_irq, uint32_t &irq) {
  while (true) {
    if (this->is_dio_fired()) {
      saw_irq = true;
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

/// Resolve the SYNC_WORD_VALID → RX_DONE race condition. Mirrors RadioSX1262::resolve_sync_race_.
bool RadioLR1121::resolve_sync_race_(uint32_t start, uint32_t timeout_ms, uint32_t &irq) {
  if ((irq & LR1121_IRQ_SYNC_WORD_VALID) == 0 || (irq & LR1121_IRQ_RX_DONE) != 0) {
    return true;
  }
  this->clear_irq_status_(LR1121_IRQ_SYNC_WORD_VALID);
  while (millis() - start <= timeout_ms) {
    if (!this->is_dio_fired()) {
      irq = this->read_irq_status_raw();
      if ((irq & LR1121_IRQ_RX_DONE) != 0)
        return true;
      App.feed_wdt();
      delay(1);
      continue;
    }
    this->clear_dio_fired();
    irq = this->read_irq_status_raw();
    if ((irq & LR1121_IRQ_RX_DONE) != 0)
      return true;
    if (irq != 0)
      this->clear_irq_status_(irq);
  }
  return false;  // timeout
}

/// Finalize receive: read the packet if RX_DONE is set, otherwise record failure.
bool RadioLR1121::finalize_receive_(RadioRxPacket &packet, uint32_t irq) {
  if ((irq & LR1121_IRQ_RX_DONE) == 0) {
    this->fill_capture_info_(true, irq, 0, 0, nullptr, 0, nullptr, 0);
    this->reset_rx_state_();
    return false;
  }
  return this->read_rx_packet(packet, true, irq);
}

// === Packet params helper ===

void RadioLR1121::set_packet_params_(uint16_t preamble_len, uint8_t payload_len, uint8_t packet_type,
                                     uint8_t crc_type) {
  // Same 9-field shape as RadioSX1262::set_packet_params_ (design §3.2 step 7 gives the LR1121
  // field values as a direct match to the SX1262's), just written via the LR1121 opcode.
  uint8_t params[9] = {
      (uint8_t) (preamble_len >> 8),    // Preamble length MSB
      (uint8_t) preamble_len,           // Preamble length LSB
      LR1121_PREAMBLE_DETECTOR_16_BIT,  // Preamble detector: 16 bits
      LR1121_SYNC_WORD_PARAM_24_BITS,   // Sync word length: 24 bits
      0x00,                             // Address comparison: off
      packet_type,                      // GFSK packet type: known length or variable size
      payload_len,                      // Configured payload length
      crc_type,                         // CRC type
      0x00,                             // Whitening: off
  };
  this->write_command_(LR1121_CMD_SET_PACKET_PARAMS, params, sizeof(params));
}

void RadioLR1121::set_rx_packet_params_() {
  // Same fixed-length raw-probe strategy as SX1262 (see RadioSX1262::set_rx_packet_params_ for
  // the full rationale): the LR1121 gets the identical treatment because the reasoning is about
  // UART-packed frame sizes, not chip-specific packet-engine behavior.
  this->set_packet_params_(8, LR1121_RX_PROBE_PACKET_LEN, LR1121_GFSK_PACKET_FIXED_LENGTH, LR1121_GFSK_CRC_OFF);
}

void RadioLR1121::write_modulation_params_() {
  // LR1121 GFSK modulation parameters for the IO-Homecontrol 868 MHz waveform, written as
  // plain Hz/bps 32-bit values (design §1.2: "RF frequency / bitrate / fdev are plain Hz /
  // bit/s 32-bit values" — no PLL-step math needed, unlike SX1262).
  // TODO(hw-verify): the field ORDER (bitrate, pulse shape, bandwidth, fdev) mirrors SX1262's
  // own field order; not independently cross-checked against the UM this session.
  uint32_t const bitrate_bps = 38400;
  uint32_t const fdev_hz = 19200;
  uint8_t mod_params[10] = {
      (uint8_t) (bitrate_bps >> 24),
      (uint8_t) (bitrate_bps >> 16),
      (uint8_t) (bitrate_bps >> 8),
      (uint8_t) bitrate_bps,                      // Bitrate: 38400 bps
      0x0B,                                       // Pulse shape: Gaussian BT=1.0 (same encoding as SX126x)
      static_cast<uint8_t>(this->rx_bandwidth_),  // Bandwidth: runtime-tunable (same encoding as SX126x)
      (uint8_t) (fdev_hz >> 24),
      (uint8_t) (fdev_hz >> 16),
      (uint8_t) (fdev_hz >> 8),
      (uint8_t) fdev_hz,  // Fdev: 19200 Hz
  };
  this->write_command_(LR1121_CMD_SET_MODULATION_PARAMS, mod_params, sizeof(mod_params));
}

uint16_t RadioLR1121::get_errors_() {
  uint8_t errors_raw[2] = {0};
  this->read_command_(LR1121_CMD_GET_ERRORS, nullptr, 0, errors_raw, sizeof(errors_raw));
  return (static_cast<uint16_t>(errors_raw[0]) << 8) | errors_raw[1];
}

void RadioLR1121::clear_errors_() { this->write_command_(LR1121_CMD_CLEAR_ERRORS, nullptr, 0); }

void RadioLR1121::clear_irq_status_(uint32_t irq_mask) {
  uint8_t clear_irq[4] = {
      (uint8_t) ((irq_mask >> 24) & 0xFF),
      (uint8_t) ((irq_mask >> 16) & 0xFF),
      (uint8_t) ((irq_mask >> 8) & 0xFF),
      (uint8_t) (irq_mask & 0xFF),
  };
  this->write_command_(LR1121_CMD_CLEAR_IRQ, clear_irq, sizeof(clear_irq));
}

void RadioLR1121::reset_rx_state_(bool force_standby) {
  if (force_standby)
    this->set_mode_standby();
  this->clear_irq_status_(0xFFFFFFFF);
  this->set_rx_packet_params_();
  this->set_mode_rx();
}

void RadioLR1121::fill_capture_info_(bool blocking_wait, uint32_t irq_status, uint8_t rx_offset, uint8_t reported_len,
                                     const uint8_t *raw, uint8_t raw_len, const uint8_t *frame, uint8_t frame_len) {
  // Note for reviewers diffing against RadioSX1262::fill_capture_info_: that driver reads RSSI
  // from GetPacketStatus, an opcode atomically tied to the just-completed reception. No such
  // opcode is in this driver's pre-verified table (design §0.3.3), so this reuses the plain
  // GetRssiInst live-RSSI read instead — an extra SPI round-trip, and RSSI as of *this call*
  // rather than strictly at packet-detection time. Revisit if a packet-status-equivalent opcode
  // is confirmed during Step 6 hardware bring-up.
  this->populate_capture_base_(blocking_wait, this->current_freq_, this->read_rssi(), raw, raw_len, frame, frame_len);
  this->last_capture_.rx_done = (irq_status & LR1121_IRQ_RX_DONE) != 0;
  this->last_capture_.crc_error = (irq_status & LR1121_IRQ_CRC_ERR) != 0;
  // RadioCaptureInfo::irq_status is uint16_t; map the 32-bit word down by taking bits [2..10]
  // and shifting right by 2 (design §3.2 "IRQ-width note" — one documented place for this).
  this->last_capture_.irq_status = static_cast<uint16_t>((irq_status >> 2) & 0x01FF);
  this->last_capture_.rx_offset = rx_offset;
  this->last_capture_.reported_len = reported_len;
}

// === ISR ===

void IRAM_ATTR RadioLR1121::gpio_intr(RadioLR1121 *arg) { arg->mark_dio_fired_from_isr(); }

// === Initialization ===

bool RadioLR1121::init() {
  // --- Pin setup ---
  this->rst_pin_->setup();
  this->irq_pin_->setup();
  this->busy_pin_->setup();

  // --- Hardware reset ---
  this->reset_hardware_();
  this->wait_busy_();
  if (this->failed_)
    return false;

  this->configure_radio_();
  if (this->failed_)
    return false;

  ESP_LOGI(TAG, "LR1121 initialized");
  return true;
}

void RadioLR1121::dump_debug() {
  uint8_t version[4] = {0};
  this->read_command_(LR1121_CMD_GET_VERSION, nullptr, 0, version, sizeof(version));
  uint32_t const irq = this->read_irq_status_raw();
  uint16_t const errors = this->get_errors_();

  ESP_LOGCONFIG(TAG, "  LR1121 Diagnostic:");
  ESP_LOGCONFIG(TAG, "    Device type: 0x%02X (expect 0x%02X)", version[0], LR1121_DEVICE_TYPE);
  ESP_LOGCONFIG(TAG, "    HW version: 0x%02X, FW version: %u.%u", version[1], version[2], version[3]);
  ESP_LOGCONFIG(TAG, "    BUSY=%d IRQ=%d", this->busy_pin_->digital_read(), this->irq_pin_->digital_read());
  ESP_LOGCONFIG(TAG, "    IRQ status: 0x%08" PRIX32, irq);
  ESP_LOGCONFIG(TAG, "    Device errors: 0x%04X", errors);
  ESP_LOGCONFIG(TAG, "    Last Stat1: 0x%02X", this->last_stat1_);
}

void RadioLR1121::configure_radio_() {
  // 1. Identity check: GetVersion — device type must be LR1121 (0x03). Response layout
  //    TODO(hw-verify): assumed [device_type, hw_version, fw_major, fw_minor] (4 bytes).
  uint8_t version[4] = {0};
  this->read_command_(LR1121_CMD_GET_VERSION, nullptr, 0, version, sizeof(version));
  if (version[0] != LR1121_DEVICE_TYPE) {
    ESP_LOGE(TAG, "Unexpected device type 0x%02X (expected 0x%02X) — not an LR1121?", version[0], LR1121_DEVICE_TYPE);
    this->failed_ = true;
    return;
  }
  ESP_LOGI(TAG, "LR1121 detected: hw=0x%02X fw=%u.%u", version[1], version[2], version[3]);

  // 2. TCXO: map the YAML TCXO_VOLTAGE_OPTIONS code (1_6V=0x01 .. 3_3V=0x08, __init__.py) to the
  //    LR1121's own 0x00-0x07 voltage code (design §0.5.3: "codes run 0x00-0x07 for 1.6-3.3V" —
  //    one less than the YAML/SX1262 numbering, so a simple -1 mapping is exact if the two
  //    tables are both linear 0.1V-ish steps in the same order; verify in Step 6).
  auto const tcxo_code = static_cast<uint8_t>(this->tcxo_voltage_yaml_code_ - 1);
  uint8_t tcxo_params[4] = {tcxo_code, LR1121_TCXO_STARTUP_DELAY_TICKS_MSB, LR1121_TCXO_STARTUP_DELAY_TICKS_MID,
                            LR1121_TCXO_STARTUP_DELAY_TICKS_LSB};
  this->write_command_(LR1121_CMD_SET_TCXO_MODE, tcxo_params, sizeof(tcxo_params));

  // 3. Clear errors, then calibrate all blocks — order matters: calibration must run on the
  //    TCXO clock, which was just configured (design §3.2 step 3).
  this->clear_errors_();
  uint8_t const cal_all = LR1121_CALIBRATE_ALL_BLOCKS;
  this->write_command_(LR1121_CMD_CALIBRATE, &cal_all, 1);
  delay(5);  // Wait for calibration to complete (same margin as SX1262).

  // 4. RF switch: T3-S3 DIO5/DIO6 table (secondhand, see radio_lr1121.h constants).
  uint8_t rfswitch_params[8] = {
      LR1121_RFSWITCH_ENABLE_DIO5_DIO6,
      LR1121_RFSWITCH_STANDBY,
      LR1121_RFSWITCH_RX,
      LR1121_RFSWITCH_TX,
      LR1121_RFSWITCH_TX_HP,
      LR1121_RFSWITCH_TX_HF,
      LR1121_RFSWITCH_GNSS,
      LR1121_RFSWITCH_WIFI,
  };
  this->write_command_(LR1121_CMD_SET_DIO_AS_RF_SWITCH, rfswitch_params, sizeof(rfswitch_params));

  // 5. GFSK packet type.
  uint8_t const pkt_type = LR1121_PACKET_TYPE_GFSK;
  this->write_command_(LR1121_CMD_SET_PACKET_TYPE, &pkt_type, 1);

  // 6. Set frequency to channel 2 (868.95 MHz) — plain Hz, no PLL-step conversion.
  this->set_frequency_register_(FREQ_CH2);

  // 7. Apply GFSK modulation parameters (detailed values live in write_modulation_params_()).
  this->write_modulation_params_();

  // 8. Default RX packet params: fixed-length GFSK with software CRC (hardware CRC unusable —
  //    same reasoning as SX1262, see radio_sx1262.cpp file header).
  this->set_rx_packet_params_();

  // 9. Sync word: 0x57 0xFD 0x99 + zero padding (identical bytes to the SX1262/SX1276 UART sync
  //    word hypothesis — written via the LR1121's own SetGfskSyncWord opcode rather than a raw
  //    register write).
  uint8_t sync_word[8] = {0x57, 0xFD, 0x99, 0x00, 0x00, 0x00, 0x00, 0x00};
  this->write_command_(LR1121_CMD_SET_GFSK_SYNC_WORD, sync_word, sizeof(sync_word));

  // 10. PA config: LP PA (design §1.2 PA row — LP first, HP is a later tuning exercise).
  //     TODO(hw-verify): parameter layout assumed [paSel=LP, paRegSupply, paDutyCycle, paHpSel].
  uint8_t pa_config[4] = {0x00, 0x01, 0x04, 0x00};
  this->write_command_(LR1121_CMD_SET_PA_CONFIG, pa_config, sizeof(pa_config));

  // 11. TX params: power in dBm, ramp 200us (0x04, same ramp encoding as SX1262).
  int8_t const power = std::max((int8_t) -9, std::min((int8_t) 22, (int8_t) this->tx_power_));
  uint8_t tx_params[2] = {(uint8_t) power, 0x04};
  this->write_command_(LR1121_CMD_SET_TX_PARAMS, tx_params, sizeof(tx_params));

  // 12. Keep the crystal path alive after RX/TX completion.
  uint8_t const fallback_mode = LR1121_FALLBACK_STDBY_XOSC;
  this->write_command_(LR1121_CMD_SET_RX_TX_FALLBACK_MODE, &fallback_mode, 1);

  // 13. IRQ config: 32-bit enable mask + first DIO's 32-bit mask (routed to DIO9) + second DIO's
  //     mask (unused, all zero) — design §Step 2 item 2: "mask 0 for the second IRQ output".
  uint8_t irq_params[12] = {
      (uint8_t) (LR1121_IRQ_DIO_ENABLE_MASK >> 24),
      (uint8_t) (LR1121_IRQ_DIO_ENABLE_MASK >> 16),
      (uint8_t) (LR1121_IRQ_DIO_ENABLE_MASK >> 8),
      (uint8_t) LR1121_IRQ_DIO_ENABLE_MASK,  // irq enable mask
      (uint8_t) (LR1121_IRQ_DIO_ENABLE_MASK >> 24),
      (uint8_t) (LR1121_IRQ_DIO_ENABLE_MASK >> 16),
      (uint8_t) (LR1121_IRQ_DIO_ENABLE_MASK >> 8),
      (uint8_t) LR1121_IRQ_DIO_ENABLE_MASK,  // DIO9 (irq1) mask
      0x00,
      0x00,
      0x00,
      0x00,  // second IRQ output — unused
  };
  this->write_command_(LR1121_CMD_SET_DIO_IRQ_PARAMS, irq_params, sizeof(irq_params));

  // 14. Attach the DIO9 interrupt.
  this->irq_pin_->attach_interrupt(&RadioLR1121::gpio_intr, this, gpio::INTERRUPT_RISING_EDGE);

  // 15. Clear any pending IRQs / errors.
  this->clear_irq_status_(0xFFFFFFFF);
  this->clear_errors_();

  // 16. Enter continuous receive.
  this->set_mode_rx();
}

// === Mode control ===

void RadioLR1121::set_mode_standby() {
  uint8_t const stdby_xosc = 0x01;  // TODO(hw-verify): assumed same small sequential enum as SetRxTxFallbackMode.
  this->write_command_(LR1121_CMD_SET_STANDBY, &stdby_xosc, 1);
}

void RadioLR1121::set_mode_rx() {
  uint8_t rx_continuous[3] = {0xFF, 0xFF, 0xFF};  // 0xFFFFFF = continuous, same sentinel as SX1262.
  this->write_command_(LR1121_CMD_SET_RX, rx_continuous, sizeof(rx_continuous));
}

// === Frequency control ===

void RadioLR1121::set_frequency_register_(uint32_t freq_hz) {
  uint8_t params[4] = {
      (uint8_t) (freq_hz >> 24),
      (uint8_t) (freq_hz >> 16),
      (uint8_t) (freq_hz >> 8),
      (uint8_t) freq_hz,
  };
  this->write_command_(LR1121_CMD_SET_RF_FREQUENCY, params, sizeof(params));
  this->current_freq_ = freq_hz;
}

void RadioLR1121::change_frequency(uint32_t freq_hz) {
  this->set_mode_standby();
  this->set_frequency_register_(freq_hz);
  this->clear_irq_status_(0xFFFFFFFF);  // Clear stale preamble/sync bits from previous channel
  this->clear_dio_fired();              // Clear stale IRQ latch from previous channel activity
  this->set_mode_rx();
}

void RadioLR1121::set_rx_bandwidth_(LR1121RxBandwidth bandwidth) {
  this->rx_bandwidth_ = bandwidth;
  this->write_modulation_params_();
}

int16_t RadioLR1121::read_rssi() {
  // GetRssiInst → -raw/2 dBm (design §3.2: "verify formula in UM"; same formula as SX1262).
  uint8_t raw = 0;
  this->read_command_(LR1121_CMD_GET_RSSI_INST, nullptr, 0, &raw, 1);
  return -(int16_t) raw / 2;
}

bool RadioLR1121::is_sync_detected() { return (this->read_irq_status_raw() & LR1121_IRQ_SYNC_WORD_VALID) != 0; }

bool RadioLR1121::is_preamble_detected() { return (this->read_irq_status_raw() & LR1121_IRQ_PREAMBLE_DETECTED) != 0; }

// === Packet TX ===

bool RadioLR1121::send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) {
  if (len == 0)
    return false;

#ifdef IOHOME_FRAME_LOG
  log_frame("TX", data, len, tx_config.freq_hz, tx_config.preamble_len);
#endif

  this->set_mode_standby();
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

  this->set_packet_params_(tx_config.preamble_len, encoded_len, LR1121_GFSK_PACKET_FIXED_LENGTH, LR1121_GFSK_CRC_OFF);

  this->clear_irq_status_(0xFFFFFFFF);
  this->write_buffer_(tx_buf, encoded_len);

  // Start TX. TODO(hw-verify): tick base for the timeout field assumed same as SX126x
  // (15.625us/tick, 0x03E800 = ~4s); the software 4000ms guard below is independent of this
  // chip-level value and provides the actual safety net regardless.
  this->clear_dio_fired();
  uint8_t tx_timeout[3] = {0x03, 0xE8, 0x00};
  this->write_command_(LR1121_CMD_SET_TX, tx_timeout, sizeof(tx_timeout));

  // Wait for an actual TxDone IRQ. The IRQ pin is shared with RX-related events, so a stale or
  // unrelated interrupt must not be treated as TX completion (same reasoning as SX1262).
  uint32_t const start = millis();
  uint32_t tx_irq = 0;
  while (true) {
    if (!this->is_dio_fired()) {
      if (millis() - start > 4000) {
        ESP_LOGE(TAG, "TX timeout — IRQ pin never fired");
        this->set_mode_standby();
        return false;
      }
      App.feed_wdt();
      delayMicroseconds(100);
      continue;
    }

    this->clear_dio_fired();
    tx_irq = this->read_irq_status_raw();
    if ((tx_irq & LR1121_IRQ_TX_DONE) != 0)
      break;

    if (tx_irq != 0) {
      this->clear_irq_status_(tx_irq);
    }

    if (millis() - start > 4000) {
      ESP_LOGE(TAG, "TX timeout — no TX_DONE IRQ (last_irq=0x%08" PRIX32 ")", tx_irq);
      this->set_mode_standby();
      return false;
    }
  }
  this->clear_dio_fired();

  this->clear_irq_status_(0xFFFFFFFF);
  this->reset_rx_state_(true);

  // Post-TX settling delay — same rationale as SX1262 (radio_sx1262.cpp), runtime-tunable via
  // lr1121_post_tx_settle_us pending independent LR1121 hardware validation (Step 7).
  delayMicroseconds(this->post_tx_settle_us_);

  return true;
}

// === Packet RX (blocking) ===

bool RadioLR1121::wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) {
  this->prepare_blocking_receive_(packet);

  uint32_t const start = millis();
  bool saw_irq = false;
  uint32_t irq = 0;

  if (!this->poll_until_activity_(start, timeout_ms, saw_irq, irq)) {
    return false;
  }

  if (saw_irq) {
    this->clear_dio_fired();
    irq = this->read_irq_status_raw();
  }

  if (!this->resolve_sync_race_(start, timeout_ms, irq)) {
    return false;
  }

  return this->finalize_receive_(packet, irq);
}

// === Shared RX helper ===

bool RadioLR1121::read_rx_packet(RadioRxPacket &packet, bool blocking_wait, uint32_t irq_status) {
  uint8_t rx_status[2] = {0};
  uint8_t rx_buf[RADIO_PACKET_BUFFER_SIZE] = {0};
  uint8_t recovered_buf[RADIO_PACKET_BUFFER_SIZE] = {0};

  // GetRxBufferStatus response layout mirrors SX1262: [length, offset].
  this->read_command_(LR1121_CMD_GET_RX_BUFFER_STATUS, nullptr, 0, rx_status, sizeof(rx_status));
  uint8_t const reported_len = std::min(rx_status[0], (uint8_t) sizeof(rx_buf));
  uint8_t const rx_offset = rx_status[1];
  uint8_t raw_probe_len = reported_len;
  if (reported_len > 0 && reported_len < 32) {
    // Same defensiveness as SX1262: pull the full raw window even for a short reported length,
    // since the useful UART-packed tail may sit past the chip-reported boundary.
    raw_probe_len = sizeof(rx_buf);
  }
  if (reported_len == LR1121_RX_PROBE_PACKET_LEN)
    raw_probe_len = LR1121_RX_PROBE_PACKET_LEN;
  if (raw_probe_len > 0)
    this->read_buffer_(rx_offset, raw_probe_len, rx_buf);

  UartProbeResult probe = find_uart_probe(rx_buf, raw_probe_len);
  if (probe.valid) {
    memcpy(recovered_buf, probe.decoded + probe.frame_start, probe.frame_len);
    memcpy(packet.data, recovered_buf, probe.frame_len);
    packet.len = probe.frame_len;
#ifdef IOHOME_FRAME_LOG
    ESP_LOGD(TAG, "UART probe: valid=1 bit_offset=%u frame_start=%u frame_len=%u decoded_len=%u", probe.bit_offset,
             probe.frame_start, probe.frame_len, probe.decoded_len);
#endif
  } else {
#ifdef IOHOME_FRAME_LOG
    char hex_buf[97] = {0};
    uint8_t dump_len = std::min(raw_probe_len, (uint8_t) 32);
    for (uint8_t i = 0; i < dump_len; i++)
      snprintf(hex_buf + (i * 3), 4, "%02X ", rx_buf[i]);
    ESP_LOGW(TAG, "UART probe: valid=0 decoded_len=%u raw_probe_len=%u", probe.decoded_len, raw_probe_len);
    ESP_LOGW(TAG, "  raw[0..%u]: %s", dump_len - 1, hex_buf);
#endif
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

bool RadioLR1121::check_for_packet(RadioRxPacket &packet) {
  if (!this->is_dio_fired())
    return false;
  this->prepare_nonblocking_receive_(packet);

  uint32_t const irq = this->read_irq_status_raw();

  if ((irq & LR1121_IRQ_SYNC_WORD_VALID) != 0 && (irq & LR1121_IRQ_RX_DONE) == 0) {
    this->clear_irq_status_(LR1121_IRQ_SYNC_WORD_VALID);
    return false;
  }

  if ((irq & LR1121_IRQ_RX_DONE) != 0) {
    return this->read_rx_packet(packet, false, irq);
  }

  this->fill_capture_info_(false, irq, 0, 0, nullptr, 0, nullptr, 0);
  this->reset_rx_state_();
  return false;
}

}  // namespace home_io_control
}  // namespace esphome

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
