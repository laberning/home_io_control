#pragma once

/// @file platform_switch.h
/// @brief Experimental binary switch entity for IO-Homecontrol devices.

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"
#include "hub_core.h"

namespace esphome {
namespace home_io_control {

class IOHomeSwitch : public switch_::Switch, public Component {
 public:
  void setup() override;
  void dump_config() override;
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }
  void set_device_id(const std::string &id) { this->device_id_ = id; }

 protected:
  void write_state(bool state) override;
  void on_device_update_(const std::string &id, const IoDevice &dev);

  IOHomeControlComponent *parent_{nullptr};
  std::string device_id_;
};

}  // namespace home_io_control
}  // namespace esphome