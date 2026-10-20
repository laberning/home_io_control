#pragma once

/// @file platform_climate_controls.h
/// @brief Generated per-heater auxiliary entities: the window binary sensor and the open-window
/// detection switch.
/// @ingroup hioc_platforms
///
/// Both are device-bound companions (DeviceBoundCompanion) that climate.py generates alongside a
/// `climate:` entity when the device's heating profile has them (HeatingProfileFeatures). They show
/// readings the hub's poll already stores in IoDevice::heating and add no radio traffic of their own,
/// except the switch's write and the read-back that confirms it.
///
/// @todo Experimental: the meaning of registers 0x010E (window sensor) and 0x0106 (open-window
///       detection) is unconfirmed on hardware
///       ([issue #165](https://github.com/laberning/home_io_control/issues/165)).

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "hub_core.h"
#include "platform_entity_base.h"
#include "proto_device_model.h"

namespace esphome {
namespace home_io_control {

/// @brief Binary sensor (`device_class: window`) that is ON while the heater's window sensor reports open.
/// @ingroup hioc_platforms
///
/// Reads IoDevice::heating.window_open. No state until the first poll has read register 0x010E, so
/// "closed" always means the heater said so.
class IOHomeWindowBinarySensor : public binary_sensor::BinarySensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }
};

/// @brief Switch that turns the heater's open-window detection on and off.
/// @ingroup hioc_platforms
///
/// A write sends register 0x0106 and reads it back; the switch shows what the heater answered, so it
/// has no state until the first read (the boot poll) and never shows a command the heater did not
/// confirm. Disabled by default and in the config entity category.
class IOHomeOpenWindowDetectionSwitch : public switch_::Switch, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;

  /// @brief Dump switch configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Write the detection setting and read it back.
  /// @param state True to switch detection on.
  void write_state(bool state) override;
};

}  // namespace home_io_control
}  // namespace esphome
