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
/// SoftPhyDriverBase::activity_irq_mask. Neither concrete driver uses it any more: both SX1262
/// and LR1121 unmask PreambleDetected at the hardware level and override this to exclude it (see
/// SX1262_IRQ_ACTIVITY_MASK / LR1121_IRQ_ACTIVITY_MASK). Kept as the base-class default for a
/// hypothetical future driver that never unmasks PreambleDetected in the first place, where "any
/// bit" is genuinely safe again.
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

/// Blocking budget @ref SoftPhyDriverBase::check_for_packet gives a length-driven receive, in
/// milliseconds — the `timeout_ms` passed to `try_early_completion_()` from the non-blocking
/// idle-loop RX path (issue #81). The real bound on how long that call can block is the frame's
/// own air time (~9.4 ms worst case, see @ref SOFT_PHY_EARLY_MIN_WINDOW_MS), not this number; this
/// value only has to be large enough not to cut a genuine frame short. It is not itself a tight
/// bound on loop() latency — the actual air time is what stays well inside a loop() tick
/// (~16-30 ms) — and either way it is dwarfed by the 1-3 s a blocking exchange already costs
/// `loop()`.
static constexpr uint32_t SOFT_PHY_IDLE_RX_COMPLETION_BUDGET_MS = 20;

static_assert(SOFT_PHY_IDLE_RX_COMPLETION_BUDGET_MS >= SOFT_PHY_EARLY_MIN_WINDOW_MS,
              "a budget below the minimum window makes try_early_completion_() decline every "
              "idle-path call, silently turning issue #81's fix into a no-op");

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

// RX_HOP_HOLDOFF_US (radio_interface.h) exists to outlast the fixed-length RX_DONE on the
// software-PHY chips. Tied here, in the one header that can see both sides of the arithmetic,
// so a change to either constant that breaks the relationship fails the build instead of
// silently turning issue #81's gate back into the bug it fixes.
static_assert(RX_HOP_HOLDOFF_US >= soft_phy_air_time_us(SOFT_PHY_RX_PROBE_PACKET_LEN),
              "the hop holdoff must outlast the fixed-length RX_DONE it exists to wait for — a "
              "shorter bound lets the hop fire while the frame it is protecting is still on air, "
              "silently turning issue #81's gate back into the bug");

/// @brief Shared RX/TX driver flow for the software-PHY radios (SX1262, LR1121).
/// @ingroup hioc_radio
class SoftPhyDriverBase : public RadioDriver {
 public:
  /// @param rst_pin Active-low hardware reset pin, forwarded to RadioDriver.
  /// @param busy_pin BUSY line polled by @ref wait_busy_ before every SPI transaction — both
  ///   concrete drivers are opcode-based chips that require this, unlike the register-based
  ///   SX1276.
  /// @param busy_timeout_ms How long @ref wait_busy_ waits for BUSY to drop before declaring the
  ///   chip failed. Chip-specific (SX1262: 10 ms: RC-oscillator timing; LR1121: 3000 ms, matched
  ///   to RadioLib's post-reset boot-ROM wait) — this class has no opinion on the value, only on
  ///   where it's stored and how it's used.
  /// @param default_response_preamble Chip-specific default for @ref response_preamble (each
  ///   concrete driver passes its own validated constant — this class has no opinion on the
  ///   value, only on where it's stored).
  /// @param default_post_tx_settle_us Chip-specific default post-TX settling delay, same rationale.
  SoftPhyDriverBase(InternalGPIOPin *rst_pin, InternalGPIOPin *busy_pin, uint32_t busy_timeout_ms,
                    uint16_t default_response_preamble, uint16_t default_post_tx_settle_us)
      : RadioDriver(rst_pin),
        busy_pin_(busy_pin),
        response_preamble_(default_response_preamble),
        post_tx_settle_us_(default_post_tx_settle_us),
        busy_timeout_ms_(busy_timeout_ms) {}

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
  ///
  /// Consults the private `preamble_latched_at_timeout_` flag first; see that member's declaration
  /// for why.
  bool is_preamble_detected() override;
  /// @brief Preamble for response/continuation frames — shared storage, see the concrete
  /// drivers' constructors/tuning defaults for the chip-specific rationale and value.
  [[nodiscard]] uint16_t response_preamble() const override { return this->response_preamble_; }
  /// @copydoc RadioDriver::is_failed
  ///
  /// Shared storage: both concrete drivers only ever set @ref failed_ from within their own
  /// SPI/opcode helpers (a BUSY timeout, a device-identity mismatch, ...), so there is nothing
  /// chip-specific left in the accessor itself.
  [[nodiscard]] bool is_failed() const override { return this->failed_; }

