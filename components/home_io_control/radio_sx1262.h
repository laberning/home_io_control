#pragma once

/// @file radio_sx1262.h
/// @brief SX1262 radio driver for IO-Homecontrol.
/// @ingroup hioc_radio
///
/// Implements the RadioDriver interface for the Semtech SX1262 transceiver.
/// Unlike the SX1276, the SX1262 uses opcode-based SPI commands and requires a
/// BUSY pin check before every SPI transaction. The capture path preserves the
/// radio-reported RX metadata so offline analysis can work from trustworthy data.
/// The IRQ-driven RX/TX orchestration this driver shares with RadioLR1121 lives in
/// SoftPhyDriverBase (see radio_soft_phy_driver_base.h) — this file has the SX1262-specific
/// SPI transport and register/opcode encoding underneath it.
/// @todo Validate Heltec V4-family boards on real hardware, especially the assumed
///       front-end module enable pins and the required TCXO voltage selection.

#include "radio_soft_phy_driver_base.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace home_io_control {

// ============================================================================
// SX1262 Opcode Constants
// ============================================================================

static constexpr uint8_t SX1262_SET_STANDBY = 0x80;
static constexpr uint8_t SX1262_SET_RX = 0x82;
static constexpr uint8_t SX1262_SET_TX = 0x83;
static constexpr uint8_t SX1262_SET_RF_FREQUENCY = 0x86;
static constexpr uint8_t SX1262_SET_RX_TX_FALLBACK_MODE = 0x93;
static constexpr uint8_t SX1262_WRITE_BUFFER = 0x0E;
static constexpr uint8_t SX1262_READ_BUFFER = 0x1E;
static constexpr uint8_t SX1262_SET_DIO_IRQ_PARAMS = 0x08;
static constexpr uint8_t SX1262_GET_IRQ_STATUS = 0x12;
static constexpr uint8_t SX1262_GET_PACKET_STATUS = 0x14;
static constexpr uint8_t SX1262_GET_DEVICE_ERRORS = 0x17;
static constexpr uint8_t SX1262_CLEAR_IRQ_STATUS = 0x02;
static constexpr uint8_t SX1262_CLEAR_DEVICE_ERRORS = 0x07;
static constexpr uint8_t SX1262_SET_PACKET_TYPE = 0x8A;
static constexpr uint8_t SX1262_SET_MODULATION_PARAMS = 0x8B;
static constexpr uint8_t SX1262_SET_PACKET_PARAMS = 0x8C;
static constexpr uint8_t SX1262_SET_BUFFER_BASE_ADDRESS = 0x8F;
static constexpr uint8_t SX1262_SET_PA_CONFIG = 0x95;
static constexpr uint8_t SX1262_SET_TX_PARAMS = 0x8E;
static constexpr uint8_t SX1262_SET_DIO2_AS_RF_SWITCH_CTRL = 0x9D;
static constexpr uint8_t SX1262_SET_DIO3_AS_TCXO_CTRL = 0x97;
static constexpr uint8_t SX1262_CALIBRATE = 0x89;
static constexpr uint8_t SX1262_CALIBRATE_IMAGE = 0x98;
static constexpr uint8_t SX1262_GET_RX_BUFFER_STATUS = 0x13;
static constexpr uint8_t SX1262_GET_RSSI_INST = 0x15;
static constexpr uint8_t SX1262_SET_REGULATOR_MODE = 0x96;
static constexpr uint8_t SX1262_GET_STATUS = 0xC0;

// SPI register access opcodes
static constexpr uint8_t SX1262_WRITE_REGISTER = 0x0D;
static constexpr uint8_t SX1262_READ_REGISTER = 0x1D;

// ============================================================================
// SX1262 IRQ Bit Masks
// ============================================================================

