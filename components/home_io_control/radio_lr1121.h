#pragma once

/// @file radio_lr1121.h
/// @brief LR1121 radio driver for IO-Homecontrol.
/// @ingroup hioc_radio
///
/// Implements the RadioDriver interface for the Semtech LR1121 transceiver on the
/// LilyGo T3-S3 board. Like the SX1262, the LR1121 has no hardware equivalent of the
/// SX1276's IoHomeOn framing, so this driver reuses the shared software PHY
/// (`radio_soft_phy.h`) wholesale: software UART bit-encoding on TX, a UART-decode
/// probe with CRC-CCITT validation on RX, fixed-length GFSK packets, software CRC.
/// This is a re-plumbed RadioSX1262 with a different SPI command set, not a new
/// bring-up from first principles (design doc §1.2). The IRQ-driven RX/TX orchestration
/// this driver shares with RadioSX1262 lives in SoftPhyDriverBase (see
/// radio_soft_phy_driver_base.h).
///
/// The chip-level difference from the SX1262 that actually changes the driver's
/// shape is the SPI transport: the LR1121 uses 16-bit opcodes and a two-transaction
/// command/response protocol (write opcode+params, wait BUSY, then a second
/// transaction clocks out a status byte + response), instead of the SX1262's
/// single-transaction opcode+NOP-then-data pattern.
///
/// Hardware bring-up (2026-08) confirmed this driver end-to-end against a real Somfy Sunea IO
/// awning motor: authenticated open/close/stop exchanges complete reliably, with correct
/// position/state feedback decoded from the device's real responses. Every constant below is
/// therefore hardware-verified unless its own comment says otherwise.

#include "radio_soft_phy_driver_base.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace home_io_control {

// ============================================================================
// LR1121 Opcode Constants (16-bit)
// ============================================================================
//
// The opcodes below marked "cross-checked" come verbatim from the design plan's
// pre-verified table (design §1.2 / implementation §0.3.3), which was itself checked
// against RadioLib's LR11x0_commands.h during planning.

static constexpr uint16_t LR1121_CMD_GET_STATUS = 0x0100;   ///< cross-checked
static constexpr uint16_t LR1121_CMD_GET_VERSION = 0x0101;  ///< cross-checked
static constexpr uint16_t LR1121_CMD_GET_ERRORS = 0x010D;   ///< cross-checked (2-byte response)
static constexpr uint16_t LR1121_CMD_CLEAR_ERRORS =
    0x010E;  ///< hardware-verified (called on every init/RX cycle, never rejected — see log_command_status_())
static constexpr uint16_t LR1121_CMD_WRITE_BUFFER = 0x0109;  ///< cross-checked
static constexpr uint16_t LR1121_CMD_READ_BUFFER = 0x010A;   ///< cross-checked
static constexpr uint16_t LR1121_CMD_WRITE_REG_MEM_MASK32 =
    0x010C;  ///< cross-checked (Semtech SWDR001 lr11xx_radio.c / RadioLib LR11x0_commands.h)
static constexpr uint16_t LR1121_CMD_CALIBRATE = 0x010F;        ///< cross-checked
static constexpr uint16_t LR1121_CMD_CALIBRATE_IMAGE = 0x0111;  ///< cross-checked (RadioLib calibrateImageRejection)
static constexpr uint16_t LR1121_CMD_SET_DIO_AS_RF_SWITCH = 0x0112;  ///< cross-checked
static constexpr uint16_t LR1121_CMD_SET_DIO_IRQ_PARAMS = 0x0113;    ///< cross-checked
static constexpr uint16_t LR1121_CMD_CLEAR_IRQ = 0x0114;             ///< cross-checked
static constexpr uint16_t LR1121_CMD_SET_TCXO_MODE = 0x0117;         ///< cross-checked
static constexpr uint16_t LR1121_CMD_SET_STANDBY = 0x011C;           ///< cross-checked
static constexpr uint16_t LR1121_CMD_GET_RX_BUFFER_STATUS = 0x0203;  ///< cross-checked
static constexpr uint16_t LR1121_CMD_GET_PKT_STATUS =
    0x0204;  ///< cross-checked (Semtech SWDR001 lr11xx_radio.c :: lr11xx_radio_get_gfsk_pkt_status —
             ///< 0 request params, 4-byte GFSK response: [rssi_sync, rssi_avg, rx_len, status_flags],
             ///< rssi_sync/avg both decode via -(raw>>1) dBm, same formula as LR1121_CMD_GET_RSSI_INST)
