#pragma once

/// @file platform_cover_favorite_button.h
/// @brief Generated favorite-position button for position-capable cover entities.
/// @ingroup hioc_platforms

#include <string>

#include "esphome/components/button/button.h"
#include "esphome/core/component.h"
#include "hub_core.h"

namespace esphome {
namespace home_io_control {

/// @brief Button entity that sends the protocol's favorite or My-position command.
/// @ingroup hioc_platforms
class IOHomeCoverFavoriteButton : public button::Button, public Component {
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
  /// @brief Handle a Home Assistant button press by queueing a favorite-position command.
  void press_action() override;

  IOHomeControlComponent *parent_{nullptr};
  std::string device_id_;
};

}  // namespace home_io_control
}  // namespace esphome