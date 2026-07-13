#pragma once

/// @file platform_last_result_text_sensor.h
/// @brief Diagnostic text sensor exposing the device's last CMD_ERROR_RESP result reason.
/// @ingroup hioc_platforms

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "platform_entity_base.h"

namespace esphome {
namespace home_io_control {

/// @brief Diagnostic text sensor that publishes the symbolic name of a device's most recent
/// CMD_ERROR_RESP result code (e.g. "LIMITATION_BY_RAIN"), letting a "nothing happened" command
/// self-explain instead of only showing up in the log. Shared by every device-bound platform
/// (cover, light, switch, lock) via platform_common.py's companion-sensor codegen.
///
/// Publishes an empty string until the first CMD_ERROR_RESP is seen, and again after any
/// subsequent successful status/command reply clears it (see detail::clear_command_result()).
/// @ingroup hioc_platforms
class IOHomeLastResultTextSensor : public text_sensor::TextSensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Register the device-update subscription and publish the initial cached state.
  void setup() override;

  /// @brief Dump text-sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }
};

}  // namespace home_io_control
}  // namespace esphome
