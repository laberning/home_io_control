#pragma once

/// @file periodic_poll_policy.h
/// @brief Schedule and failure bookkeeping for the opt-in periodic polls.
/// @ingroup hioc_hub
///
/// Owns the per-device periodic schedule of one opt-in poll. The hub runs one instance per poll kind:
/// the limitation read behind `rain_sensor_poll_interval`, the event subscriptions behind
/// `sun_level_poll_interval` and `battery_level_poll_interval` (ADR 0050, ADR 0064), and the status
/// read behind `status_refresh_interval` (ADR 0066).
/// Kept apart from StatusPollPolicy on purpose: that one runs bounded tracking windows which end
/// when a device stops, this one is an unbounded periodic schedule. Pure logic with injected
/// timestamps and random numbers — fully host-testable without a clock or a random source.

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace esphome {
namespace home_io_control {

/// Earliest first poll after boot. Sits after the entities' initial status request
/// (INITIAL_STATUS_REQUEST_DELAY_MS) so the boot burst is spread over time.
static constexpr uint32_t PERIODIC_POLL_INITIAL_DELAY_MS = 15000;
/// Width of the random window added to PERIODIC_POLL_INITIAL_DELAY_MS, so devices do not start in step.
static constexpr uint32_t PERIODIC_POLL_INITIAL_SPREAD_MS = 30000;
/// Per-poll offset, as a percentage of the interval, applied symmetrically around it. At the
/// one-minute minimum that is six seconds either way — more than one exchange lasts — so two
/// devices sharing an interval drift apart within a few polls. Symmetric on purpose: the mean stays
/// the configured interval, so the transmit time per hour in the documentation stays true.
static constexpr uint32_t PERIODIC_POLL_JITTER_PERCENT = 10;
/// Delay before asking again when the device is moving. Keeps the poll out of a running manoeuvre
/// without waiting a whole interval.
static constexpr uint32_t PERIODIC_POLL_MOVING_RETRY_MS = 30000;
/// Delay before asking once more after a poll the device left unanswered (silence, or a challenge
/// with no final answer). A solar motor misses about one poll in four in the field (issue #156) and
/// answers a second try within seconds as often as a first, so one quick retry keeps a reading from
/// going stale over a whole interval. Only an unanswered retry counts as a miss.
static constexpr uint32_t PERIODIC_POLL_MISS_RETRY_MS = 60000;
/// Tries per poll. A missed poll only costs one interval of latency, so the full retry budget is
/// not worth its transmit time here.
static constexpr uint8_t PERIODIC_POLL_MAX_TRIES = 2;
/// Consecutive unanswered polls after which the last reading is no longer usable.
static constexpr uint8_t PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN = 3;

/// @brief Delay of a device's first poll after its interval is set.
/// @param random A uniformly distributed 32-bit random number.
/// @return PERIODIC_POLL_INITIAL_DELAY_MS plus an offset in [0, PERIODIC_POLL_INITIAL_SPREAD_MS].
[[nodiscard]] inline uint32_t periodic_poll_first_delay_ms(uint32_t random) {
  return PERIODIC_POLL_INITIAL_DELAY_MS + (random % (PERIODIC_POLL_INITIAL_SPREAD_MS + 1));
}

/// @brief One poll interval moved by a uniform offset of up to PERIODIC_POLL_JITTER_PERCENT either way.
///
/// The offset scales @p random over the whole 32-bit range, so 0 gives the lower bound, UINT32_MAX
/// the upper bound and the middle of the range the unmodified interval. Computed in 64 bit so a
/// 24 hour interval cannot overflow.
/// @param interval_ms The configured interval.
/// @param random A uniformly distributed 32-bit random number.
/// @return The delay until the next poll, in [interval - jitter, interval + jitter].
[[nodiscard]] inline uint32_t periodic_poll_jittered_interval_ms(uint32_t interval_ms, uint32_t random) {
  const uint64_t jitter = static_cast<uint64_t>(interval_ms) * PERIODIC_POLL_JITTER_PERCENT / 100;
  const uint64_t offset = (static_cast<uint64_t>(random) * 2 * jitter) / UINT32_MAX;  // 0 .. 2 * jitter
  return static_cast<uint32_t>(interval_ms - jitter + offset);
}

/// @brief Per-device periodic poll schedule, miss retry and miss counter for one poll kind.
class PeriodicPollPolicy {
 public:
  /// Set the poll interval and schedule the first poll at periodic_poll_first_delay_ms(@p random)
  /// after @p now. 0 removes the device from the schedule.
  void set_interval(const std::string &device_id, uint32_t interval_ms, uint32_t now, uint32_t random);
  /// Return the configured interval (0 when the device is not scheduled).
  [[nodiscard]] uint32_t get_interval(const std::string &device_id) const;

  /// True when any device's poll is due at @p now. Lets the caller draw a random number only when
  /// pop_due_device() will use it.
  [[nodiscard]] bool has_due(uint32_t now) const;

  /// Return one device whose poll is due and immediately schedule its next poll at
  /// periodic_poll_jittered_interval_ms() after @p now, so a poll that is later dropped from the
  /// operation queue (pairing flushes background work) is skipped rather than lost forever.
  /// Returns nullopt when nothing is due. Comparison is wrap-safe across the 32-bit millis() rollover.
  [[nodiscard]] std::optional<std::string> pop_due_device(uint32_t now, uint32_t random);

  /// Pull the device's next poll forward to @p delay_ms after @p now. Never pushes it later.
  void defer(const std::string &device_id, uint32_t delay_ms, uint32_t now);
  /// The device has just given what the poll asks for without being polled: schedule its next poll
  /// periodic_poll_jittered_interval_ms() after @p now, earlier or later than it was, and drop an
  /// armed retry. No-op for an unscheduled device.
  void restart_interval(const std::string &device_id, uint32_t now, uint32_t random);

  /// Count one failed poll (saturating), with no retry: for a poll the device answered with
  /// something that is not a reading (an error reply, an unexpected shape), which asking again
  /// soon would not change.
  /// @return True once PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN is reached, i.e. the last reading is no
  ///         longer usable. Stays true until on_poll_succeeded().
  bool on_poll_failed(const std::string &device_id);
  /// Handle a poll the device left unanswered. A scheduled poll arms one retry
  /// PERIODIC_POLL_MISS_RETRY_MS after @p now (pulled forward like defer()) and counts nothing; the
  /// retry, if it is left unanswered too, counts one miss like on_poll_failed().
  /// @return Same as on_poll_failed(): true while the last reading is no longer usable.
  bool on_poll_unanswered(const std::string &device_id, uint32_t now);
  /// True while a retry armed by on_poll_unanswered() has not been popped yet.
  [[nodiscard]] bool retry_armed(const std::string &device_id) const;
  /// Clear the unanswered-poll count.
  void on_poll_succeeded(const std::string &device_id);
  /// True the first time it is called for a device that answered the poll's request with an error
  /// reply, false afterwards, so the log names that once instead of every poll.
  [[nodiscard]] bool first_error_reply(const std::string &device_id);
  /// Consecutive failed polls (0 for an unscheduled device).
  [[nodiscard]] uint8_t get_failures(const std::string &device_id) const;
  /// Absolute millis() time of the device's next poll (0 for an unscheduled device).
  [[nodiscard]] uint32_t get_next_poll(const std::string &device_id) const;

 private:
  /// Schedule state of one device.
  struct Entry {
    uint32_t interval_ms{0};     ///< Configured interval.
    uint32_t next_poll{0};       ///< Absolute millis() time of the next poll.
    uint8_t failures{0};         ///< Consecutive failed polls (a retried poll counts once).
    bool error_reported{false};  ///< True once first_error_reply() has returned true.
    bool retry_armed{false};     ///< The next pop is the retry of an unanswered poll.
    bool polling_retry{false};   ///< The poll popped last is such a retry.
  };

  std::map<std::string, Entry> entries_;  ///< Scheduled devices, keyed by device id.
};

}  // namespace home_io_control
}  // namespace esphome
