/// @file status_poll_policy.cpp
/// @brief StatusPollPolicy implementation.
/// @ingroup hioc_hub

#include "status_poll_policy.h"

#include <algorithm>
#include <array>

namespace esphome {
namespace home_io_control {

void StatusPollPolicy::set_interval(const std::string &device_id, uint32_t interval_ms) {
  tracking_[device_id].interval_ms = interval_ms;
}

uint32_t StatusPollPolicy::get_interval(const std::string &device_id) const {
  auto it = tracking_.find(device_id);
  return (it != tracking_.end()) ? it->second.interval_ms : 0;
}

void StatusPollPolicy::begin_tracking(const std::string &device_id, uint32_t initial_delay_ms, uint32_t now) {
  PollTracking &t = tracking_[device_id];
  t.poll_deadline = now + MAX_TRACKED_STATUS_POLL_WINDOW_MS;
  t.next_update = (initial_delay_ms == 0) ? 0 : (now + initial_delay_ms);
  t.status_poll_failures = 0;
  t.auth_poll_failures = 0;
  t.moving_poll_misses = 0;
  t.stop_settle = false;
}

void StatusPollPolicy::clear(const std::string &device_id) {
  auto it = tracking_.find(device_id);
  if (it == tracking_.end())
    return;
  PollTracking &t = it->second;
  t.next_update = 0;
  t.poll_deadline = 0;
  t.status_poll_failures = 0;
  t.auth_poll_failures = 0;
  t.moving_poll_misses = 0;
  t.stop_settle = false;
  // interval_ms is configuration — NOT cleared; remains set for future tracking cycles.
}

void StatusPollPolicy::set_next_update(const std::string &device_id, uint32_t abs_time) {
  tracking_[device_id].next_update = abs_time;
}

uint32_t StatusPollPolicy::get_next_update(const std::string &device_id) const {
  auto it = tracking_.find(device_id);
  return (it != tracking_.end()) ? it->second.next_update : 0;
}

uint32_t StatusPollPolicy::get_poll_deadline(const std::string &device_id) const {
  auto it = tracking_.find(device_id);
  return (it != tracking_.end()) ? it->second.poll_deadline : 0;
}

bool StatusPollPolicy::is_tracking_active(const std::string &device_id, uint32_t now) const {
  auto it = tracking_.find(device_id);
  if (it == tracking_.end())
    return false;
  const PollTracking &t = it->second;
  return t.poll_deadline != 0 && now <= t.poll_deadline;
}

uint32_t StatusPollPolicy::on_exchange_failed(const std::string &device_id, bool auth_like, uint32_t now,
                                              bool device_moving) {
  auto it = tracking_.find(device_id);
  if (it == tracking_.end())
    return 0;
  PollTracking &t = it->second;
  // A one-off poll of an untracked device fails here too; only a window that was open can run out.
  const bool was_tracking = t.poll_deadline != 0;

  // A device believed travelling keeps its normal cadence for its first misses rather than leave a
  // gap (MOVING_POLL_MISSES_SKIPPED), but never waits longer than the ladder's first step would.
  // Only a miss outside a failure streak is skipped: a streak already runs the ladder.
  if (device_moving && t.moving_poll_misses < MOVING_POLL_MISSES_SKIPPED && t.status_poll_failures == 0 &&
      t.auth_poll_failures == 0 && is_tracking_active(device_id, now)) {
    const uint32_t delay_ms =
        std::min(settle_delay_ms(t.interval_ms, 0, /*cap_for_stop=*/false), STATUS_RETRY_AFTER_FAIL_MS);
    if (now + delay_ms <= t.poll_deadline) {
      t.moving_poll_misses++;
      t.next_update = now + delay_ms;
      return delay_ms;
    }
  }

  uint32_t backoff_ms;
  if (auth_like) {
    t.status_poll_failures = 0;
    if (t.auth_poll_failures < UINT8_MAX)
      t.auth_poll_failures++;
    backoff_ms = retry_delay_ms(t.auth_poll_failures, true);
  } else {
    t.auth_poll_failures = 0;
    if (t.status_poll_failures < UINT8_MAX)
      t.status_poll_failures++;
    backoff_ms = retry_delay_ms(t.status_poll_failures, false);
  }

  if (is_tracking_active(device_id, now) && now + backoff_ms <= t.poll_deadline) {
    t.next_update = now + backoff_ms;
    return backoff_ms;
  }

  if (was_tracking)
    note_ended_(device_id);
  clear(device_id);
  return 0;
}

void StatusPollPolicy::clear_failure_streaks(const std::string &device_id) {
  auto it = tracking_.find(device_id);
  if (it == tracking_.end())
    return;
  it->second.status_poll_failures = 0;
  it->second.auth_poll_failures = 0;
  it->second.moving_poll_misses = 0;
}

void StatusPollPolicy::mark_stop_settle(const std::string &device_id) {
  auto it = tracking_.find(device_id);
  if (it != tracking_.end())
    it->second.stop_settle = true;
}

bool StatusPollPolicy::take_stop_settle(const std::string &device_id) {
  auto it = tracking_.find(device_id);
  if (it == tracking_.end())
    return false;
  const bool marked = it->second.stop_settle;
  it->second.stop_settle = false;
  return marked;
}

uint8_t StatusPollPolicy::get_status_poll_failures(const std::string &device_id) const {
  auto it = tracking_.find(device_id);
  return (it != tracking_.end()) ? it->second.status_poll_failures : 0;
}

uint8_t StatusPollPolicy::get_auth_poll_failures(const std::string &device_id) const {
  auto it = tracking_.find(device_id);
  return (it != tracking_.end()) ? it->second.auth_poll_failures : 0;
}

uint8_t StatusPollPolicy::get_moving_poll_misses(const std::string &device_id) const {
  auto it = tracking_.find(device_id);
  return (it != tracking_.end()) ? it->second.moving_poll_misses : 0;
}

std::optional<std::string> StatusPollPolicy::pop_due_device(uint32_t now) {
  for (auto &pair : tracking_) {
    PollTracking &t = pair.second;
    if (t.next_update == 0 || now <= t.next_update)
      continue;
    if (!is_tracking_active(pair.first, now)) {
      if (t.poll_deadline != 0)
        note_ended_(pair.first);
      clear(pair.first);
      continue;
    }
    t.next_update = 0;
    return pair.first;
  }
  return std::nullopt;
}

std::optional<std::string> StatusPollPolicy::pop_ended_device() {
  if (ended_.empty())
    return std::nullopt;
  std::string id = ended_.front();
  ended_.erase(ended_.begin());
  return id;
}

void StatusPollPolicy::note_ended_(const std::string &device_id) {
  if (std::find(ended_.begin(), ended_.end(), device_id) == ended_.end())
    ended_.push_back(device_id);
}

namespace {

/// The two backoff ladders, one delay per consecutive failure; the last entry repeats.
constexpr std::array<uint32_t, 5> SILENT_RETRY_LADDER_MS{
    STATUS_RETRY_AFTER_FAIL_MS, STATUS_RETRY_AFTER_FAIL_STEP2_MS, STATUS_RETRY_AFTER_FAIL_STEP3_MS,
    STATUS_RETRY_AFTER_FAIL_STEP4_MS, STATUS_RETRY_AFTER_FAIL_MAX_MS};
constexpr std::array<uint32_t, 4> AUTH_RETRY_LADDER_MS{
    STATUS_AUTH_RETRY_AFTER_FAIL_MS, STATUS_AUTH_RETRY_AFTER_FAIL_STEP2_MS, STATUS_AUTH_RETRY_AFTER_FAIL_STEP3_MS,
    STATUS_AUTH_RETRY_AFTER_FAIL_MAX_MS};

/// The delay for the @p consecutive_failures-th failure in a row (0 is read as 1) on @p ladder.
template<size_t N> uint32_t ladder_step(const std::array<uint32_t, N> &ladder, uint8_t consecutive_failures) {
  const size_t index = consecutive_failures <= 1 ? 0 : consecutive_failures - 1;
  return ladder[std::min(index, N - 1)];
}

}  // namespace

uint32_t StatusPollPolicy::retry_delay_ms(uint8_t consecutive_failures, bool auth_like) {
  return auth_like ? ladder_step(AUTH_RETRY_LADDER_MS, consecutive_failures)
                   : ladder_step(SILENT_RETRY_LADDER_MS, consecutive_failures);
}

}  // namespace home_io_control
}  // namespace esphome
