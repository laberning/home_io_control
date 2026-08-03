#pragma once

/// @file platform_discover_button.h
/// @brief Button entity that triggers device discovery and pairing.
/// @ingroup hioc_platforms

#include "esphome/components/button/button.h"
#include "hub_core.h"

namespace esphome {
namespace home_io_control {

/// @brief Button entity that triggers device discovery and pairing when pressed in Home Assistant.
/// @ingroup hioc_platforms
class IOHomeDiscoverButton : public button::Button, public Component {
 public:
  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }
  void dump_config() override {}

 protected:
  /// @brief When button is pressed, queue a discovery/pair operation.
  void press_action() override { this->parent_->queue_discover_and_pair(); }
  IOHomeControlComponent *parent_{nullptr};
};

}  // namespace home_io_control
}  // namespace esphome
