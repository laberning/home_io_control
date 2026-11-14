#pragma once

/// @file device_event_log.h
/// @brief The log of device event reports behind the "Last Device Event" sensor.
/// @ingroup hioc_hub
///
/// Whether a frame qualifies is decided elsewhere (decisions::should_record_device_event()); this
/// class only records what it is handed.

#include "proto_frame.h"

#include <cstdint>
#include <functional>
#include <string>

namespace esphome {
namespace home_io_control {

/// @brief Counts and renders the device events the hub records, and tells its subscriber.
///
/// A recording is only ever a record: it never answers the frame and never applies it to a device,
/// whatever the frame claims to carry (ADR 0022, ADR 0060).
/// @ingroup hioc_hub
class DeviceEventLog {
 public:
  /// Record @p frame: log it raw, count it, render it as `#<n> t=<uptime s> <SRC> <cmd> <payload hex>`
  /// and call the subscriber. The counter makes a repeated identical report a new value, which Home
  /// Assistant's history needs (it records state changes only).
  /// @param frame The received frame.
  /// @param verbose Log at INFO instead of DEBUG. A subscription outlives the build that made it and
  /// must not fill a normal build's log, so the hub passes `diagnostic_probes`.
  void record(const IoFrame &frame, bool verbose);

  /// @return How many device events have been recorded since boot.
  [[nodiscard]] uint32_t count() const { return this->count_; }

  /// @return The most recent event as rendered by record(), or an empty string before the first one.
  [[nodiscard]] const std::string &last() const { return this->last_; }

  /// Register the callback invoked after every recorded event. Single-slot: one text sensor
  /// subscribes.
  /// @param cb Callable with no arguments.
  void set_callback(std::function<void()> cb) { this->callback_ = std::move(cb); }

 private:
  uint32_t count_{0};
  std::string last_;
  std::function<void()> callback_;
};

}  // namespace home_io_control
}  // namespace esphome
