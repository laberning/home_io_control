#pragma once

/// @file platform_climate.h
/// @brief Experimental climate entity for IO-Homecontrol heating devices (CMD_WRITE_PRIVATE 0x20).
/// @ingroup hioc_platforms
///
/// Maps Home Assistant climate semantics (HVAC mode, target temperature, presets) onto the six
/// 2W heating functions in proto_heating.h, transmitting through the hub's single shared heating
/// send path (IOHomeControlComponent::send_heating_command()). Both this entity and the
/// `heating_control` hub action use that one path.
///
/// Write-only state: the `set_*` functions are write-only — nothing decodes what the radiator
/// actually did into this entity. (`power_on` and `midnight_sync` are register *reads*; their
/// 0x21 ACK payload is logged at DEBUG but not decoded into any field.) The entity publishes only
/// what it has just successfully sent ("last commanded, never confirmed") and never at request
/// time; there is no status poll and no current temperature. It deliberately does NOT use the
/// OptimisticState overlay (ADR 0030) — that machinery keeps a prediction apart from a competing
/// observation, and heating has no observation stream for it to sit against.
///
/// @warning Unvalidated on hardware. The protocol is derived from the iohomecontrol project's
///          Cozytouch support (Atlantic / Thermor / Sauter radiators); no such device has ever
///          been exercised. See docs/home_io_control.md's experimental banner.

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
/// HA-to-protocol mapping and the write-only publish discipline live here.
class IOHomeClimate : public climate::Climate, public Component, public DeviceBoundEntity {
 public:
  /// @brief Register the entity with the shared hub (no status poll — heating has no readback).
  void setup() override;
  /// @brief Dump configuration to the log.
  void dump_config() override;
  /// @brief Get setup priority (DATA).
  /// @return setup_priority::DATA.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::DATA; }

  /// @brief Static traits: modes OFF/HEAT/AUTO, presets HOME/AWAY, temperature range
  /// HEATING_TEMP_MIN_C..HEATING_TEMP_MAX_C (7.0..28.0 C) at a 0.5 C step. No current
  /// temperature (no sensor evidence in the protocol).
  climate::ClimateTraits traits() override;

  /// @brief Apply a Home Assistant climate control request.
  ///
  /// Translates the request to one or more send_heating_command() calls via a single static
  /// mapping table (climate_mode <-> HeatingMode, climate_preset <-> presence). Publishes each
  /// changed field only after its send returns success; a failed send leaves the entity state
  /// untouched.
  /// @param call The requested attribute changes.
  void control(const climate::ClimateCall &call) override;

 protected:
  /// @brief Inbound device-update hook. Intentionally empty: a write-only heating device produces
  /// no state this entity can publish. Link health / active issue reach the user through the
  /// companion diagnostic sensors instead.
  void on_device_update_(const std::string &id, const IoDevice &dev);

  /// @brief Apply the request's HVAC mode via SET_MODE, if present. @return true if state was published.
  bool apply_mode_(const climate::ClimateCall &call);
  /// @brief Apply the "Program" custom preset via SET_MODE(prog), if present. @return true if published.
  bool apply_custom_preset_(const climate::ClimateCall &call);
  /// @brief Apply a HOME/AWAY preset via SET_PRESENCE, if present. @return true if published.
  bool apply_preset_(const climate::ClimateCall &call);
  /// @brief Apply the target temperature via SET_TEMPERATURE, if present. @return true if published.
  bool apply_target_temperature_(const climate::ClimateCall &call);
};

}  // namespace home_io_control
}  // namespace esphome
