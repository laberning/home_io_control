#pragma once

/// @file platform_climate.h
/// @brief Experimental climate entity for IO-Homecontrol heating devices (CMD_WRITE_PRIVATE 0x20).
/// @ingroup hioc_platforms
///
/// Maps Home Assistant climate semantics (HVAC mode, target temperature, presets) onto the heater's
/// private registers in proto_heating.h, transmitting through the hub's heating send path
/// (IOHomeControlComponent::send_heating_command() and read_heating_register()). Both this entity
/// and the `heating_control` hub action use that one path.
///
/// State is observed, not predicted. The hub polls the heater (room temperature, setpoint, mode,
/// regulator state; default every 5 minutes) and reads the written register back after every
/// accepted write; this entity publishes from IoDevice::heating, which only a decoded reply changes.
/// Until the first read succeeds the entity shows the last commanded mode and setpoint, and nothing
/// else: current temperature and action stay unset. The HOME/AWAY presets map to a write-only
/// register, so they stay "last commanded" for good. The entity deliberately does NOT use the
/// OptimisticState overlay (ADR 0030): heating has no prediction to keep apart from an observation,
/// because there is never a prediction (ADR 0054).
///
/// @todo The reply shape of a register read is unconfirmed against real hardware (see
///       proto_heating.h); confirm with a capture from an Atlantic radiator
///       ([issue #165](https://github.com/laberning/home_io_control/issues/165)).
///
/// @warning Unvalidated on hardware. See docs/configuration/climate.md's experimental banner.

#include "esphome/components/climate/climate.h"
#include "esphome/core/component.h"
#include "platform_entity_base.h"
#include "proto_heating.h"

namespace esphome {
namespace home_io_control {

/// @brief Climate entity for IO-Homecontrol heating devices.
/// @ingroup hioc_platforms
///
/// The device-binding setters and setup() registration ritual come from DeviceBoundEntity; the
/// HA-to-protocol mapping and the publish-from-readings discipline live here.
class IOHomeClimate : public climate::Climate, public Component, public DeviceBoundEntity {
 public:
  /// @brief Register the entity with the shared hub and schedule the first poll of the heater.
  void setup() override;
  /// @brief Dump configuration to the log.
  void dump_config() override;
  /// @brief Get setup priority (DATA).
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

  /// @brief Traits: modes OFF/HEAT/AUTO, presets HOME/AWAY plus the custom preset "Program",
  /// current temperature and action, and a target range from HEATING_TEMP_MIN_C to the heater's
  /// own maximum setpoint (HEATING_TEMP_MAX_C until it has been read) at a 0.5 C step.
  climate::ClimateTraits traits() override;

  /// @brief Apply a Home Assistant climate control request.
  ///
  /// Translates the request to one or more send_heating_command() calls via a single static
  /// mapping table (climate_mode <-> HeatingMode, climate_preset <-> presence). After an accepted
  /// mode or setpoint write the register is read back; what the heater answers is what the entity
  /// then shows. Only while no read has succeeded yet does an accepted write publish the commanded
  /// value. A failed send leaves the entity state untouched.
  /// @param call The requested attribute changes.
  void control(const climate::ClimateCall &call) override;

 protected:
  /// @brief Inbound device-update hook: fold IoDevice::heating into the entity and publish when
  /// something it shows changed. Ignores updates for other devices.
  void on_device_update_(const std::string &id, const IoDevice &dev);

  /// @brief Copy the readings into the entity's fields.
  /// @param readings What the hub has read from the heater.
  /// @return true when a published field (including the traits' maximum) changed.
  bool sync_from_readings_(const HeatingReadings &readings);

  /// @brief Fold an observed HeatingMode value into the mode and the "Program" custom preset.
  /// @param heating_mode HeatingMode value read from the heater.
  /// @return true when the mode or the custom preset changed; an unknown value changes nothing.
  bool apply_observed_mode_(uint8_t heating_mode);

  /// @brief Read one register back after an accepted write so the entity shows the heater's value.
  /// @param fn Register that was just written.
  void confirm_write_(HeatingFunction fn);

  /// @brief Apply the request's HVAC mode via SET_MODE, if present. @return true if state was published.
  bool apply_mode_(const climate::ClimateCall &call);
  /// @brief Apply the "Program" custom preset via SET_MODE(prog), if present. @return true if published.
  bool apply_custom_preset_(const climate::ClimateCall &call);
  /// @brief Apply a HOME/AWAY preset via SET_PRESENCE, if present. @return true if published.
  bool apply_preset_(const climate::ClimateCall &call);
  /// @brief Apply the target temperature via SET_TEMPERATURE, if present. @return true if published.
  bool apply_target_temperature_(const climate::ClimateCall &call);

  bool observed_{false};                         ///< True once a register read has been folded in.
  float max_temperature_c_{HEATING_TEMP_MAX_C};  ///< Upper end of the target range (heater's own maximum once read).
};

}  // namespace home_io_control
}  // namespace esphome
