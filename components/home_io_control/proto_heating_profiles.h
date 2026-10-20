#pragma once

/// @file proto_heating_profiles.h
/// @brief Heating profiles: what a climate card offers for each kind of heater, and how.
/// @ingroup hioc_protocol
///
/// A profile is one row per (node type, subtype) that a `climate:` entity can drive. It names the
/// control path (the private register protocol of proto_heating.h, or the standard EXECUTE with a
/// heating level as the main parameter), the HVAC modes and presets the card shows, how each maps to
/// a wire value in both directions, which registers a poll reads and which companion entities exist.
/// The platform asks the profile and never branches on a device type.
///
/// The enums here are protocol-side: they carry no ESPHome type. The platform maps HeatingHvacMode
/// and HeatingPresetId onto ESPHome's climate modes and presets.
///
/// \todo Heating-level control of the plain electrical heater (node type 52, subtype 0) has no capture
///       or hardware report yet. Confirm with one before calling it supported.
/// \todo Every 52/1 seen is an Atlantic radiator. A non-Atlantic 52/1 would need the heating-level
///       path instead; the profile keys on (type, subtype) only.

#include "proto_device_model.h"
#include "proto_heating.h"
#include "proto_sizes.h"

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace home_io_control {

/// @brief How a heater is commanded and read.
enum class HeatingControlPath : uint8_t {
  PRIVATE_REGISTERS,  ///< CMD_WRITE_PRIVATE register reads and writes (proto_heating.h).
  HEATING_LEVEL,      ///< Standard EXECUTE with a heating level as the main parameter; read with 0x03.
};

/// @brief HVAC modes a profile can offer.
enum class HeatingHvacMode : uint8_t {
  OFF,   ///< Heater off.
  HEAT,  ///< Heating on.
  AUTO,  ///< The device decides.
};

/// @brief Presets a profile can offer. CUSTOM entries carry their own name.
enum class HeatingPresetId : uint8_t {
  HOME,     ///< Someone is in the room.
  AWAY,     ///< Nobody is in the room (the frost-protection level on a level heater).
  COMFORT,  ///< Comfort level.
  ECO,      ///< Economy level.
  BOOST,    ///< Boost level.
  CUSTOM,   ///< Named by HeatingPresetEntry::custom_name.
};

/// @brief Main-parameter prefix byte of a heating level: the level occupies the low byte.
static constexpr uint8_t HEATING_LEVEL_MP_PREFIX = 0xFC;

/// @brief Heating levels (low byte of the main parameter, prefix HEATING_LEVEL_MP_PREFIX).
namespace heating_level {
static constexpr uint8_t OFF = 0x00;               ///< Off.
static constexpr uint8_t FROST_PROTECTION = 0x01;  ///< Frost protection.
static constexpr uint8_t ECO = 0x02;               ///< Economy.
static constexpr uint8_t COMFORT_MINUS_2 = 0x03;   ///< Comfort, two degrees lower.
static constexpr uint8_t COMFORT_MINUS_1 = 0x04;   ///< Comfort, one degree lower.
static constexpr uint8_t COMFORT = 0x05;           ///< Comfort.
static constexpr uint8_t BOOST = 0x07;             ///< Boost.
static constexpr uint8_t SECURED = 0x3F;           ///< Secured; reported by the device, never commanded.
}  // namespace heating_level

/// @brief One HVAC mode the card offers and the wire value that selects it.
struct HeatingModeEntry {
  HeatingHvacMode hvac;  ///< Mode shown on the card.
  uint8_t wire;          ///< PRIVATE_REGISTERS: mode register value. HEATING_LEVEL: level low byte.
};

/// @brief One preset the card offers.
struct HeatingPresetEntry {
  HeatingPresetId id;        ///< Standard preset, or CUSTOM.
  const char *custom_name;   ///< Name of a CUSTOM preset; nullptr otherwise.
  HeatingFunction write_fn;  ///< PRIVATE_REGISTERS: register the preset writes. Unused for levels.
  uint8_t wire;              ///< Value written: register value, or level low byte.
  bool commandable;          ///< False for a preset the device only reports (secured).
  bool observed;             ///< False when no register reports it back (presence): the card then
                             ///< shows the last command.
};

/// @brief What a device-reported wire value shows on the card.
struct HeatingObservedState {
  uint8_t wire;          ///< Mode register value, or level low byte, as the device reports it.
  HeatingHvacMode hvac;  ///< HVAC mode shown.
  int8_t preset;         ///< Index into the profile's presets shown with it, or -1 for none.
};

/// @brief Companion entities and traits a profile has.
struct HeatingProfileFeatures {
  bool target_temperature;     ///< The card has a setpoint.
  bool current_temperature;    ///< The card shows a measured temperature.
  bool action;                 ///< The card shows heating / idle.
  bool window_sensor;          ///< A Window binary sensor exists (0x010E).
  bool open_window_detection;  ///< An open-window-detection switch exists (0x0106).
};

/// @brief One heater kind.
struct HeatingProfile {
  DeviceType type;                        ///< Node type.
  uint8_t subtype;                        ///< Subtype (0 = the type's own row).
  HeatingControlPath path;                ///< How the heater is commanded and read.
  HeatingProfileFeatures features;        ///< What the card and its companions offer.
  const HeatingModeEntry *modes;          ///< HVAC modes offered, with the value each writes.
  size_t mode_count;                      ///< Number of entries in `modes`.
  const HeatingPresetEntry *presets;      ///< Presets offered.
  size_t preset_count;                    ///< Number of entries in `presets`.
  const HeatingObservedState *observed;   ///< Device-reported value -> card state.
  size_t observed_count;                  ///< Number of entries in `observed`.
  const HeatingFunction *poll_registers;  ///< PRIVATE_REGISTERS: registers every poll reads, in order.
  size_t poll_register_count;             ///< Number of entries in `poll_registers`.
  const HeatingFunction *boot_registers;  ///< PRIVATE_REGISTERS: registers read once until they have been read.
  size_t boot_register_count;             ///< Number of entries in `boot_registers`.
};

/// @brief Look up the heating profile for a (type, subtype).
/// @param type Node type.
/// @param subtype Subtype; one with no profile of its own falls back to the type's profile (subtype 0),
///        the same rule as find_device_type_info().
/// @return The profile, or nullptr when no climate entity drives this device.
const HeatingProfile *find_heating_profile(DeviceType type, uint8_t subtype);

/// @brief Does a climate entity drive this device over the private register protocol?
/// @details The gate for every CMD_WRITE_PRIVATE frame the hub sends: a heater that takes the standard
///          EXECUTE must never be sent a register write.
/// @param type Node type.
/// @param subtype Subtype.
/// @return true when the profile's control path is HeatingControlPath::PRIVATE_REGISTERS.
bool device_uses_heating_registers(DeviceType type, uint8_t subtype);

/// @brief Does a climate entity drive this device with heating levels?
/// @param type Node type.
/// @param subtype Subtype.
/// @return true when the profile's control path is HeatingControlPath::HEATING_LEVEL.
bool device_uses_heating_levels(DeviceType type, uint8_t subtype);

/// @brief Main parameter of a heating level, as it travels in an EXECUTE and in a status reply.
/// @param level Level low byte (heating_level::*).
/// @return HEATING_LEVEL_MP_PREFIX in the high byte, `level` in the low byte.
constexpr uint16_t heating_level_main_parameter(uint8_t level) {
  return static_cast<uint16_t>((HEATING_LEVEL_MP_PREFIX << BITS_PER_BYTE) | level);
}

/// @brief Read a heating level out of a main parameter.
/// @param main_parameter The 16-bit value from a status reply.
/// @param[out] level Level low byte.
/// @return true when the value is a heating level (high byte HEATING_LEVEL_MP_PREFIX).
bool decode_heating_level(uint16_t main_parameter, uint8_t &level);

/// @brief Find what a device-reported wire value shows on the card.
/// @param profile The device's profile.
/// @param wire Mode register value or level low byte.
/// @return The matching entry, or nullptr when the value is not one the profile maps.
const HeatingObservedState *find_observed_state(const HeatingProfile &profile, uint8_t wire);

}  // namespace home_io_control
}  // namespace esphome
