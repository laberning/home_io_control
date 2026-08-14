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

/// Raw bytes read in the first stage of a length-driven receive — enough to hold CTRL0's UART
/// cell (10 bits) at any of the probe's bit alignments (up to 9 bits of slack).
static constexpr uint8_t SOFT_PHY_EARLY_HEADER_RAW_BYTES = 3;

/// Air-time margin added before every mid-reception buffer read, in raw bytes.
///
/// Covers the lag between a byte finishing on air and the chip having it in its data buffer, plus
/// the granularity of the polled sync-word observation. Two byte-times is generous at this line
/// rate and still leaves a length-driven receive far ahead of the fixed-length RX_DONE.
static constexpr uint8_t SOFT_PHY_EARLY_READ_MARGIN_BYTES = 2;

/// Poll interval while waiting out a frame's remaining air time, in microseconds.
static constexpr uint32_t SOFT_PHY_EARLY_POLL_US = 100;

/// Smallest receive window a length-driven receive will be attempted in, in milliseconds.
///
/// The longest possible frame (FRAME_MAX_SIZE + CRC, UART-packed) occupies ~9.4 ms of air time, so
/// a caller with less than this left cannot finish one either way. Declining up front keeps the
/// early path from spending a short window's whole budget on a receive it cannot complete.
static constexpr uint32_t SOFT_PHY_EARLY_MIN_WINDOW_MS = 12;

/// Protocol line rate. The same 38400 bps every driver programs into its own bitrate register.
static constexpr uint32_t SOFT_PHY_LINE_RATE_BPS = 38400;
/// Microseconds in a second, for the air-time arithmetic below.
static constexpr uint32_t SOFT_PHY_US_PER_SECOND = 1000000;

/// @brief On-air time in microseconds for `raw_bytes` bytes at the protocol's line rate.
///
/// One byte is 8 / 38400 s = 208.333 µs. Computed as an integer division rounded *up*, so the
/// result never falls short of a whole byte's air time and a caller that waits on it never reads
/// the chip's buffer early. The numerator peaks around 360 million for the longest frame this is
/// ever asked about, well inside uint32_t.
constexpr uint32_t soft_phy_air_time_us(uint32_t raw_bytes) {
  const uint32_t bit_periods = raw_bytes * BITS_PER_BYTE * SOFT_PHY_US_PER_SECOND;
  return (bit_periods + SOFT_PHY_LINE_RATE_BPS - 1) / SOFT_PHY_LINE_RATE_BPS;
}

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
  /// Minimal path back into RX immediately after a transmission — see the definition for why this
  /// is deliberately not @ref reset_rx_state_.
  void rearm_rx_after_tx_();

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

  /// @brief Data-buffer offset an in-flight reception is being written to, or a negative value
  /// when this chip must not be read before RX_DONE.
  ///
  /// Neither chip's RX_DONE marks the end of the *frame*: with no hardware framing, RX runs in
  /// fixed-length mode at @ref SOFT_PHY_RX_PROBE_PACKET_LEN, so RX_DONE arrives a fixed ~10 ms
  /// after the sync word no matter how short the frame actually was. That delay lands squarely on
  /// the protocol's tightest turnaround — the hub's reply to a device's challenge — so a driver
  /// that can read its buffer while reception is still running opts in here and the shared flow
  /// finishes on the frame's own air time instead (see `try_early_completion_`).
  ///
  /// Default is -1: wait for RX_DONE exactly as before. SX1262 overrides it with the RX base
  /// address it programs in configure_buffer_base(), which is where a single in-flight packet
  /// always starts. LR1121 is left on the RX_DONE path pending hardware validation.
  [[nodiscard]] virtual int16_t early_rx_read_offset() const { return -1; }

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
  /// Resolve the SYNC_WORD_VALID → RX_DONE race condition common to both chips, and — on a chip
  /// that opts into @ref early_rx_read_offset — give the length-driven receive its chance first.
  /// @param early_completed Set when a whole CRC-valid frame was recovered without waiting for
  ///   RX_DONE; `packet` is then already populated and the caller is done.
  bool resolve_sync_race_(uint32_t start, uint32_t timeout_ms, uint32_t &irq, RadioRxPacket &packet,
                          bool &early_completed);
  /// Finish a reception on the frame's own air time rather than on the chip's fixed-length
  /// RX_DONE. Returns true only when a CRC-valid frame was recovered.
  bool try_early_completion_(RadioRxPacket &packet, uint32_t sync_us, uint32_t irq_status, uint32_t start_ms,
                             uint32_t timeout_ms);
  /// Finalize receive: read the packet if RX_DONE is set, otherwise record failure.
  bool finalize_receive_(RadioRxPacket &packet, uint32_t irq);

  uint16_t response_preamble_;
  uint16_t post_tx_settle_us_;
};

}  // namespace home_io_control
}  // namespace esphome
