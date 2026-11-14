/// @file proto_heating_profiles.cpp
/// @brief The heating profile table and its lookups.
/// @ingroup hioc_protocol

#include "proto_heating_profiles.h"

#include "proto_device_types.h"
#include "proto_sizes.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <span>

namespace esphome {
namespace home_io_control {

namespace {

/// Mask for the low byte of a main parameter, where a heating level sits.
constexpr uint16_t LOW_BYTE_MASK = 0xFF;

/// Wire scale of the temperature registers: the device holds whole tenths of a degree.
constexpr float TENTHS_PER_DEGREE = 10.0F;

/// The card shows no preset for a device-reported value.
constexpr int8_t NO_PRESET = -1;

/// Compile-time failure for preset_index(): a non-constexpr call is not a constant expression, so a
/// lookup that reaches it stops the build.
void preset_missing_from_list() {}

/// @return Whether two optional names are the same text (nullptr equals nullptr only).
constexpr bool same_name(const char *a, const char *b) {
  if (a == nullptr || b == nullptr)
    return a == b;
  while (*a != '\0' && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

/// @brief Index of a preset in a profile's preset list, found by its id and, for CUSTOM, its name.
/// @details Evaluated at compile time, so an observed state that names a preset the list lacks (or a
///          misspelled custom name) fails the build, and reordering the list moves every index with it.
/// @param presets The profile's presets.
/// @param id Preset id to find.
/// @param custom_name Name of a CUSTOM preset; nullptr for a standard one.
consteval int8_t preset_index(std::span<const HeatingPresetEntry> presets, HeatingPresetId id,
                              const char *custom_name = nullptr) {
  for (size_t i = 0; i < presets.size(); i++) {
    if (presets[i].id == id && same_name(presets[i].custom_name, custom_name))
      return static_cast<int8_t>(i);
  }
  preset_missing_from_list();
  return NO_PRESET;
}

// === Electrical heater with an adjustable setpoint (52/1): private registers ===

constexpr HeatingModeEntry SETPOINT_HEATER_MODES[] = {
    {HeatingHvacMode::OFF, static_cast<uint8_t>(HeatingMode::OFF)},
    {HeatingHvacMode::HEAT, static_cast<uint8_t>(HeatingMode::MANUAL)},
    {HeatingHvacMode::AUTO, static_cast<uint8_t>(HeatingMode::AUTO)},
};

/// Both automatic modes show as HVAC auto; these presets are how a user picks between them.
constexpr HeatingPresetEntry SETPOINT_HEATER_PRESETS[] = {
    {"Program", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, static_cast<uint8_t>(HeatingMode::PROG), true},
    {"Auto", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, static_cast<uint8_t>(HeatingMode::AUTO), true},
};

constexpr HeatingObservedState SETPOINT_HEATER_OBSERVED[] = {
    {static_cast<uint8_t>(HeatingMode::AUTO), HeatingHvacMode::AUTO,
     preset_index(SETPOINT_HEATER_PRESETS, HeatingPresetId::CUSTOM, "Auto")},
    {static_cast<uint8_t>(HeatingMode::MANUAL), HeatingHvacMode::HEAT, NO_PRESET},
    // AUTO and PROG both show as HVAC auto; the preset tells them apart.
    {static_cast<uint8_t>(HeatingMode::PROG), HeatingHvacMode::AUTO,
     preset_index(SETPOINT_HEATER_PRESETS, HeatingPresetId::CUSTOM, "Program")},
    // The setpoint comes from another controller; heat is the nearest mode. Never written here.
    {static_cast<uint8_t>(HeatingMode::EXTERNAL_SETPOINT), HeatingHvacMode::HEAT, NO_PRESET},
    {static_cast<uint8_t>(HeatingMode::OFF), HeatingHvacMode::OFF, NO_PRESET},
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

// === Heater with a night light (52/1, model HEATER_WITH_LIGHT): the radiator plus its light ===

/// The light's state and brightness, read after the radiator's own poll. Both change from the heater's
/// own controller (issue #222: a Cozytouch switches the light), so they are read on every poll, not once.
constexpr HeatingFunction HEATER_WITH_LIGHT_EXTRA_POLL[] = {
    HeatingFunction::NIGHT_LIGHT,
    HeatingFunction::NIGHT_LIGHT_BRIGHTNESS,
};

// === Towel dryer (52/1, model TOWEL_DRYER): private registers of its own ===
//
// Register values as two Atlantic Adelis units report them (issue #202, corpus
// atlantic_adelis_probe_*): the mode in 0x0136, boost and drying in 0x013E, separate from the mode.

/// PROG is the device's own program, so it is HVAC auto; there is no separate "Program" preset as on
/// the radiator. Value 1 of the register has never been seen and is not offered.
constexpr HeatingModeEntry TOWEL_DRYER_MODES[] = {
    {HeatingHvacMode::OFF, static_cast<uint8_t>(TowelDryerMode::OFF)},
    {HeatingHvacMode::HEAT, static_cast<uint8_t>(TowelDryerMode::MANUAL)},
    {HeatingHvacMode::AUTO, static_cast<uint8_t>(TowelDryerMode::PROG)},
};

/// The wire value of a preset the device only reports: it has no register value of its own (the period
/// comes from TOWEL_DRYER_PROGRAMME_PERIOD) and is never written.
constexpr uint8_t TOWEL_DRYER_NO_WIRE = 0;

/// Boost and drying are timed runs on top of the mode; "none" ends the one that runs. Comfort and eco are
/// the periods of the device's own programme: shown while it runs, never commanded.
constexpr HeatingPresetEntry TOWEL_DRYER_PRESETS[] = {
    {nullptr, HeatingPresetId::NONE, HeatingFunction::TEMPORARY_STATE,
     static_cast<uint8_t>(HeatingTemporaryState::NONE), true},
    {nullptr, HeatingPresetId::BOOST, HeatingFunction::TEMPORARY_STATE,
     static_cast<uint8_t>(HeatingTemporaryState::BOOST), true},
    {"Drying", HeatingPresetId::CUSTOM, HeatingFunction::TEMPORARY_STATE,
     static_cast<uint8_t>(HeatingTemporaryState::DRYING), true},
    {nullptr, HeatingPresetId::COMFORT, HeatingFunction::TEMPORARY_STATE, TOWEL_DRYER_NO_WIRE, false},
    {nullptr, HeatingPresetId::ECO, HeatingFunction::TEMPORARY_STATE, TOWEL_DRYER_NO_WIRE, false},
};

/// The mode register names the mode only; TOWEL_DRYER_OBSERVED_PRESETS names the preset.
constexpr HeatingObservedState TOWEL_DRYER_OBSERVED[] = {
    {static_cast<uint8_t>(TowelDryerMode::OFF), HeatingHvacMode::OFF, NO_PRESET},
    {static_cast<uint8_t>(TowelDryerMode::PROG), HeatingHvacMode::AUTO, NO_PRESET},
    {static_cast<uint8_t>(TowelDryerMode::MANUAL), HeatingHvacMode::HEAT, NO_PRESET},
};

/// Temporary state -> preset. The hvac column is not consulted for a preset table. Comfort and eco have no
/// row: the temporary state does not tell them apart.
constexpr HeatingObservedState TOWEL_DRYER_OBSERVED_PRESETS[] = {
    {static_cast<uint8_t>(HeatingTemporaryState::NONE), HeatingHvacMode::HEAT,
     preset_index(TOWEL_DRYER_PRESETS, HeatingPresetId::NONE)},
    {static_cast<uint8_t>(HeatingTemporaryState::BOOST), HeatingHvacMode::HEAT,
     preset_index(TOWEL_DRYER_PRESETS, HeatingPresetId::BOOST)},
    {static_cast<uint8_t>(HeatingTemporaryState::DRYING), HeatingHvacMode::HEAT,
     preset_index(TOWEL_DRYER_PRESETS, HeatingPresetId::CUSTOM, "Drying")},
};

/// Room temperature first, as on the radiator. The setpoint is 0x0103, which a climate write is known to
/// change on this device (issue #202); the window registers are not read, the dryer has no window function.
constexpr HeatingFunction TOWEL_DRYER_POLL[] = {
    HeatingFunction::ROOM_TEMPERATURE, HeatingFunction::SET_TEMPERATURE, HeatingFunction::TOWEL_MODE,
    HeatingFunction::TEMPORARY_STATE,  HeatingFunction::WORKING_RATE,
};

/// The maximum setpoint, and the eco setback that tells an eco period from a comfort one. The setback is
/// read again when the temperature in force fits neither period (the user changed it on the display).
constexpr HeatingFunction TOWEL_DRYER_BOOT[] = {
    HeatingFunction::MAX_SETPOINT,
    HeatingFunction::ECO_SETBACK,
};

/// The programme runs when the mode register reads PROG and no boost or drying is on top of it. The
/// temperature in force then is the comfort setpoint in a comfort period and the comfort setpoint minus the
/// setback in an eco period.
/// \todo Both periods are read on one unit (BD684B); a second unit or a second read confirms them. Until
///       then the card shows them as experimental.
constexpr HeatingProgrammePeriod TOWEL_DRYER_PROGRAMME_PERIOD = {
    static_cast<uint8_t>(TowelDryerMode::PROG),
    static_cast<uint8_t>(HeatingTemporaryState::NONE),
    HeatingFunction::TEMPERATURE_IN_FORCE,
    HeatingFunction::SET_TEMPERATURE,
    HeatingFunction::ECO_SETBACK,
    preset_index(TOWEL_DRYER_PRESETS, HeatingPresetId::COMFORT),
    preset_index(TOWEL_DRYER_PRESETS, HeatingPresetId::ECO),
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
    {heating_level::OFF, HeatingHvacMode::OFF, NO_PRESET},
    {heating_level::FROST_PROTECTION, HeatingHvacMode::HEAT, preset_index(LEVEL_HEATER_PRESETS, HeatingPresetId::AWAY)},
    {heating_level::ECO, HeatingHvacMode::HEAT, preset_index(LEVEL_HEATER_PRESETS, HeatingPresetId::ECO)},
    {heating_level::COMFORT, HeatingHvacMode::HEAT, preset_index(LEVEL_HEATER_PRESETS, HeatingPresetId::COMFORT)},
    {heating_level::BOOST, HeatingHvacMode::HEAT, preset_index(LEVEL_HEATER_PRESETS, HeatingPresetId::BOOST)},
    {heating_level::COMFORT_MINUS_1, HeatingHvacMode::HEAT,
     preset_index(LEVEL_HEATER_PRESETS, HeatingPresetId::CUSTOM, "Comfort -1")},
    {heating_level::COMFORT_MINUS_2, HeatingHvacMode::HEAT,
     preset_index(LEVEL_HEATER_PRESETS, HeatingPresetId::CUSTOM, "Comfort -2")},
    {heating_level::SECURED, HeatingHvacMode::HEAT,
     preset_index(LEVEL_HEATER_PRESETS, HeatingPresetId::CUSTOM, "Secured")},
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
    {heating_alias::COMFORT, HeatingHvacMode::HEAT,
     preset_index(TEMPERATURE_INTERFACE_PRESETS, HeatingPresetId::COMFORT)},
    {heating_alias::THERMOSTAT_ECO, HeatingHvacMode::HEAT,
     preset_index(TEMPERATURE_INTERFACE_PRESETS, HeatingPresetId::ECO)},
    {heating_alias::SECURED, HeatingHvacMode::HEAT,
     preset_index(TEMPERATURE_INTERFACE_PRESETS, HeatingPresetId::CUSTOM, "Secured")},
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
    {heating_alias::COMFORT, HeatingHvacMode::HEAT, preset_index(HEAT_PUMP_PRESETS, HeatingPresetId::COMFORT)},
    {heating_alias::HEAT_PUMP_ECO, HeatingHvacMode::HEAT, preset_index(HEAT_PUMP_PRESETS, HeatingPresetId::ECO)},
    {heating_alias::HEAT_PUMP_SETBACK, HeatingHvacMode::HEAT,
     preset_index(HEAT_PUMP_PRESETS, HeatingPresetId::CUSTOM, "Setback")},
    {heating_alias::HEAT_PUMP_HALTED, HeatingHvacMode::HEAT,
     preset_index(HEAT_PUMP_PRESETS, HeatingPresetId::CUSTOM, "Halted")},
};

constexpr float TEMPERATURE_VISUAL_MIN_C = 5.0F;   ///< Lowest target the card offers for a temperature profile.
constexpr float TEMPERATURE_VISUAL_MAX_C = 30.0F;  ///< Highest target the card offers for a temperature profile.

/// The electrical heater with an adjustable setpoint (52/1), the AGILIA radiator: a table row, and the
/// base of the model that adds a night light.
constexpr HeatingProfile SETPOINT_HEATER_PROFILE = {
    .type = DeviceType::ELECTRICAL_HEATER,
    .subtype = 1,
    .model = DeviceModel::NONE,
    .path = HeatingControlPath::PRIVATE_REGISTERS,
    .features = {.target_temperature = true, .current_temperature = true, .action = true, .mode_observed = true},
    .modes = SETPOINT_HEATER_MODES,
    .visual_min_c = HEATING_TEMP_MIN_C,
    .visual_max_c = HEATING_TEMP_MAX_C,
    .presets = SETPOINT_HEATER_PRESETS,
    .observed = SETPOINT_HEATER_OBSERVED,
    .poll_registers = SETPOINT_HEATER_POLL,
    .boot_registers = SETPOINT_HEATER_BOOT,
};

/// @brief The radiator's profile under the HEATER_WITH_LIGHT model, with the light read after its poll.
/// @details Built from SETPOINT_HEATER_PROFILE rather than restated, so the two stay one heater with one
///          extra part.
constexpr HeatingProfile heater_with_light_profile() {
  HeatingProfile profile = SETPOINT_HEATER_PROFILE;
  profile.model = DeviceModel::HEATER_WITH_LIGHT;
  profile.extra_poll_registers = HEATER_WITH_LIGHT_EXTRA_POLL;
  return profile;
}

/// The heater with a night light (52/1, model HEATER_WITH_LIGHT).
constexpr HeatingProfile HEATER_WITH_LIGHT_PROFILE = heater_with_light_profile();

constexpr HeatingProfile HEATING_PROFILES[] = {
    {
        .type = DeviceType::HEATING_TEMPERATURE_INTERFACE,
        .subtype = 0,
        .model = DeviceModel::NONE,
        .path = HeatingControlPath::TEMPERATURE_EXECUTE,
        .features = {.target_temperature = true, .current_temperature = false, .action = false, .mode_observed = false},
        .modes = TEMPERATURE_INTERFACE_MODES,
        .setpoint_fp = THERMOSTAT_MANUAL,
        .visual_min_c = TEMPERATURE_VISUAL_MIN_C,
        .visual_max_c = TEMPERATURE_VISUAL_MAX_C,
        .presets = TEMPERATURE_INTERFACE_PRESETS,
        .observed = TEMPERATURE_INTERFACE_OBSERVED,
        .mp_prefix = HEATING_ALIAS_MP_PREFIX,
    },
    {
        .type = DeviceType::HEAT_PUMP,
        .subtype = 0,
        .model = DeviceModel::NONE,
        .path = HeatingControlPath::TEMPERATURE_EXECUTE,
        .features = {.target_temperature = true, .current_temperature = false, .action = false, .mode_observed = false},
        .modes = HEAT_PUMP_MODES,
        .visual_min_c = TEMPERATURE_VISUAL_MIN_C,
        .visual_max_c = TEMPERATURE_VISUAL_MAX_C,
        .presets = HEAT_PUMP_PRESETS,
        .observed = HEAT_PUMP_OBSERVED,
        .mp_prefix = HEATING_ALIAS_MP_PREFIX,
        .fixed_range = true,
    },
    {
        .type = DeviceType::ELECTRICAL_HEATER,
        .subtype = 0,
        .model = DeviceModel::NONE,
        .path = HeatingControlPath::HEATING_LEVEL,
        .features = {.target_temperature = false, .current_temperature = false, .action = false, .mode_observed = true},
        .modes = LEVEL_HEATER_MODES,
        .presets = LEVEL_HEATER_PRESETS,
        .observed = LEVEL_HEATER_OBSERVED,
        .mp_prefix = HEATING_LEVEL_MP_PREFIX,
    },
    SETPOINT_HEATER_PROFILE,
    HEATER_WITH_LIGHT_PROFILE,
    {
        .type = DeviceType::ELECTRICAL_HEATER,
        .subtype = 1,
        .model = DeviceModel::TOWEL_DRYER,
        .path = HeatingControlPath::PRIVATE_REGISTERS,
        .features = {.target_temperature = true, .current_temperature = true, .action = true, .mode_observed = true},
        .modes = TOWEL_DRYER_MODES,
        .visual_min_c = HEATING_TEMP_MIN_C,
        .visual_max_c = HEATING_TEMP_MAX_C,
        .programme_period = &TOWEL_DRYER_PROGRAMME_PERIOD,
        .presets = TOWEL_DRYER_PRESETS,
        .observed = TOWEL_DRYER_OBSERVED,
        .poll_registers = TOWEL_DRYER_POLL,
        .boot_registers = TOWEL_DRYER_BOOT,
        .preset_observed = TOWEL_DRYER_OBSERVED_PRESETS,
        .mode_fn = HeatingFunction::TOWEL_MODE,
        .preset_fn = HeatingFunction::TEMPORARY_STATE,
        .action_source = HeatingActionSource::WORKING_RATE,
    },
};

// === Table consistency, checked at build time ===

/// @return Whether every observed row of `rows` shows no preset or one inside a list of `preset_count`.
constexpr bool observed_within(std::span<const HeatingObservedState> rows, size_t preset_count) {
  return std::ranges::all_of(rows, [preset_count](const HeatingObservedState &row) {
    return row.preset >= NO_PRESET && row.preset < static_cast<int8_t>(preset_count);
  });
}

/// @return Whether a profile's programme period (if any) names presets inside its preset list.
constexpr bool programme_period_within(const HeatingProfile &profile) {
  const HeatingProgrammePeriod *period = profile.programme_period;
  const auto count = static_cast<int8_t>(profile.presets.size());
  return period == nullptr || (period->comfort_preset < count && period->eco_preset < count);
}

/// @return Whether every preset index a profile names lies inside its preset list.
constexpr bool preset_indices_within_lists() {
  return std::ranges::all_of(HEATING_PROFILES, [](const HeatingProfile &profile) {
    return observed_within(profile.observed, profile.presets.size()) &&
           observed_within(profile.preset_observed, profile.presets.size()) && programme_period_within(profile);
  });
}

static_assert(preset_indices_within_lists(), "a profile names a preset index outside its preset list");

const HeatingProfile *find_exact_profile(DeviceType type, uint8_t subtype, DeviceModel model) {
  for (const HeatingProfile &profile : HEATING_PROFILES) {
    if (profile.type == type && profile.subtype == subtype && profile.model == model)
      return &profile;
  }
  return nullptr;
}

}  // namespace

const HeatingProfile *find_heating_profile(DeviceType type, uint8_t subtype, DeviceModel model) {
  if (model != DeviceModel::NONE) {
    if (const HeatingProfile *model_profile = find_exact_profile(type, subtype, model))
      return model_profile;
  }
  const HeatingProfile *profile = find_exact_profile(type, subtype, DeviceModel::NONE);
  if (profile != nullptr || subtype == 0)
    return profile;
  // A subtype the type table names on its own (the Atlantic heat pumps, 22/1 and 22/2) is a different
  // device from the type's own row and must not inherit its profile; only an unnamed subtype does.
  return find_exact_device_type_info(type, subtype) == nullptr ? find_exact_profile(type, 0, DeviceModel::NONE)
                                                               : nullptr;
}

const HeatingProfile *find_heating_profile(const IoDevice &dev) {
  return find_heating_profile(dev.type, dev.subtype, dev.model);
}

// The path predicates ask the (type, subtype) profile: a model's profile keeps its path.

bool device_uses_heating_registers(DeviceType type, uint8_t subtype) {
  const HeatingProfile *profile = find_heating_profile(type, subtype, DeviceModel::NONE);
  return profile != nullptr && profile->path == HeatingControlPath::PRIVATE_REGISTERS;
}

bool device_accepts_register_probe(DeviceType type, uint8_t subtype) {
  // A water heater's register family is a fact of the (type, subtype), as the heater's control path is.
  return has_feature(device_features(type, subtype, DeviceModel::NONE), device_feature::WATER_HEATER) ||
         device_uses_heating_registers(type, subtype);
}

bool device_uses_heating_levels(DeviceType type, uint8_t subtype) {
  const HeatingProfile *profile = find_heating_profile(type, subtype, DeviceModel::NONE);
  return profile != nullptr && profile->path == HeatingControlPath::HEATING_LEVEL;
}

bool device_uses_heating_temperature(DeviceType type, uint8_t subtype) {
  const HeatingProfile *profile = find_heating_profile(type, subtype, DeviceModel::NONE);
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

namespace {

/// @return The row of `rows` for a wire value, or nullptr when there is none.
const HeatingObservedState *find_observed_row(std::span<const HeatingObservedState> rows, uint8_t wire) {
  for (const HeatingObservedState &row : rows) {
    if (row.wire == wire)
      return &row;
  }
  return nullptr;
}

}  // namespace

const HeatingObservedState *find_observed_state(const HeatingProfile &profile, uint8_t wire) {
  return find_observed_row(profile.observed, wire);
}

const HeatingObservedState *find_observed_preset(const HeatingProfile &profile, uint8_t wire) {
  return find_observed_row(profile.preset_observed, wire);
}

// === Programme period ===

bool programme_period_active(const HeatingProfile &profile, const HeatingReadings &readings) {
  const HeatingProgrammePeriod *period = profile.programme_period;
  if (period == nullptr)
    return false;
  // value_of() is -1 for an unread register, which never equals a (non-negative) register value.
  const auto is_value = [&readings](HeatingFunction fn, uint8_t value) {
    return readings.value_of(fn) == static_cast<int16_t>(value);
  };
  return is_value(profile.mode_fn, period->programme_mode) && is_value(profile.preset_fn, period->idle_preset_state);
}

namespace {

/// @return A temperature in whole tenths of a degree, the resolution the device holds it in.
int32_t to_tenths(float celsius) { return static_cast<int32_t>(std::lround(celsius * TENTHS_PER_DEGREE)); }

}  // namespace

ProgrammePeriodState resolve_programme_period(const HeatingProfile &profile, const HeatingReadings &readings) {
  ProgrammePeriodState state;
  if (!programme_period_active(profile, readings))
    return state;
  const HeatingProgrammePeriod &period = *profile.programme_period;
  state.active = true;
  if (!readings.has(period.in_force_fn))
    return state;
  // The temperature in force is held in whole tenths, so the comparison is on whole tenths: 20.0 - 3.5 is
  // 16.5 only there, not in float.
  state.target_c = readings.temperature_of(period.in_force_fn);
  if (!readings.has(period.comfort_fn))
    return state;
  const int32_t in_force = to_tenths(state.target_c);
  const int32_t comfort = to_tenths(readings.temperature_of(period.comfort_fn));
  if (in_force == comfort) {
    state.preset_index = period.comfort_preset;
  } else if (readings.has(period.setback_fn) &&
             in_force == comfort - to_tenths(readings.temperature_of(period.setback_fn))) {
    state.preset_index = period.eco_preset;
  }
  return state;
}

void clear_inactive_programme_reading(const HeatingProfile &profile, HeatingReadings &readings) {
  if (profile.programme_period != nullptr && !programme_period_active(profile, readings))
    readings.clear(profile.programme_period->in_force_fn);
}

}  // namespace home_io_control
}  // namespace esphome
