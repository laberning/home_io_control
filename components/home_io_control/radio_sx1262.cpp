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
  // Field order and the byte->bit preamble conversion are shared with LR1121 — see
  // SoftPhyDriverBase::build_gfsk_packet_params. Only the detector length (8 bits / 1 byte here),
  // the sync-word selector, and the opcode/transport are SX1262-specific.
  uint8_t params[GFSK_PACKET_PARAMS_LEN];
  build_gfsk_packet_params({.preamble_bytes = preamble_len,
                            .preamble_detector = 0x04,  // 8 bits (1 byte)
                            .sync_word_param = SX1262_SYNC_WORD_PARAM_24_BITS,
                            .packet_type = packet_type,
                            .payload_len = payload_len,
                            .crc_type = crc_type},
                           params);
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
  // Bandwidth: the register byte is runtime-tunable via rx_bandwidth_ — see
  // TuningConfig::sx1262_rx_bandwidth (tuning_config.h) for the current default (58.6 kHz). The
  // narrower filter is safe because the measured TX->RX turnaround (~390 us plus a 500 us settle)
  // is well inside what it tolerates, and reception improves as the filter narrows on this
  // waveform — matching the SX1276's long-validated 41.7 kHz default on the identical waveform.
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
  uint8_t buf_base[2] = {SX1262_TX_BUFFER_BASE, SX1262_RX_BUFFER_BASE};
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

void RadioSX1262::apply_tx_modulation_workaround_() {
  // Read-modify-write so the reserved bits of the register keep whatever the chip put there.
  uint8_t tx_modulation = 0;
  this->read_register_(SX1262_REG_TX_MODULATION, &tx_modulation, 1);
  tx_modulation |= SX1262_TX_MODULATION_GFSK_BIT;
  this->write_register_(SX1262_REG_TX_MODULATION, &tx_modulation, 1);
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

  // 12. Sync word: 0x57 0xFD 0x99 (24 bits). Not an independent value: it is exactly
  // uart_encode_packet({0x55, 0xFF, 0x33}) — SX1276's own raw, confirmed-working sync bytes — read
  // starting 6 bits into the first UART cell instead of at the start bit. SyncWordDerivation
  // (radio_soft_phy_test.cpp) confirms this is the unique 10-bit-cell offset that reproduces it.
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

  // 16. IRQ config: map TxDone + RxDone + SyncWordValid + CrcErr to DIO1. irqMask additionally
  // enables PreambleDetected system-wide (SX126x irqMask gates GetIrqStatus itself, not just DIO
  // routing — confirmed against Semtech's own driver docs) so is_preamble_detected() can finally
  // return true on this chip; dio1Mask deliberately leaves it out so the ISR still only wakes on
  // a terminal event, not on every preamble. See
  // SX1262_IRQ_ACTIVITY_MASK's doc comment (radio_sx1262.h) for the other half of this change —
  // poll_until_activity_() must not treat a bare preamble as terminal, or this unmask tears down
  // RX mid-reception instead of fixing anything.
  uint8_t irq_params[8] = {
      0x00, 0x4F,  // irqMask: TxDone(0x0001) | RxDone(0x0002) | PreambleDetected(0x0004) |
                   // SyncWordValid(0x0008) | CrcErr(0x0040)
      0x00, 0x4B,  // dio1Mask: unchanged — TxDone|RxDone|SyncWordValid|CrcErr only
      0x00, 0x00,  // dio2Mask: none
      0x00, 0x00,  // dio3Mask: none
  };
  this->write_opcode_(SX1262_SET_DIO_IRQ_PARAMS, irq_params, sizeof(irq_params));

  // 17. Attach DIO1 interrupt
  this->dio1_pin_->attach_interrupt(&RadioSX1262::gpio_intr, this, gpio::INTERRUPT_RISING_EDGE);

  // 18. Clear any pending IRQs, and report the device-error word before clearing it. A chip that
  // came up with XOSC_START_ERR (wrong tcxo_voltage for the board), PLL_LOCK_ERR or IMG_CALIB_ERR
  // still initializes and still transmits — it just does so off-frequency or off-calibration,
  // which on air looks like flaky exchanges at any range rather than an outright failure. Clearing
  // it unseen threw away the one cheap piece of evidence for that.
  this->clear_irq_status(0xFFFF);
  uint16_t const init_errors = this->get_device_errors_();
  if (init_errors != 0)
    ESP_LOGW(TAG, "SX1262 device errors after init: 0x%04X — check tcxo_voltage for this board", init_errors);
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
