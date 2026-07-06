/// @file status_poll_policy.cpp
/// @brief StatusPollPolicy implementation.
/// @ingroup hioc_hub

#include "status_poll_policy.h"

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

uint32_t StatusPollPolicy::on_exchange_failed(const std::string &device_id, bool auth_like, uint32_t now) {
  auto it = tracking_.find(device_id);
  if (it == tracking_.end())
    return 0;
  PollTracking &t = it->second;

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

  clear(device_id);
  return 0;
}

void StatusPollPolicy::clear_failure_streaks(const std::string &device_id) {
  auto it = tracking_.find(device_id);
  if (it == tracking_.end())
    return;
  it->second.status_poll_failures = 0;
  it->second.auth_poll_failures = 0;
}

uint8_t StatusPollPolicy::get_status_poll_failures(const std::string &device_id) const {
  auto it = tracking_.find(device_id);
  return (it != tracking_.end()) ? it->second.status_poll_failures : 0;
}

uint8_t StatusPollPolicy::get_auth_poll_failures(const std::string &device_id) const {
  auto it = tracking_.find(device_id);
  return (it != tracking_.end()) ? it->second.auth_poll_failures : 0;
}

std::optional<std::string> StatusPollPolicy::pop_due_device(uint32_t now) {
  for (auto &pair : tracking_) {
    PollTracking &t = pair.second;
    if (t.next_update == 0 || now <= t.next_update)
      continue;
    if (!is_tracking_active(pair.first, now)) {
      clear(pair.first);
      continue;
    }
    t.next_update = 0;
    return pair.first;
  }
  return std::nullopt;
}

uint32_t StatusPollPolicy::retry_delay_ms(uint8_t consecutive_failures, bool auth_like) {
  if (auth_like) {
    if (consecutive_failures <= 1)
      return STATUS_AUTH_RETRY_AFTER_FAIL_MS;
    if (consecutive_failures == 2)
      return STATUS_AUTH_RETRY_AFTER_FAIL_STEP2_MS;
    return STATUS_AUTH_RETRY_AFTER_FAIL_MAX_MS;
  }
  if (consecutive_failures <= 1)
    return STATUS_RETRY_AFTER_FAIL_MS;
  if (consecutive_failures == 2)
    return STATUS_RETRY_AFTER_FAIL_STEP2_MS;
  if (consecutive_failures == 3)
    return STATUS_RETRY_AFTER_FAIL_STEP3_MS;
  if (consecutive_failures == 4)
    return STATUS_RETRY_AFTER_FAIL_STEP4_MS;
  return STATUS_RETRY_AFTER_FAIL_MAX_MS;
}

}  // namespace home_io_control
}  // namespace esphome
