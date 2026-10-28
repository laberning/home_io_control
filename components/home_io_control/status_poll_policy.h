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
#include <vector>

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
///
/// A device that challenges the poll is awake and heard it. A single such miss is usually a
/// closing reply lost while the device switched its load — on a cover, the motor stopping at the
/// end of its travel (issue #178) — so the first retry waits no longer than after a silent miss.
/// Only a repeated one, which is what a wrong key looks like, backs off far: there every further
/// try is a full authenticated exchange that cannot succeed.
/// @{
static constexpr uint32_t STATUS_AUTH_RETRY_AFTER_FAIL_MS =
    STATUS_RETRY_AFTER_FAIL_MS;  ///< First retry after a challenge-seen failure.
static constexpr uint32_t STATUS_AUTH_RETRY_AFTER_FAIL_STEP2_MS = 30000;   ///< Second retry.
static constexpr uint32_t STATUS_AUTH_RETRY_AFTER_FAIL_STEP3_MS = 120000;  ///< Third retry.
static constexpr uint32_t STATUS_AUTH_RETRY_AFTER_FAIL_MAX_MS = 300000;  ///< Steady-state after repeated auth failures.
/// @}

/// Consecutive failed polls of a moving device that are skipped rather than backed off.
///
/// A poll while a device is believed travelling is a single try (SCHEDULED_POLL_MAX_TRIES), so one
/// lost frame fails it. Waiting the ladders' first step only leaves a gap in the position updates
/// (issue #178). The first misses in a row keep the normal cadence instead (never slower than the
/// ladder's first step); only the next one enters the ladder, which still catches a device that is
/// really gone or answers with the wrong key, two polls later. The skipped misses do not count as
/// ladder failures, so the next poll stays a single try.
static constexpr uint8_t MOVING_POLL_MISSES_SKIPPED = 2;

/// Hard stop for bounded follow-up polling after a command or remote activity.
static constexpr uint32_t MAX_TRACKED_STATUS_POLL_WINDOW_MS = 600000;
/// Delay before the first post-boot status request from an entity.
static constexpr uint32_t INITIAL_STATUS_REQUEST_DELAY_MS = 5000;
/// Interval between regular polls of a heater (its `status_poll_interval` overrides it). Slow on
/// purpose: a heater's state moves over minutes, and a poll costs one exchange per register.
static constexpr uint32_t HEATING_POLL_DEFAULT_INTERVAL_MS = 300000;

/// @brief Delay of the extra heater poll after a register write the heater acknowledged.
///
/// The write is read back at once, but whether the heater now heats shows only in its regulator
/// register, which the regular poll reads. Without this extra poll the card's heating / idle trails a
/// setpoint change by up to one poll interval, and longer when a poll fails (issue #165). The
/// regular cadence restarts from this poll.
static constexpr uint32_t HEATING_POST_WRITE_POLL_DELAY_MS = 45000;
/// Delay before polling after overheard remote traffic.
static constexpr uint32_t REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS = 2000;
/// How long a device's authenticated status or this hub's own read-triggered poll keeps a further
/// overheard read from triggering another poll (0 = never polled, so the first read polls). Covers
/// have no background poll, so a read by another controller is the cue that something the hub did not
/// hear (a rain closure, a local button) may have changed.
static constexpr uint32_t FOREIGN_READ_RESYNC_MS = 600000;
/// A device challenges a request 2.5-3.3 ms after it; 100 ms is far above that and far below the next
/// request. A challenge within this window of a heard request from the same controller answers that
/// request; a challenge with no such request means the request frame was missed.
static constexpr uint32_t FOREIGN_CHALLENGE_AFTER_REQUEST_MS = 100;
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
/// poll interval is configured (interval_ms == 0). Short on purpose so devices without a remaining time are
/// re-checked promptly and the motion-tracking loop terminates soon after the device comes to rest.
static constexpr uint32_t DEFAULT_SETTLE_POLL_DELAY_MS = 3000;
/// Upper bound on the settle-poll delay after a STOP command. STOP should confirm the resting
/// position quickly, so it always settles faster than a normal move, regardless of the configured
/// interval or remaining time.
static constexpr uint32_t STOP_SETTLE_POLL_CAP_MS = 1000;

/// @brief Resolve the follow-up settle-poll delay while a device may still be moving.
///
/// This is the single source of truth for the motion-tracking cadence. The delay is the configured
/// @p interval_ms, or DEFAULT_SETTLE_POLL_DELAY_MS when no interval is configured (interval_ms == 0).
/// The device's remaining travel time plus a margin (@p remaining_delay_ms, 0 when absent) and a STOP command
/// (@p cap_for_stop) may only shorten the delay, never lengthen it.
///
/// Note: this resolves the *settle* delay only — it is intentionally independent of whether a
/// device keeps polling after coming to rest. Periodic monitoring after rest remains opt-in and is
/// gated on interval_ms != 0 by the command paths, so defaulting the settle delay here does not turn
/// every device into a periodic poller.
///
/// @return Settle delay in milliseconds (always non-zero).
[[nodiscard]] inline uint32_t settle_delay_ms(uint32_t interval_ms, uint32_t remaining_delay_ms, bool cap_for_stop) {
  uint32_t delay = (interval_ms != 0) ? interval_ms : DEFAULT_SETTLE_POLL_DELAY_MS;
  if (remaining_delay_ms != 0)
    delay = std::min(delay, remaining_delay_ms);
  if (cap_for_stop)
    delay = std::min(delay, STOP_SETTLE_POLL_CAP_MS);
  return delay;
}

