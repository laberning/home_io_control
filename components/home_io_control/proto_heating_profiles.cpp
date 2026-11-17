/// @file proto_heating_profiles.cpp
/// @brief The heating profile table and its lookups.
/// @ingroup hioc_protocol

#include "proto_heating_profiles.h"

#include "proto_codecs.h"
#include "proto_device_kinds.h"
#include "proto_device_types.h"
#include "proto_sizes.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <span>

namespace esphome {
namespace home_io_control {

namespace {

/// Mask for the low byte of a main parameter, where a heating level sits.
constexpr uint16_t LOW_BYTE_MASK = 0xFF;

/// The card shows no preset for a device-reported value.
constexpr int8_t NO_PRESET = -1;

/// Compile-time failure for preset_index() and level_heater_preset(): a non-constexpr call is not a constant
/// expression, so a lookup that reaches it stops the build.
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
    {"Program", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, static_cast<uint8_t>(HeatingMode::PROG)},
    {"Auto", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, static_cast<uint8_t>(HeatingMode::AUTO)},
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

/// The open-window-detection setting, read on every poll rather than once: the heater's own controller
/// changes it (issue #222: a change from the vendor app). A missed read keeps the switch as it was and does
/// not fail the poll, so the setting costs no poll the five registers above delivered.
constexpr HeatingFunction SETPOINT_HEATER_EXTRA_POLL[] = {
    HeatingFunction::SET_OPEN_WINDOW_DETECTION,
};

/// Properties and settings that change rarely: read until they have been read once.
constexpr HeatingFunction SETPOINT_HEATER_BOOT[] = {
    HeatingFunction::MAX_SETPOINT,
};

// === Heater with a night light (52/1, model HEATER_WITH_LIGHT): the radiator plus its light ===

/// The radiator's own extra reads, then the light's state and brightness. Both light registers change from
/// the heater's own controller (issue #222: a Cozytouch switches the light), so they are read on every
/// poll, not once.
constexpr HeatingFunction HEATER_WITH_LIGHT_EXTRA_POLL[] = {
    HeatingFunction::SET_OPEN_WINDOW_DETECTION,
    HeatingFunction::NIGHT_LIGHT,
    HeatingFunction::NIGHT_LIGHT_BRIGHTNESS,
};

static_assert(std::ranges::equal(SETPOINT_HEATER_EXTRA_POLL,
                                 std::span(HEATER_WITH_LIGHT_EXTRA_POLL).first(std::size(SETPOINT_HEATER_EXTRA_POLL))),
              "the heater with a light reads the radiator's extra registers first");

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

/// Boost and drying are timed runs on top of the mode; "none" ends the one that runs. The comfort and eco
/// periods of the device's own programme are not presets (nothing selects them); the level the heater
/// applies is read from its status (HeatingReadings::level) and shown by the `heating_level:` sensor.
constexpr HeatingPresetEntry TOWEL_DRYER_PRESETS[] = {
    {nullptr, HeatingPresetId::NONE, HeatingFunction::TEMPORARY_STATE,
     static_cast<uint8_t>(HeatingTemporaryState::NONE)},
    {nullptr, HeatingPresetId::BOOST, HeatingFunction::TEMPORARY_STATE,
     static_cast<uint8_t>(HeatingTemporaryState::BOOST)},
    {"Drying", HeatingPresetId::CUSTOM, HeatingFunction::TEMPORARY_STATE,
     static_cast<uint8_t>(HeatingTemporaryState::DRYING)},
};

/// The mode register names the mode only; TOWEL_DRYER_OBSERVED_PRESETS names the preset.
constexpr HeatingObservedState TOWEL_DRYER_OBSERVED[] = {
    {static_cast<uint8_t>(TowelDryerMode::OFF), HeatingHvacMode::OFF, NO_PRESET},
    {static_cast<uint8_t>(TowelDryerMode::PROG), HeatingHvacMode::AUTO, NO_PRESET},
    {static_cast<uint8_t>(TowelDryerMode::MANUAL), HeatingHvacMode::HEAT, NO_PRESET},
};

/// Temporary state -> preset. The hvac column is not consulted for a preset table.
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

/// The maximum setpoint.
constexpr HeatingFunction TOWEL_DRYER_BOOT[] = {
    HeatingFunction::MAX_SETPOINT,
};

/// The programme runs when the mode shows as auto (PROG) and no boost or drying is on top of it. The temperature
/// in force then is the card's target, and a target set meanwhile is written to the derogation register
/// instead of the comfort setpoint.
/// The override is taken in an eco period and ignored in a comfort period (issue #202), so a target in a comfort
/// period is written to the comfort setpoint instead (ADR 0094).
/// \todo The level the status read reports in each period is captured on one unit (BD684B); a second unit
///       confirms it. Until then the level sensor is experimental.
constexpr HeatingProgrammePeriod TOWEL_DRYER_PROGRAMME_PERIOD = {
    .in_force_fn = HeatingFunction::TEMPERATURE_IN_FORCE,
    .comfort_fn = HeatingFunction::SET_TEMPERATURE,
    .derogation_fn = HeatingFunction::DEROGATED_SETPOINT,
    .reports_level = true,
    .override_in_eco_only = true,
    .idle_preset_state = static_cast<uint8_t>(HeatingTemporaryState::NONE),
};

// === Plain electrical heater (52/0): heating levels ===

constexpr HeatingModeEntry LEVEL_HEATER_MODES[] = {
    {HeatingHvacMode::OFF, heating_level::OFF},
    {HeatingHvacMode::HEAT, heating_level::COMFORT},
};

constexpr HeatingPresetEntry LEVEL_HEATER_PRESETS[] = {
    // write_fn is unused on this path; SET_MODE is a placeholder that is never sent.
    {nullptr, HeatingPresetId::AWAY, HeatingFunction::SET_MODE, heating_level::FROST_PROTECTION},
    {nullptr, HeatingPresetId::ECO, HeatingFunction::SET_MODE, heating_level::ECO},
    {nullptr, HeatingPresetId::COMFORT, HeatingFunction::SET_MODE, heating_level::COMFORT},
    {nullptr, HeatingPresetId::BOOST, HeatingFunction::SET_MODE, heating_level::BOOST},
    {"Comfort -1", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, heating_level::COMFORT_MINUS_1},
    {"Comfort -2", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, heating_level::COMFORT_MINUS_2},
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
    // Reported by the device, never commanded: the card shows heat, no preset.
    {heating_level::SECURED, HeatingHvacMode::HEAT, NO_PRESET},
};

/// @brief The plain heater's preset entry for one heating level.
/// @details Picks the entry by its level rather than by its position, so a reordered LEVEL_HEATER_PRESETS cannot
///          hand a model the wrong preset. Evaluated at compile time, so a level the list does not hold fails
///          the build, as in preset_index().
/// @param level Heating level (heating_level) of the wanted entry.
consteval HeatingPresetEntry level_heater_preset(uint8_t level) {
  for (const HeatingPresetEntry &entry : LEVEL_HEATER_PRESETS) {
    if (entry.wire == level)
      return entry;
  }
  preset_missing_from_list();
  return LEVEL_HEATER_PRESETS[0];
}

/// The pilot-wire interface's presets: the orders the pilot wire carries a controller can set, so the level list
/// without Boost. The interface drops a boost it is sent, so it is not offered.
constexpr HeatingPresetEntry PILOT_WIRE_PRESETS[] = {
    level_heater_preset(heating_level::FROST_PROTECTION), level_heater_preset(heating_level::ECO),
    level_heater_preset(heating_level::COMFORT),          level_heater_preset(heating_level::COMFORT_MINUS_1),
    level_heater_preset(heating_level::COMFORT_MINUS_2),
};

static_assert(std::ranges::none_of(PILOT_WIRE_PRESETS,
                                   [](const HeatingPresetEntry &entry) { return entry.id == HeatingPresetId::BOOST; }),
              "the pilot-wire interface offers no boost");
static_assert(std::size(PILOT_WIRE_PRESETS) + 1 == std::size(LEVEL_HEATER_PRESETS),
              "the pilot-wire presets are the plain heater's without exactly one (boost)");

/// The levels the pilot-wire interface could report, mapped onto its own preset list: the plain heater's
/// observed states without Boost (the indices differ, so the rows are restated).
constexpr HeatingObservedState PILOT_WIRE_OBSERVED[] = {
    {heating_level::OFF, HeatingHvacMode::OFF, NO_PRESET},
    {heating_level::FROST_PROTECTION, HeatingHvacMode::HEAT, preset_index(PILOT_WIRE_PRESETS, HeatingPresetId::AWAY)},
    {heating_level::ECO, HeatingHvacMode::HEAT, preset_index(PILOT_WIRE_PRESETS, HeatingPresetId::ECO)},
    {heating_level::COMFORT, HeatingHvacMode::HEAT, preset_index(PILOT_WIRE_PRESETS, HeatingPresetId::COMFORT)},
    {heating_level::COMFORT_MINUS_1, HeatingHvacMode::HEAT,
     preset_index(PILOT_WIRE_PRESETS, HeatingPresetId::CUSTOM, "Comfort -1")},
    {heating_level::COMFORT_MINUS_2, HeatingHvacMode::HEAT,
     preset_index(PILOT_WIRE_PRESETS, HeatingPresetId::CUSTOM, "Comfort -2")},
    {heating_level::SECURED, HeatingHvacMode::HEAT, NO_PRESET},
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
    {nullptr, HeatingPresetId::COMFORT, HeatingFunction::SET_MODE, heating_alias::COMFORT, NO_FP},
    {nullptr, HeatingPresetId::ECO, HeatingFunction::SET_MODE, heating_alias::THERMOSTAT_ECO, NO_FP},
    {"Secured", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, heating_alias::SECURED, NO_FP},
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
    {nullptr, HeatingPresetId::COMFORT, HeatingFunction::SET_MODE, heating_alias::COMFORT, HEAT_PUMP_ON},
    {nullptr, HeatingPresetId::ECO, HeatingFunction::SET_MODE, heating_alias::HEAT_PUMP_ECO, HEAT_PUMP_ON},
    {"Setback", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, heating_alias::HEAT_PUMP_SETBACK, HEAT_PUMP_ON},
    {"Halted", HeatingPresetId::CUSTOM, HeatingFunction::SET_MODE, heating_alias::HEAT_PUMP_HALTED, HEAT_PUMP_ON},
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

/// The programme of a radiator runs while its mode shows as auto, in PROG and in AUTO alike; it has no timed run
/// on top. The temperature in force is register 0x012A, a target set meanwhile goes to the override register.
/// \todo Experimental: neither 0x012A nor 0x011D has been read or written on an AGILIA, Hekla or EMOTION 3 H.
///       Confirm both on a radiator. A heater that answers the read empty keeps the comfort-setpoint write.
constexpr HeatingProgrammePeriod SETPOINT_HEATER_PROGRAMME_PERIOD = {
    .in_force_fn = HeatingFunction::HEATER_TEMPERATURE_IN_FORCE,
    .comfort_fn = HeatingFunction::SET_TEMPERATURE,
    .derogation_fn = HeatingFunction::DEROGATED_SETPOINT,
};

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
    .programme_period = &SETPOINT_HEATER_PROGRAMME_PERIOD,
    .presets = SETPOINT_HEATER_PRESETS,
    .observed = SETPOINT_HEATER_OBSERVED,
    .poll_registers = SETPOINT_HEATER_POLL,
    .boot_registers = SETPOINT_HEATER_BOOT,
    .extra_poll_registers = SETPOINT_HEATER_EXTRA_POLL,
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

/// The plain electrical heater (52/0, no model): heating levels in the standard EXECUTE.
constexpr HeatingProfile LEVEL_HEATER_PROFILE = {
    .type = DeviceType::ELECTRICAL_HEATER,
    .subtype = 0,
    .model = DeviceModel::NONE,
    .path = HeatingControlPath::HEATING_LEVEL,
    .features = {.target_temperature = false, .current_temperature = false, .action = false, .mode_observed = true},
    .mp_prefix = HEATING_LEVEL_MP_PREFIX,
    .modes = LEVEL_HEATER_MODES,
    .presets = LEVEL_HEATER_PRESETS,
    .observed = LEVEL_HEATER_OBSERVED,
};

/// @brief The interface's profile under the PILOT_WIRE_INTERFACE model, without the boost level.
/// @details Built from LEVEL_HEATER_PROFILE rather than restated: the same control path, modes and main
///          parameter prefix, only the presets and the levels read back differ.
constexpr HeatingProfile pilot_wire_interface_profile() {
  HeatingProfile profile = LEVEL_HEATER_PROFILE;
  profile.model = DeviceModel::PILOT_WIRE_INTERFACE;
  profile.presets = PILOT_WIRE_PRESETS;
  profile.observed = PILOT_WIRE_OBSERVED;
  return profile;
}

/// The interface for pilot-wire heaters (52/0, model PILOT_WIRE_INTERFACE).
constexpr HeatingProfile PILOT_WIRE_INTERFACE_PROFILE = pilot_wire_interface_profile();

constexpr HeatingProfile HEATING_PROFILES[] = {
    {
        .type = DeviceType::HEATING_TEMPERATURE_INTERFACE,
        .subtype = 0,
        .model = DeviceModel::NONE,
        .path = HeatingControlPath::TEMPERATURE_EXECUTE,
        .features = {.target_temperature = true, .current_temperature = false, .action = false, .mode_observed = false},
        .mp_prefix = HEATING_ALIAS_MP_PREFIX,
        .modes = TEMPERATURE_INTERFACE_MODES,
        .setpoint_fp = THERMOSTAT_MANUAL,
        .visual_min_c = TEMPERATURE_VISUAL_MIN_C,
        .visual_max_c = TEMPERATURE_VISUAL_MAX_C,
        .presets = TEMPERATURE_INTERFACE_PRESETS,
        .observed = TEMPERATURE_INTERFACE_OBSERVED,
    },
    {
        .type = DeviceType::HEAT_PUMP,
        .subtype = 0,
        .model = DeviceModel::NONE,
        .path = HeatingControlPath::TEMPERATURE_EXECUTE,
        .features = {.target_temperature = true, .current_temperature = false, .action = false, .mode_observed = false},
        .mp_prefix = HEATING_ALIAS_MP_PREFIX,
        .fixed_range = true,
        .modes = HEAT_PUMP_MODES,
        .visual_min_c = TEMPERATURE_VISUAL_MIN_C,
        .visual_max_c = TEMPERATURE_VISUAL_MAX_C,
        .presets = HEAT_PUMP_PRESETS,
        .observed = HEAT_PUMP_OBSERVED,
    },
    LEVEL_HEATER_PROFILE,
    PILOT_WIRE_INTERFACE_PROFILE,
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

/// @return Whether every preset index a profile names lies inside its preset list.
constexpr bool preset_indices_within_lists() {
  return std::ranges::all_of(HEATING_PROFILES, [](const HeatingProfile &profile) {
    return observed_within(profile.observed, profile.presets.size()) &&
           observed_within(profile.preset_observed, profile.presets.size());
  });
}

static_assert(preset_indices_within_lists(), "a profile names a preset index outside its preset list");

/// @return Whether every preset of every level-path profile is a level a controller can set. The secured
///         level is the one a heater reports and never takes, so a preset for it would be a choice that
///         sends nothing.
constexpr bool level_presets_are_commands() {
  return std::ranges::all_of(HEATING_PROFILES, [](const HeatingProfile &profile) {
    return profile.path != HeatingControlPath::HEATING_LEVEL ||
           std::ranges::none_of(profile.presets,
                                [](const HeatingPresetEntry &entry) { return entry.wire == heating_level::SECURED; });
  });
}

static_assert(level_presets_are_commands(), "a level preset sends a command: the secured level is reported, never set");

/// @return Whether every programme period whose override applies in an eco period only also reports its level:
///         the period a target is planned from is the level of the heater's status read (ADR 0094).
constexpr bool eco_only_override_reports_level() {
  return std::ranges::all_of(HEATING_PROFILES, [](const HeatingProfile &profile) {
    return profile.programme_period == nullptr || !profile.programme_period->override_in_eco_only ||
           profile.programme_period->reports_level;
  });
}

static_assert(eco_only_override_reports_level(),
              "an override taken in an eco period only needs the level of the status read to know the period");

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

std::span<const HeatingProfile> heating_profiles() { return HEATING_PROFILES; }

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

bool decode_heating_level(uint16_t main_parameter, uint8_t &level) {
  if ((main_parameter >> BITS_PER_BYTE) != HEATING_LEVEL_MP_PREFIX)
    return false;
  level = static_cast<uint8_t>(main_parameter & LOW_BYTE_MASK);
  return true;
}

const char *heating_level_name(uint8_t level) {
  switch (level) {
    case heating_level::OFF:
      return "off";
    case heating_level::FROST_PROTECTION:
      return "frost protection";
    case heating_level::ECO:
      return "eco";
    case heating_level::COMFORT_MINUS_2:
      return "comfort -2";
    case heating_level::COMFORT_MINUS_1:
      return "comfort -1";
    case heating_level::COMFORT:
      return "comfort";
    case heating_level::BOOST:
      return "boost";
    case heating_level::SECURED:
      return "secured";
    default:
      return nullptr;
  }
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

namespace {

/// @return Whether the mode register, as read, shows the programme (a mode the card shows as `auto`).
bool mode_shows_programme(const HeatingProfile &profile, const HeatingReadings &readings) {
  // value_of() is -1 for an unread register, and a value the table does not know has no row: neither is the
  // programme.
  const int16_t mode = readings.value_of(profile.mode_fn);
  const HeatingObservedState *shown = mode >= 0 ? find_observed_state(profile, static_cast<uint8_t>(mode)) : nullptr;
  return shown != nullptr && shown->hvac == HeatingHvacMode::AUTO;
}

}  // namespace

bool programme_period_active(const HeatingProfile &profile, const HeatingReadings &readings) {
  const HeatingProgrammePeriod *period = profile.programme_period;
  if (period == nullptr || !mode_shows_programme(profile, readings))
    return false;
  // A profile that keeps boost and drying in a register of its own runs the programme only with none on top.
  return profile.preset_observed.empty() ||
         readings.value_of(profile.preset_fn) == static_cast<int16_t>(period->idle_preset_state);
}

std::optional<HeatingFunction> timed_run_register(const HeatingProfile &profile, const HeatingReadings &readings) {
  if (profile.programme_period == nullptr || profile.preset_observed.empty() ||
      !mode_shows_programme(profile, readings))
    return std::nullopt;
  return profile.preset_fn;
}

float programme_temperature_in_force(const HeatingProfile &profile, const HeatingReadings &readings) {
  if (!programme_period_active(profile, readings))
    return NAN;
  return readings.temperature_of(profile.programme_period->in_force_fn);
}

void clear_inactive_programme_reading(const HeatingProfile &profile, HeatingReadings &readings) {
  if (profile.programme_period != nullptr && !programme_period_active(profile, readings))
    readings.clear(profile.programme_period->in_force_fn);
}

// === Heater writes ===

namespace {

/// Plan the write of one wire value on the register or level path.
HeatingWritePlan plan_wire_write(const HeatingProfile &profile, HeatingFunction fn, uint8_t wire) {
  HeatingWritePlan plan;
  if (profile.path == HeatingControlPath::HEATING_LEVEL) {
    plan.kind = HeatingPlanKind::LEVEL;
    plan.level = wire;  // the settle poll of the level write reads it back
  } else {
    plan.kind = HeatingPlanKind::REGISTER;
    plan.register_fn = fn;
    plan.register_value = static_cast<float>(wire);
    plan.read_back = true;
    plan.read_back_fn = fn;
  }
  return plan;
}

/// Plan a write of a main parameter with a functional parameter on the temperature path.
HeatingWritePlan plan_temperature_write(uint16_t main_parameter, const HeatingFp &fp) {
  HeatingWritePlan plan;
  plan.kind = HeatingPlanKind::TEMPERATURE;
  plan.main_parameter = main_parameter;
  plan.fp = fp;
  return plan;
}

/// A plan that writes nothing, with the reason.
HeatingWritePlan refused(HeatingRefusal refusal) {
  HeatingWritePlan plan;
  plan.refusal = refusal;
  return plan;
}

/// Encode a target over the range the profile calls for: a profile with a fixed range never takes the
/// YAML range, whatever the record holds.
bool encode_target(const HeatingProfile &profile, const HeatingDeviceFacts &facts, float celsius,
                   uint16_t &main_parameter) {
  if (!facts.has_record)
    return false;
  const float min_c = profile.fixed_range ? HEATING_TEMPERATURE_RANGE_MIN_C : facts.range_min_c;
  const float max_c = profile.fixed_range ? HEATING_TEMPERATURE_RANGE_MAX_C : facts.range_max_c;
  return encode_temperature_main_parameter(celsius, min_c, max_c, main_parameter);
}

HeatingWritePlan plan_mode_write(const HeatingProfile &profile, const HeatingWriteRequest &request,
                                 const HeatingDeviceFacts &facts) {
  const auto entry = std::find_if(profile.modes.begin(), profile.modes.end(),
                                  [&](const HeatingModeEntry &mode) { return mode.hvac == request.hvac; });
  if (entry == profile.modes.end())
    return refused(HeatingRefusal::NO_MATCH);
  if (profile.path != HeatingControlPath::TEMPERATURE_EXECUTE)
    return plan_wire_write(profile, profile.mode_fn, entry->wire);

  uint16_t main_parameter = entry->main_parameter;
  if (main_parameter == HEATING_MP_FROM_TARGET) {
    // Heat (manual) is the current target temperature with the manual flag: the target the card shows,
    // else the one the device last reported.
    const float target = std::isnan(request.shown_target_c) ? facts.reported_setpoint_c : request.shown_target_c;
    if (std::isnan(target) || !encode_target(profile, facts, target, main_parameter))
      return refused(HeatingRefusal::NO_TARGET);
  }
  return plan_temperature_write(main_parameter, entry->fp);
}

HeatingWritePlan plan_preset_write(const HeatingProfile &profile, const HeatingWriteRequest &request) {
  int8_t index = -1;
  for (size_t i = 0; i < profile.presets.size(); i++) {
    const HeatingPresetEntry &entry = profile.presets[i];
    const bool custom = request.preset == HeatingPresetId::CUSTOM;
    if (entry.id == request.preset && (!custom || same_name(entry.custom_name, request.custom_name))) {
      index = static_cast<int8_t>(i);
      break;
    }
  }
  if (index < 0)
    return refused(HeatingRefusal::NO_MATCH);
  const HeatingPresetEntry &entry = profile.presets[index];

  // A setpoint alias is a main parameter, with the functional parameter the profile names for it.
  HeatingWritePlan plan =
      profile.path == HeatingControlPath::TEMPERATURE_EXECUTE
          ? plan_temperature_write(heating_prefixed_main_parameter(profile.mp_prefix, entry.wire), entry.fp)
          : plan_wire_write(profile, entry.write_fn, entry.wire);
  plan.preset_index = index;
  return plan;
}

HeatingWritePlan plan_target_write(const HeatingProfile &profile, const HeatingWriteRequest &request,
                                   const HeatingDeviceFacts &facts) {
  if (!profile.features.target_temperature)
    return refused(HeatingRefusal::NO_SETPOINT);
  if (profile.path == HeatingControlPath::TEMPERATURE_EXECUTE) {
    // The temperature travels in the main parameter, encoded over the device's range.
    uint16_t main_parameter = 0;
    if (!encode_target(profile, facts, request.target_c, main_parameter))
      return refused(HeatingRefusal::UNENCODABLE);
    return plan_temperature_write(main_parameter, profile.setpoint_fp);
  }
  // A heater that did not say what it runs gets no target: the register it belongs in is not known.
  if (profile.programme_period != nullptr && !facts.programme_state_answered)
    return refused(HeatingRefusal::PROGRAMME_STATE_UNANSWERED);
  // Before the heater has answered the read of its temperature in force, an override and a comfort write are
  // both guesses.
  if (facts.in_force_unresolved && profile.programme_period != nullptr)
    return refused(HeatingRefusal::IN_FORCE_UNKNOWN);
  // While the programme runs the target is a temporary override that holds until the programme's next change;
  // in any other mode it is the comfort setpoint.
  bool override_programme = facts.programme_runs && profile.programme_period != nullptr;
  if (override_programme && profile.programme_period->override_in_eco_only) {
    // The heater ignores the override in a comfort period, so the target changes the comfort temperature there.
    if (facts.period == HeatingPeriod::UNKNOWN)
      return refused(HeatingRefusal::PERIOD_UNKNOWN);
    override_programme = facts.period == HeatingPeriod::ECO;
  }
  const HeatingFunction target_fn =
      override_programme ? profile.programme_period->derogation_fn : HeatingFunction::SET_TEMPERATURE;
  HeatingWritePlan plan;
  plan.kind = HeatingPlanKind::REGISTER;
  plan.temporary_override = override_programme;
  plan.register_fn = target_fn;
  plan.register_value = request.target_c;
  plan.read_back = true;
  plan.read_back_fn = target_fn;
  return plan;
}

}  // namespace

bool heating_request_reads_programme_state(const HeatingProfile &profile, const HeatingWriteRequest &request) {
  return request.kind == HeatingWriteKind::TARGET && profile.programme_period != nullptr;
}

namespace {
/// The period a heating level stands for; any other level (frost protection, boost, none read) is no period.
HeatingPeriod period_of_level(int16_t level) {
  switch (level) {
    case heating_level::COMFORT:
      return HeatingPeriod::COMFORT;
    case heating_level::ECO:
      return HeatingPeriod::ECO;
    default:
      return HeatingPeriod::UNKNOWN;
  }
}
}  // namespace

HeatingDeviceFacts heating_device_facts(const IoDevice *dev, bool programme_state_answered) {
  HeatingDeviceFacts facts;
  facts.programme_state_answered = programme_state_answered;
  if (dev == nullptr)
    return facts;
  facts.has_record = true;
  facts.range_min_c = dev->temperature_range_min_c;
  facts.range_max_c = dev->temperature_range_max_c;
  facts.reported_setpoint_c = dev->heating.setpoint_c;
  const HeatingProfile *profile = find_heating_profile(*dev);
  // A heater that has reported its temperature in force has the programme's registers: a read missed since
  // (no reply, or the programme just resumed) leaves the value unknown but not the rule.
  const bool programme_shown = profile != nullptr && programme_period_active(*profile, dev->heating);
  facts.programme_runs = programme_shown && dev->temperature_in_force == TemperatureInForceSupport::REPORTED;
  facts.in_force_unresolved = programme_shown && dev->temperature_in_force == TemperatureInForceSupport::UNKNOWN;
  // The stored level is the one the read made for this request refreshed (HeaterController::read_programme_state()).
  if (profile != nullptr && profile->programme_period != nullptr && profile->programme_period->override_in_eco_only)
    facts.period = period_of_level(dev->heating.level);
  return facts;
}

namespace {
/// The override register holds tenths of a degree, so two values closer than half a tenth are the same value.
constexpr float OVERRIDE_TOLERANCE_C = 0.05F;
}  // namespace

bool heating_override_taken(float requested_c, float read_back_c) {
  return !std::isnan(read_back_c) && std::fabs(requested_c - read_back_c) < OVERRIDE_TOLERANCE_C;
}

HeatingWritePlan plan_heating_write(const HeatingProfile &profile, const HeatingWriteRequest &request,
                                    const HeatingDeviceFacts &facts) {
  switch (request.kind) {
    case HeatingWriteKind::MODE:
      return plan_mode_write(profile, request, facts);
    case HeatingWriteKind::PRESET:
      return plan_preset_write(profile, request);
    case HeatingWriteKind::TARGET:
      return plan_target_write(profile, request, facts);
  }
  return refused(HeatingRefusal::NO_MATCH);
}

// === The climate card: what it shows ===

bool HeatingCardState::shows_same_as(const HeatingCardState &other) const {
  const auto same_temperature = [](float a, float b) { return (std::isnan(a) && std::isnan(b)) || a == b; };
  return this->mode == other.mode && this->preset_index == other.preset_index && this->action == other.action &&
         same_temperature(this->target_c, other.target_c) && same_temperature(this->current_c, other.current_c) &&
         same_temperature(this->max_target_c, other.max_target_c);
}

HeatingCardState initial_heating_card(const HeatingProfile &profile) {
  HeatingCardState card;
  if (profile.features.target_temperature)
    card.max_target_c = profile.visual_max_c;
  return card;
}

namespace {

/// Show a device-reported mode register value or heating level. Sets the preset too, unless the profile keeps
/// its preset in a register of its own.
void apply_observed_state(const HeatingProfile &profile, uint8_t wire, HeatingCardState &card) {
  const HeatingObservedState *observed = find_observed_state(profile, wire);
  if (observed == nullptr)
    return;  // unknown value: leave the card as it was
  // A mode nothing reads back stays the last command; the table then only names the preset.
  if (profile.features.mode_observed)
    card.mode = observed->hvac;
  // With a preset table of its own, the mode register says nothing about the preset.
  if (profile.preset_observed.empty())
    card.preset_index = observed->preset;
}

/// Show a device-reported value of the profile's preset register.
void apply_observed_preset(const HeatingProfile &profile, uint8_t wire, HeatingCardState &card) {
  const HeatingObservedState *observed = find_observed_preset(profile, wire);
  if (observed != nullptr)
    card.preset_index = observed->preset;
}

/// Fold the temperatures of the readings into the card; sets `seen` when a reading was present.
void reduce_temperatures(const HeatingProfile &profile, const HeatingReadings &readings, float target_c,
                         HeatingCardState &card, bool &seen) {
  if (profile.features.current_temperature) {
    card.current_c = readings.room_temperature_c;
    seen = seen || !std::isnan(readings.room_temperature_c);
  }
  if (profile.features.target_temperature && !std::isnan(target_c)) {
    seen = true;
    card.target_c = target_c;
  }
  // The heater's own ceiling, but never past what a write can encode.
  if (profile.features.target_temperature && !std::isnan(readings.max_setpoint_c) &&
      readings.max_setpoint_c > HEATING_TEMP_MIN_C) {
    seen = true;
    card.max_target_c = std::min(readings.max_setpoint_c, profile.visual_max_c);
  }
}

/// Fold the profile's action source (the regulator state, or the working rate) into the card.
void reduce_action(const HeatingProfile &profile, const HeatingReadings &readings, bool mode_known,
                   HeatingCardState &card, bool &seen) {
  if (!profile.features.action)
    return;
  // The action follows the mode: an off heater is not "idle". Until the profile's action source has been read
  // only the off case is known.
  if (card.mode == HeatingHvacMode::OFF && mode_known) {
    card.action = HeatingAction::OFF;
  } else if (profile.action_source == HeatingActionSource::REGULATOR && readings.regulator >= 0) {
    seen = true;
    card.action = readings.regulator == static_cast<int16_t>(HeatingRegulatorState::HEATING) ? HeatingAction::HEATING
                                                                                             : HeatingAction::IDLE;
  } else if (profile.action_source == HeatingActionSource::WORKING_RATE && readings.working_rate >= 0) {
    seen = true;
    card.action = readings.working_rate > 0 ? HeatingAction::HEATING : HeatingAction::IDLE;
  }
}

}  // namespace

HeatingCardState reduce_heating_card(const HeatingProfile &profile, const HeatingReadings &readings,
                                     const HeatingCardState &prior) {
  HeatingCardState card = prior;
  bool seen = false;
  // While the heater's own programme runs, the temperature it heats to replaces the comfort setpoint.
  const float in_force_c = programme_temperature_in_force(profile, readings);
  reduce_temperatures(profile, readings, std::isnan(in_force_c) ? readings.setpoint_c : in_force_c, card, seen);

  // The profile's mode register, or the heating level, whichever this profile's device reports.
  const int16_t wire =
      profile.path == HeatingControlPath::PRIVATE_REGISTERS ? readings.value_of(profile.mode_fn) : readings.level;
  if (wire >= 0) {
    seen = true;
    apply_observed_state(profile, static_cast<uint8_t>(wire), card);
  }
  // The preset kept in a register of its own (a towel dryer's temporary state, "none" while only the
  // programme runs).
  const int16_t preset_wire = !profile.preset_observed.empty() ? readings.value_of(profile.preset_fn) : -1;
  if (preset_wire >= 0) {
    seen = true;
    apply_observed_preset(profile, static_cast<uint8_t>(preset_wire), card);
  }
  if (wire < 0 && profile.path == HeatingControlPath::TEMPERATURE_EXECUTE && !std::isnan(readings.setpoint_c)) {
    // The device reports a plain temperature, so it is on no alias preset.
    card.preset_index = NO_PRESET;
  }
  reduce_action(profile, readings, wire >= 0, card, seen);

  // Where nothing reads the mode back, a device that reports a setpoint or an alias is evidently on: show
  // heat until a mode is commanded, instead of the card's default "off".
  if (!profile.features.mode_observed && !card.mode_commanded && seen && card.mode == HeatingHvacMode::OFF)
    card.mode = HeatingHvacMode::HEAT;

  card.observed = card.observed || seen;
  return card;
}

namespace {

/// A mode command: the commanded mode stands in until a read has succeeded, and always where nothing reads
/// the mode back.
HeatingCardCommand command_mode(const HeatingProfile &profile, const HeatingWriteRequest &request,
                                const HeatingCardState &prior) {
  HeatingCardCommand result{.card = prior};
  result.shown = !prior.observed || !profile.features.mode_observed;
  result.card.mode_commanded = true;
  if (!result.shown)
    return result;
  result.card.mode = request.hvac;
  // A real HVAC mode supersedes a preset on the register and level paths; a temperature heater's preset
  // follows its status reply instead, and so does a preset kept in a register of its own (a towel dryer's
  // boost outlives a mode change).
  if (profile.path != HeatingControlPath::TEMPERATURE_EXECUTE && profile.preset_observed.empty())
    result.card.preset_index = NO_PRESET;
  return result;
}

/// A preset command: once a read has succeeded the card follows the read-back, not the command.
HeatingCardCommand command_preset(const HeatingProfile &profile, const HeatingWritePlan &plan,
                                  const HeatingCardState &prior) {
  HeatingCardCommand result{.card = prior};
  result.shown = !prior.observed;
  if (!result.shown)
    return result;
  const HeatingPresetEntry &entry = profile.presets[plan.preset_index];
  if (!profile.preset_observed.empty()) {
    // The preset has a register and a table of its own: its value is never a mode value.
    apply_observed_preset(profile, entry.wire, result.card);
  } else if (profile.features.mode_observed) {
    // The read-back uses the same table, so a commanded preset looks like a read one.
    apply_observed_state(profile, entry.wire, result.card);
  } else {
    result.card.preset_index = plan.preset_index;
    if (entry.id != HeatingPresetId::CUSTOM)
      result.card.mode = HeatingHvacMode::HEAT;  // every standard alias preset heats
  }
  return result;
}

/// A target command: shown until a read has succeeded.
HeatingCardCommand command_target(const HeatingWriteRequest &request, const HeatingWritePlan &plan,
                                  const HeatingCardState &prior) {
  HeatingCardCommand result{.card = prior};
  result.shown = !prior.observed;
  if (!result.shown)
    return result;
  result.card.target_c = request.target_c;
  // A temperature heater's alias preset ends with the explicit setpoint; the settle poll reads it back.
  if (plan.kind == HeatingPlanKind::TEMPERATURE)
    result.card.preset_index = NO_PRESET;
  return result;
}

}  // namespace

HeatingCardCommand command_heating_card(const HeatingProfile &profile, const HeatingWriteRequest &request,
                                        const HeatingWritePlan &plan, const HeatingCardState &prior) {
  switch (request.kind) {
    case HeatingWriteKind::MODE:
      return command_mode(profile, request, prior);
    case HeatingWriteKind::PRESET:
      return command_preset(profile, plan, prior);
    case HeatingWriteKind::TARGET:
      return command_target(request, plan, prior);
  }
  return {.card = prior};
}

HeatingHardwareStatus heating_hardware_status(const HeatingProfile *profile) {
  if (profile == nullptr)
    return HeatingHardwareStatus::UNVALIDATED;
  if (profile->path == HeatingControlPath::HEATING_LEVEL)
    return HeatingHardwareStatus::LEVELS_CONFIRMED;
  if (profile->path != HeatingControlPath::PRIVATE_REGISTERS)
    return HeatingHardwareStatus::UNVALIDATED;
  return profile->experimental_writes ? HeatingHardwareStatus::WRITES_EXPERIMENTAL : HeatingHardwareStatus::CONFIRMED;
}

}  // namespace home_io_control
}  // namespace esphome