static constexpr uint16_t LR1121_CMD_GET_RSSI_INST = 0x0205;            ///< cross-checked
static constexpr uint16_t LR1121_CMD_SET_GFSK_SYNC_WORD = 0x0206;       ///< cross-checked
static constexpr uint16_t LR1121_CMD_SET_RX = 0x0209;                   ///< cross-checked
static constexpr uint16_t LR1121_CMD_SET_TX = 0x020A;                   ///< cross-checked
static constexpr uint16_t LR1121_CMD_SET_RF_FREQUENCY = 0x020B;         ///< cross-checked
static constexpr uint16_t LR1121_CMD_SET_PACKET_TYPE = 0x020E;          ///< cross-checked
static constexpr uint16_t LR1121_CMD_SET_MODULATION_PARAMS = 0x020F;    ///< cross-checked
static constexpr uint16_t LR1121_CMD_SET_PACKET_PARAMS = 0x0210;        ///< cross-checked
static constexpr uint16_t LR1121_CMD_SET_TX_PARAMS = 0x0211;            ///< cross-checked
static constexpr uint16_t LR1121_CMD_SET_RX_TX_FALLBACK_MODE = 0x0213;  ///< cross-checked
static constexpr uint16_t LR1121_CMD_SET_PA_CONFIG = 0x0215;            ///< cross-checked

// ============================================================================
// LR1121 IRQ Bit Masks (32-bit word)
// ============================================================================
// Bit positions are cross-checked (design §1.2 chip comparison table).

static constexpr uint32_t LR1121_IRQ_TX_DONE = 1UL << 2;
static constexpr uint32_t LR1121_IRQ_RX_DONE = 1UL << 3;
static constexpr uint32_t LR1121_IRQ_PREAMBLE_DETECTED = 1UL << 4;
static constexpr uint32_t LR1121_IRQ_SYNC_WORD_VALID = 1UL << 5;
static constexpr uint32_t LR1121_IRQ_CRC_ERR = 1UL << 7;
static constexpr uint32_t LR1121_IRQ_TIMEOUT = 1UL << 10;

/// IRQ bits that represent a *terminal* radio event — a frame finished decoding, an outbound
/// frame completed, or the chip gave up on its own. Deliberately excludes PREAMBLE_DETECTED,
/// even though it's part of LR1121_IRQ_DIO_ENABLE_MASK: a preamble alone means a frame may
/// still be arriving, so SoftPhyDriverBase's poll_until_activity_()/check_for_packet() (gated by
/// @ref RadioLR1121::activity_irq_mask) must not treat it as terminal — doing so would tear down
/// RX mid-reception.
static constexpr uint32_t LR1121_IRQ_ACTIVITY_MASK =
    LR1121_IRQ_TX_DONE | LR1121_IRQ_RX_DONE | LR1121_IRQ_SYNC_WORD_VALID | LR1121_IRQ_CRC_ERR | LR1121_IRQ_TIMEOUT;

/// DIO-routed IRQ enable mask: TxDone|RxDone|PreambleDetected|SyncWordValid|Timeout
/// (design implementation §Step 2, item 2 — deliberately excludes CrcErr from the DIO-routed
/// set, unlike SX1262; CrcErr is still visible in the raw status word for capture diagnostics).
static constexpr uint32_t LR1121_IRQ_DIO_ENABLE_MASK = LR1121_IRQ_TX_DONE | LR1121_IRQ_RX_DONE |
                                                       LR1121_IRQ_PREAMBLE_DETECTED | LR1121_IRQ_SYNC_WORD_VALID |
                                                       LR1121_IRQ_TIMEOUT;

// ============================================================================
// LR1121 chip identity / GFSK / packet constants
// ============================================================================

