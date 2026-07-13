#pragma once

/// @file platform_last_result_text_sensor.h
/// @brief Diagnostic text sensor exposing the device's last CMD_ERROR_RESP result reason.
/// @ingroup hioc_platforms

#include <string>

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "hub_core.h"

namespace esphome {
namespace home_io_control {

/// @brief Diagnostic text sensor that publishes the symbolic name of a device's most recent
/// CMD_ERROR_RESP result code (e.g. "LIMITATION_BY_RAIN"), letting a "nothing happened" command
/// self-explain instead of only showing up in the log. Shared by every device-bound platform
/// (cover, light, switch, lock) via platform_common.py's create_last_result_sensor().
///
/// Publishes an empty string until the first CMD_ERROR_RESP is seen, and again after any
/// subsequent successful status/command reply clears it (see detail::clear_command_result()).
/// @ingroup hioc_platforms
class IOHomeLastResultTextSensor : public text_sensor::TextSensor, public Component {
 public:
  /// @brief Set the parent controller component.
  /// @param parent Pointer to the IOHomeControlComponent instance.
  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }

  /// @brief Set the device ID whose last command result this sensor exposes.
  /// @param id Hexadecimal node ID string (for example "123ABC").
  void set_device_id(const std::string &id) { this->device_id_ = id; }

  /// @brief Register callbacks and publish the initial cached state.
  void setup() override;

  /// @brief Dump text-sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Publish the current cached result reason after a hub update.
  /// @param id Device ID that updated.
  /// @param dev Updated cached device record.
  void on_device_update_(const std::string &id, const IoDevice &dev);

  IOHomeControlComponent *parent_{nullptr};
  std::string device_id_;
};

}  // namespace home_io_control
}  // namespace esphome
