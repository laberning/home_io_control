#pragma once

/// @file platform_last_contact_sensor.h
/// @brief Diagnostic sensor exposing seconds elapsed since a device's last received frame.
/// @ingroup hioc_platforms

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Diagnostic sensor that publishes seconds elapsed since the last frame received from a
/// device (see detail::update_link_health() in hub_internal.h).
///
/// This is an age, not a timestamp: it resets to ~0 on every frame from the device — including
/// replies to the hub's own status polls and commands, not just traffic the device sends
/// unprompted — and counts up from there. A periodic heartbeat (see HEARTBEAT_INTERVAL_MS in the
/// .cpp) re-publishes it even when the device stays quiet, so the value keeps advancing in Home
/// Assistant instead of freezing at whatever it was at the last frame. Publishes nothing until
/// the first frame is seen.
/// @ingroup hioc_platforms
class IOHomeLastContactSensor : public sensor::Sensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Register the device-update subscription, start the heartbeat, and publish the
  /// initial cached state.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Compute and publish seconds since `dev.last_seen_ms`; no-op before the first frame.
  /// @param dev Device to read `last_seen_ms` from.
  void publish_age_(const IoDevice &dev);
};

}  // namespace home_io_control
}  // namespace esphome
