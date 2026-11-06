#pragma once

/// @file platform_cover_controls.h
/// @brief Generated per-cover auxiliary entities: the command button (favorite / ventilation),
/// the silent-operation toggle, the travel speed select, the rain binary sensor and the battery
/// sensors.
/// @ingroup hioc_platforms
///
/// All are device-bound companions (DeviceBoundCompanion), generated alongside a `cover:` entity
/// by cover.py when the device type supports them, and both act on an already-registered device
/// rather than owning one. They live together here because they are the cover platform's small
/// auxiliary surface, not full entity platforms of their own.

#include <optional>
#include <string>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/select/select.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "hub_core.h"
#include "platform_companion_sensors.h"
#include "platform_entity_base.h"
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
/// \todo Confirm on a real garage opener that PARTIAL (0xD809) and on a real gate opener that
///       PEDESTRIAN (0xD807) move the motor to that position (experimental; the buttons are
///       disabled by default).
/// @ingroup hioc_platforms
class IOHomeCoverCommandButton : public button::Button, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Set which cover command a press sends.
  /// @param command CoverCommand::FAVORITE for the favorite-position companion,
  ///        CoverCommand::VENT for the ventilation companion, CoverCommand::PARTIAL or
  ///        CoverCommand::PEDESTRIAN for the experimental partial / pedestrian companions.
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

/// @brief Sensor publishing the raw sun value a device answers its sun level poll with.
/// @ingroup hioc_platforms
///
/// Only generated when the cover declares `sun_level_poll_interval:`, which also schedules the poll
/// that feeds it (a subscription to EVENT_LOCAL_SENSOR, ADR 0064). Publishes the number as the device
/// sends it, with no unit: 0 at night, rising with sun on the panel. No state until the first
/// answered poll, and again after several unanswered polls.
/// \todo Unit and scale unknown; confirmed on one Somfy RS100 Solar io only (issue #156).
class IOHomeSunLevelSensor : public IOHomeCompanionSensor {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

 protected:
  std::optional<uint16_t> published_;  ///< Last value handed to publish_state(); avoids re-publishing on every update.
  bool published_once_{false};         ///< True once anything (a value or "unknown") was published.
};

/// @brief Sensor (`device_class: battery`, %) publishing the level a device answers its battery level
/// poll with.
/// @ingroup hioc_platforms
///
/// Only generated when the cover declares `battery_level_poll_interval:`, which also schedules the
/// poll that feeds it (a subscription to EVENT_BATTERY_LEVEL_CHANGE, ADR 0064). Independent of the
/// `battery: true` entities, which read the coarse level from status replies at no radio cost.
/// \todo Percent rests on one captured value (100); confirmed on one Somfy RS100 Solar io only.
class IOHomeBatteryLevelSensor : public IOHomeCompanionSensor {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

 protected:
  std::optional<uint16_t> published_;  ///< Last value handed to publish_state(); avoids re-publishing on every update.
  bool published_once_{false};         ///< True once anything (a value or "unknown") was published.
};

/// @brief Binary sensor (`device_class: battery`) that is ON while the device reports a low battery.
/// @ingroup hioc_platforms
///
/// Home Assistant's own low-battery convention, so generic battery tooling (low-battery
/// notification blueprints, battery views) picks it up without per-entity setup. Generated with
/// IOHomeBatteryTextSensor when the cover declares `battery: true`. It reads IoDevice::battery,
/// which only status replies update: the boot poll, the settle polls after a move, and the
/// device's own status updates. No state while the device reports no battery; the first status
/// that shows none logs one INFO line, so a mains device with the key set explains itself.
class IOHomeBatteryLowBinarySensor : public binary_sensor::BinarySensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Publish from a device record: ON for LOW_CHARGE, OFF for NORMAL/FULL, no state otherwise.
  void publish_battery_(const IoDevice &dev);

  bool logged_not_reported_{false};  ///< The "reports no battery" INFO line was written once already.
};

/// @brief Diagnostic text sensor naming the device's coarse battery level: "full", "normal" or "low".
/// @ingroup hioc_platforms
///
/// Disabled by default: IOHomeBatteryLowBinarySensor is the entity automations should use. This
/// one shows all three levels, which is useful while it is still open whether a device ever reports
/// anything but "full". Publishes nothing while the device reports no battery.
class IOHomeBatteryTextSensor : public text_sensor::TextSensor, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;

  /// @brief Dump sensor configuration to the log.
  void dump_config() override;

  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }
};

}  // namespace home_io_control
}  // namespace esphome
