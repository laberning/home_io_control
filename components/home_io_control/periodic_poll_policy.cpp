/// @file periodic_poll_policy.cpp
/// @brief Schedule and failure bookkeeping for the opt-in periodic polls.
/// @ingroup hioc_hub

#include "periodic_poll_policy.h"

#include <algorithm>

namespace esphome {
namespace home_io_control {

namespace {

/// True when @p deadline has been reached at @p now, correct across the millis() rollover.
bool reached(uint32_t now, uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }

}  // namespace

void PeriodicPollPolicy::set_interval(const std::string &device_id, uint32_t interval_ms, uint32_t now,
                                      uint32_t random) {
  if (interval_ms == 0) {
    entries_.erase(device_id);
    return;
  }
  Entry &e = entries_[device_id];
  e.interval_ms = interval_ms;
  e.next_poll = now + periodic_poll_first_delay_ms(random);
  e.failures = 0;
  e.retry_armed = false;
  e.polling_retry = false;
}

uint32_t PeriodicPollPolicy::get_interval(const std::string &device_id) const {
  const auto it = entries_.find(device_id);
  return it != entries_.end() ? it->second.interval_ms : 0;
}

bool PeriodicPollPolicy::has_due(uint32_t now) const {
  return std::any_of(entries_.begin(), entries_.end(),
                     [now](const auto &entry) { return reached(now, entry.second.next_poll); });
}

std::optional<std::string> PeriodicPollPolicy::pop_due_device(uint32_t now, uint32_t random) {
  for (auto &[device_id, e] : entries_) {
    if (!reached(now, e.next_poll))
      continue;
    e.next_poll = now + periodic_poll_jittered_interval_ms(e.interval_ms, random);
    // A retry dropped from the queue (pairing flushes background work) leaves no armed flag
    // behind: the pop consumes it either way.
    e.polling_retry = e.retry_armed;
    e.retry_armed = false;
    return device_id;
  }
  return std::nullopt;
}

void PeriodicPollPolicy::defer(const std::string &device_id, uint32_t delay_ms, uint32_t now) {
  const auto it = entries_.find(device_id);
  if (it == entries_.end())
    return;
  const uint32_t retry_at = now + delay_ms;
  if (reached(it->second.next_poll, retry_at))  // next_poll is at or after retry_at: pull it earlier
    it->second.next_poll = retry_at;
}

void PeriodicPollPolicy::restart_interval(const std::string &device_id, uint32_t now, uint32_t random) {
  const auto it = entries_.find(device_id);
  if (it == entries_.end())
    return;
  it->second.next_poll = now + periodic_poll_jittered_interval_ms(it->second.interval_ms, random);
  it->second.retry_armed = false;
}

bool PeriodicPollPolicy::on_poll_failed(const std::string &device_id) {
  const auto it = entries_.find(device_id);
  if (it == entries_.end())
    return false;
  if (it->second.failures < UINT8_MAX)
    it->second.failures++;
  return it->second.failures >= PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN;
}

bool PeriodicPollPolicy::on_poll_unanswered(const std::string &device_id, uint32_t now) {
  const auto it = entries_.find(device_id);
  if (it == entries_.end())
    return false;
  Entry &e = it->second;
  if (e.polling_retry)
    return this->on_poll_failed(device_id);
  e.retry_armed = true;
  this->defer(device_id, PERIODIC_POLL_MISS_RETRY_MS, now);
  return e.failures >= PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN;
}

bool PeriodicPollPolicy::retry_armed(const std::string &device_id) const {
  const auto it = entries_.find(device_id);
  return it != entries_.end() && it->second.retry_armed;
}

void PeriodicPollPolicy::on_poll_succeeded(const std::string &device_id) {
  const auto it = entries_.find(device_id);
  if (it != entries_.end())
    it->second.failures = 0;
}

bool PeriodicPollPolicy::first_error_reply(const std::string &device_id) {
  const auto it = entries_.find(device_id);
  if (it == entries_.end() || it->second.error_reported)
    return false;
  it->second.error_reported = true;
  return true;
}

uint8_t PeriodicPollPolicy::get_failures(const std::string &device_id) const {
  const auto it = entries_.find(device_id);
  return it != entries_.end() ? it->second.failures : 0;
}

uint32_t PeriodicPollPolicy::get_next_poll(const std::string &device_id) const {
  const auto it = entries_.find(device_id);
  return it != entries_.end() ? it->second.next_poll : 0;
}

}  // namespace home_io_control
}  // namespace esphome