static constexpr uint8_t LR1121_DEVICE_TYPE = 0x03;  ///< cross-checked (design §1.2 "Identity" row)

static constexpr uint8_t LR1121_PACKET_TYPE_GFSK = 0x01;          ///< cross-checked (design §3.2 step 5)
static constexpr uint8_t LR1121_GFSK_CRC_OFF = 0x01;              ///< cross-checked (same encoding as SX126x)
static constexpr uint8_t LR1121_GFSK_PACKET_FIXED_LENGTH = 0x00;  ///< cross-checked (same encoding as SX126x)
/// Preamble detector length selector: 16 bits. Shares the SX126x GFSK preamble-detector enum
/// (0x00 off, 0x04=8bit, 0x05=16bit, 0x06=24bit, 0x07=32bit) — the LR11xx sub-GHz GFSK modem
/// descends from the same core.
static constexpr uint8_t LR1121_PREAMBLE_DETECTOR_16_BIT = 0x05;
/// Sync word length: 24 bits, encoded as a literal bit count — cross-checked, identical encoding
/// to SX1262's SX1262_SYNC_WORD_PARAM_24_BITS.
static constexpr uint8_t LR1121_SYNC_WORD_PARAM_24_BITS = 0x18;

/// Calibrate "all blocks" bitmask. LR11xx calibration blocks are LF-RC(0)/HF-RC(1)/PLL(2)/
/// ADC(3)/IMG(4)/PLL-TX(5), so "all" = 0x3F — NOT the SX126x value (0x7F), which uses a
/// different bit layout for a different chip.
static constexpr uint8_t LR1121_CALIBRATE_ALL_BLOCKS = 0x3F;

/// SetRxTxFallbackMode value for STDBY_XOSC. LR11xx fallback-mode is a small sequential enum
/// (FS=0x00, STDBY_RC=0x01, STDBY_XOSC=0x02), unlike SX126x's raw standby-mode byte (0x30)
/// reused directly in that field.
static constexpr uint8_t LR1121_FALLBACK_STDBY_XOSC = 0x02;

// ============================================================================
// Vendor errata / calibration workaround registers (analysis/lr1121_bring_up_investigation.md
// §4.1-4.3) — Semtech's own driver (lr11xx_radio.c) and RadioLib both apply these unconditionally;
// this driver's initial version never touched them. Register addresses/masks/values are byte-for-
// byte matches to `lr11xx_radio_apply_high_acp_workaround` and `lr11xx_workaround_gfsk_reset`.
// ============================================================================

/// High-ACP (adjacent channel power) TX-quality erratum: clear bit 30 of this register before
/// every SetRx/SetTx. Without it the chip's TX spectrum has excess spectral regrowth — plausibly
/// why our own (tolerant) monitors decode our frames byte-exact while the awning's spec-compliant
/// receiver mostly can't.
static constexpr uint32_t LR1121_REG_HIGH_ACP_WORKAROUND_ADDR = 0x00F30054;
static constexpr uint32_t LR1121_REG_HIGH_ACP_WORKAROUND_MASK = 1UL << 30;
static constexpr uint32_t LR1121_REG_HIGH_ACP_WORKAROUND_VALUE = 0x00000000;

/// GFSK modulation workaround register trio, standard (non-0.6/1.2kbps) values for our 38.4kbps
/// config — applied after every modulation-params write, mirroring RadioLib's workaroundGFSK()
/// ("always the first step, even when resetting" per its own comment on the first write).
static constexpr uint32_t LR1121_REG_GFSK_WORKAROUND_1_ADDR = 0x00F20344;
static constexpr uint32_t LR1121_REG_GFSK_WORKAROUND_1_MASK = 0x00000030;
static constexpr uint32_t LR1121_REG_GFSK_WORKAROUND_1_VALUE = 0x00000010;
static constexpr uint32_t LR1121_REG_GFSK_WORKAROUND_2_ADDR = 0x00F20348;
static constexpr uint32_t LR1121_REG_GFSK_WORKAROUND_2_MASK = 0x00000005;
static constexpr uint32_t LR1121_REG_GFSK_WORKAROUND_2_VALUE = 0x00000001;
static constexpr uint32_t LR1121_REG_GFSK_WORKAROUND_3_ADDR = 0x00F20244;
static constexpr uint32_t LR1121_REG_GFSK_WORKAROUND_3_MASK = 0x0001FF03;
static constexpr uint32_t LR1121_REG_GFSK_WORKAROUND_3_VALUE = 0x00000A01;

