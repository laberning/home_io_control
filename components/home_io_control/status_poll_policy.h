#pragma once

/// @file status_poll_policy.h
/// @brief Per-device poll scheduling, failure backoff, and follow-up-poll state machine.
/// @ingroup hioc_hub
///
/// Owns all per-device poll bookkeeping (intervals, deadlines, failure counters).
/// Pure logic with injected timestamps — fully host-testable without a clock mock.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace esphome {
namespace home_io_control {

/// @name Background-poll retry delays after silent failures (no reply received)
/// @{
static constexpr uint32_t STATUS_RETRY_AFTER_FAIL_MS = 5000;         ///< First retry after a silent failure.
static constexpr uint32_t STATUS_RETRY_AFTER_FAIL_STEP2_MS = 15000;  ///< Second retry after a silent failure.
static constexpr uint32_t STATUS_RETRY_AFTER_FAIL_STEP3_MS = 30000;  ///< Third retry after a silent failure.
static constexpr uint32_t STATUS_RETRY_AFTER_FAIL_STEP4_MS = 60000;  ///< Fourth retry after a silent failure.
static constexpr uint32_t STATUS_RETRY_AFTER_FAIL_MAX_MS =
    300000;  ///< Steady-state backoff after many silent failures.
/// @}

/// @name Background-poll retry delays after auth-shaped failures (0x3C challenge seen)
/// @{
static constexpr uint32_t STATUS_AUTH_RETRY_AFTER_FAIL_MS = 30000;  ///< First retry after a challenge-seen failure.
static constexpr uint32_t STATUS_AUTH_RETRY_AFTER_FAIL_STEP2_MS = 120000;  ///< Second retry.
static constexpr uint32_t STATUS_AUTH_RETRY_AFTER_FAIL_MAX_MS = 300000;  ///< Steady-state after repeated auth failures.
/// @}

/// Hard stop for bounded follow-up polling after a command or remote activity.
static constexpr uint32_t MAX_TRACKED_STATUS_POLL_WINDOW_MS = 600000;
/// Delay before the first post-boot status request from an entity.
static constexpr uint32_t INITIAL_STATUS_REQUEST_DELAY_MS = 5000;
/// Delay before polling after overheard remote traffic.
static constexpr uint32_t REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS = 2000;
/// Hold queued background polls back for this long after any 1W frame, so a poll the hub itself
/// scheduled does not occupy the (half-duplex) radio while the remote is still transmitting.
/// Comfortably longer than the ~160ms reliability burst; short enough to barely shift poll timing
/// otherwise. Only background polls yield — see decisions::defer_background_poll_for_1w_activity().
static constexpr uint32_t ONEWAY_QUIET_PERIOD_MS = 700;
/// Hard cap on how long sustained 1W traffic may hold a background poll back in total, measured
/// from the start of the burst rather than the most recent frame. ONEWAY_QUIET_PERIOD_MS re-arms on
/// every frame, so without this cap traffic arriving faster than the quiet period apart — a stuck
/// remote, a chatty neighbour's sensor — could starve background polls, and therefore device state
/// freshness, indefinitely. Comfortably above a legitimate ~160ms burst; bounds the worst case
/// without meaningfully affecting normal operation.
static constexpr uint32_t ONEWAY_POLL_DEFER_CAP_MS = 5000;

/// Default follow-up settle-poll delay used while a device may still be moving, when no explicit
/// poll interval is configured (interval_ms == 0). Short on purpose so hint-less devices are
/// re-checked promptly and the motion-tracking loop terminates soon after the device comes to rest.
static constexpr uint32_t DEFAULT_SETTLE_POLL_DELAY_MS = 3000;
/// Upper bound on the settle-poll delay after a STOP command. STOP should confirm the resting
/// position quickly, so it always settles faster than a normal move, regardless of the configured
/// interval or device hint.
static constexpr uint32_t STOP_SETTLE_POLL_CAP_MS = 1000;

/// @brief Resolve the follow-up settle-poll delay while a device may still be moving.
///
/// This is the single source of truth for the motion-tracking cadence. The delay is the configured
/// @p interval_ms, or DEFAULT_SETTLE_POLL_DELAY_MS when no interval is configured (interval_ms == 0).
/// A device-provided byte-7 hint (@p hint_delay_ms, 0 when absent) and a STOP command
/// (@p cap_for_stop) may only shorten the delay, never lengthen it.
///
/// Note: this resolves the *settle* delay only — it is intentionally independent of whether a
/// device keeps polling after coming to rest. Periodic monitoring after rest remains opt-in and is
/// gated on interval_ms != 0 by the command paths, so defaulting the settle delay here does not turn
/// every device into a periodic poller.
///
/// @return Settle delay in milliseconds (always non-zero).
[[nodiscard]] inline uint32_t settle_delay_ms(uint32_t interval_ms, uint32_t hint_delay_ms, bool cap_for_stop) {
  uint32_t delay = (interval_ms != 0) ? interval_ms : DEFAULT_SETTLE_POLL_DELAY_MS;
  if (hint_delay_ms != 0)
    delay = std::min(delay, hint_delay_ms);
  if (cap_for_stop)
    delay = std::min(delay, STOP_SETTLE_POLL_CAP_MS);
  return delay;
}

/// @brief Per-device poll scheduling state.
struct PollTracking {
  uint32_t interval_ms{0};    ///< Configured follow-up poll interval (0 = no configured cadence; delays fall back to
                              ///< device hint / default settle delay).
  uint32_t next_update{0};    ///< Absolute millis() timestamp for the next poll; 0 = idle.
  uint32_t poll_deadline{0};  ///< Hard stop for bounded follow-up polling.
  uint8_t status_poll_failures{0};  ///< Consecutive background poll failures (silent — no reply).
  uint8_t auth_poll_failures{0};    ///< Consecutive background poll failures that saw a 0x3C challenge.
};

/// @brief Per-hub poll scheduling and failure-backoff policy.
///
/// Maintains a PollTracking entry per device; all hub poll bookkeeping lives
/// here and nowhere else. Callers inject 'now' timestamps so the class is
/// fully testable without a hardware clock.
class StatusPollPolicy {
 public:
  // --- Interval configuration ---
  /// Set the configured follow-up poll interval (ms; 0 = legacy one-shot settle only).
  void set_interval(const std::string &device_id, uint32_t interval_ms);
  /// Return the configured interval (0 when not set or unconfigured).
  [[nodiscard]] uint32_t get_interval(const std::string &device_id) const;

