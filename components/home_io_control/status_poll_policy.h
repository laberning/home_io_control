#pragma once

/// @file status_poll_policy.h
/// @brief Per-device poll scheduling, failure backoff, and follow-up-poll state machine.
/// @ingroup hioc_hub
///
/// Owns all per-device poll bookkeeping (intervals, deadlines, failure counters).
/// Pure logic with injected timestamps — fully host-testable without a clock mock.

#include "proto_timing.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
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
/// Steady-state backoff after many silent failures. Longer than what is left of a follow-up window
/// (MAX_TRACKED_STATUS_POLL_WINDOW_MS) by the time it is reached, so inside a window this step ends
/// the window (StatusPollPolicy::on_exchange_failed()).
static constexpr uint32_t STATUS_RETRY_AFTER_FAIL_MAX_MS = 300000;
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
/// Steady-state after repeated auth failures. Like STATUS_RETRY_AFTER_FAIL_MAX_MS, it ends a follow-up
/// window rather than wait inside it.
static constexpr uint32_t STATUS_AUTH_RETRY_AFTER_FAIL_MAX_MS = 300000;
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

/// Hard stop for bounded follow-up polling after a command, remote activity or the first moving report,
/// measured from the moment the window opens (ADR 0061, amended by ADR 0079).
///
/// The longest cover travel measured is about 65 s, a VELUX roof window at its silent speed
/// (docs/configuration/cover.md, travel speeds). A cover queued behind another on a shared supply
/// runs after that one, so the worst case is two such travels, about 130 s: 240 s covers it with
/// almost twice the margin, and a cover that is still reporting moving after that is shown at rest.
static constexpr uint32_t MAX_TRACKED_STATUS_POLL_WINDOW_MS = 240000;
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
// REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS lives in proto_timing.h: it is also the default of the
// `remote_activity_poll_delay_ms` tuning parameter, whose header may not include this one.
/// How long a device's authenticated status or this hub's own read-triggered poll keeps a further
/// overheard read from triggering another poll (0 = never polled, so the first read polls). Covers
/// have no background poll unless they opt into `status_refresh_interval`, so a read by another
/// controller is the cue that something the hub did not hear (a rain closure, a local button) may have
/// changed.
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

/// @name Predicted-end read
///
/// A moving cover that reports how long it has left is read once more, with the settling plan, a
/// second after that time has run out: the position it reports then is the final one. That holds for
/// every count, a short one or 0 included: the read at the end is the one most likely to meet the
/// second in which the motor switches its load and a reply is lost. The ordinary progress reads keep
/// running in between and stay single tries.
/// The aim, the dropped progress read and the final-position read at count + 1 s are confirmed on a
/// Somfy Sunea io awning
/// (tests/corpus/captures/statuspoll/somfy_awning_statuspoll_close_with_remaining_time_predicted_end_read_sx1262.yaml).
/// \todo Experimental: the stale-count rule has not met a frozen count on hardware, and a
///       battery-powered cover's read at the end of its count has not run on one (ADR 0076).
/// @{
/// The predicted-end read is aimed this long after the remaining time has run out. The motor takes up
/// to about a second after its count reaches zero to stop and switch its load.
static constexpr uint32_t PREDICTED_END_MARGIN_MS = 1000;
/// A progress read due within this long before the predicted-end read is dropped: the settling read
/// follows at once and a read of its own in front of it only fills the air.
static constexpr uint32_t PREDICTED_END_DROPS_PROGRESS_WITHIN_MS = 1000;
/// Reports in a row whose remaining time did not fall below the previous one before the count is
/// taken as frozen and the predicted-end read is given up (progress reads only).
static constexpr uint8_t PREDICTED_END_STALE_REPORTS = 3;
/// @}