/// Banded image calibration for 868.25-869.85MHz +/-4MHz (~860-876MHz), matching RadioLib's
/// setFrequency() margin. CalibImage params are {floor((fmin-1)/4), ceil((fmax+1)/4)}.
static constexpr uint8_t LR1121_IMAGE_CAL_FREQ1 = 0xD7;
static constexpr uint8_t LR1121_IMAGE_CAL_FREQ2 = 0xDB;

// ============================================================================
// T3-S3 RF-switch table — sourced from the Meshtastic `tlora_t3s3_v1` variant's rfswitch.h,
// cross-checked against LilyGo's own T3-S3 LR1121 example and confirmed on real hardware.
// ============================================================================
//
// SetDioAsRfSwitch parameter layout: [enable_mask, standby, rx, tx, tx_hp, tx_hf, gnss, wifi]
// (8 bytes total), matching RadioLib's Lr11x0::setDioAsRfSwitch() signature. Each mode byte is
// a bitmask of which switch-controlled DIOs (bit0=DIO5, bit1=DIO6, ...) should be driven high
// in that mode.
static constexpr uint8_t LR1121_RFSWITCH_ENABLE_DIO5_DIO6 = 0x03;  ///< DIO5 + DIO6 are switch pins.
static constexpr uint8_t LR1121_RFSWITCH_STANDBY = 0x00;           ///< Both low.
static constexpr uint8_t LR1121_RFSWITCH_RX = 0x01;                ///< DIO5 high.
static constexpr uint8_t LR1121_RFSWITCH_TX = 0x02;                ///< DIO6 high (both LP and HP PA).
static constexpr uint8_t LR1121_RFSWITCH_TX_HP = 0x02;             ///< Same as TX (LP PA only used today).
static constexpr uint8_t LR1121_RFSWITCH_TX_HF = 0x00;             ///< 2.4GHz path unused; both low.
static constexpr uint8_t LR1121_RFSWITCH_GNSS = 0x00;              ///< Unused; both low.
static constexpr uint8_t LR1121_RFSWITCH_WIFI = 0x00;              ///< Unused; both low.

/// LR1121 TCXO voltage on the T3-S3 board — 3.0V, confirmed on real hardware.
static constexpr uint8_t LR1121_TCXO_STARTUP_DELAY_TICKS_MSB = 0x00;
static constexpr uint8_t LR1121_TCXO_STARTUP_DELAY_TICKS_MID = 0x01;
/// 0x140 ticks at 30.52us/tick (32.768kHz RTC) is ~9.8ms, not the ~5ms the SX1262-derived comment
/// used to claim (that chip's tick base differs) — harmless either way (longer startup is safe).
static constexpr uint8_t LR1121_TCXO_STARTUP_DELAY_TICKS_LSB = 0x40;

/// LR1121-specific per-channel dwell while waiting for authenticated exchange responses.
///
/// NOT seeded from the SX1262 value anymore (was 90, matching SX1262_EXCHANGE_RESPONSE_WAIT_SLICE_MS
/// exactly) — see 2026-07-17 hardware bring-up: golden captures for this exact device
/// (tests/corpus/captures/somfy_awning/exchange_open_sx1276.yaml) show the device's first reply
/// arriving ~287ms after the request, on the *same* channel the request went out on, against a
/// 300ms total wait budget split into 90ms per-channel hop slices (CH2→CH3→CH1→CH2). That only
/// leaves the *last* ~30ms hop-back-to-CH2 slice to catch it — survivable for SX1262/SX1276, but
/// LR1121's two-transaction 16-bit-opcode SPI protocol makes each hop's SetStandby/SetRfFrequency/
/// SetRx round-trip slower, which can push the hop-back-to-CH2 moment past 287ms and cause a clean
/// miss every try. Set above the largest response-wait budget (RESPONSE_WAIT_MS=500 in
/// proto_timing.h) so this driver never hops away from the request's channel while waiting for a
/// reply — real replies for this device only ever arrive on that same channel anyway, so hopping
/// during the wait was actively counterproductive, not just slow.
static constexpr int32_t LR1121_EXCHANGE_RESPONSE_WAIT_SLICE_MS = 600;

