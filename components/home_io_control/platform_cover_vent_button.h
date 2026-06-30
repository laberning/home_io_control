#pragma once

/// @file platform_cover_vent_button.h
/// @brief Generated ventilation-position button for window-type cover entities.
/// @ingroup hioc_platforms
///
/// This button sends the protocol's ventilation command to window-type devices
/// (WINDOW_OPENER, VENTILATION_POINT). The vent command moves the actuator to
/// a predefined partially-open position suitable for air exchange.

#include <string>

#include "esphome/components/button/button.h"
#include "esphome/core/component.h"
#include "hub_core.h"

namespace esphome {
namespace home_io_control {

/// @brief Button entity that sends the ventilation position command.
///
/// Generated automatically by the Python codegen for cover entities whose
/// io_device_type is a window or ventilation device. Pressing this button
/// in Home Assistant sends CoverCommand::VENT via the authenticated exchange.
/// @ingroup hioc_platforms
class IOHomeCoverVentButton : public button::Button, public Component {
 public:
  /// @brief Set the parent controller component.
  /// @param parent Pointer to the IOHomeControlComponent instance.
  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }

  /// @brief Set the device ID controlled by this button.
  /// @param id Hexadecimal node ID string (e.g. "123ABC").
  void set_device_id(const std::string &id) { this->device_id_ = id; }

  /// @brief Initialize the generated button.
  void setup() override {}

  /// @brief Dump button configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Handle a Home Assistant button press by queueing a ventilation command.
  void press_action() override;

  IOHomeControlComponent *parent_{nullptr};
  std::string device_id_;
};

}  // namespace home_io_control
}  // namespace esphome
