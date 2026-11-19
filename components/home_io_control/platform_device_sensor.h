#pragma once

/// @file platform_device_sensor.h
/// @brief The `sensor:` entry that declares a device which has no other main entity, and schedules its readings.
/// @ingroup hioc_platforms
///
/// A heat pump or another device that keeps its values in data objects has no cover, light or climate entity to
/// declare it. The `sensor:` platform is its entry: it registers the device with the hub like every main entity
/// (DeviceBoundEntity) and asks the hub to read each reading the YAML names on its own schedule
/// (HubDevicePort::set_device_reading_poll_interval()). The readings themselves are the generic reading sensors
/// (platform_device_readings.h) the generated code creates next to it. The entry's own sensor shows nothing and is
/// marked internal by the codegen, so Home Assistant sees only the readings and the companion diagnostics.

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "platform_entity_base.h"
#include "proto_device_readings.h"

namespace esphome {
namespace home_io_control {

/// @brief Declares a device by a `sensor:` entry and schedules the data-object readings of its keys.
/// @ingroup hioc_platforms
class IOHomeDeviceSensor : public sensor::Sensor, public Component, public DeviceBoundEntity {
 public:
  /// @brief Set the interval every reading of the entry is read at (the YAML `update_interval:`, which ESPHome's
  /// generated code passes here in milliseconds); the hub raises one below its minimum.
  /// @param interval_ms Read interval in milliseconds; zero keeps each reading's own default.
  void set_update_interval(uint32_t interval_ms) { this->update_interval_ms_ = interval_ms; }

  /// @brief Register the device with the hub and schedule its readings.
  void setup() override;

  /// @brief Dump the declared device and its scheduled readings to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  uint32_t update_interval_ms_{0};  ///< The entry's `update_interval:`; 0 when it sets none.
};

}  // namespace home_io_control
}  // namespace esphome