/// @brief Per-device poll scheduling state.
struct PollTracking {
  uint32_t interval_ms{0};    ///< Configured follow-up poll interval (0 = no configured cadence; delays fall back to
                              ///< remaining time / default settle delay).
  uint32_t next_update{0};    ///< Absolute millis() timestamp for the next poll; 0 = idle.
  uint32_t poll_deadline{0};  ///< Hard stop for bounded follow-up polling.
  uint8_t status_poll_failures{0};  ///< Consecutive background poll failures (silent — no reply).
  uint8_t auth_poll_failures{0};    ///< Consecutive background poll failures that saw a 0x3C challenge.
  uint8_t moving_poll_misses{0};    ///< Consecutive failed polls of a moving device skipped so far (see
                                    ///< MOVING_POLL_MISSES_SKIPPED); not counted in the two streaks above.
  bool stop_settle{false};          ///< The next poll confirms an accepted STOP (see STOP_SETTLE_POLL_TRIES).
};

/// @brief What an overheard frame of another controller is.
enum class ForeignFrameKind : uint8_t {
  REQUEST_TO_DEVICE,         ///< Another controller's command to a registered device.
  DEVICE_CHALLENGE_TO_OTHER  ///< A registered device's challenge (0x3C) addressed to another controller.
};

/// @brief What the hub does about an overheard foreign frame.
enum class ForeignAction : uint8_t {
  IGNORE,        ///< Nothing: no poll, no tracking change.
  ARM,           ///< Schedule a poll REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS from now and begin tracking.
  PUSH_PENDING,  ///< A poll armed by foreign traffic is pending: move its timer out, change nothing else.
};

/// @brief Per-device bookkeeping of overheard foreign traffic.
///
/// Kept apart from PollTracking: it is not part of the polling window's lifecycle, so
/// begin_tracking() and clear() never reset it. Timestamps are millis() values, 0 = never; all
/// comparisons use unsigned `now - t`, which stays correct across a millis() wrap.
struct ForeignTraffic {
  uint8_t last_request_cmd{0};    ///< Command of the last request heard to this device.
  std::string last_request_from;  ///< Controller that sent it (frame source); empty = none heard.
  uint32_t last_request_ms{0};    ///< When it was heard; 0 = never.
  uint32_t last_read_poll_ms{0};  ///< When a read last armed a poll; 0 = never.
  uint32_t pending_due_ms{0};     ///< When the armed poll is due; 0 = none pending.
};

/// @brief What the hub does with a status report that reads as moving.
enum class MovingReportAction : uint8_t {
  FOLLOW,  ///< A window is open: schedule the next poll as usual.
  OPEN,    ///< No window: one was opened now; schedule the next poll as usual.
  END,     ///< The window ran out: it is closed and reported ended; no further poll.
  NONE,    ///< No window and none may be opened: no poll.
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
  /// remaining-time or default settle delays computed at response time.
  void begin_tracking(const std::string &device_id, uint32_t initial_delay_ms, uint32_t now);
  /// Stop all polling for a device and reset failure counts. Preserves interval_ms.
  void clear(const std::string &device_id);

  /// Decide what a status report that reads as moving means for the follow-up window.
  ///
  /// An open window is followed and its deadline stays: a later report never extends it. A window
  /// whose deadline has passed is closed and reported ended (see pop_ended_device()); the report
  /// that finds it expired does not reopen it, so a poll the hub scheduled itself cannot chain
  /// windows. Without a window, the report opens one at @p now when @p may_open allows it.
  /// @param device_id Device the report belongs to.
  /// @param now Current millis().
  /// @param may_open True when this kind of device may start tracking from a report alone.
  MovingReportAction on_moving_report(const std::string &device_id, uint32_t now, bool may_open);

  // --- Next-update scheduling ---
  /// Schedule the next poll at an absolute millis() timestamp.
  void set_next_update(const std::string &device_id, uint32_t abs_time);
  [[nodiscard]] uint32_t get_next_update(const std::string &device_id) const;

