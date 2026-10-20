/// @file proto_heating_profiles.cpp
/// @brief The heating profile table and its lookups.
/// @ingroup hioc_protocol

#include "proto_heating_profiles.h"

#include "proto_sizes.h"

#include <iterator>

namespace esphome {
namespace home_io_control {

namespace {

/// Mask for the low byte of a main parameter, where a heating level sits.
constexpr uint16_t LOW_BYTE_MASK = 0xFF;

// === Electrical heater with an adjustable setpoint (52/1): private registers ===

constexpr HeatingModeEntry SETPOINT_HEATER_MODES[] = {
    {HeatingHvacMode::OFF, static_cast<uint8_t>(HeatingMode::OFF)},
    {HeatingHvacMode::HEAT, static_cast<uint8_t>(HeatingMode::MANUAL)},
    {HeatingHvacMode::AUTO, static_cast<uint8_t>(HeatingMode::AUTO)},
};

/// Index of the "Program" entry in SETPOINT_HEATER_PRESETS, named by SETPOINT_HEATER_OBSERVED.
constexpr int8_t SETPOINT_HEATER_PROGRAM_PRESET = 2;

constexpr HeatingPresetEntry SETPOINT_HEATER_PRESETS[] = {
    // Presence is written to a register nothing reads back, so these show the last command.
    {HeatingPresetId::HOME, nullptr, HeatingFunction::SET_PRESENCE, 1, true, false},
    {HeatingPresetId::AWAY, nullptr, HeatingFunction::SET_PRESENCE, 0, true, false},
    // The device's own weekly schedule is a mode of its own, not a presence.
    {HeatingPresetId::CUSTOM, "Program", HeatingFunction::SET_MODE, static_cast<uint8_t>(HeatingMode::PROG), true,
     true},
};

constexpr HeatingObservedState SETPOINT_HEATER_OBSERVED[] = {
    {static_cast<uint8_t>(HeatingMode::AUTO), HeatingHvacMode::AUTO, -1},
    {static_cast<uint8_t>(HeatingMode::MANUAL), HeatingHvacMode::HEAT, -1},
    // The schedule has no HVAC mode: auto, with the preset naming it.
    {static_cast<uint8_t>(HeatingMode::PROG), HeatingHvacMode::AUTO, SETPOINT_HEATER_PROGRAM_PRESET},
    // The setpoint comes from another controller; heat is the nearest mode. Never written here.
    {static_cast<uint8_t>(HeatingMode::EXTERNAL_SETPOINT), HeatingHvacMode::HEAT, -1},
    {static_cast<uint8_t>(HeatingMode::OFF), HeatingHvacMode::OFF, -1},
};

/// The room temperature leads: it is the value a user looks at, so a poll that dies half-way still
/// delivered it.
constexpr HeatingFunction SETPOINT_HEATER_POLL[] = {
    HeatingFunction::ROOM_TEMPERATURE, HeatingFunction::SET_TEMPERATURE, HeatingFunction::SET_MODE,
    HeatingFunction::REGULATOR_STATE,  HeatingFunction::WINDOW_OPEN,
};

/// Properties and settings that change rarely: read until they have been read once.
constexpr HeatingFunction SETPOINT_HEATER_BOOT[] = {
    HeatingFunction::MAX_SETPOINT,
    HeatingFunction::SET_OPEN_WINDOW_DETECTION,
};

// === Plain electrical heater (52/0): heating levels ===

constexpr HeatingModeEntry LEVEL_HEATER_MODES[] = {
    {HeatingHvacMode::OFF, heating_level::OFF},
    {HeatingHvacMode::HEAT, heating_level::COMFORT},
};

constexpr HeatingPresetEntry LEVEL_HEATER_PRESETS[] = {
    // write_fn is unused on this path; SET_MODE is a placeholder that is never sent.
    {HeatingPresetId::AWAY, nullptr, HeatingFunction::SET_MODE, heating_level::FROST_PROTECTION, true, true},
    {HeatingPresetId::ECO, nullptr, HeatingFunction::SET_MODE, heating_level::ECO, true, true},
    {HeatingPresetId::COMFORT, nullptr, HeatingFunction::SET_MODE, heating_level::COMFORT, true, true},
    {HeatingPresetId::BOOST, nullptr, HeatingFunction::SET_MODE, heating_level::BOOST, true, true},
    {HeatingPresetId::CUSTOM, "Comfort -1", HeatingFunction::SET_MODE, heating_level::COMFORT_MINUS_1, true, true},
    {HeatingPresetId::CUSTOM, "Comfort -2", HeatingFunction::SET_MODE, heating_level::COMFORT_MINUS_2, true, true},
    // The device reports it, a controller cannot set it.
    {HeatingPresetId::CUSTOM, "Secured", HeatingFunction::SET_MODE, heating_level::SECURED, false, true},
};

constexpr HeatingObservedState LEVEL_HEATER_OBSERVED[] = {
    {heating_level::OFF, HeatingHvacMode::OFF, -1},
    {heating_level::FROST_PROTECTION, HeatingHvacMode::HEAT, 0},
    {heating_level::ECO, HeatingHvacMode::HEAT, 1},
    {heating_level::COMFORT, HeatingHvacMode::HEAT, 2},
    {heating_level::BOOST, HeatingHvacMode::HEAT, 3},
    {heating_level::COMFORT_MINUS_1, HeatingHvacMode::HEAT, 4},
    {heating_level::COMFORT_MINUS_2, HeatingHvacMode::HEAT, 5},
    {heating_level::SECURED, HeatingHvacMode::HEAT, 6},
};

constexpr HeatingProfile HEATING_PROFILES[] = {
    {DeviceType::ELECTRICAL_HEATER,
     0,
     HeatingControlPath::HEATING_LEVEL,
     {/*target_temperature=*/false, /*current_temperature=*/false, /*action=*/false, /*window_sensor=*/false,
      /*open_window_detection=*/false},
     LEVEL_HEATER_MODES,
     std::size(LEVEL_HEATER_MODES),
     LEVEL_HEATER_PRESETS,
     std::size(LEVEL_HEATER_PRESETS),
     LEVEL_HEATER_OBSERVED,
     std::size(LEVEL_HEATER_OBSERVED),
     nullptr,
     0,
     nullptr,
     0},
    {DeviceType::ELECTRICAL_HEATER,
     1,
     HeatingControlPath::PRIVATE_REGISTERS,
     {/*target_temperature=*/true, /*current_temperature=*/true, /*action=*/true, /*window_sensor=*/true,
      /*open_window_detection=*/true},
     SETPOINT_HEATER_MODES,
     std::size(SETPOINT_HEATER_MODES),
     SETPOINT_HEATER_PRESETS,
     std::size(SETPOINT_HEATER_PRESETS),
     SETPOINT_HEATER_OBSERVED,
     std::size(SETPOINT_HEATER_OBSERVED),
     SETPOINT_HEATER_POLL,
     std::size(SETPOINT_HEATER_POLL),
     SETPOINT_HEATER_BOOT,
     std::size(SETPOINT_HEATER_BOOT)},
};

const HeatingProfile *find_exact_profile(DeviceType type, uint8_t subtype) {
  for (const HeatingProfile &profile : HEATING_PROFILES) {
    if (profile.type == type && profile.subtype == subtype)
      return &profile;
  }
  return nullptr;
}

}  // namespace

const HeatingProfile *find_heating_profile(DeviceType type, uint8_t subtype) {
  const HeatingProfile *profile = find_exact_profile(type, subtype);
  return profile != nullptr || subtype == 0 ? profile : find_exact_profile(type, 0);
}

bool device_uses_heating_registers(DeviceType type, uint8_t subtype) {
  const HeatingProfile *profile = find_heating_profile(type, subtype);
  return profile != nullptr && profile->path == HeatingControlPath::PRIVATE_REGISTERS;
}

bool device_uses_heating_levels(DeviceType type, uint8_t subtype) {
  const HeatingProfile *profile = find_heating_profile(type, subtype);
  return profile != nullptr && profile->path == HeatingControlPath::HEATING_LEVEL;
}

bool decode_heating_level(uint16_t main_parameter, uint8_t &level) {
  if ((main_parameter >> BITS_PER_BYTE) != HEATING_LEVEL_MP_PREFIX)
    return false;
  level = static_cast<uint8_t>(main_parameter & LOW_BYTE_MASK);
  return true;
}

const HeatingObservedState *find_observed_state(const HeatingProfile &profile, uint8_t wire) {
  for (size_t i = 0; i < profile.observed_count; i++) {
    if (profile.observed[i].wire == wire)
      return &profile.observed[i];
  }
  return nullptr;
}

}  // namespace home_io_control
}  // namespace esphome
