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

#include <cinttypes>
#include <cstdio>

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.sx1262";
static const uint8_t SX1262_SYNC_WORD_PARAM_24_BITS = 0x18;

namespace {

/// One row of the GetDeviceErrors bit → name table used by sx1262_format_device_errors().
struct Sx1262DeviceErrorBit {
  uint16_t mask;
  const char *name;
};

constexpr Sx1262DeviceErrorBit SX1262_DEVICE_ERROR_BITS[] = {
    {SX1262_DEV_ERR_RC64K_CALIB, "RC64K_CALIB_ERR"}, {SX1262_DEV_ERR_RC13M_CALIB, "RC13M_CALIB_ERR"},
    {SX1262_DEV_ERR_PLL_CALIB, "PLL_CALIB_ERR"},     {SX1262_DEV_ERR_ADC_CALIB, "ADC_CALIB_ERR"},
    {SX1262_DEV_ERR_IMG_CALIB, "IMG_CALIB_ERR"},     {SX1262_DEV_ERR_XOSC_START, "XOSC_START_ERR"},
    {SX1262_DEV_ERR_PLL_LOCK, "PLL_LOCK_ERR"},       {SX1262_DEV_ERR_PA_RAMP, "PA_RAMP_ERR"},
};

/// One tick of the SX1262 TCXO startup-delay field is 15.625 µs = 1000/64 µs, so
/// ticks = ceil(delay_us * 64 / 1000). The field is 24-bit; the largest delay this driver ever
/// asks for (50 ms) is 3200 ticks, well inside it.
constexpr uint32_t sx1262_tcxo_startup_ticks(uint32_t delay_us) { return (delay_us * 64U + 999U) / 1000U; }

}  // namespace

void sx1262_format_device_errors(uint16_t errors, char *buf, size_t buf_size) {
  if (buf == nullptr || buf_size == 0)
    return;
  buf[0] = '\0';
  if (errors == 0) {
    snprintf(buf, buf_size, "none");
    return;
  }

  size_t pos = 0;
  uint16_t named = 0;
  for (const auto &bit : SX1262_DEVICE_ERROR_BITS) {
    if ((errors & bit.mask) == 0)
      continue;
    named |= bit.mask;
    const int n = snprintf(buf + pos, buf_size - pos, "%s%s", pos > 0 ? "|" : "", bit.name);
    if (n <= 0 || static_cast<size_t>(n) >= buf_size - pos)
      return;  // buffer full — leave what fit, already NUL-terminated by snprintf
    pos += static_cast<size_t>(n);
  }

  const uint16_t unknown = errors & static_cast<uint16_t>(~named);
  if (unknown != 0)
    snprintf(buf + pos, buf_size - pos, "%sUNKNOWN_0x%04X", pos > 0 ? "|" : "", unknown);
}

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
  char errbuf[SX1262_DEVICE_ERROR_STR_SIZE];
  sx1262_format_device_errors(errors, errbuf, sizeof(errbuf));
  ESP_LOGCONFIG(TAG, "    Device errors: 0x%04X (%s)", errors, errbuf);
  if (this->tcxo_startup_attempts_ > 1) {
    ESP_LOGCONFIG(TAG, "    TCXO startup: %u attempts, %" PRIu32 " ms final delay",
                  static_cast<unsigned>(this->tcxo_startup_attempts_), this->tcxo_startup_delay_us_ / 1000);
  }
}