static constexpr uint16_t SX1262_IRQ_TX_DONE = 0x0001;
static constexpr uint16_t SX1262_IRQ_RX_DONE = 0x0002;
static constexpr uint16_t SX1262_IRQ_PREAMBLE_DETECTED = 0x0004;
static constexpr uint16_t SX1262_IRQ_SYNC_WORD_VALID = 0x0008;
static constexpr uint16_t SX1262_IRQ_CRC_ERR = 0x0040;

/// IRQ bits that represent a *terminal* radio event — a frame finished decoding, an outbound
/// frame completed, or the chip reported a bad CRC. Deliberately excludes PREAMBLE_DETECTED even
/// though `configure_radio_()` now includes it in `irqMask`: a preamble alone means a frame may
/// still be arriving, so SoftPhyDriverBase's poll_until_activity_()/check_for_packet() (gated by
/// @ref RadioSX1262::activity_irq_mask) must not treat it as terminal — doing so would tear down
/// RX mid-reception. Same trap, same fix shape as @ref LR1121_IRQ_ACTIVITY_MASK.
static constexpr uint16_t SX1262_IRQ_ACTIVITY_MASK =
    SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE | SX1262_IRQ_SYNC_WORD_VALID | SX1262_IRQ_CRC_ERR;
// Sync word register base address
static constexpr uint16_t SX1262_REG_SYNC_WORD = 0x06C0;
static constexpr uint16_t SX1262_REG_RX_GAIN = 0x08AC;
static constexpr uint16_t SX1262_REG_TX_CLAMP_CONFIG = 0x08D8;

/// TX modulation-quality erratum register (SX1262 datasheet §15.1, "Modulation Quality with
/// 500 kHz LoRa Bandwidth" — the title names LoRa, but the workaround table covers every
/// modulation). Bit 2 must be cleared *only* for LoRa at BW 500 kHz and set to 1 for everything
/// else, explicitly including any (G)FSK configuration — which is all this driver ever uses.
/// Semtech's own driver and RadioLib apply it as `TxModulation` / `fixModulationQuality()`.
/// Leaving it at its reset value degrades transmitted modulation quality, which on this protocol
/// shows up as a peer that intermittently fails to decode an otherwise strong frame.
/// Counterpart to the already-applied TxClamp erratum (@ref SX1262_REG_TX_CLAMP_CONFIG).
static constexpr uint16_t SX1262_REG_TX_MODULATION = 0x0889;
/// Bit 2 of @ref SX1262_REG_TX_MODULATION — the (G)FSK-correct value is 1.
static constexpr uint8_t SX1262_TX_MODULATION_GFSK_BIT = 0x04;

/// Data-buffer split programmed by configure_buffer_base(): TX packets build from 0x00, RX
/// packets land at 0x80. The RX base doubles as the read offset for a length-driven receive —
/// a single in-flight packet always starts exactly there.
static constexpr uint8_t SX1262_TX_BUFFER_BASE = 0x00;
static constexpr uint8_t SX1262_RX_BUFFER_BASE = 0x80;

static constexpr uint8_t SX1262_GFSK_PACKET_TYPE_KNOWN_LENGTH = 0x00;
static constexpr uint8_t SX1262_GFSK_CRC_OFF = 0x01;
static constexpr uint8_t SX1262_FALLBACK_STDBY_XOSC = 0x30;

/// How long SoftPhyDriverBase::wait_busy_() waits for BUSY to drop before declaring the chip
/// failed. Short: this chip's RC-oscillator-clocked commands settle quickly, unlike LR1121's
/// post-reset boot ROM (see LR1121_BUSY_TIMEOUT_MS in radio_lr1121.h).
static constexpr uint32_t SX1262_BUSY_TIMEOUT_MS = 10;

// ============================================================================
// SX1262 Radio Driver
// ============================================================================

