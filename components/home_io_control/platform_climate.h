#pragma once

/// @file platform_climate.h
/// @brief Experimental climate entity for IO-Homecontrol heaters, driven by a heating profile.
/// @ingroup hioc_platforms
///
/// Maps Home Assistant climate semantics (HVAC mode, target temperature, presets) onto the device's
/// heating profile (proto_heating_profiles.h). The profile says what the card offers and how each
/// choice reaches the device: as private registers (proto_heating.h, sent through
/// IOHomeControlComponent::send_heating_command() and read_heating_register()) or as a heating level
/// in the standard EXECUTE (IOHomeControlComponent::set_heating_level()). This class has no
/// per-device-type branch; the `heating_control` hub action shares the register send path.
///
/// State is observed, not predicted. The hub polls the heater (default every 5 minutes) and this
/// entity publishes from IoDevice::heating, which only a decoded reply changes. After an accepted
/// register write the register is read back; after a level write the hub's settle poll reads the
/// level. Until the first read succeeds the entity shows the last commanded mode, setpoint or preset,
/// and nothing else. A preset that no register reports back (presence) stays "last commanded". The
/// entity deliberately does NOT use the OptimisticState overlay (ADR 0030) — there is never a
/// prediction to keep apart from an observation (ADR 0054, ADR 0055).
///
/// @todo The reply shape of a register read and the heating-level EXECUTE are unconfirmed against real
///       hardware; confirm with a capture from an Atlantic radiator
///       ([issue #165](https://github.com/laberning/home_io_control/issues/165)) and a plain heater.
///
/// @warning Unvalidated on hardware. See docs/configuration/climate.md's experimental banner.

#include "esphome/components/climate/climate.h"
#include "esphome/core/component.h"
#include "platform_entity_base.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"

#include <vector>

namespace esphome {
namespace home_io_control {

/// @brief Climate entity for IO-Homecontrol heating devices.
/// @ingroup hioc_platforms
///
/// The device-binding setters and setup() registration ritual come from DeviceBoundEntity; the
/// profile-to-ESPHome mapping and the publish-from-readings discipline live here.
class IOHomeClimate : public climate::Climate, public Component, public DeviceBoundEntity {
 public:
  /// @brief Register the entity with the shared hub and schedule the first poll of the heater.
  void setup() override;
  /// @brief Dump configuration to the log.
  void dump_config() override;
  /// @brief Get setup priority (DATA).
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

  /// @brief Traits from the device's heating profile: its HVAC modes and presets, current temperature
  /// and action where the profile has them, and a target range from HEATING_TEMP_MIN_C to the
  /// heater's own maximum setpoint (HEATING_TEMP_MAX_C until it has been read) at a 0.5 C step. A
  /// device whose type is not known yet gets the off mode only.
  climate::ClimateTraits traits() override;

  /// @brief Apply a Home Assistant climate control request.
  ///
  /// Each requested mode, preset and setpoint is looked up in the profile and sent over its control
  /// path. After an accepted register write the register is read back; what the heater answers is
  /// what the entity then shows. Only while no read has succeeded yet does an accepted write publish
  /// the commanded value. A failed send leaves the entity state untouched.
  /// @param call The requested attribute changes.
  void control(const climate::ClimateCall &call) override;

  /// @brief Override the temperature encoding range (YAML `min_temperature:` / `max_temperature:`) of a
  /// heater that carries a temperature in its main parameter; applied to the device in setup().
  /// @param min_c Lower end of the range in degrees Celsius.
  /// @param max_c Upper end of the range.
  void set_temperature_range(float min_c, float max_c) {
    this->range_min_c_ = min_c;
    this->range_max_c_ = max_c;
    this->has_range_ = true;
  }

 protected:
  /// @brief The device's heating profile: from the declared type, else from the type the hub learned.
  /// @return The profile, or nullptr while the type is unknown.
  [[nodiscard]] const HeatingProfile *profile_() const;

  /// @brief Inbound device-update hook: fold IoDevice::heating into the entity and publish when
  /// something it shows changed. Ignores updates for other devices.
  void on_device_update_(const std::string &id, const IoDevice &dev);

  /// @brief Copy the readings into the entity's fields.
  /// @param profile The device's profile.
  /// @param readings What the hub has read from the heater.
  /// @return true when a published field (including the traits' maximum) changed.
  bool sync_from_readings_(const HeatingProfile &profile, const HeatingReadings &readings);

