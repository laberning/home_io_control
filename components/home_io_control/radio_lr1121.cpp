/// @file radio_lr1121.cpp
/// @brief LR1121 radio driver implementation for IO-Homecontrol.
/// @ingroup hioc_radio
///
/// See radio_lr1121.h for the architectural context: this driver is a re-plumbed
/// RadioSX1262 over a different SPI command set. Everything about the software UART
/// PHY (TX bit-encoding, RX probe/CRC recovery) is identical to the SX1262 and lives
/// in radio_soft_phy.{h,cpp}; the IRQ-driven RX/TX orchestration both drivers share is
/// in SoftPhyDriverBase (radio_soft_phy_driver_base.{h,cpp}). This file only has to
/// reproduce the LR1121-specific transport (16-bit opcodes, two-transaction
/// command/response, 32-bit IRQ word) and the chip's own GFSK/RF-switch/TCXO/PA
/// configuration.

// Opcode payloads, line-coding widths, and recovery thresholds are written in the same shape
// as the chip protocol and on-air framing they reproduce.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

#include "radio_lr1121.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include <cinttypes>

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.lr1121";

// The LR11xx runs an internal boot ROM after reset before BUSY is meaningful — cross-checked
// against RadioLib's LRxxxx::reset(), which waits ~300ms ("typical transition duration should
// be 273 ms") and then polls BUSY with a 3s timeout; matched here exactly rather than picking
// an arbitrary shorter value, since the only cost of a longer timeout is on the already-failing
// path (a genuinely dead chip takes a few seconds longer to report failure).
static const uint32_t LR1121_BUSY_TIMEOUT_MS = 3000;

// === SPI Communication (16-bit opcode, two-transaction) ===