  // --- Failure tracking ---
  /// Record a failed background poll; apply backoff or clear tracking if the window expired.
  ///
  /// When @p device_moving is true, no failure streak is running and fewer than
  /// MOVING_POLL_MISSES_SKIPPED misses in a row have been skipped, the miss is skipped instead: the
  /// next poll keeps the normal cadence (settle_delay_ms() without a remaining time, at most
  /// STATUS_RETRY_AFTER_FAIL_MS) and the failure streaks are left alone.
  /// @param device_id Device whose poll failed.
  /// @param auth_like True when the failed exchange saw a 0x3C challenge.
  /// @param now Current millis().
  /// @param device_moving True when a single-try poll of a device believed travelling failed.
  /// @return The delay applied in ms, or 0 if tracking was cleared.
  uint32_t on_exchange_failed(const std::string &device_id, bool auth_like, uint32_t now, bool device_moving = false);
  /// Reset failure streaks (and skipped moving misses) after a successful exchange response.
  void clear_failure_streaks(const std::string &device_id);
  [[nodiscard]] uint8_t get_status_poll_failures(const std::string &device_id) const;
  [[nodiscard]] uint8_t get_auth_poll_failures(const std::string &device_id) const;
  /// Consecutive failed polls of a moving device skipped so far.
  [[nodiscard]] uint8_t get_moving_poll_misses(const std::string &device_id) const;
  [[nodiscard]] uint32_t get_poll_deadline(const std::string &device_id) const;

  // --- Post-STOP settle poll ---
  /// Mark the device's next poll as the one confirming an accepted STOP, so it gets
  /// STOP_SETTLE_POLL_TRIES. Only a tracked device can be marked; begin_tracking() and clear()
  /// drop the mark, so a newer command or remote activity never inherits it.
  void mark_stop_settle(const std::string &device_id);
  /// Consume the mark set by mark_stop_settle(): true once, for the first poll dispatched after it.
  [[nodiscard]] bool take_stop_settle(const std::string &device_id);

  // --- Queries ---
  /// True when the device has an active bounded polling window (deadline set and not expired).
  [[nodiscard]] bool is_tracking_active(const std::string &device_id, uint32_t now) const;

  // --- Due-device scan ---
  /// Return and consume the first device whose next_update is overdue.
  /// Clears expired tracking entries as a side effect. Returns nullopt if nothing is due.
  [[nodiscard]] std::optional<std::string> pop_due_device(uint32_t now);

  // --- Overheard foreign traffic ---
  /// Decide what an overheard frame of another controller means for this device.
  ///
  /// Order: a pending foreign-armed poll answers first (PUSH_PENDING for any frame, timer pushed
  /// out); a request that writes ARMs; any other request ARMs only when neither @p last_status_ms
  /// nor the last read-triggered poll is younger than FOREIGN_READ_RESYNC_MS (0 = never, so the first
  /// read polls); a challenge is IGNOREd when the last request heard from the same controller was
  /// within FOREIGN_CHALLENGE_AFTER_REQUEST_MS and not a write, otherwise it ARMs. Every request
  /// updates the last-request record; a challenge does not. ARM and PUSH_PENDING set the pending due
  /// time; an ARM from a read stamps the read-poll time.
  /// @param device_id Registered device the frame concerns.
  /// @param kind Request to the device, or the device's challenge to another controller.
  /// @param controller The other controller: the request's source, or the challenge's destination.
  /// @param cmd Command of the frame.
  /// @param now Current millis().
  /// @param last_status_ms millis() of the device's last authenticated status; 0 = never.
  ForeignAction on_foreign_frame(const std::string &device_id, ForeignFrameKind kind, const std::string &controller,
                                 uint8_t cmd, uint32_t now, uint32_t last_status_ms);
  /// The poll armed by foreign traffic ran (or was dropped): nothing is pending any more.
  void on_remote_poll_ran(const std::string &device_id);
  /// True while a poll armed by foreign traffic is pending. A pending poll whose signal was lost
  /// expires by itself once @p now is more than REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS past its due time.
  [[nodiscard]] bool is_remote_poll_pending(const std::string &device_id, uint32_t now) const;

  // --- Tracking that ran out ---
  /// Return and consume a device whose polling window ended without the device being reported at
  /// rest: the window expired with a poll still pending (pop_due_device()), or the failure ladder no
  /// longer fits in it (on_exchange_failed()). An explicit clear() — a stopped report, a failed
  /// build — is not "ran out" and is never returned. Returns nullopt when there is none.
  [[nodiscard]] std::optional<std::string> pop_ended_device();

 private:
  std::map<std::string, PollTracking> tracking_;
  std::map<std::string, ForeignTraffic> foreign_;  ///< Overheard-traffic state, outside the tracking lifecycle.
  std::vector<std::string> ended_;  ///< Devices whose window ran out, for pop_ended_device(); no duplicates.

  /// Record that a device's window ran out, once.
  void note_ended_(const std::string &device_id);

  /// Compute retry delay from failure count and failure class.
  static uint32_t retry_delay_ms(uint8_t consecutive_failures, bool auth_like);
};

}  // namespace home_io_control
}  // namespace esphome
