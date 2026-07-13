#pragma once

/// @file platform_device_name_text_sensor.h
/// @brief Diagnostic text sensor exposing the stored IO-Homecontrol device name.
/// @ingroup hioc_platforms

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Diagnostic text sensor that publishes the cached device name.
///
/// Beyond the shared companion behavior it also queues one boot-time GET_NAME request so the
/// cache gets populated without waiting for unrelated traffic.
/// @ingroup hioc_platforms
class IOHomeDeviceNameTextSensor : public text_sensor::TextSensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Register the device-update subscription and schedule an initial name fetch.
  void setup() override;

  /// @brief Dump text-sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }
};

}  // namespace home_io_control
}  // namespace esphome