void RadioSX1262::configure_tcxo_() {
  // Strictly increasing startup-delay ladder. A slow TCXO that misses the first (nominal 5 ms)
  // window often starts inside a longer one; each rung must actually be longer than the last, so
  // a retry buys the oscillator real extra time.
  static constexpr uint32_t TCXO_STARTUP_DELAYS_US[] = {5000, 10000, 50000};
  // Margin over the programmed startup window for wait_busy_() to also cover CALIBRATE running on
  // the fresh clock, in milliseconds.
  static constexpr uint32_t TCXO_BUSY_MARGIN_MS = 25;

  this->tcxo_startup_attempts_ = 0;
  this->tcxo_startup_delay_us_ = 0;

  // The chip holds BUSY through the whole programmed TCXO startup window (up to 50 ms on the last
  // rung) plus calibration before it can latch XOSC_START_ERR. wait_busy_()'s normal
  // SX1262_BUSY_TIMEOUT_MS (10 ms, sized for RC-oscillator timing) is far shorter than that, so a
  // slow TCXO would trip a BUSY timeout, latch failed_, and brick the component in exactly the
  // case this retry exists to rescue. Widen the budget for the bring-up and restore it after.
  const uint32_t saved_busy_timeout_ms = this->get_busy_timeout_ms_();

  uint16_t last_errors = 0;
  for (const uint32_t delay_us : TCXO_STARTUP_DELAYS_US) {
    this->set_busy_timeout_ms_((delay_us / 1000) + TCXO_BUSY_MARGIN_MS);

    const uint32_t ticks = sx1262_tcxo_startup_ticks(delay_us);
    uint8_t tcxo_params[4] = {this->tcxo_voltage_, static_cast<uint8_t>((ticks >> 16) & 0xFF),
                              static_cast<uint8_t>((ticks >> 8) & 0xFF), static_cast<uint8_t>(ticks & 0xFF)};
    this->write_opcode_(SX1262_SET_DIO3_AS_TCXO_CTRL, tcxo_params, sizeof(tcxo_params));

    // Sleep out most of the startup window on the host before touching the chip again so
    // wait_busy_() only has to absorb the remainder plus calibration.
    delay((delay_us / 1000) + 2);

    // XOSC_START_ERR / IMG_CALIB_ERR are the expected POR state with a TCXO fitted — clear the
    // latch here so the post-CALIBRATE read below reflects only this attempt, which is what lets
    // the early break work. Our LR1121 driver clears in the same spot for the same reason.
    this->clear_device_errors_();

    uint8_t const cal = 0x7F;  // calibrate all blocks
    this->write_opcode_(SX1262_CALIBRATE, &cal, 1);
    delay(5);  // wait for calibration to complete (same margin as the LR1121 driver); the widened
               // busy_timeout_ absorbs any overrun on the next transaction's wait_busy_()

    this->tcxo_startup_attempts_++;
    this->tcxo_startup_delay_us_ = delay_us;

    if (this->failed_)
      break;  // a BUSY timeout already failed the chip — escalating further just reads garbage

    last_errors = this->get_device_errors_();
    if ((last_errors & SX1262_DEV_ERR_XOSC_START) == 0)
      break;  // TCXO is running — stop escalating
  }

  // Restore the steady-state BUSY budget — but if the ladder had to escalate, keep a ceiling wide
  // enough for the startup window that actually worked. configure_radio_() step 4 re-enters
  // STDBY_XOSC immediately after this, which powers the TCXO again and holds BUSY for that same
  // window, so step 5's wait_busy_() would otherwise time out and latch failed_ on a board the
  // ladder just rescued. busy_timeout_ms_ is a ceiling, never a delay, so a permanently wider
  // value costs a healthy board nothing; runtime never re-pays the startup wait (step 6b sets the
  // XOSC standby fallback and set_mode_standby() uses STDBY_XOSC, so the chip never drops to RC).
  if (this->tcxo_startup_attempts_ > 1) {
    const uint32_t escalated = (this->tcxo_startup_delay_us_ / 1000) + TCXO_BUSY_MARGIN_MS;
    this->set_busy_timeout_ms_(saved_busy_timeout_ms > escalated ? saved_busy_timeout_ms : escalated);
  } else {
    this->set_busy_timeout_ms_(saved_busy_timeout_ms);
  }

  // A BUSY timeout during the ladder already logged "BUSY timeout" and will fail init()
  if (this->failed_)
    return;

  // Say nothing on the happy path (one attempt): the log stays identical to a single-shot bring-up.
  if (this->tcxo_startup_attempts_ <= 1)
    return;
  if ((last_errors & SX1262_DEV_ERR_XOSC_START) != 0) {
    ESP_LOGE(TAG,
             "SX1262 TCXO never started after %u attempts (XOSC_START_ERR persists) — check "
             "tcxo_voltage for this board and the TCXO part itself",
             static_cast<unsigned>(this->tcxo_startup_attempts_));
  } else {
    ESP_LOGW(TAG, "SX1262 TCXO started after %u attempts (%" PRIu32 " ms startup delay)",
             static_cast<unsigned>(this->tcxo_startup_attempts_), this->tcxo_startup_delay_us_ / 1000);
  }
}

void RadioSX1262::configure_radio_() {
  // 1. Standby on RC oscillator (safe starting point)
  uint8_t const stdby_rc = 0x00;
  this->write_opcode_(SX1262_SET_STANDBY, &stdby_rc, 1);

  // 2-3. Bring up the reference clock and calibrate. A board with a bare crystal (tcxo_voltage:
  // none) has no DIO3-controlled TCXO: skip that programming and calibrate straight off the
  // crystal. Otherwise configure the TCXO with a bounded XOSC-start retry (see configure_tcxo_()).
  if (this->tcxo_voltage_ == TCXO_VOLTAGE_NONE) {
    // Clear the expected POR device-error latch before calibrating, same as the TCXO path does
    // per attempt, so the step-18 read below only ever reports a genuine post-init fault.
    this->clear_device_errors_();
    uint8_t const cal = 0x7F;
    this->write_opcode_(SX1262_CALIBRATE, &cal, 1);
    delay(5);  // Wait for calibration to complete
  } else {
    this->configure_tcxo_();
  }

  // 4. Standby on XOSC (reference clock now running)
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

  // 18. Clear any pending IRQs, and report the device-error word before clearing it. Both the TCXO
  // and bare-crystal paths in step 2-3 clear the expected POR flags (XOSC_START_ERR, IMG_CALIB_ERR
  // with a TCXO fitted) before calibrating, so a non-zero word here is a genuine post-init fault —
  // a PLL that would not lock, calibration that failed, or a TCXO that never started even after
  // the retry ladder. Such a chip still initializes and still transmits, just off-frequency or
  // off-calibration, so surfacing the decoded flags is the one cheap piece of evidence for it.
  this->clear_irq_status(0xFFFF);
  uint16_t const init_errors = this->get_device_errors_();
  if (init_errors != 0) {
    char errbuf[SX1262_DEVICE_ERROR_STR_SIZE];
    sx1262_format_device_errors(init_errors, errbuf, sizeof(errbuf));
    ESP_LOGW(TAG, "SX1262 device errors after init: 0x%04X (%s)", init_errors, errbuf);
  }
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