  /// @brief Fold the temperatures (current, setpoint, maximum) of the readings into the entity.
  /// @param profile The device's profile.
  /// @param readings What the hub has read from the heater.
  /// @param[in,out] seen Set when a reading was present.
  /// @return true when a published field changed.
  bool sync_temperatures_(const HeatingProfile &profile, const HeatingReadings &readings, bool &seen);

  /// @brief Fold the regulator state into the entity's action.
  /// @param profile The device's profile.
  /// @param readings What the hub has read from the heater.
  /// @param mode_known True when the mode or level has been read (an off heater is then not idle).
  /// @param[in,out] seen Set when a reading was present.
  /// @return true when the action changed.
  bool sync_action_(const HeatingProfile &profile, const HeatingReadings &readings, bool mode_known, bool &seen);

  /// @brief Show a device-reported mode register value or heating level.
  /// @param profile The device's profile.
  /// @param wire Value as the device reports it.
  /// @return true when the mode or a preset changed; a value the profile does not map changes nothing.
  bool apply_observed_state_(const HeatingProfile &profile, uint8_t wire);

  /// @brief Send one preset value (or, on the register and level paths, one mode value) over the
  /// profile's control path.
  /// @param profile The device's profile.
  /// @param fn Register to write (PRIVATE_REGISTERS only).
  /// @param wire Register value, or the low byte of a level or alias.
  /// @return true when the device accepted it.
  bool write_(const HeatingProfile &profile, HeatingFunction fn, uint8_t wire);

  /// @brief Send one preset over the profile's control path: a register or level write, or, for a
  /// TEMPERATURE_EXECUTE heater, its alias as the main parameter with the entry's functional parameter.
  /// @param profile The device's profile.
  /// @param entry The preset entry.
  /// @return true when the device accepted it.
  bool write_preset_(const HeatingProfile &profile, const HeatingPresetEntry &entry);

  /// @brief Encode a target temperature as a main parameter over the device's range (the fixed range
  /// for a profile that has one).
  /// @param profile The device's profile.
  /// @param celsius The target.
  /// @param[out] main_parameter The encoded value.
  /// @return false when the temperature or the range cannot be encoded.
  bool encode_target_(const HeatingProfile &profile, float celsius, uint16_t &main_parameter);

  /// @brief Send an HVAC mode to a TEMPERATURE_EXECUTE heater: the mode entry's main parameter (the
  /// target temperature for heat on a thermostat) with its functional parameter.
  /// @param profile The device's profile.
  /// @param entry The mode entry.
  /// @return true when the device accepted it; false when it did not, or when heat needs a target
  ///         temperature that is not known yet.
  bool write_temperature_mode_(const HeatingProfile &profile, const HeatingModeEntry &entry);

  /// @brief Read one register back after an accepted write so the entity shows the heater's value.
  /// @param fn Register that was just written.
  void confirm_write_(HeatingFunction fn);

  /// @brief Apply the request's HVAC mode, if present. @return true if state was published.
  bool apply_mode_(const HeatingProfile &profile, const climate::ClimateCall &call);
  /// @brief Apply a custom preset, if present. @return true if published.
  bool apply_custom_preset_(const HeatingProfile &profile, const climate::ClimateCall &call);
  /// @brief Apply a standard preset, if present. @return true if published.
  bool apply_preset_(const HeatingProfile &profile, const climate::ClimateCall &call);
  /// @brief Apply the target temperature, if present. @return true if published.
  bool apply_target_temperature_(const HeatingProfile &profile, const climate::ClimateCall &call);

  bool observed_{false};                                ///< True once a reading has been folded in.
  bool has_range_{false};                               ///< YAML overrode the temperature encoding range.
  float range_min_c_{HEATING_TEMPERATURE_RANGE_MIN_C};  ///< YAML `min_temperature:`.
  float range_max_c_{HEATING_TEMPERATURE_RANGE_MAX_C};  ///< YAML `max_temperature:`.
  bool mode_commanded_{false};                   ///< A mode was commanded (matters where nothing reads the mode back).
  float max_temperature_c_{HEATING_TEMP_MAX_C};  ///< Upper end of the target range (heater's own maximum once read).
  std::vector<const char *> custom_presets_;     ///< Custom preset names of the profile, registered in setup().
};

}  // namespace home_io_control
}  // namespace esphome