/// @brief SX1262 implementation of RadioDriver.
/// @ingroup hioc_radio
///
/// Manages the SX1262 via opcode‑based SPI using the SpiAccess interface.
/// Configures the chip in FSK mode with software CRC‑CCITT to match the
/// IO‑Homecontrol protocol (the SX1262 lacks the SX1276's IoHomeOn mode).
/// The IRQ-driven RX/TX orchestration is inherited from SoftPhyDriverBase; this class supplies
/// the SPI transport and every SX1262-specific register/opcode encoding underneath it.
/// @todo Confirm whether additional SX1262 board variants need board-specific FEM
///       defaults beyond the currently documented Heltec V3/V4 assumptions.
class RadioSX1262 : public SoftPhyDriverBase {
 public:
  RadioSX1262(SpiAccess *spi, InternalGPIOPin *rst_pin, InternalGPIOPin *dio1_pin, InternalGPIOPin *busy_pin,
              uint8_t tx_power, uint8_t tcxo_voltage, InternalGPIOPin *fem_en_pin = nullptr,
              InternalGPIOPin *vfem_pin = nullptr, InternalGPIOPin *fem_pa_pin = nullptr)
      : SoftPhyDriverBase(rst_pin, busy_pin, SX1262_BUSY_TIMEOUT_MS, SX1262_RESPONSE_PREAMBLE,
                          SX1262_POST_TX_SETTLE_US),
        spi_(spi),
        dio1_pin_(dio1_pin),
        fem_en_pin_(fem_en_pin),
        vfem_pin_(vfem_pin),
        fem_pa_pin_(fem_pa_pin),
        tx_power_(tx_power),
        tcxo_voltage_(tcxo_voltage) {}

  /// @copydoc RadioDriver::init
  bool init() override;
  /// @brief Apply SX1262 runtime tuning: RX bandwidth, response preamble, post-TX settle delay.
  void apply_tuning(const TuningConfig &tuning) override {
    this->set_rx_bandwidth_(tuning.sx1262_rx_bandwidth);
    this->set_response_preamble_(tuning.sx1262_response_preamble);
    this->set_post_tx_settle_us_(tuning.sx1262_post_tx_settle_us);
  }
  /// @brief Per-channel dwell for a rotating listen (SX1262).
  ///
  /// See @ref SX1262_DISCOVERY_HOP_SLICE_MS for why a short dwell is correct here despite SX1262's
  /// slower per-channel retune than the SX1276's FastHop. Governs discovery and the broadcast
  /// roll-call alike (see @ref RadioDriver::hop_dwell_ms). The value comes from the user-facing
  /// `sx1262_discovery_hop_slice_ms` tuning field.
  [[nodiscard]] uint16_t hop_dwell_ms(const TuningConfig &tuning) const override {
    return tuning.sx1262_discovery_hop_slice_ms;
  }
  /// @brief TX→RX turnaround capability (SX1262): slow.
  ///
  /// The TX→standby→RX transition plus the post-TX settle delay is too slow to
  /// catch a device's immediate reply through the standard exchange wait; in
  /// pairing, the key-confirm (0x33) is missed that way. PairingEngine therefore
  /// uses its dedicated key-confirm wait with key-init re-trigger on this driver.
  [[nodiscard]] bool has_fast_tx_rx_turnaround() const override { return false; }
  /// @copydoc RadioDriver::set_mode_rx
  void set_mode_rx() override;
  /// @copydoc RadioDriver::set_mode_standby
  void set_mode_standby() override;
  [[nodiscard]] const char *chip_name() const override { return "sx1262"; }
  /// @brief Dump SX1262‑specific debug info.
  void dump_debug() override;

 protected:
  // --- Tuning helper unique to this chip (its RX-bandwidth enum is not shared, see
  // tuning_config.h's LR1121RxBandwidth comment for why the two chips' encodings diverged) ---
  /// Apply the RX bandwidth selector and rewrite the modulation parameters.
  /// @param bandwidth Bandwidth selector enum.
  void set_rx_bandwidth_(SX1262RxBandwidth bandwidth);