/// @name Frozen-count back-off
///
/// A count that has stopped falling is a cover that is waiting or stuck, not one that is nearly there,
/// so the progress reads that follow back off instead of returning to the 3 s cadence (ADR 0079,
/// amending ADR 0076). The steps grow with each further report whose count did not fall and stop at
/// the last; a falling count starts over. The back-off only begins after PREDICTED_END_STALE_REPORTS
/// reports in a row that did not fall, and the slowest falling counts seen drop at every poll (3 s
/// between two polls on a VELUX SML, 1 s per second on a Somfy awning), so a slow but alive count
/// is not taken as frozen. 10 s, 15 s and 20 s keep a cover that starts after waiting within 20 s of
/// being seen, while a stuck one costs a read per 20 s instead of one per 3 s.
/// \todo Experimental: no frozen count has been captured on hardware yet.
/// @{
static constexpr uint32_t FROZEN_COUNT_BACKOFF_FIRST_MS = 10000;   ///< Delay after the first frozen report.
static constexpr uint32_t FROZEN_COUNT_BACKOFF_SECOND_MS = 15000;  ///< Delay after the second.
static constexpr uint32_t FROZEN_COUNT_BACKOFF_MAX_MS = 20000;     ///< Delay after the third and every later one.
/// @}

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
  bool stop_settle{false};          ///< The next poll reads where an accepted STOP left the device (a settling read).
  bool confirm_stopped{false};      ///< The next poll confirms a stop off target (a settling read, see
                                    ///< decisions::StoppedReport::SETTLING).
  uint32_t settle_update{0};        ///< Absolute millis() timestamp of the predicted-end read; 0 = none planned.
  std::optional<uint16_t> last_remaining_s;  ///< Remaining time of the last report that was aimed at (> 2 s).
  uint8_t stale_reports{0};                  ///< Reports in a row whose remaining time did not fall.
  bool predicted_end_read{false};            ///< The next poll is the predicted-end read (a settling read).
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

  /// Aim or re-aim the predicted-end read from a trusted status report of a moving device.
  ///
  /// A remaining time, whatever its size (0 included), plans the read at @p now + remaining time +
  /// PREDICTED_END_MARGIN_MS, replacing an earlier aim; an aim past the end of the follow-up window is not planned. A
  /// count that did not fall since the last aimed report counts as stale; after PREDICTED_END_STALE_REPORTS in a row
  /// the read is given up and only progress reads remain (a falling count aims again). No count (@p remaining_s empty)
  /// drops the aim: a report that stopped, or a device that sends none. A report also withdraws a predicted-end read
  /// already popped but not yet sent, since it replaces the prediction that called for it.
  ///
  /// The poll that confirms a stop off target is never predicted: its report passes no count, and its
  /// delay stays the default one (see decisions::classify_stopped_report()).
  /// @param device_id Device the report belongs to; tracked devices only.
  /// @param remaining_s Usable remaining travel time in seconds, empty when there is none.
  /// @param now Current millis().
  void on_trusted_report(const std::string &device_id, std::optional<uint16_t> remaining_s, uint32_t now);
  /// How long the next progress read waits when the device's remaining time has stopped falling.
  ///
  /// Zero while the count falls or none is known. From the PREDICTED_END_STALE_REPORTS-th report in a row
  /// whose count did not fall: FROZEN_COUNT_BACKOFF_FIRST_MS, then FROZEN_COUNT_BACKOFF_SECOND_MS, then
  /// FROZEN_COUNT_BACKOFF_MAX_MS for every further one. A caller whose own cadence is longer keeps it.
  /// @param device_id Device whose last trusted report was passed to on_trusted_report().
  [[nodiscard]] uint32_t frozen_count_delay_ms(const std::string &device_id) const;
  /// The planned predicted-end read's timestamp (0 = none).
  [[nodiscard]] uint32_t get_settle_update(const std::string &device_id) const;

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
  /// Mark the device's next poll as the one reading where an accepted STOP left it, so it is a
  /// settling read (decisions::ReadKind::SETTLING). Only a tracked device can be marked;
  /// begin_tracking() and clear() drop the mark, so a newer command or remote activity never
  /// inherits it.
  void mark_stop_settle(const std::string &device_id);
  /// Consume the mark set by mark_stop_settle(): true once, for the first poll dispatched after it.
  [[nodiscard]] bool take_stop_settle(const std::string &device_id);
  /// Mark (or unmark) the device's next poll as the one confirming a stop off target, so it is a
  /// settling read too. Every trusted status report sets it from its own verdict, so a report that
  /// no longer needs confirming (moving again, or at rest) drops a mark an earlier one left. Only a
  /// tracked device can be marked; begin_tracking() and clear() drop the mark as for the STOP mark.
  /// @param device_id Device whose next poll is meant.
  /// @param needed    True when the latest report stopped off target for the first time.
  void set_confirm_stopped(const std::string &device_id, bool needed);
  /// Consume the mark set by set_confirm_stopped(): true once, for the first poll dispatched after it.
  [[nodiscard]] bool take_confirm_stopped(const std::string &device_id);
  /// True when the next poll is the predicted-end read (the mark take_settling_read() consumes).
  [[nodiscard]] bool is_predicted_end_read(const std::string &device_id) const;
  /// Consume the marks (none short-circuits another) for the poll being dispatched.
  /// @return True when that poll settles a position: it follows an accepted STOP, confirms a stop off
  ///         target, or is the predicted-end read.
  [[nodiscard]] bool take_settling_read(const std::string &device_id);

  // --- Queries ---
  /// True when the device has an active bounded polling window (deadline set and not expired).
  [[nodiscard]] bool is_tracking_active(const std::string &device_id, uint32_t now) const;

  // --- Due-device scan ---
  /// Return and consume the first device with a poll overdue.
  ///
  /// A device's predicted-end read (on_trusted_report()) wins over its progress read: when it is due,
  /// the device is returned with its predicted-end mark set (take_settling_read()) and a progress
  /// read due too is dropped. A progress read due while the predicted-end read follows within
  /// PREDICTED_END_DROPS_PROGRESS_WITHIN_MS is dropped as well; the predicted-end read stays planned.
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

  // --- Catch-up read after linked-remote activity (ADR 0080) ---
  /// A linked remote was heard: open a burst, or keep the open one. Only the exchanges that end
  /// while it is open set its due time (on_exchange_ended()), so a burst during which the hub never
  /// used the radio never comes due.
  void on_linked_remote_activity();
  /// An exchange ended at @p now. While a burst is open its catch-up becomes due @p delay_ms after
  /// @p now, later or earlier than before; a @p delay_ms of 0 (the catch-up is switched off) closes
  /// the burst instead. No-op without an open burst.
  void on_exchange_ended(uint32_t now, uint32_t delay_ms);
  /// True once when the open burst's catch-up is due at @p now. The burst is closed first, so the
  /// catch-up's own reads, which are exchanges too, do not reopen it. Wrap-safe across a millis() rollover.
  [[nodiscard]] bool take_due_catchup(uint32_t now);
  /// Mark the device's next status read as a catch-up read: a press the hub missed may have set it
  /// moving since its last report, so the read treats its state as unknown
  /// (decisions::AttemptRequest::with_state_unknown()). Needs no tracked device; begin_tracking() and
  /// clear() drop the mark, since heard activity or a report is news again.
  void mark_state_unknown(const std::string &device_id);
  /// Consume the mark set by mark_state_unknown(): true once, for the status read dispatched after it.
  [[nodiscard]] bool take_state_unknown(const std::string &device_id);

 private:
  std::map<std::string, PollTracking> tracking_;
  bool catchup_burst_open_{false};  ///< A linked remote was heard and its catch-up has not run yet.
  uint32_t catchup_due_ms_{0};      ///< When the open burst's catch-up is due; 0 = no exchange has ended in it yet.
  std::set<std::string> state_unknown_;            ///< Devices whose next status read is a catch-up read.
  std::map<std::string, ForeignTraffic> foreign_;  ///< Overheard-traffic state, outside the tracking lifecycle.
  std::vector<std::string> ended_;  ///< Devices whose window ran out, for pop_ended_device(); no duplicates.

  /// Record that a device's window ran out, once.
  void note_ended_(const std::string &device_id);

  /// Compute retry delay from failure count and failure class.
  static uint32_t retry_delay_ms(uint8_t consecutive_failures, bool auth_like);
};

}  // namespace home_io_control
}  // namespace esphome
