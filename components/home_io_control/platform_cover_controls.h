#pragma once

/// @file platform_cover_controls.h
/// @brief Generated per-cover auxiliary entities: the command button (favorite / ventilation),
/// the silent-operation toggle and the rain binary sensor.
/// @ingroup hioc_platforms
///
/// Both are device-bound companions (DeviceBoundCompanion), generated alongside a `cover:` entity
/// by cover.py when the device type supports them, and both act on an already-registered device
/// rather than owning one. They live together here because they are the cover platform's small
/// auxiliary surface, not full entity platforms of their own.

#include <string>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "hub_core.h"
#include "platform_entity_base.h"
#include "proto_device_model.h"

namespace esphome {
namespace home_io_control {

/// @brief Button entity that sends one non-positional cover command on press.
///
/// One class covers both companions the cover platform generates: codegen calls set_command()
/// with CoverCommand::FAVORITE for the favorite/"My"-position button (position-capable device
/// types) and CoverCommand::VENT for the ventilation button (window-type devices —
/// WINDOW_OPENER, VENTILATION_POINT — where the vent command moves the actuator to a predefined
/// partially-open position suitable for air exchange). The press path, the parent/device guards
/// and the log wording are identical for both; only the command byte differs.
/// @ingroup hioc_platforms
class IOHomeCoverCommandButton : public button::Button, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Set which cover command a press sends.
  /// @param command CoverCommand::FAVORITE for the favorite-position companion,
  ///        CoverCommand::VENT for the ventilation companion.
  void set_command(CoverCommand command) { this->command_ = command; }

  /// @brief Initialize the generated button.
  void setup() override {}

  /// @brief Dump button configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Handle a Home Assistant button press by queueing this button's cover command, which
  /// is then sent to the device via the authenticated exchange like any other cover command.
  void press_action() override;

  CoverCommand command_{CoverCommand::FAVORITE};
};

/// @brief Switch entity that selects a cover's travel profile at runtime.
/// @ingroup hioc_platforms
///
/// ON sends position commands with the manufacturer app's "silent operation" extended block, which
/// makes the motor travel more slowly; OFF sends the normal payload. Only generated when the cover
/// declares `silent:` in YAML, so a config that never mentions it gains no entity — the YAML value
/// is the boot state and this switch moves it afterwards.
///
/// Device-bound like IOHomeCoverCommandButton rather than hub-level like
/// IOHomeAcceptForeignPairingSwitch: the setting is per actuator. Nothing on the wire reports a
/// device's current profile, so this reflects only what this hub has been told to send — there is
/// no readback to reconcile against, and none is faked.
class IOHomeCoverSilentSwitch : public switch_::Switch, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Publish the boot state declared in YAML so Home Assistant starts in sync.
  void setup() override;

  /// @brief Dump switch configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Apply the requested travel profile to the bound device.
  /// @param state True for silent (slower) operation.
  void write_state(bool state) override;
};

/// @brief Binary sensor that reports whether the window currently limits its own opening for rain.
/// @ingroup hioc_platforms
///
/// ON when the device's minimum limitation read at least the rain threshold, OFF when it read
/// below it. Only generated when the cover declares `rain_sensor_poll_interval:`, which is also what
/// schedules the poll that feeds it. It has no state until the first answered poll, and loses it
/// again after several unanswered polls: "off" always means a device said so. Independent of the
/// Active Issue sensor, which reports what the last move or a clamped command implied.
class IOHomeRainBinarySensor : public binary_sensor::BinarySensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Publish a rain state: true for RAIN, false for DRY, no state for UNKNOWN.
  void publish_rain_state_(RainSensorState state);
};

}  // namespace home_io_control
}  // namespace esphome
