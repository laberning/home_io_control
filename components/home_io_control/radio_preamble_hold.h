#pragma once

/// @file radio_preamble_hold.h
/// @brief The idle receiver's preamble hold: bounded slices that renew on the same channel.
/// @ingroup hioc_radio
///
/// A start preamble is on air for a while before its sync word, and the idle hop would leave the
/// channel inside it. A driver therefore holds the hop on the channel from the first sighting of a
/// preamble. Two things bound the hold, and they pull in opposite directions:
///
/// - A detection with no frame behind it (noise, a strong transmitter on the adjacent channel)
///   must not park the receiver for long, so a hold lasts one short slice.
/// - A wake-up preamble is far longer than any slice (at least WAKE_PREAMBLE_MIN_MS), so when a slice ends the
///   receiver stays on the channel anyway if the preamble is still on air: the driver restarts
///   RX there, which discards the stale detection, and keeps holding if the preamble is detected
///   again within a few milliseconds. Leaving instead puts the receiver away for several dwells,
///   and a preamble of fixed length puts its sync word at the same phase of that cycle on every
///   copy, so a chip that is away at that phase loses all of them.
///
/// No renew starts once the run of holds on one channel reaches a chain limit
/// (IDLE_PREAMBLE_HOLD_CHAIN_MS), so a detector that keeps firing on noise costs the scan about that long (the
/// limit plus at most one re-detection wait and one slice).
/// PreambleHold is that state machine and nothing else: it reads no clock and touches no chip. The
/// driver passes the time, the chip's "preamble seen" reading and its RX restart, so one rule serves
/// every driver and is tested without a radio.

#include "proto_timing.h"

#include <cstdint>

namespace esphome {
namespace home_io_control {

/// Bound on a run of renewed idle preamble holds on one channel, in milliseconds: no renew starts
/// once the run is this old. It covers a preamble up to the wake-up threshold, WAKE_PREAMBLE_MIN_MS
/// (checked below), the same duration on every radio. A longer wake-up outlasts the run; the hop
/// then leaves the channel and holds again when it next finds the preamble there. A detector that
/// keeps firing on noise costs the idle scan at most this plus the last renew's re-detection wait
/// and slice per channel visit.
/// \todo The default wake-up (LOW_POWER_WAKE_MS) and a remote's wake-up of about 600 ms outlast this
/// run; whether a longer run catches their frames more often needs a bench run.
static constexpr uint32_t IDLE_PREAMBLE_HOLD_CHAIN_MS = 300;
static_assert(static_cast<float>(IDLE_PREAMBLE_HOLD_CHAIN_MS) >= WAKE_PREAMBLE_MIN_MS,
              "the hold chain must cover a preamble up to the wake-up threshold");

/// @brief The three durations of a PreambleHold, in milliseconds.
struct PreambleHoldTiming {
  uint32_t slice_ms{0};     ///< How long one hold lasts from the first sighting of a preamble.
  uint32_t redetect_ms{0};  ///< After a renew, how long to wait for the preamble to be detected again.
  uint32_t chain_ms{0};     ///< No renew starts once the run of holds on one channel is this old.
};

/// @brief Preamble hold state with optional renewal (see the file comment).
///
/// All times are `millis()` values; differences are taken with unsigned arithmetic so a clock wrap
/// inside a hold is harmless. The two entry points differ only in what happens when a slice ends:
/// hold() lets the caller's wait end there, hold_renewing() renews.
class PreambleHold {
 public:
  explicit constexpr PreambleHold(PreambleHoldTiming timing) : timing_(timing) {}

  /// True while a hold has started and not been released, also after its slice has run out.
  [[nodiscard]] bool active() const { return this->active_; }

  /// @brief Forget the hold and any renewal in progress.
  ///
  /// For every event that makes the old detection meaningless: a retune, an RX reset or re-arm, a
  /// frame arriving.
  void release() {
    this->active_ = false;
    this->awaiting_redetect_ = false;
  }

  /// @brief Hold without renewing, for a wait that ends at the slice.
  /// @param now_ms Current `millis()`.
  /// @param preamble_seen The chip's current reading: a preamble is detected.
  /// @return True to keep RX running on this channel; false once the slice is over (the hold stays
  /// active until release(), so the caller's RX reset ends it) or when nothing is held.
  bool hold(uint32_t now_ms, bool preamble_seen) {
    if (!preamble_seen)
      return false;
    if (!this->active_) {
      this->begin_(now_ms);
      return true;
    }
    return this->in_slice_(now_ms);
  }

  /// @brief Hold for the idle hop: renew the hold instead of letting it end while a preamble may
  /// still be on air.
  ///
  /// A running slice answers without calling anything. When it ends inside the chain bound,
  /// `restart_rx` runs (it must retune to the same channel, restart the receiver and call
  /// release(), which is what a retune does) and the hold then waits `redetect_ms` for
  /// `preamble_seen` to turn true again, which starts the next slice of the same chain. Past the
  /// chain bound it answers false and stays expired until release().
  /// @param now_ms Current `millis()`.
  /// @param preamble_seen Callable returning the chip's current preamble reading; called only
  /// while no slice is running, because reading may cost an SPI transfer.
  /// @param restart_rx Callable restarting RX on the current channel.
  /// @return True while the hop must stay on this channel.
  template<typename Seen, typename Restart>
  bool hold_renewing(uint32_t now_ms, Seen preamble_seen, Restart restart_rx) {
    if (this->active_) {
      if (this->in_slice_(now_ms))
        return true;
      if (now_ms - this->chain_start_ms_ >= this->timing_.chain_ms)
        return false;
      restart_rx();  // clears the hold with the retune; the wait below replaces it
      this->awaiting_redetect_ = true;
      this->redetect_since_ms_ = now_ms;
      return true;
    }
    if (preamble_seen()) {
      this->begin_(now_ms);
      return true;
    }
    if (this->awaiting_redetect_ && now_ms - this->redetect_since_ms_ < this->timing_.redetect_ms)
      return true;
    this->awaiting_redetect_ = false;
    return false;
  }

 private:
  /// A preamble is seen with no hold running: start a slice. After a renew it continues the chain,
  /// otherwise it starts one.
  void begin_(uint32_t now_ms) {
    this->active_ = true;
    this->slice_start_ms_ = now_ms;
    if (!this->awaiting_redetect_)
      this->chain_start_ms_ = now_ms;
    this->awaiting_redetect_ = false;
  }
  [[nodiscard]] bool in_slice_(uint32_t now_ms) const {
    return now_ms - this->slice_start_ms_ < this->timing_.slice_ms;
  }

  PreambleHoldTiming timing_;
  uint32_t slice_start_ms_{0};     ///< When the running slice began; valid while `active_`.
  uint32_t chain_start_ms_{0};     ///< When the first slice of the current run began.
  uint32_t redetect_since_ms_{0};  ///< When the last renew restarted RX; valid while awaiting.
  bool active_{false};             ///< A slice has started and not been released.
  bool awaiting_redetect_{false};  ///< A renew restarted RX and the preamble has not been seen again yet.
};

}  // namespace home_io_control
}  // namespace esphome
