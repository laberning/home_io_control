#pragma once

/// @file radio_soft_phy_driver_base.h
/// @brief Shared driver flow for radios using the software PHY (SX1262, LR1121).
/// @ingroup hioc_radio
///
/// RadioSX1262 and RadioLR1121 both lack the SX1276's IoHomeOn hardware framing, so both
/// reproduce IO-Homecontrol framing in software on top of generic GFSK support
/// (`radio_soft_phy.h`'s UART bit-encode/probe). Beyond that shared bit-level codec, the two
/// drivers' IRQ-driven RX state machine and TX orchestration are identical in every detail that
/// isn't chip-specific transport or register encoding, so this class holds that shared flow once
/// instead of each driver maintaining its own copy.
///
/// This class holds everything the two drivers do identically: `wait_for_packet()`/
/// `check_for_packet()`'s IRQ polling and sync/RX-done race resolution, `read_rx_packet()`'s
/// buffer-read and UART-probe recovery, `send_packet()`'s TX orchestration, `read_rssi()`'s
/// formula, and the response-preamble/post-TX-settle tuning fields. What genuinely differs
/// between the two chips — SPI opcode encoding/transport, IRQ bit values and word width,
/// register-level packet/modulation parameter encoding, and the handful of one-off steps one
/// chip needs that the other doesn't (SX1262's buffer-base-address write, LR1121's high-ACP
/// pre-TX workaround and preamble-tolerant activity check) — stays behind virtual primitives and
/// hooks implemented by `RadioSX1262`/`RadioLR1121`.

#include "radio_interface.h"
#include "radio_soft_phy.h"

#include <cstdint>

namespace esphome {
namespace home_io_control {

/// Fixed raw-RX probe length: chosen from captures of 23-25 byte protocol frames after UART
/// packing and CRC appending — the longest frame (25 bytes + 2 CRC) UART-packs to 34 raw bytes,
/// so 48 bytes preserves complete traffic (with margin for leading noise before the frame start)
/// without relying on either chip's variable-length engine. This is a protocol-frame-size
/// property, not a chip quirk, so both drivers share one value — used here for the raw-probe
/// threshold in @ref SoftPhyDriverBase::read_rx_packet and by each driver's own
/// `set_rx_packet_params()` for the configured RX payload length.
static constexpr uint8_t SOFT_PHY_RX_PROBE_PACKET_LEN = 48;

/// Sentinel meaning "every IRQ bit counts as activity" — the default for @ref
/// SoftPhyDriverBase::activity_irq_mask, correct for chips (SX1262) whose hardware-level IRQ
/// mask already excludes the one bit (PreambleDetected) that would need special handling.
static constexpr uint32_t SOFT_PHY_ALL_IRQ_BITS = 0xFFFFFFFF;

/// @brief Shared RX/TX driver flow for the software-PHY radios (SX1262, LR1121).
/// @ingroup hioc_radio
class SoftPhyDriverBase : public RadioDriver {
 public:
  /// @param rst_pin Active-low hardware reset pin, forwarded to RadioDriver.
  /// @param default_response_preamble Chip-specific default for @ref response_preamble (each
  ///   concrete driver passes its own validated constant — this class has no opinion on the
  ///   value, only on where it's stored).
  /// @param default_post_tx_settle_us Chip-specific default post-TX settling delay, same rationale.
  SoftPhyDriverBase(InternalGPIOPin *rst_pin, uint16_t default_response_preamble, uint16_t default_post_tx_settle_us)
      : RadioDriver(rst_pin),
        response_preamble_(default_response_preamble),
        post_tx_settle_us_(default_post_tx_settle_us) {}

  /// @copydoc RadioDriver::send_packet
  bool send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) override;
  /// @copydoc RadioDriver::wait_for_packet
  bool wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) override;
  /// @copydoc RadioDriver::check_for_packet
  bool check_for_packet(RadioRxPacket &packet) override;
  /// @copydoc RadioDriver::change_frequency
  void change_frequency(uint32_t freq_hz) override;
  /// @copydoc RadioDriver::read_rssi
  ///
  /// Same formula on both chips (`-(int16_t) raw / 2`); only the opcode used to read the single
  /// raw byte differs, via @ref read_rssi_raw_byte.
  int16_t read_rssi() override;
  /// @copydoc RadioDriver::is_sync_detected
  bool is_sync_detected() override;
  /// @copydoc RadioDriver::is_preamble_detected
  bool is_preamble_detected() override;
  /// @brief Preamble for response/continuation frames — shared storage, see the concrete
  /// drivers' constructors/tuning defaults for the chip-specific rationale and value.
  [[nodiscard]] uint16_t response_preamble() const override { return this->response_preamble_; }

 protected:
  // --- Tuning helpers shared by both drivers (values/defaults stay chip-specific) ---
  /// Set the preamble length used for response/continuation frames within an exchange.
  void set_response_preamble_(uint16_t preamble) { this->response_preamble_ = preamble; }
  /// Set the delay between TX completion and re-entering RX.
  void set_post_tx_settle_us_(uint16_t delay_us) { this->post_tx_settle_us_ = delay_us; }

