#pragma once

/// @file platform_water_heater.h
/// @brief Water heater entity for a domestic hot water heater, and its opt-in companions.
/// @ingroup hioc_platforms
///
/// Maps a domestic hot water heater (device_feature::WATER_HEATER, proto_dhw.h) onto an ESPHome
/// `water_heater`: water temperature, target temperature and operating mode. The heater's three modes map
/// to ESPHome's as follows (ADR 0068):
///
/// | Heater (register 0x0006) | ESPHome mode  |
/// |--------------------------|---------------|
/// | 0 automatic              | ECO           |
/// | 1 manual, ECO on         | HEAT_PUMP     |
/// | 2 manual, ECO off        | PERFORMANCE   |
///
/// Each heater mode takes the ESPHome mode whose Home Assistant meaning fits what heats: automatic adapts to
/// consumption and uses the element as needed (energy saving), manual ECO on heats with the heat pump only,
/// manual ECO off lets the element assist.
///
/// OFF is not offered: no heater has been seen reporting an off state.
///
/// In automatic mode the heater chooses its own target, and its display shows none, so the entity publishes
/// no target while the heater reports automatic mode, and a target set then is not sent.
///
/// State is observed, not predicted, as on the climate entity: the hub polls the heater (default every 5
/// minutes) and this entity publishes from its readings (reading_value(), proto_device_readings.h), which only a
/// decoded reply changes. Until a reading has arrived an accepted write shows the commanded value; after that the
/// read-back decides.
///
/// Away mode is part of the entity: Home Assistant's away switch starts and ends the heater's holiday mode
/// (ADR 0081). The heater reports it, and boost, in the operating-mode word the poll reads, so the entity
/// shows away only once the heater said so. While away no target is shown (the heater reports a main parameter
/// of 0, which is no target) and a target set then is logged and dropped.
///
/// The companions show readings the same poll already stores and add no radio traffic. Energy, heat pump running and
/// electric backup running are the generic reading entities (platform_device_readings.h) the codegen builds from the
/// reading mirror; the heating-source text sensor combines the two running readings, and the boost switch shows the
/// boost state of the operating-mode word the poll reads and writes it.
///
/// The readings, the target EXECUTE, the mode write, boost and away are confirmed on one Atlantic Calypso
/// Connecte ([issue #198](https://github.com/laberning/home_io_control/issues/198)): its display follows the
/// target and mode writes, and boost and away start and end from Home Assistant.
/// That heater accepts only its own fixed targets (one per shower count, all whole degrees), so a value
/// between two steps is not taken.
///
/// \todo The mode mapping and the target steps rest on that one heater (and on its manual for the heat-pump-only
/// setting); confirm them on a second one, and confirm that the element never runs in manual ECO on.

#include "esphome/components/switch/switch.h"
#include "esphome/components/water_heater/water_heater.h"
#include "esphome/core/component.h"
#include "platform_companion_sensors.h"
#include "platform_entity_base.h"
#include "proto_device_readings.h"
#include "proto_dhw.h"

namespace esphome {
namespace home_io_control {

/// @brief ESPHome water heater mode for a heater mode.
/// @param mode Mode as register 0x0006 holds it.
/// @return The mapped ESPHome mode (see the file comment).
water_heater::WaterHeaterMode water_heater_mode_for(DhwMode mode);

/// @brief Heater mode for an ESPHome water heater mode.
/// @param mode ESPHome mode.
/// @param[out] out The heater mode; untouched when false is returned.
/// @return false for a mode the heater does not offer (OFF, ELECTRIC, HIGH_DEMAND, GAS).
bool dhw_mode_for(water_heater::WaterHeaterMode mode, DhwMode &out);

/// @brief Water heater entity for a domestic hot water heater.
/// @ingroup hioc_platforms
///
/// The device-binding setters and setup() registration ritual come from DeviceBoundEntity.
class IOHomeWaterHeater : public water_heater::WaterHeater, public Component, public DeviceBoundEntity {
 public:
  /// @brief Register the entity with the shared hub and schedule the first poll of the heater.
  void setup() override;
  /// @brief Dump configuration to the log.
  void dump_config() override;
  /// @brief Get setup priority (DATA).
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

  /// @brief A call bound to this entity (the pattern of ESPHome's template water heater).
  /// @return A new call.
  water_heater::WaterHeaterCallInternal make_call() override { return water_heater::WaterHeaterCallInternal(this); }

  /// @brief Apply a Home Assistant request: a mode is written to register 0x0006, away mode goes through the
  /// operating-mode word, a target is sent in an EXECUTE. The mode and away go first; a target is then not sent while
  /// the heater holds automatic mode (it chooses its own), judged on the mode read back after the mode write. A failed
  /// send leaves its field as it was.
  /// @param call The requested changes, already checked against traits() by ESPHome.
  void control(const water_heater::WaterHeaterCall &call) override;

 protected:
  /// @brief Current and target temperature, the three modes, and the target range
  /// DHW_TARGET_MIN_C..DHW_TARGET_MAX_C at a 1 C step.
  water_heater::WaterHeaterTraits traits() override;

  /// @brief Inbound device-update hook: fold the device's readings into the entity and publish when a shown field
  /// changed.
  void on_device_update_(const IoDevice &dev);

  /// @brief Copy the readings into the entity's fields. The target is shown as unknown (NaN) while the
  /// heater reports automatic mode or away; away follows the heater's report.
  /// @param dev What the hub has read from the heater.
  /// @return true when a published field changed.
  bool sync_from_readings_(const IoDevice &dev);

  /// @brief True when the mode shown is the one the heater's automatic mode maps to.
  [[nodiscard]] bool holds_automatic_mode_() const;

  bool observed_{false};  ///< True once a reading has been folded in.
};

/// @brief Text sensor of what heats the water: `heat pump`, `electric backup`, `heat pump and electric backup`
/// or `idle`.
/// @ingroup hioc_platforms
///
/// Built from the readings the two running binary sensors show (DeviceReading::HOT_WATER_HEAT_PUMP and
/// HOT_WATER_ELECTRIC_BACKUP; dhw_heating_source_name()). No state until the first poll has read them, so `idle`
/// always means the heater said so.
class IOHomeDhwHeatingSourceSensor : public IOHomeCompanionTextSensor {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;
  /// @brief Dump sensor configuration to the log.
  void dump_config() override;
};

/// @brief Switch that starts and ends the heater's boost.
/// @ingroup hioc_platforms
///
/// A write goes through HubHeatingPort::set_water_heater_boost(), which writes the boost duration and
/// the operating-mode word and reads the status back; the switch shows what the heater reports in that word
/// (ADR 0081), so it has no state until the first poll and never shows a command the heater did not confirm.
/// Switching on asks for a boost of duration 1, the only value seen. Switching off sends the word that ends
/// both boost and away.
///
/// It never writes at boot: setup() restores nothing, and the codegen sets restore mode DISABLED, so a
/// reboot cannot end a boost that is running.
///
/// Boost on and off are confirmed on an Atlantic Calypso Connecté (issue #198).
class IOHomeDhwBoostSwitch : public switch_::Switch, public Component, public DeviceBoundCompanion {
 public:
  /// @brief Subscribe to the device's updates and publish what is already known.
  void setup() override;
  /// @brief Dump switch configuration to the log.
  void dump_config() override;
  /// @brief Get setup priority so the parent hub is available first.
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// @brief Start or end the boost; the read-back publishes the state.
  /// @param state True to start a boost.
  void write_state(bool state) override;
};

}  // namespace home_io_control
}  // namespace esphome
