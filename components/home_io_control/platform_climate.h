#pragma once

/// @file platform_climate.h
/// @brief Experimental climate entity for IO-Homecontrol heaters, driven by a heating profile.
/// @ingroup hioc_platforms
///
/// Maps Home Assistant climate semantics (HVAC mode, target temperature, presets) onto the device's
/// heating profile (proto_heating_profiles.h). The profile says what the card offers and how each
/// choice reaches the device: as private registers (proto_heating.h, sent through
/// IOHomeControlComponent::send_heating_command() and read_heating_register()) or as a heating level
/// or a temperature in the standard EXECUTE. The entity maps ESPHome's call to a HeatingWriteRequest,
/// plan_heating_write() decides which of those it becomes, and IOHomeControlComponent::execute_heating_plan()
/// sends it and reads the register back. This class has no per-device-type branch; the `heating_control`
/// hub action shares the register send path.
///
/// What the card shows is a HeatingCardState that reduce_heating_card() folds from the readings and
/// command_heating_card() from an accepted command; the entity holds it in one member and maps it to
/// ESPHome's fields. It reads no profile control path.
///
/// State is observed, not predicted. The hub polls the heater (default every 5 minutes) and this
/// entity publishes from IoDevice::heating, which only a decoded reply changes. After an accepted
/// register write the register is read back; after a level write the hub's settle poll reads the
/// level. Until the first read succeeds the entity shows the last commanded mode, setpoint or preset,
/// and nothing else. The entity deliberately does NOT use the OptimisticState overlay (ADR 0030) —
/// there is never a prediction to keep apart from an observation (ADR 0054, ADR 0055).
///
/// The register path is confirmed on an Atlantic AGILIA IO radiator: setpoint and mode writes, their
/// read-back, and the poll's room temperature and regulator readings
/// ([issue #165](https://github.com/laberning/home_io_control/issues/165), corpus
/// tests/corpus/captures/exchange/atlantic_agilia_exchange_status_read.yaml).
///
/// @todo The heating-level and temperature EXECUTE paths are unconfirmed on hardware; confirm with a
///       capture from a plain heater, a heating temperature interface or a heat pump.
///
/// @warning The EXECUTE-commanded profiles are unvalidated on hardware. See
///          docs/configuration/climate.md.

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

  /// @brief Inbound device-update hook: fold IoDevice::heating into the card and publish when
  /// something it shows changed.
  void on_device_update_(const IoDevice &dev);

  /// @brief Show the card state through ESPHome's fields: mode, preset or custom preset, action and
  /// temperatures.
  /// @details The one place the card (HeatingCardState, computed by reduce_heating_card() and
  ///          command_heating_card()) meets ESPHome's types.
  /// @param profile The device's profile; it names the presets the card's index selects.
  void show_card_(const HeatingProfile &profile);

  /// @brief Plan one request with plan_heating_write() and log why it is no write.
  /// @param profile The device's profile.
  /// @param request What the card asks for.
  /// @return The plan; HeatingWritePlan::is_write() false when it was refused.
  HeatingWritePlan plan_write_(const HeatingProfile &profile, const HeatingWriteRequest &request);

  /// @brief Log the refusal of a plan that is the user's to fix (a preset the device only reports, no
  /// setpoint, no target temperature for heat); the other refusals are silent.
  void log_refusal_(const HeatingProfile &profile, const HeatingWritePlan &plan) const;

  /// @brief Apply the request's HVAC mode, if present. @return true if state was published.
  bool apply_mode_(const HeatingProfile &profile, const climate::ClimateCall &call);
  /// @brief Apply a custom preset, if present. @return true if published.
  bool apply_custom_preset_(const HeatingProfile &profile, const climate::ClimateCall &call);
  /// @brief Apply a standard preset, if present; "none" selects the profile's NONE entry where it has one.
  /// @return true if published.
  bool apply_preset_(const HeatingProfile &profile, const climate::ClimateCall &call);
  /// @brief Send one request and show it: the tail every apply path shares.
  /// @details Plans the request (a request the device cannot take is refused and logged), hands the plan to
  ///          the hub and, once the device accepted it, lets command_heating_card() say what the card
  ///          shows: before a read has succeeded the command, afterwards the read-back. A register write is
  ///          read back.
  /// @return true if the card changed and must be published.
  bool send_and_show_(const HeatingProfile &profile, const HeatingWriteRequest &request);
  /// @brief Apply the target temperature, if present. @return true if published.
  bool apply_target_temperature_(const HeatingProfile &profile, const climate::ClimateCall &call);

  HeatingCardState card_;                               ///< What the card shows; the entity's ESPHome fields follow it.
  bool has_range_{false};                               ///< YAML overrode the temperature encoding range.
  float range_min_c_{HEATING_TEMPERATURE_RANGE_MIN_C};  ///< YAML `min_temperature:`.
  float range_max_c_{HEATING_TEMPERATURE_RANGE_MAX_C};  ///< YAML `max_temperature:`.
  std::vector<const char *> custom_presets_;            ///< Custom preset names of the profile, registered in setup().
};

}  // namespace home_io_control
}  // namespace esphome
