/// @file proto_heating_profiles.cpp
/// @brief The heating profile table and its lookups.
/// @ingroup hioc_protocol

#include "proto_heating_profiles.h"

#include "proto_device_types.h"
#include "proto_sizes.h"

#include <algorithm>
#include <cmath>
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
constexpr int8_t SETPOINT_HEATER_PROGRAM_PRESET = 0;
/// Index of the "Auto" entry in SETPOINT_HEATER_PRESETS, named by SETPOINT_HEATER_OBSERVED.
constexpr int8_t SETPOINT_HEATER_AUTO_PRESET = 1;

/// Both automatic modes show as HVAC auto; these presets are how a user picks between them.
constexpr HeatingPresetEntry SETPOINT_HEATER_PRESETS[] = {
    {"Program", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, static_cast<uint8_t>(HeatingMode::PROG), true},
    {"Auto", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, static_cast<uint8_t>(HeatingMode::AUTO), true},
};

constexpr HeatingObservedState SETPOINT_HEATER_OBSERVED[] = {
    {static_cast<uint8_t>(HeatingMode::AUTO), HeatingHvacMode::AUTO, SETPOINT_HEATER_AUTO_PRESET},
    {static_cast<uint8_t>(HeatingMode::MANUAL), HeatingHvacMode::HEAT, -1},
    // AUTO and PROG both show as HVAC auto; the preset tells them apart.
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
    {nullptr, HeatingPresetId::AWAY, HeatingFunction::SET_MODE, heating_level::FROST_PROTECTION, true},
    {nullptr, HeatingPresetId::ECO, HeatingFunction::SET_MODE, heating_level::ECO, true},
    {nullptr, HeatingPresetId::COMFORT, HeatingFunction::SET_MODE, heating_level::COMFORT, true},
    {nullptr, HeatingPresetId::BOOST, HeatingFunction::SET_MODE, heating_level::BOOST, true},
    {"Comfort -1", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, heating_level::COMFORT_MINUS_1, true},
    {"Comfort -2", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, heating_level::COMFORT_MINUS_2, true},
    // The device reports it, a controller cannot set it.
    {"Secured", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, heating_level::SECURED, false},
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

// === Heating temperature interface (14/0) and heat pump (22/0): temperature in the main parameter ===

constexpr HeatingFp NO_FP{};
constexpr HeatingFp THERMOSTAT_MANUAL{HeatingFpKind::FP9, heating_mode_flags::MANUAL};
constexpr HeatingFp HEAT_PUMP_ON{HeatingFpKind::FP16, heating_fp16::ON};

/// Thermostat modes as the device's own controller sends them: off is the off marker with FP9 0, auto is
/// the comfort alias with FP9 1, heat (manual) is the current target temperature with FP9 2.
constexpr HeatingModeEntry TEMPERATURE_INTERFACE_MODES[] = {
    {HeatingHvacMode::OFF, 0, HEATING_OFF_MP, {HeatingFpKind::FP9, heating_mode_flags::OFF}},
    {HeatingHvacMode::HEAT, 0, HEATING_MP_FROM_TARGET, THERMOSTAT_MANUAL},
    {HeatingHvacMode::AUTO,
     0,
     heating_prefixed_main_parameter(HEATING_ALIAS_MP_PREFIX, heating_alias::COMFORT),
     {HeatingFpKind::FP9, heating_mode_flags::AUTO}},
};

/// A setpoint alias is selected by its main parameter alone, with no functional parameter.
constexpr HeatingPresetEntry TEMPERATURE_INTERFACE_PRESETS[] = {
    {nullptr, HeatingPresetId::COMFORT, HeatingFunction::SET_MODE, heating_alias::COMFORT, true, NO_FP},
    {nullptr, HeatingPresetId::ECO, HeatingFunction::SET_MODE, heating_alias::THERMOSTAT_ECO, true, NO_FP},
    {"Secured", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, heating_alias::SECURED, true, NO_FP},
};

/// The mode is the last command (nothing reads it back), so the hvac column is not consulted; the rows
/// only name the preset an alias stands for.
constexpr HeatingObservedState TEMPERATURE_INTERFACE_OBSERVED[] = {
    {heating_alias::COMFORT, HeatingHvacMode::HEAT, 0},
    {heating_alias::THERMOSTAT_ECO, HeatingHvacMode::HEAT, 1},
    {heating_alias::SECURED, HeatingHvacMode::HEAT, 2},
};

/// Heat pump: off is the off marker with FP16 off; on is the comfort alias with FP16 on, which is also
/// what a preset sends. No mode selects cooling.
constexpr HeatingModeEntry HEAT_PUMP_MODES[] = {
    {HeatingHvacMode::OFF, 0, HEATING_OFF_MP, {HeatingFpKind::FP16, heating_fp16::OFF}},
    {HeatingHvacMode::HEAT, 0, heating_prefixed_main_parameter(HEATING_ALIAS_MP_PREFIX, heating_alias::COMFORT),
     HEAT_PUMP_ON},
};

constexpr HeatingPresetEntry HEAT_PUMP_PRESETS[] = {
    {nullptr, HeatingPresetId::COMFORT, HeatingFunction::SET_MODE, heating_alias::COMFORT, true, HEAT_PUMP_ON},
    {nullptr, HeatingPresetId::ECO, HeatingFunction::SET_MODE, heating_alias::HEAT_PUMP_ECO, true, HEAT_PUMP_ON},
    {"Setback", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, heating_alias::HEAT_PUMP_SETBACK, true,
     HEAT_PUMP_ON},
    {"Halted", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, heating_alias::HEAT_PUMP_HALTED, true, HEAT_PUMP_ON},
};

constexpr HeatingObservedState HEAT_PUMP_OBSERVED[] = {
    {heating_alias::COMFORT, HeatingHvacMode::HEAT, 0},
    {heating_alias::HEAT_PUMP_ECO, HeatingHvacMode::HEAT, 1},
    {heating_alias::HEAT_PUMP_SETBACK, HeatingHvacMode::HEAT, 2},
    {heating_alias::HEAT_PUMP_HALTED, HeatingHvacMode::HEAT, 3},
};

constexpr float TEMPERATURE_VISUAL_MIN_C = 5.0F;   ///< Lowest target the card offers for a temperature profile.
constexpr float TEMPERATURE_VISUAL_MAX_C = 30.0F;  ///< Highest target the card offers for a temperature profile.

constexpr HeatingProfile HEATING_PROFILES[] = {
    {DeviceType::HEATING_TEMPERATURE_INTERFACE,
     0,
     HeatingControlPath::TEMPERATURE_EXECUTE,
     {/*target_temperature=*/true, /*current_temperature=*/false, /*action=*/false, /*mode_observed=*/false},
     HEATING_ALIAS_MP_PREFIX,
     TEMPERATURE_VISUAL_MIN_C,
     TEMPERATURE_VISUAL_MAX_C,
     THERMOSTAT_MANUAL,
     /*fixed_range=*/false,
     TEMPERATURE_INTERFACE_MODES,
     std::size(TEMPERATURE_INTERFACE_MODES),
     TEMPERATURE_INTERFACE_PRESETS,
     std::size(TEMPERATURE_INTERFACE_PRESETS),
     TEMPERATURE_INTERFACE_OBSERVED,
     std::size(TEMPERATURE_INTERFACE_OBSERVED),
     nullptr,
     0,
     nullptr,
     0},
    {DeviceType::HEAT_PUMP,
     0,
     HeatingControlPath::TEMPERATURE_EXECUTE,
     {/*target_temperature=*/true, /*current_temperature=*/false, /*action=*/false, /*mode_observed=*/false},
     HEATING_ALIAS_MP_PREFIX,
     TEMPERATURE_VISUAL_MIN_C,
     TEMPERATURE_VISUAL_MAX_C,
     NO_FP,
     /*fixed_range=*/true,
     HEAT_PUMP_MODES,
     std::size(HEAT_PUMP_MODES),
     HEAT_PUMP_PRESETS,
     std::size(HEAT_PUMP_PRESETS),
     HEAT_PUMP_OBSERVED,
     std::size(HEAT_PUMP_OBSERVED),
     nullptr,
     0,
     nullptr,
     0},
    {DeviceType::ELECTRICAL_HEATER,
     0,
     HeatingControlPath::HEATING_LEVEL,
     {/*target_temperature=*/false, /*current_temperature=*/false, /*action=*/false, /*mode_observed=*/true},
     HEATING_LEVEL_MP_PREFIX,
     0.0F,
     0.0F,
     NO_FP,
     /*fixed_range=*/false,
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
     {/*target_temperature=*/true, /*current_temperature=*/true, /*action=*/true, /*mode_observed=*/true},
     0,
     HEATING_TEMP_MIN_C,
     HEATING_TEMP_MAX_C,
     NO_FP,
     /*fixed_range=*/false,
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
  if (profile != nullptr || subtype == 0)
    return profile;
  // A subtype the type table names on its own (the Atlantic heat pumps, 22/1 and 22/2) is a different
  // device from the type's own row and must not inherit its profile; only an unnamed subtype does.
  return find_exact_device_type_info(type, subtype) == nullptr ? find_exact_profile(type, 0) : nullptr;
}

bool device_uses_heating_registers(DeviceType type, uint8_t subtype) {
  const HeatingProfile *profile = find_heating_profile(type, subtype);
  return profile != nullptr && profile->path == HeatingControlPath::PRIVATE_REGISTERS;
}

bool device_uses_heating_levels(DeviceType type, uint8_t subtype) {
  const HeatingProfile *profile = find_heating_profile(type, subtype);
  return profile != nullptr && profile->path == HeatingControlPath::HEATING_LEVEL;
}

bool device_uses_heating_temperature(DeviceType type, uint8_t subtype) {
  const HeatingProfile *profile = find_heating_profile(type, subtype);
  return profile != nullptr && profile->path == HeatingControlPath::TEMPERATURE_EXECUTE;
}

bool encode_temperature_main_parameter(float celsius, float min_c, float max_c, uint16_t &main_parameter) {
  if (!std::isfinite(celsius) || !std::isfinite(min_c) || !std::isfinite(max_c) || !(max_c > min_c))
    return false;
  const float steps = std::round((celsius - min_c) / (max_c - min_c) * static_cast<float>(HEATING_TEMPERATURE_MP_SPAN));
  main_parameter = static_cast<uint16_t>(std::clamp(steps, 0.0F, static_cast<float>(HEATING_TEMPERATURE_MP_SPAN)));
  return true;
}

bool decode_temperature_main_parameter(uint16_t main_parameter, float min_c, float max_c, float &celsius) {
  if (main_parameter > HEATING_TEMPERATURE_MP_SPAN || !std::isfinite(min_c) || !std::isfinite(max_c) ||
      !(max_c > min_c))
    return false;
  celsius =
      min_c + (static_cast<float>(main_parameter) / static_cast<float>(HEATING_TEMPERATURE_MP_SPAN) * (max_c - min_c));
  return true;
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
