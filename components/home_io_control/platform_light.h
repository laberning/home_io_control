#pragma once

/// @file platform_light.h
/// @brief Experimental binary light entity for IO-Homecontrol devices.

#include "esphome/core/component.h"
#include "esphome/components/light/light_output.h"
#include "hub_core.h"

namespace esphome {
namespace home_io_control {

class IOHomeLight : public light::LightOutput, public Component {
 public:
  void setup() override;
  void dump_config() override;
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

  light::LightTraits get_traits() override;
  void setup_state(light::LightState *state) override { this->state_ = state; }
  void write_state(light::LightState *state) override;

  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }
  void set_device_id(const std::string &id) { this->device_id_ = id; }

 protected:
  void on_device_update_(const std::string &id, const IoDevice &dev);

  IOHomeControlComponent *parent_{nullptr};
  light::LightState *state_{nullptr};
  std::string device_id_;
  // Updating LightState from a device callback also triggers write_state(). Use a one-shot guard
  // so inbound radio status does not get echoed straight back as a new outbound command.
  bool suppress_write_{false};
};

}  // namespace home_io_control
}  // namespace esphome