void RadioLR1121::wait_busy_() {
  // Once a BUSY timeout has failed the driver, every later wait_busy_() call would otherwise
  // re-run the same timeout again — init()'s remaining ~16 configure_radio_() steps would each
  // block for LR1121_BUSY_TIMEOUT_MS before init() finally returns false. Short-circuit instead:
  // the chip is already known-unresponsive, so there's nothing to wait for.
  if (this->failed_)
    return;
  uint32_t const start = millis();
  while (this->busy_pin_->digital_read()) {
    if (millis() - start > LR1121_BUSY_TIMEOUT_MS) {
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

void RadioLR1121::log_command_status_(uint16_t opcode) const {
  // Stat1 bits [3:1] encode the previous command's fate (0=CMD_FAIL, 1=CMD_PERR "processing
  // error", 2=CMD_OK, 3=CMD_DAT); bit 0 is a separate IRQ-active flag. Any FAIL/PERR means the
  // chip silently rejected the command — surfaced unconditionally (not gated on
  // IOHOME_FRAME_LOG) since this whole driver has been blind to command rejections until now.
  uint8_t const cmd_status = (this->last_stat1_ >> 1) & 0x07;
  if (cmd_status == 0x00 || cmd_status == 0x01) {
    ESP_LOGW(TAG, "cmd 0x%04X rejected: stat1=0x%02X (%s)", opcode, this->last_stat1_,
             cmd_status == 0x00 ? "CMD_FAIL" : "CMD_PERR");
  }
#ifdef IOHOME_FRAME_LOG
  ESP_LOGV(TAG, "cmd 0x%04X stat1=0x%02X", opcode, this->last_stat1_);
#endif
}

void RadioLR1121::write_reg_mem_mask32_(uint32_t addr, uint32_t mask, uint32_t value) {
  uint8_t params[12] = {
      (uint8_t) (addr >> 24),  (uint8_t) (addr >> 16),  (uint8_t) (addr >> 8),  (uint8_t) addr,
      (uint8_t) (mask >> 24),  (uint8_t) (mask >> 16),  (uint8_t) (mask >> 8),  (uint8_t) mask,
      (uint8_t) (value >> 24), (uint8_t) (value >> 16), (uint8_t) (value >> 8), (uint8_t) value,
  };
  this->write_command_(LR1121_CMD_WRITE_REG_MEM_MASK32, params, sizeof(params));
}

void RadioLR1121::apply_high_acp_workaround_() {
  this->write_reg_mem_mask32_(LR1121_REG_HIGH_ACP_WORKAROUND_ADDR, LR1121_REG_HIGH_ACP_WORKAROUND_MASK,
                              LR1121_REG_HIGH_ACP_WORKAROUND_VALUE);
}

void RadioLR1121::apply_gfsk_workaround_() {
  this->write_reg_mem_mask32_(LR1121_REG_GFSK_WORKAROUND_1_ADDR, LR1121_REG_GFSK_WORKAROUND_1_MASK,
                              LR1121_REG_GFSK_WORKAROUND_1_VALUE);
  this->write_reg_mem_mask32_(LR1121_REG_GFSK_WORKAROUND_2_ADDR, LR1121_REG_GFSK_WORKAROUND_2_MASK,
                              LR1121_REG_GFSK_WORKAROUND_2_VALUE);
  this->write_reg_mem_mask32_(LR1121_REG_GFSK_WORKAROUND_3_ADDR, LR1121_REG_GFSK_WORKAROUND_3_MASK,
                              LR1121_REG_GFSK_WORKAROUND_3_VALUE);
}

void RadioLR1121::calibrate_image_() {
  uint8_t params[2] = {LR1121_IMAGE_CAL_FREQ1, LR1121_IMAGE_CAL_FREQ2};
  this->write_command_(LR1121_CMD_CALIBRATE_IMAGE, params, sizeof(params));
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
  // "read IRQ status non-destructively via GetStatus") — cross-checked against RadioLib's
  // LRxxxx::getIrqStatus(): 6 bytes total [Stat1, Stat2, IRQ_b3..IRQ_b0]. read_command_
  // already consumes Stat1 into last_stat1_, so only 5 more bytes remain: Stat2 (resp[0],
  // unused) followed by the 4 IRQ bytes (resp[1..4]).
  uint8_t resp[5] = {0};
  this->read_command_(LR1121_CMD_GET_STATUS, nullptr, 0, resp, sizeof(resp));
  return (static_cast<uint32_t>(resp[1]) << 24) | (static_cast<uint32_t>(resp[2]) << 16) |
         (static_cast<uint32_t>(resp[3]) << 8) | static_cast<uint32_t>(resp[4]);
}

// === Packet params helper ===

void RadioLR1121::set_packet_params_(uint16_t preamble_len, uint8_t payload_len, uint8_t packet_type,
                                     uint8_t crc_type) {
  // Same 9-field shape as RadioSX1262::set_packet_params_ (design §3.2 step 7 gives the LR1121
  // field values as a direct match to the SX1262's), just written via the LR1121 opcode.
  //
  // preamble_len arrives in bytes (see RadioSX1262::set_packet_params_ for why); this chip's
  // field is bit-denominated too — Semtech names it lr11xx_radio_pkt_params_gfsk_t.pbl_len_in_bit
  // (reference/SWTL001/lr11xx_driver/src/lr11xx_radio_types.h:545) — so convert here as well.
  const uint16_t preamble_bits = preamble_len * 8;
  uint8_t params[9] = {
      (uint8_t) (preamble_bits >> 8),   // Preamble length MSB
      (uint8_t) preamble_bits,          // Preamble length LSB
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

void RadioLR1121::set_rx_packet_params() {
  // Same fixed-length raw-probe strategy as SX1262 (SOFT_PHY_RX_PROBE_PACKET_LEN, shared —
  // see its doc comment in radio_soft_phy_driver_base.h): the LR1121 gets the identical
  // treatment because the reasoning is about UART-packed frame sizes, not chip-specific
  // packet-engine behavior.
  this->set_packet_params_(8, SOFT_PHY_RX_PROBE_PACKET_LEN, LR1121_GFSK_PACKET_FIXED_LENGTH, LR1121_GFSK_CRC_OFF);
}

void RadioLR1121::write_modulation_params_() {
  // LR1121 GFSK modulation parameters for the IO-Homecontrol 868 MHz waveform, written as
  // plain Hz/bps 32-bit values (design §1.2: "RF frequency / bitrate / fdev are plain Hz /
  // bit/s 32-bit values" — no PLL-step math needed, unlike SX1262). Field order (bitrate,
  // pulse shape, bandwidth, fdev) cross-checked against RadioLib/Semtech and confirmed on
  // real hardware.
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

  // GFSK workaround register trio (analysis §4.2) — RadioLib/Semtech apply this after every
  // modulation-params write, not just once at init, so it must re-run on every bandwidth retune.
  this->apply_gfsk_workaround_();
}

uint16_t RadioLR1121::get_errors_() {
  uint8_t errors_raw[2] = {0};
  this->read_command_(LR1121_CMD_GET_ERRORS, nullptr, 0, errors_raw, sizeof(errors_raw));
  return (static_cast<uint16_t>(errors_raw[0]) << 8) | errors_raw[1];
}

void RadioLR1121::clear_errors_() { this->write_command_(LR1121_CMD_CLEAR_ERRORS, nullptr, 0); }

void RadioLR1121::clear_irq_status(uint32_t irq_mask) {
  uint8_t clear_irq[4] = {
      (uint8_t) ((irq_mask >> 24) & 0xFF),
      (uint8_t) ((irq_mask >> 16) & 0xFF),
      (uint8_t) ((irq_mask >> 8) & 0xFF),
      (uint8_t) (irq_mask & 0xFF),
  };
  this->write_command_(LR1121_CMD_CLEAR_IRQ, clear_irq, sizeof(clear_irq));
}

void RadioLR1121::fill_capture_info(bool blocking_wait, uint32_t irq_status, uint8_t rx_offset, uint8_t reported_len,
                                    const uint8_t *raw, uint8_t raw_len, const uint8_t *frame, uint8_t frame_len) {
  // GetPktStatus (0x0204), not GetRssiInst (0x0205): the latter is a live, instantaneous RSSI
  // read unrelated to any specific frame, whereas GetPktStatus is atomically tied to the
  // last-received packet — the reported RSSI must reflect the frame that was actually received,
  // not the channel's state whenever this function happens to run. Mirrors
  // RadioSX1262::fill_capture_info()'s use of its own GetPacketStatus. byte[0] is rssi_sync
  // (RSSI at sync-word detection), matching the sync-not-avg field SX1262 uses
  // (packet_status[1] there).
  uint8_t pkt_status[4] = {0};
  this->read_command_(LR1121_CMD_GET_PKT_STATUS, nullptr, 0, pkt_status, sizeof(pkt_status));

  this->populate_capture_base_(blocking_wait, this->current_freq_, -(int16_t) pkt_status[0] / 2, raw, raw_len, frame,
                               frame_len);
  this->last_capture_.rx_done = (irq_status & LR1121_IRQ_RX_DONE) != 0;
  this->last_capture_.crc_error = (irq_status & LR1121_IRQ_CRC_ERR) != 0;
  // RadioCaptureInfo::irq_status is uint16_t; map the 32-bit word down by taking bits [2..10]
  // and shifting right by 2 (design §3.2 "IRQ-width note" — one documented place for this).
  this->last_capture_.irq_status = static_cast<uint16_t>((irq_status >> 2) & 0x01FF);
  this->last_capture_.packet_status = pkt_status[3];
  this->last_capture_.rx_offset = rx_offset;
  this->last_capture_.reported_len = reported_len;
}

uint8_t RadioLR1121::read_rssi_raw_byte() {
  uint8_t raw = 0;
  this->read_command_(LR1121_CMD_GET_RSSI_INST, nullptr, 0, &raw, 1);
  return raw;
}

void RadioLR1121::get_rx_buffer_status(uint8_t &reported_len, uint8_t &rx_offset) {
  // GetRxBufferStatus response layout mirrors SX1262: [length, offset].
  uint8_t rx_status[2] = {0};
  this->read_command_(LR1121_CMD_GET_RX_BUFFER_STATUS, nullptr, 0, rx_status, sizeof(rx_status));
  reported_len = rx_status[0];
  rx_offset = rx_status[1];
}

void RadioLR1121::start_tx() {
  // Tick base is 30.52us (32.768kHz RTC), so 0x03E800 is ~7.8s, not the ~4s the field was
  // originally assumed to encode; the software 4000ms guard in the shared send_packet() is what
  // actually bounds TX and is independent of this chip-level value.
  uint8_t tx_timeout[3] = {0x03, 0xE8, 0x00};
  this->write_command_(LR1121_CMD_SET_TX, tx_timeout, sizeof(tx_timeout));
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
  ESP_LOGCONFIG(TAG, "    Device type: 0x%02X (expect 0x%02X)", version[1], LR1121_DEVICE_TYPE);
  ESP_LOGCONFIG(TAG, "    HW version: 0x%02X, FW version: %u.%u", version[0], version[2], version[3]);
  if (lr1121_firmware_is_outdated(version[2], version[3])) {
    ESP_LOGCONFIG(TAG,
                  "    Newer firmware available (%u.%u), see "
                  "https://github.com/Lora-net/radio_firmware_images/blob/master/lr1121/transceiver/README.md",
                  LR1121_KNOWN_LATEST_FW_MAJOR, LR1121_KNOWN_LATEST_FW_MINOR);
  }
  ESP_LOGCONFIG(TAG, "    BUSY=%d IRQ=%d", this->busy_pin_->digital_read(), this->irq_pin_->digital_read());
  ESP_LOGCONFIG(TAG, "    IRQ status: 0x%08" PRIX32, irq);
  ESP_LOGCONFIG(TAG, "    Device errors: 0x%04X", errors);
  ESP_LOGCONFIG(TAG, "    Last Stat1: 0x%02X", this->last_stat1_);
}

void RadioLR1121::configure_radio_() {
  // 1. Identity check: GetVersion — device type must be LR1121 (0x03). Response layout
  //    cross-checked against RadioLib's LR11x0::getVersion(): [hw_version, device_type,
  //    fw_major, fw_minor] (4 bytes) — device type is byte 1, not byte 0.
  uint8_t version[4] = {0};
  this->read_command_(LR1121_CMD_GET_VERSION, nullptr, 0, version, sizeof(version));
  if (version[1] != LR1121_DEVICE_TYPE) {
    ESP_LOGE(TAG, "Unexpected device type 0x%02X (expected 0x%02X) — not an LR1121?", version[1], LR1121_DEVICE_TYPE);
    this->failed_ = true;
    return;
  }
  ESP_LOGI(TAG, "LR1121 detected: hw=0x%02X fw=%u.%u", version[0], version[2], version[3]);

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

  // 3b. Banded image calibration (analysis §4.3) — Calibrate(0x3F) calibrates the IMG block at
  //     the chip's default band, not ours; both reference drivers issue an explicit banded call.
  this->calibrate_image_();

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
  this->set_frequency_register(FREQ_CH2);

  // 7. Apply GFSK modulation parameters (detailed values live in write_modulation_params_()).
  this->write_modulation_params_();

  // 8. Default RX packet params: fixed-length GFSK with software CRC (hardware CRC unusable —
  //    same reasoning as SX1262, see radio_sx1262.cpp file header).
  this->set_rx_packet_params();

  // 9. Sync word: 0x57 0xFD 0x99 + zero padding (identical bytes to the SX1262/SX1276 UART sync
  //    word hypothesis — written via the LR1121's own SetGfskSyncWord opcode rather than a raw
  //    register write).
  uint8_t sync_word[8] = {0x57, 0xFD, 0x99, 0x00, 0x00, 0x00, 0x00, 0x00};
  this->write_command_(LR1121_CMD_SET_GFSK_SYNC_WORD, sync_word, sizeof(sync_word));

  // 10. PA config: select LP or HP PA path based on the configured power — cross-checked against
  //     RadioLib's LR1120::setOutputPower()/checkOutputPower(): the LP path only covers -17..14dBm
  //     (regPaSupply=internal regulator); anything above requires the HP path (regPaSupply=VBAT,
  //     -9..22dBm). This board's config requests 17dBm, which is out of the LP path's valid
  //     range — always configuring LP here regardless of tx_power_ was invalid, and is the likely
  //     reason TX completed digitally (TX_DONE fired, no error) while the awning never received
  //     anything: the PA was asked to output power outside the range its selected path supports.
  bool const use_hp_pa = this->tx_power_ > 14;
  uint8_t pa_config[4] = {
      (uint8_t) (use_hp_pa ? 0x01 : 0x00),  // paSel: 0=LP, 1=HP
      (uint8_t) (use_hp_pa ? 0x01 : 0x00),  // regPaSupply: 0=internal regulator (LP), 1=VBAT (HP)
      0x04,                                 // paDutyCycle
      0x07,                                 // paHpSel — RadioLib always sets this, even for LP
  };
  this->write_command_(LR1121_CMD_SET_PA_CONFIG, pa_config, sizeof(pa_config));

  // 11. TX params: power in dBm, clamped to the selected PA path's valid range (see step 10).
  //     Ramp register is LR11x0-specific, not the SX126x encoding this used to assume: RadioLib's
  //     LR1120::setOutputPower() passes `roundRampTime(us) - 3` — the shared ramp-time enum
  //     starts with three LR2021-only fast steps (2/4/8us) that don't exist on the LR11x0, so its
  //     own register value for a given ramp time is 3 less than the shared table's index. 0x0C is
  //     ~200us on this chip (the previous 0x04 was actually ~80us).
  int8_t const min_power = use_hp_pa ? -9 : -17;
  int8_t const max_power = use_hp_pa ? 22 : 14;
  int8_t const power = std::max(min_power, std::min(max_power, (int8_t) this->tx_power_));
  uint8_t tx_params[2] = {(uint8_t) power, 0x0C};
  this->write_command_(LR1121_CMD_SET_TX_PARAMS, tx_params, sizeof(tx_params));

  // 12. Keep the crystal path alive after RX/TX completion.
  uint8_t const fallback_mode = LR1121_FALLBACK_STDBY_XOSC;
  this->write_command_(LR1121_CMD_SET_RX_TX_FALLBACK_MODE, &fallback_mode, 1);

  // 13. IRQ config: two 32-bit masks — first DIO's mask (routed to DIO9) + second DIO's mask
  //     (unused, all zero). Cross-checked against RadioLib's LRxxxx::setDioIrqParams(irq1, irq2):
  //     the command takes exactly these two fields, no separate "enable" field (design §Step 2
  //     item 2: "mask 0 for the second IRQ output").
  uint8_t irq_params[8] = {
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
  this->clear_irq_status(0xFFFFFFFF);
  this->clear_errors_();

  // 16. Enter continuous receive.
  this->set_mode_rx();
}

// === Mode control ===

void RadioLR1121::set_mode_standby() {
  uint8_t const stdby_xosc = 0x01;  // Same small sequential enum as SetRxTxFallbackMode (0x01=STDBY_XOSC there too).
  this->write_command_(LR1121_CMD_SET_STANDBY, &stdby_xosc, 1);
}

void RadioLR1121::set_mode_rx() {
  // High-ACP workaround (analysis §4.1) — Semtech applies this unconditionally before every
  // SetRx, so it lives here rather than only at init to cover every set_mode_rx() call site.
  this->apply_high_acp_workaround_();
  uint8_t rx_continuous[3] = {0xFF, 0xFF, 0xFF};  // 0xFFFFFF = continuous, same sentinel as SX1262.
  this->write_command_(LR1121_CMD_SET_RX, rx_continuous, sizeof(rx_continuous));
}

// === Frequency control ===

void RadioLR1121::set_frequency_register(uint32_t freq_hz) {
  uint8_t params[4] = {
      (uint8_t) (freq_hz >> 24),
      (uint8_t) (freq_hz >> 16),
      (uint8_t) (freq_hz >> 8),
      (uint8_t) freq_hz,
  };
  this->write_command_(LR1121_CMD_SET_RF_FREQUENCY, params, sizeof(params));
  this->current_freq_ = freq_hz;
}

void RadioLR1121::set_rx_bandwidth_(LR1121RxBandwidth bandwidth) {
  this->rx_bandwidth_ = bandwidth;
  this->write_modulation_params_();
}

}  // namespace home_io_control
}  // namespace esphome

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