  // --- SPI communication (opcode‑based) ---
  /// Write an opcode with optional parameter bytes.
  /// @param opcode SX1262 opcode.
  /// @param params Pointer to parameter buffer (may be nullptr).
  /// @param len Parameter length.
  void write_opcode_(uint8_t opcode, const uint8_t *params, uint8_t len);
  /// Read response from an opcode.
  /// @param opcode Opcode that was previously written.
  /// @param data Output buffer.
  /// @param len Expected number of bytes to read.
  void read_opcode_(uint8_t opcode, uint8_t *data, uint8_t len);
  /// Write to a register (SX1262 uses opcodes for register access).
  /// @param addr 16‑bit register address.
  /// @param data Pointer to data bytes.
  /// @param len Number of bytes.
  void write_register_(uint16_t addr, const uint8_t *data, uint8_t len);
  /// Read from a register.
  /// @param addr 16‑bit register address.
  /// @param data Output buffer.
  /// @param len Number of bytes to read.
  void read_register_(uint16_t addr, uint8_t *data, uint8_t len);
  /// Write into the SX1262 TX/RX buffer at a given offset.
  /// @param offset Buffer offset.
  /// @param data Payload bytes.
  /// @param len Payload length.
  void write_buffer_(uint8_t offset, const uint8_t *data, uint8_t len);
  /// Read from the SX1262 RX buffer.
  /// @param offset Buffer offset.
  /// @param data Output buffer.
  /// @param len Number of bytes to read.
  void read_buffer_(uint8_t offset, uint8_t *data, uint8_t len);

  // --- Radio configuration ---
  /// Full radio initialization (called from init()).
  void configure_radio_();
  /// @copydoc SoftPhyDriverBase::set_frequency_register
  void set_frequency_register(uint32_t freq_hz) override;
  /// Configure packet parameters (preamble, payload length, CRC).
  /// @param preamble_len Preamble length in symbols.
  /// @param payload_len Expected payload length.
  /// @param packet_type Fixed for GFSK.
  /// @param crc_type CRC configuration (off or on).
  void set_packet_params_(uint16_t preamble_len, uint8_t payload_len, uint8_t packet_type, uint8_t crc_type);
  /// Apply the runtime bandwidth setting to the SX1262 modulation parameters.
  void write_modulation_params_();
  /// Apply the Semtech TX modulation-quality erratum workaround (datasheet §15.1). The datasheet
  /// requires it "before any packet transmission", so it hangs off @ref before_tx_arm rather than
  /// running once at init — same placement rationale as RadioLR1121's high-ACP workaround.
  void apply_tx_modulation_workaround_();
  /// @copydoc SoftPhyDriverBase::set_rx_packet_params
  void set_rx_packet_params() override;
  /// @copydoc SoftPhyDriverBase::set_tx_packet_params
  void set_tx_packet_params(uint16_t preamble_len, uint8_t payload_len) override {
    this->set_packet_params_(preamble_len, payload_len, SX1262_GFSK_PACKET_TYPE_KNOWN_LENGTH, SX1262_GFSK_CRC_OFF);
  }
  /// @copydoc SoftPhyDriverBase::clear_irq_status
  void clear_irq_status(uint32_t irq_mask) override;
  /// @brief Read device error flags (and clear them).
  /// @return Error bitmask.
  uint16_t get_device_errors_();
  /// @brief Clear device error flags.
  void clear_device_errors_();
  /// @copydoc SoftPhyDriverBase::configure_buffer_base
  ///
  /// SX1262's buffer base addresses (TX=0x00, RX=0x80) are re-asserted on every RX reset — LR1121
  /// has no equivalent register.
  void configure_buffer_base() override;
  /// @copydoc SoftPhyDriverBase::early_rx_read_offset
  ///
  /// The SX1262 writes demodulated bytes into its data buffer as they arrive, and a single
  /// in-flight packet always starts at the RX base address programmed by configure_buffer_base(),
  /// so the shared flow can read the frame out on its own air time rather than waiting the fixed
  /// ~10 ms for the 48-byte RX_DONE.
  [[nodiscard]] int16_t early_rx_read_offset() const override { return SX1262_RX_BUFFER_BASE; }
  /// @copydoc SoftPhyDriverBase::fill_capture_info
  ///
  /// Note: `crc_error` is set from the CrcErr IRQ bit — unlike the SX1276, this
  /// driver sees and reports frames with a bad CRC.
  void fill_capture_info(bool blocking_wait, uint32_t irq_status, uint8_t rx_offset, uint8_t reported_len,
                         const uint8_t *raw, uint8_t raw_len, const uint8_t *frame, uint8_t frame_len) override;

