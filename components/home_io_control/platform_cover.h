#pragma once

/// @file platform_cover.h
/// @brief ESPHome cover entity for IO-Homecontrol devices.
///
/// Maps IO-Homecontrol devices (shutters, awnings, blinds) to Home Assistant cover entities
/// with position control and real-time feedback.
///
/// Position mapping between Home Assistant and IO-Homecontrol:
///   HA:  1.0 = fully open,  0.0 = fully closed
///   IO:  0   = fully open,  100  = fully closed
/// The conversion is: ha_position = 1.0 - (io_position / 100.0)

#include "esphome/core/component.h"
#include "esphome/components/cover/cover.h"
#include "hub_core.h"

namespace esphome {
namespace home_io_control {

class IOHomeCover : public cover::Cover, public Component {
 public:
  IOHomeCover() { this->position = UNKNOWN_POSITION; }
  void setup() override;
  void dump_config() override;
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

  cover::CoverTraits get_traits() override;

  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }
  void set_device_id(const std::string &id) { this->device_id_ = id; }
  /// If true, the position mapping is inverted (some devices like horizontal awnings
  /// report 0 as closed and 100 as open, opposite to the standard).
  void set_invert_position(bool invert) { this->invert_ = invert; }
  [[nodiscard]] bool supports_tilt() const;

 protected:
  /// Called when Home Assistant sends a cover command (open/close/stop/set position).
  void control(const cover::CoverCall &call) override;
  /// Called by the controller when the device reports a new status.
  void on_device_update_(const std::string &id, const IoDevice &dev);

  IOHomeControlComponent *parent_{nullptr};
  std::string device_id_;
  bool invert_{false};
};

}  // namespace home_io_control
}  // namespace esphome