  // --- Tracking lifecycle ---
  /// Begin bounded follow-up polling after a command or remote activity.
  /// Sets a deadline regardless of whether a configured interval exists; interval-0 devices use
  /// device-hinted or default settle delays computed at response time.
  void begin_tracking(const std::string &device_id, uint32_t initial_delay_ms, uint32_t now);
  /// Stop all polling for a device and reset failure counts. Preserves interval_ms.
  void clear(const std::string &device_id);

  // --- Next-update scheduling ---
  /// Schedule the next poll at an absolute millis() timestamp.
  void set_next_update(const std::string &device_id, uint32_t abs_time);
  [[nodiscard]] uint32_t get_next_update(const std::string &device_id) const;

  // --- Failure tracking ---
  /// Record a failed background poll; apply backoff or clear tracking if the window expired.
  /// Returns the backoff delay applied in ms, or 0 if tracking was cleared.
  uint32_t on_exchange_failed(const std::string &device_id, bool auth_like, uint32_t now);
  /// Reset failure streaks after a successful exchange response.
  void clear_failure_streaks(const std::string &device_id);
  [[nodiscard]] uint8_t get_status_poll_failures(const std::string &device_id) const;
  [[nodiscard]] uint8_t get_auth_poll_failures(const std::string &device_id) const;
  [[nodiscard]] uint32_t get_poll_deadline(const std::string &device_id) const;

  // --- Queries ---
  /// True when the device has an active bounded polling window (deadline set and not expired).
  [[nodiscard]] bool is_tracking_active(const std::string &device_id, uint32_t now) const;

  // --- Due-device scan ---
  /// Return and consume the first device whose next_update is overdue.
  /// Clears expired tracking entries as a side effect. Returns nullopt if nothing is due.
  [[nodiscard]] std::optional<std::string> pop_due_device(uint32_t now);

 private:
  std::map<std::string, PollTracking> tracking_;

  /// Compute retry delay from failure count and failure class.
  static uint32_t retry_delay_ms(uint8_t consecutive_failures, bool auth_like);
};

}  // namespace home_io_control
}  // namespace esphome