// ============================================================================
// LR1121 Radio Driver
// ============================================================================

/// @brief LR1121 implementation of RadioDriver.
/// @ingroup hioc_radio
///
/// Manages the LR1121 via 16-bit-opcode, two-transaction SPI using the SpiAccess
/// interface. Configures the chip in GFSK mode with software CRC-CCITT to match the
/// IO-Homecontrol protocol (the LR1121, like the SX1262, lacks the SX1276's IoHomeOn
/// mode). The IRQ-driven RX/TX orchestration is inherited from SoftPhyDriverBase; this
/// class supplies the SPI transport and every LR1121-specific register/opcode encoding
/// underneath it. See the file header for the driver's relationship to RadioSX1262.
class RadioLR1121 : public SoftPhyDriverBase {
 public:
  RadioLR1121(SpiAccess *spi, InternalGPIOPin *rst_pin, InternalGPIOPin *irq_pin, InternalGPIOPin *busy_pin,
              uint8_t tx_power, uint8_t tcxo_voltage_yaml_code)
      : SoftPhyDriverBase(rst_pin, LR1121_RESPONSE_PREAMBLE, LR1121_POST_TX_SETTLE_US),
        spi_(spi),
        irq_pin_(irq_pin),
        busy_pin_(busy_pin),
        tx_power_(tx_power),
        tcxo_voltage_yaml_code_(tcxo_voltage_yaml_code) {}

  /// @copydoc RadioDriver::init
  bool init() override;
  /// @brief Apply LR1121 runtime tuning: RX bandwidth, response preamble, post-TX settle delay.
  void apply_tuning(const TuningConfig &tuning) override {
    this->set_rx_bandwidth_(tuning.lr1121_rx_bandwidth);
    this->set_response_preamble_(tuning.lr1121_response_preamble);
    this->set_post_tx_settle_us_(tuning.lr1121_post_tx_settle_us);
  }
  /// @brief Per-channel dwell while waiting for exchange responses (LR1121).
  [[nodiscard]] uint32_t exchange_wait_slice_ms() const override { return LR1121_EXCHANGE_RESPONSE_WAIT_SLICE_MS; }
  /// @brief Per-channel dwell while pairing discovery hops (LR1121).
  ///
  /// LR1121 frequency changes require a standby→SetRfFrequency→RX cycle (no fast hop),
  /// same as the SX1262, so discovery needs the equivalent longer dwell. The value comes
  /// from the user-facing `lr1121_discovery_hop_slice_ms` tuning field.
  [[nodiscard]] uint16_t discovery_hop_slice_ms(const TuningConfig &tuning) const override {
    return tuning.lr1121_discovery_hop_slice_ms;
  }
  /// @brief TX→RX turnaround capability (LR1121): slow, same as SX1262.
  ///
  /// See @ref RadioSX1262::has_fast_tx_rx_turnaround for the rationale — the LR1121 needs
  /// the same standby/settle cycle between TX and RX.
  [[nodiscard]] bool has_fast_tx_rx_turnaround() const override { return false; }
  /// @copydoc RadioDriver::set_mode_rx
  void set_mode_rx() override;
  /// @copydoc RadioDriver::set_mode_standby
  void set_mode_standby() override;
  [[nodiscard]] bool is_failed() const override { return this->failed_; }
  [[nodiscard]] const char *chip_name() const override { return "lr1121"; }
  /// @brief Dump LR1121-specific debug info.
  void dump_debug() override;

 protected:
  // --- Tuning helper unique to this chip (its RX-bandwidth enum is not shared with SX1262 —
  // see tuning_config.h's LR1121RxBandwidth comment: two of the five borrowed SX1262 byte values
  // turned out wrong for this chip during 2026-07-17 bring-up) ---
  /// Apply the RX bandwidth selector and rewrite the modulation parameters.
  void set_rx_bandwidth_(LR1121RxBandwidth bandwidth);

