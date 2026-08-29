#pragma once

/// @file platform_oneway_last_command_text_sensor.h
/// @brief Diagnostic text sensor reporting the last 1W command a controller identity transmitted.
/// @ingroup hioc_platforms
///
/// **This is the only feedback 1W transmit can ever give.** There is no reply frame, so a command
/// a device ignored is indistinguishable on the radio from one it obeyed. A user with a wrong key,
/// a desynced counter or a missing enrollment would otherwise see nothing at all — not an error,
/// not a timeout, nothing.
///
/// So this sensor deliberately does *not* claim the device acted, and its text must never be
/// worded as if it did. It reports what the hub transmitted, which is the half that is knowable,
/// and it surfaces the **sequence** used so a stuck user can judge whether the counter is the
/// problem and whether `initial_sequence:` needs bumping.

#include <string>

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "hub_core.h"
#include "oneway_transmitter.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Build the sensor string for one command report.
///
/// Pure and free-standing so the wording is unit-testable: the host ESP_LOG stub discards its
/// arguments, so a formatter buried in a publish call could not be asserted on at all.
/// @param report The attempt to describe.
/// @return Human-readable summary, e.g. `STOP -> awning seq 1234` or `STOP -> awning seq 1234 (not sent)`.
std::string format_oneway_command_report(const OneWayCommandReport &report);

/// @brief Diagnostic text sensor: what this identity last put on air.
/// @ingroup hioc_platforms
class IOHomeOneWayLastCommandTextSensor : public text_sensor::TextSensor, public Component, public HubBoundEntity {
 public:
  /// @brief Set the controller identity this sensor reports on.
  /// @param id Handle from the `oneway_controllers:` block.
  void set_controller_id(const std::string &id) { this->controller_id_ = id; }

  /// @brief Subscribe to the hub's per-command reports.
  void setup() override;

  /// @brief Dump text-sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  std::string controller_id_;
};

}  // namespace home_io_control
}  // namespace esphome