  // --- Shared RX/TX orchestration (moved verbatim from RadioSX1262/RadioLR1121) ---
  /// Read a received packet from the buffer and return the raw bytes reported by the chip.
  /// Virtual to allow test doubles (both concrete drivers' tests override this).
  virtual bool read_rx_packet(RadioRxPacket &packet, bool blocking_wait, uint32_t irq_status);
  /// Reset RX state machine and buffer. Optionally force standby first.
  void reset_rx_state_(bool force_standby = true);

  /// @brief Read the raw IRQ status word from the radio.
  /// Virtual to allow test doubles (both concrete drivers' tests override this).
  virtual uint32_t read_irq_status_raw() = 0;
  /// Clear IRQ status bits.
  /// @param irq_mask Bitmask of IRQs to clear (each driver narrows to its own IRQ word width).
  virtual void clear_irq_status(uint32_t irq_mask) = 0;

  /// @name Chip-specific IRQ bit values
  /// Each driver's own IRQ bit constants, exposed as accessors so the shared RX/TX orchestration
  /// never needs to name a chip-specific constant directly.
  ///@{
  [[nodiscard]] virtual uint32_t sync_word_valid_bit() const = 0;
  [[nodiscard]] virtual uint32_t rx_done_bit() const = 0;
  [[nodiscard]] virtual uint32_t tx_done_bit() const = 0;
  [[nodiscard]] virtual uint32_t preamble_detected_bit() const = 0;
  ///@}

  /// @brief IRQ bits that count as "activity" for the internal `poll_until_activity_()` helper
  /// and @ref check_for_packet.
  ///
  /// Default is "any bit" — correct for SX1262, whose `SetDioIrqParams` mask already excludes
  /// `PreambleDetected` system-wide, so a preamble-only reading can never reach this check in the
  /// first place. LR1121 routes `PreambleDetected` to its IRQ pin for other reasons and overrides
  /// this to exclude it: a preamble-only reading means a frame may still be arriving, and treating
  /// it as terminal activity would tear down RX mid-reception.
  [[nodiscard]] virtual uint32_t activity_irq_mask() const { return SOFT_PHY_ALL_IRQ_BITS; }

  /// Set RF frequency via the chip's own frequency register/opcode encoding, and update
  /// `current_freq_`. Called from both @ref change_frequency and the shared `send_packet()`.
  virtual void set_frequency_register(uint32_t freq_hz) = 0;
  /// Configure RX-specific packet parameters (preamble detector length, fixed probe length).
  virtual void set_rx_packet_params() = 0;
  /// Configure TX packet parameters for one outgoing UART-encoded frame.
  /// @param preamble_len Preamble length in symbols, from the caller's RadioTxConfig.
  /// @param payload_len UART-encoded payload length in bytes.
  virtual void set_tx_packet_params(uint16_t preamble_len, uint8_t payload_len) = 0;
  /// Read the single raw RSSI byte (chip-specific opcode); formula is shared, see @ref read_rssi.
  virtual uint8_t read_rssi_raw_byte() = 0;
  /// Write the UART-encoded TX payload into the chip's TX buffer.
  virtual void write_tx_buffer(const uint8_t *data, uint8_t len) = 0;
  /// Read the chip-reported RX length and buffer offset (raw, before any clamping).
  virtual void get_rx_buffer_status(uint8_t &reported_len, uint8_t &rx_offset) = 0;
  /// Read `len` bytes from the RX buffer starting at `offset`.
  virtual void read_rx_buffer(uint8_t offset, uint8_t *data, uint8_t len) = 0;
  /// Issue the SetTx opcode with the fixed TX timeout — identical 3-byte payload on both chips,
  /// differing only in opcode/transport, so this stays a thin chip-specific wrapper.
  virtual void start_tx() = 0;
  /// Populate the RadioCaptureInfo from chip-specific telemetry (RSSI opcode, packet-status byte,
  /// and IRQ-word-width narrowing all differ per chip).
  virtual void fill_capture_info(bool blocking_wait, uint32_t irq_status, uint8_t rx_offset, uint8_t reported_len,
                                 const uint8_t *raw, uint8_t raw_len, const uint8_t *frame, uint8_t frame_len) = 0;

  /// @brief Hook run immediately before every `SetTx`. No-op by default; LR1121 overrides this to
  /// apply its high-ACP TX-quality workaround, which Semtech's own reference applies unconditionally
  /// before every SetRx/SetTx.
  virtual void before_tx_arm() {}
  /// @brief Hook run as part of @ref reset_rx_state_, before re-entering RX. No-op by default;
  /// SX1262 overrides this to (re-)write its buffer base address, which LR1121 doesn't need.
  virtual void configure_buffer_base() {}

 private:
  // === wait_for_packet/check_for_packet state-machine helpers (private) ===
  /// Poll for first *terminal* radio activity (IRQ pin or an IRQ status bit within
  /// @ref activity_irq_mask) within timeout.
  bool poll_until_activity_(uint32_t start, uint32_t timeout_ms, uint32_t &irq);
  /// Resolve the SYNC_WORD_VALID → RX_DONE race condition common to both chips.
  bool resolve_sync_race_(uint32_t start, uint32_t timeout_ms, uint32_t &irq);
  /// Finalize receive: read the packet if RX_DONE is set, otherwise record failure.
  bool finalize_receive_(RadioRxPacket &packet, uint32_t irq);

  uint16_t response_preamble_;
  uint16_t post_tx_settle_us_;
};

}  // namespace home_io_control
}  // namespace esphome