  // --- SPI communication (16-bit opcode, two-transaction) ---
  /// Wait until BUSY pin is low before any SPI transaction.
  void wait_busy_();
  /// Write-only command: opcode + params, single NSS cycle.
  /// @param opcode LR1121 16-bit opcode.
  /// @param params Pointer to parameter buffer (may be nullptr).
  /// @param len Parameter length.
  void write_command_(uint16_t opcode, const uint8_t *params, uint8_t len);
  /// Read-type command: write transaction, wait BUSY, then a second NSS cycle clocks out
  /// a Stat1 status byte followed by `out_len` response bytes.
  /// @param opcode LR1121 16-bit opcode.
  /// @param params Pointer to request parameter buffer (may be nullptr).
  /// @param params_len Request parameter length.
  /// @param out Output buffer for the response bytes (excludes Stat1).
  /// @param out_len Number of response bytes to read.
  void read_command_(uint16_t opcode, const uint8_t *params, uint8_t params_len, uint8_t *out, uint8_t out_len);
  /// Write into the LR1121 TX buffer (always from the chip's internal write pointer, which
  /// resets to the buffer base for a fresh WriteBuffer sequence — unlike SX1262 there is no
  /// separate offset parameter here; see design §3.2 "SPI transport").
  void write_buffer_(const uint8_t *data, uint8_t len);
  /// Read from the LR1121 RX buffer at a given offset (as reported by GetRxBufferStatus).
  void read_buffer_(uint8_t offset, uint8_t len, uint8_t *data);
  /// Log a warning if the most recently observed Stat1 command-status byte indicates the
  /// chip rejected the previous command (FAIL/PERR — see radio_lr1121.h §4.4).
  void log_command_status_(uint16_t opcode) const;
  /// WriteRegMemMask32: read-modify-write a 32-bit register through `mask`/`value`. Used by the
  /// vendor errata workarounds below; opcode 0x010C (design analysis §4.1/§4.2).
  void write_reg_mem_mask32_(uint32_t addr, uint32_t mask, uint32_t value);
  /// Apply the Semtech high-ACP TX-quality erratum workaround (analysis §4.1). Must run before
  /// every SetRx/SetTx — called from set_mode_rx() and @ref before_tx_arm rather than once at
  /// init, mirroring Semtech's own call sites.
  void apply_high_acp_workaround_();
  /// Apply the GFSK modulation workaround register trio (analysis §4.2). Called at the end of
  /// write_modulation_params_() so it re-applies on every bandwidth retune too.
  void apply_gfsk_workaround_();
  /// Issue a banded image calibration for the 868MHz operating range (analysis §4.3).
  void calibrate_image_();

  // --- Radio configuration ---
  /// Full radio initialization (called from init()).
  void configure_radio_();
  /// @copydoc SoftPhyDriverBase::set_frequency_register
  ///
  /// Plain Hz, 32-bit — no PLL-step conversion needed on this chip.
  void set_frequency_register(uint32_t freq_hz) override;
  /// Configure GFSK packet parameters (preamble, payload length, CRC).
  void set_packet_params_(uint16_t preamble_len, uint8_t payload_len, uint8_t packet_type, uint8_t crc_type);
  /// Apply the runtime bandwidth setting to the LR1121 modulation parameters.
  void write_modulation_params_();
  /// @copydoc SoftPhyDriverBase::set_rx_packet_params
  void set_rx_packet_params() override;
  /// @copydoc SoftPhyDriverBase::set_tx_packet_params
  void set_tx_packet_params(uint16_t preamble_len, uint8_t payload_len) override {
    this->set_packet_params_(preamble_len, payload_len, LR1121_GFSK_PACKET_FIXED_LENGTH, LR1121_GFSK_CRC_OFF);
  }
  /// @copydoc SoftPhyDriverBase::clear_irq_status
  void clear_irq_status(uint32_t irq_mask) override;
  /// @brief Read device error flags (for diagnostics only — see dump_debug()).
  /// @return Error bitmask (2-byte response, cross-checked against RadioLib).
  uint16_t get_errors_();
  /// @brief Clear device error flags. Called both during init (after TCXO configuration, before
  ///        calibration) and at the end of configure_radio_() to discard init-time noise.
  void clear_errors_();
  /// @copydoc SoftPhyDriverBase::fill_capture_info
  ///
  /// Note: RadioCaptureInfo::irq_status is uint16_t; the 32-bit IRQ word is mapped down by
  /// taking bits [2..10] and shifting right by 2 (design §3.2 "IRQ-width note").
  void fill_capture_info(bool blocking_wait, uint32_t irq_status, uint8_t rx_offset, uint8_t reported_len,
                         const uint8_t *raw, uint8_t raw_len, const uint8_t *frame, uint8_t frame_len) override;