  /// DIO1 ISR — sets dio_fired flag. Runs in interrupt context.
  static void gpio_intr(RadioSX1262 *arg);

  /// @copydoc SoftPhyDriverBase::read_irq_status_raw
  uint32_t read_irq_status_raw() override;
  /// @copydoc SoftPhyDriverBase::sync_word_valid_bit
  [[nodiscard]] uint32_t sync_word_valid_bit() const override { return SX1262_IRQ_SYNC_WORD_VALID; }
  /// @copydoc SoftPhyDriverBase::rx_done_bit
  [[nodiscard]] uint32_t rx_done_bit() const override { return SX1262_IRQ_RX_DONE; }
  /// @copydoc SoftPhyDriverBase::tx_done_bit
  [[nodiscard]] uint32_t tx_done_bit() const override { return SX1262_IRQ_TX_DONE; }
  /// @copydoc SoftPhyDriverBase::preamble_detected_bit
  [[nodiscard]] uint32_t preamble_detected_bit() const override { return SX1262_IRQ_PREAMBLE_DETECTED; }
  /// @copydoc SoftPhyDriverBase::activity_irq_mask
  ///
  /// The base class default ("any bit") stopped being safe the moment `configure_radio_()`
  /// unmasked PreambleDetected — see @ref SX1262_IRQ_ACTIVITY_MASK's doc comment.
  [[nodiscard]] uint32_t activity_irq_mask() const override { return SX1262_IRQ_ACTIVITY_MASK; }
  /// @copydoc SoftPhyDriverBase::read_rssi_raw_byte
  uint8_t read_rssi_raw_byte() override;
  /// @copydoc SoftPhyDriverBase::write_tx_buffer
  void write_tx_buffer(const uint8_t *data, uint8_t len) override {
    this->write_buffer_(SX1262_TX_BUFFER_BASE, data, len);
  }
  /// @copydoc SoftPhyDriverBase::get_rx_buffer_status
  void get_rx_buffer_status(uint8_t &reported_len, uint8_t &rx_offset) override;
  /// @copydoc SoftPhyDriverBase::read_rx_buffer
  void read_rx_buffer(uint8_t offset, uint8_t *data, uint8_t len) override { this->read_buffer_(offset, data, len); }
  /// @copydoc SoftPhyDriverBase::start_tx
  void start_tx() override;
  /// @copydoc SoftPhyDriverBase::before_tx_arm
  ///
  /// TX modulation-quality erratum (@ref SX1262_REG_TX_MODULATION): the datasheet requires bit 2
  /// be set for every (G)FSK transmission, so it is re-asserted before each SetTx rather than
  /// assumed to survive from init.
  void before_tx_arm() override { this->apply_tx_modulation_workaround_(); }

 private:
  SpiAccess *spi_;
  InternalGPIOPin *dio1_pin_;
  InternalGPIOPin *fem_en_pin_;
  InternalGPIOPin *vfem_pin_;
  InternalGPIOPin *fem_pa_pin_;
  uint8_t tx_power_;
  uint8_t tcxo_voltage_;
  SX1262RxBandwidth rx_bandwidth_{SX1262RxBandwidth::BW_58_6_KHZ};  ///< Runtime-tunable RX bandwidth.
};

}  // namespace home_io_control
}  // namespace esphome
