#pragma once

/// @file platform_device_name_text_sensor.h
/// @brief Diagnostic text sensor exposing the stored IO-Homecontrol device name.
/// @ingroup hioc_platforms

#include <string>

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "hub_core.h"

namespace esphome {
namespace home_io_control {

/// @brief Diagnostic text sensor that publishes the cached device name.
/// @ingroup hioc_platforms
class IOHomeDeviceNameTextSensor : public text_sensor::TextSensor, public Component {
 public:
  /// @brief Set the parent controller component.
  /// @param parent Pointer to the IOHomeControlComponent instance.
  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }

  /// @brief Set the device ID whose cached name this sensor exposes.
  /// @param id Hexadecimal node ID string (for example "123ABC").
  void set_device_id(const std::string &id) { this->device_id_ = id; }

  /// @brief Register callbacks and schedule an initial name fetch.
  void setup() override;

  /// @brief Dump text-sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Publish the current cached device name after a hub update.
  /// @param id Device ID that updated.
  /// @param dev Updated cached device record.
  void on_device_update_(const std::string &id, const IoDevice &dev);

  IOHomeControlComponent *parent_{nullptr};
  std::string device_id_;
};

}  // namespace home_io_control
}  // namespace esphome