 protected:
  // --- Tuning helpers shared by both drivers (values/defaults stay chip-specific) ---
  /// Set the preamble length used for response/continuation frames within an exchange.
  void set_response_preamble_(uint16_t preamble) { this->response_preamble_ = preamble; }
  /// Set the delay between TX completion and re-entering RX.
  void set_post_tx_settle_us_(uint16_t delay_us) { this->post_tx_settle_us_ = delay_us; }
  /// @brief Wait until @ref busy_pin_ reads low, feeding the watchdog while polling.
  ///
  /// Shared verbatim between SX1262 and LR1121 — the two chips differ only in how long they're
  /// willing to wait (`busy_timeout_ms_`) and, prior to this refactor, differed by accident in
  /// whether a call short-circuits once the driver already latched `failed_`: LR1121 had the
  /// guard (needed at its 3000 ms timeout — without it, every remaining `configure_radio_()` step
  /// after the first failure would re-run the full timeout, turning one bad boot into tens of
  /// seconds of hang), SX1262 didn't (harmless at its 10 ms timeout, but still the same bug
  /// shape). Unified here means both chips get the guard now.
  void wait_busy_();
  /// Set on a BUSY timeout or a chip-identity check failing; see @ref is_failed.
  bool failed_{false};
  /// BUSY line, read directly by both concrete drivers' own `dump_debug()` in addition to
  /// @ref wait_busy_, so this stays protected rather than folding entirely into the private
  /// wait-loop state below.
  InternalGPIOPin *busy_pin_;

  // --- Shared RX/TX orchestration (moved verbatim from RadioSX1262/RadioLR1121) ---
  /// Read a received packet from the buffer and return the raw bytes reported by the chip.
  /// Virtual to allow test doubles (both concrete drivers' tests override this).
  virtual bool read_rx_packet(RadioRxPacket &packet, bool blocking_wait, uint32_t irq_status);
  /// Reset RX state machine and buffer. Optionally force standby first.
  void reset_rx_state_(bool force_standby = true);
  /// Minimal path back into RX immediately after a transmission — see the definition for why this
  /// is deliberately not @ref reset_rx_state_.
  void rearm_rx_after_tx_();

  /// @brief The GFSK SetPacketParams fields both software-PHY chips program identically.
  ///
  /// Both drivers build the same nine-byte SetPacketParams payload in the same order; only the
  /// preamble-detector length and the sync-word selector are chip constants, and only the opcode
  /// and transport differ. Everything else — the field order and the byte/bit preamble conversion
  /// that the `63e2502` fix had to patch in two places — lives in @ref build_gfsk_packet_params.
  struct SoftPhyPacketParams {
    uint16_t preamble_bytes;    ///< Byte-denominated, like every other preamble value in this codebase.
    uint8_t preamble_detector;  ///< Chip-specific detector-length selector.
    uint8_t sync_word_param;    ///< Chip-specific 24-bit sync-word selector.
    uint8_t packet_type;        ///< Known-length / fixed-length selector.
    uint8_t payload_len;        ///< Configured payload length.
    uint8_t crc_type;           ///< Chip-specific CRC-mode selector.
  };
  /// Byte count of the GFSK SetPacketParams payload — identical on both software-PHY chips.
  static constexpr uint8_t GFSK_PACKET_PARAMS_LEN = 9;
  /// @brief Fill the nine-byte GFSK SetPacketParams payload shared by both chips.
  ///
  /// Owns the field order and the ×8 preamble conversion: the SetPacketParams preamble field is
  /// bit-denominated on both chips (Semtech names it `preamble_len_in_bits` /
  /// `pbl_len_in_bit`), but every caller in this codebase passes a byte count. Address comparison
  /// and whitening are always off.
  /// @param p   The chip-independent field values (byte-denominated preamble, chip constants).
  /// @param out Buffer for exactly @ref GFSK_PACKET_PARAMS_LEN bytes.
  ///
  /// Named without the trailing `_` the file's other protected helpers carry (the plan asked for
  /// `build_gfsk_packet_params_`): it uses no instance state, so clang-tidy's
  /// `readability-convert-member-functions-to-static` wants it `static`, and `.clang-tidy`'s
  /// `ClassMethodCase = lower_case` then rejects a trailing `_` on a `static` method. Leaving it
  /// `static` and dropping the underscore keeps clang-tidy quiet without a suppression.
  static void build_gfsk_packet_params(const SoftPhyPacketParams &p, uint8_t out[GFSK_PACKET_PARAMS_LEN]);

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
  /// Default is "any bit" — safe only for a driver whose `SetDioIrqParams`-equivalent mask never
  /// includes `PreambleDetected` in the first place, so a preamble-only reading can never reach
  /// this check. Neither current driver qualifies: both SX1262 and LR1121 unmask
  /// `PreambleDetected` (each for its own reason) and override this to exclude it — a
  /// preamble-only reading means a frame may still be arriving, and treating it as terminal
  /// activity would tear down RX mid-reception.
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
  /// always starts. LR1121 also opts in, at the base of its one shared TX/RX buffer — see
  /// RadioLR1121::early_rx_read_offset for why a wrong guess there is safe only in combination
  /// with @ref invalidate_stale_rx_content_after_tx.
  [[nodiscard]] virtual int16_t early_rx_read_offset() const { return -1; }