  /// IRQ pin (DIO9) ISR — sets dio_fired flag. Runs in interrupt context.
  static void gpio_intr(RadioLR1121 *arg);

  /// @copydoc SoftPhyDriverBase::read_irq_status_raw
  ///
  /// Reads the raw 32-bit IRQ status word from the radio (via GetStatus).
  uint32_t read_irq_status_raw() override;
  /// @copydoc SoftPhyDriverBase::sync_word_valid_bit
  [[nodiscard]] uint32_t sync_word_valid_bit() const override { return LR1121_IRQ_SYNC_WORD_VALID; }
  /// @copydoc SoftPhyDriverBase::rx_done_bit
  [[nodiscard]] uint32_t rx_done_bit() const override { return LR1121_IRQ_RX_DONE; }
  /// @copydoc SoftPhyDriverBase::tx_done_bit
  [[nodiscard]] uint32_t tx_done_bit() const override { return LR1121_IRQ_TX_DONE; }
  /// @copydoc SoftPhyDriverBase::preamble_detected_bit
  [[nodiscard]] uint32_t preamble_detected_bit() const override { return LR1121_IRQ_PREAMBLE_DETECTED; }
  /// @copydoc SoftPhyDriverBase::activity_irq_mask
  ///
  /// Excludes PREAMBLE_DETECTED, unlike the base's "any bit" default — see
  /// LR1121_IRQ_ACTIVITY_MASK's doc comment for why this chip needs the distinction.
  [[nodiscard]] uint32_t activity_irq_mask() const override { return LR1121_IRQ_ACTIVITY_MASK; }
  /// @copydoc SoftPhyDriverBase::read_rssi_raw_byte
  uint8_t read_rssi_raw_byte() override;
  /// @copydoc SoftPhyDriverBase::write_tx_buffer
  void write_tx_buffer(const uint8_t *data, uint8_t len) override { this->write_buffer_(data, len); }
  /// @copydoc SoftPhyDriverBase::get_rx_buffer_status
  void get_rx_buffer_status(uint8_t &reported_len, uint8_t &rx_offset) override;
  /// @copydoc SoftPhyDriverBase::read_rx_buffer
  void read_rx_buffer(uint8_t offset, uint8_t *data, uint8_t len) override { this->read_buffer_(offset, len, data); }
  /// @copydoc SoftPhyDriverBase::start_tx
  void start_tx() override;
  /// @copydoc SoftPhyDriverBase::before_tx_arm
  ///
  /// High-ACP workaround (analysis §4.1) — Semtech applies this unconditionally before every
  /// SetTx, same as before every SetRx (see set_mode_rx()).
  void before_tx_arm() override { this->apply_high_acp_workaround_(); }

 private:
  SpiAccess *spi_;
  InternalGPIOPin *irq_pin_;
  InternalGPIOPin *busy_pin_;
  uint8_t tx_power_;
  uint8_t tcxo_voltage_yaml_code_;  ///< Raw TCXO_VOLTAGE_OPTIONS code from YAML (1_6V=0x01 .. 3_3V=0x08).
  bool failed_{false};
  uint8_t last_stat1_{0};                                            ///< Most recent Stat1 byte (diagnostics).
  LR1121RxBandwidth rx_bandwidth_{LR1121RxBandwidth::BW_117_3_KHZ};  ///< Runtime-tunable RX bandwidth.
};

}  // namespace home_io_control
}  // namespace esphome
