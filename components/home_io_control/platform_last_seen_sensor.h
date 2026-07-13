#pragma once

/// @file platform_last_seen_sensor.h
/// @brief Diagnostic sensor exposing seconds-since-boot of a device's last received frame.
/// @ingroup hioc_platforms

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Diagnostic sensor that publishes uptime-seconds at the last frame received from a
/// device (see detail::update_link_health() in hub_internal.h).
///
/// The hub has no wall-clock time source (no `time:` dependency), so this cannot be a Home
/// Assistant `timestamp`-class sensor; it publishes seconds since controller boot instead.
/// Combine with Home Assistant's own "last changed" state-tracking on this entity for a
/// relative "how long ago" view. Publishes nothing until the first frame is seen.
/// @ingroup hioc_platforms
class IOHomeLastSeenSensor : public sensor::Sensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Register the device-update subscription and publish the initial cached state.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }
};

}  // namespace home_io_control
}  // namespace esphome