  /// @brief Blocking budget for the idle-path length-driven receive, in milliseconds (issue #81).
  ///
  /// Virtual only so tests can widen it. The host clock stubs advance `millis()`/`micros()` by one
  /// unit per call (`tests/include/esphome/core/hal.h`), so a frame's few thousand microseconds of
  /// air time also burns a few thousand fake milliseconds — any production-sized budget expires
  /// inside `wait_for_air_time()`'s first stage and the whole path becomes untestable. The existing
  /// blocking-path early-completion tests dodge this by passing `wait_for_packet()` a 20000 ms
  /// timeout; this path has no caller-supplied timeout to widen, so the seam has to live here.
  [[nodiscard]] virtual uint32_t idle_rx_completion_budget_ms() const { return SOFT_PHY_IDLE_RX_COMPLETION_BUDGET_MS; }

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
  /// @brief Hook run from @ref rearm_rx_after_tx_, after a transmission and before re-entering RX.
  /// No-op by default.
  ///
  /// SX1262 has a real address split written once at init (@ref configure_buffer_base): TX always
  /// builds at its own base, RX always lands at a different one, so nothing a transmission wrote
  /// can ever appear where a length-driven receive (@ref early_rx_read_offset) later reads from.
  /// LR1121 has no such split — one shared 256-byte buffer for both directions, and `WriteBuffer`
  /// always starts from the buffer base (see `write_buffer_`'s doc comment) — so after every LR1121
  /// transmission, the offset a length-driven receive will read from holds a real, CRC-valid,
  /// UART-encoded copy of the hub's own last-sent frame. That is not noise: if
  /// `early_rx_read_offset()`'s offset guess ever turns out to be wrong, reading that residue back
  /// would pass every stage of `try_early_completion_()` and hand back the hub's own transmission as
  /// a phantom received packet — worse than doing nothing, since `check_for_packet()` would then
  /// tear down and re-arm RX (issue #81's `force_standby` path) over whatever real reception was
  /// actually in progress. RadioLR1121 overrides this to overwrite that offset with a few
  /// non-frame-shaped bytes after every TX, so a wrong offset guess degrades back to reading genuine
  /// garbage — which correctly fails the length-driven receive's stage 1 or stage 3 — instead of a
  /// valid-looking phantom frame.
  virtual void invalidate_stale_rx_content_after_tx() {}

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
  uint32_t busy_timeout_ms_;

  /// @brief Whether `PreambleDetected` was latched at the exact moment a dwell's
  /// `poll_until_activity_()` gave up waiting, captured before `reset_rx_state_()` wipes the IRQ
  /// word out from under it.
  ///
  /// `PreambleDetected` is deliberately excluded from @ref activity_irq_mask (a bare preamble must
  /// not look like terminal activity), and that exclusion is exactly why a dwell *can* time out
  /// while the bit is still latched — the timeout branch has no way to notice, since
  /// activity_irq_mask() never told it to look. What happens next is a separate problem:
  /// `reset_rx_state_()` runs immediately afterward and clears the whole IRQ word, so a live
  /// re-read a few tens of microseconds later (in `listen()`'s `preamble_or_sync_incoming()` guard)
  /// would need the chip to re-observe a fresh preamble from scratch — which takes a little longer
  /// on a chip with a longer preamble-detector length — and it almost never has done so by the time
  /// the guard asks. This flag is the only way that information survives the reset long enough for
  /// the guard to read it.
  ///
  /// `is_sync_detected()` needs no equivalent snapshot — not because no path clears sync before a
  /// timeout (`resolve_sync_race_()` does exactly that: it clears `SYNC_WORD_VALID` up front once a
  /// reception is underway, and can still time out later waiting for RX_DONE), but because that
  /// path never calls `reset_rx_state_()` on its way out. Whatever `PreambleDetected` state existed
  /// going into it survives untouched, which is all the guard actually depends on there.
  ///
  /// Lifetime is bounded to a single `poll_until_activity_()` call so a stale detection can never
  /// suppress a later, unrelated hop: every exit from that function's loop sets this flag from
  /// scratch (true only on the timeout branch, and only when the bit was actually set on that exact
  /// read), and @ref is_preamble_detected consumes (clears) it the moment it is read as true.
  bool preamble_latched_at_timeout_{false};
};

}  // namespace home_io_control
}  // namespace esphome
