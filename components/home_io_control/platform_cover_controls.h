#pragma once

/// @file platform_cover_controls.h
/// @brief Generated per-cover auxiliary entities: the command button (favorite / ventilation), the
/// silent-operation toggle and the travel speed select.
/// @ingroup hioc_platforms
///
/// All are device-bound companions (DeviceBoundCompanion), generated alongside a `cover:` entity
/// by cover.py when the device type supports them, and act on an already-registered device
/// rather than owning one. They live together here because they are the cover platform's small
/// auxiliary surface, not full entity platforms of their own. The cover's rain, sun level and battery
/// entities are readings, shown by the generic reading entities (platform_device_readings.h).

#include <string>

#include "esphome/components/button/button.h"
#include "esphome/components/select/select.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "platform_entity_base.h"
#include "proto_device_kinds.h"
#include "proto_device_model.h"

namespace esphome {
namespace home_io_control {

/// @brief Button entity that sends one non-positional cover command on press.
///
/// One class covers every companion the cover platform generates from NAMED_POSITIONS: codegen
/// calls set_command() with CoverCommand::FAVORITE for the favorite/"My"-position button
/// (position-capable device types), CoverCommand::VENT for the ventilation button (window-type
/// devices — WINDOW_OPENER, VENTILATION_POINT — where the vent command moves the actuator to a
/// predefined partially-open position suitable for air exchange), CoverCommand::PARTIAL for the
/// Partial Position button (garage openers) and CoverCommand::PEDESTRIAN for the Pedestrian
/// Position button (gate openers). The press path, the parent/device guards and the log wording
/// are identical for all of them; only the command byte differs.
/// \todo Confirm on a real gate opener that PEDESTRIAN (0xD807) moves the motor to that position
///       (experimental; the button is disabled by default). PARTIAL (0xD809) runs a garage opener's
///       stored partial position (issue #163).
/// @ingroup hioc_platforms
class IOHomeCoverCommandButton : public button::Button, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Set which cover command a press sends.
  /// @param command CoverCommand::FAVORITE for the favorite-position companion,
  ///        CoverCommand::VENT for the ventilation companion, CoverCommand::PARTIAL for the
  ///        partial companion, CoverCommand::PEDESTRIAN for the experimental pedestrian companion.
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
/// makes the motor travel more slowly (TravelSpeed::SILENT); OFF sends the normal payload
/// (TravelSpeed::FAST). Only generated when the cover declares `silent:` in YAML, so a config that
/// never mentions it gains no entity — the YAML value is the boot state and this switch moves it
/// afterwards. IOHomeCoverTravelSpeedSelect is the three-way form of the same setting.
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

/// @brief Select entity that sets a cover's travel speed at runtime: `default`, `silent` or `fast`.
/// @ingroup hioc_platforms
///
/// `default` sends the 6-byte execute frame a wall pad sends, which carries no speed profile and
/// leaves the speed to the motor; `silent` and `fast` send the 8-byte frame with profile 0x05 or
/// 0x06 (TravelSpeed). Only generated when the cover declares `travel_speed:` in YAML, which is its
/// boot state; this select moves it afterwards. Like IOHomeCoverSilentSwitch it reflects only what
/// this hub has been told to send: nothing on the wire reports a device's speed.
class IOHomeCoverTravelSpeedSelect : public select::Select, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Publish the boot state declared in YAML so Home Assistant starts in sync.
  void setup() override;

  /// @brief Dump select configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Apply the selected travel speed to the bound device.
  /// @param value "default", "silent" or "fast" (travel_speed_name()).
  void control(const std::string &value) override;
};

}  // namespace home_io_control
}  // namespace esphome
