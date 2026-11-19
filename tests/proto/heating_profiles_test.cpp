/// @file heating_profiles_test.cpp
/// @brief Tests for the heating profile table: lookup, profile contents, the towel dryer, programme and pilot-wire
/// kinds, the card reads and the temperature in force.
///
/// The level and temperature codecs are in heating_main_parameter_test.cpp, what a poll reads in
/// heater_poll_reads_test.cpp.

#include "proto_codecs.h"
#include "test_helpers.h"
#include "heating_profiles.h"
#include "payload_frame.h"
#include "reading_store.h"
#include "proto_commands.h"
#include "proto_device_types.h"
#include "proto_heating_profiles.h"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "device_table_walk.h"
#include "pinned_poll.h"

using namespace esphome::home_io_control;
using test::PinnedPoll;
using test::pinned_poll;

// === Lookup ===

TEST(HeatingProfiles, EachElectricalHeaterKindHasItsOwnControlPath) {
  const HeatingProfile *setpoint = find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE);
  ASSERT_NE(setpoint, nullptr);
  EXPECT_EQ(setpoint->path, HeatingControlPath::PRIVATE_REGISTERS);

  const HeatingProfile *plain = find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE);
  ASSERT_NE(plain, nullptr);
  EXPECT_EQ(plain->path, HeatingControlPath::HEATING_LEVEL);

  EXPECT_EQ(find_heating_profile(DeviceType::ELECTRICAL_HEATER, 9, DeviceModel::NONE), plain)
      << "an unnamed subtype falls back to subtype 0";
  EXPECT_EQ(find_heating_profile(DeviceType::HEAT_PUMP, 1, DeviceModel::NONE), nullptr)
      << "the Atlantic heat pump is a named row of its own and does not inherit the generic heat pump's profile";
  EXPECT_EQ(find_heating_profile(DeviceType::HEAT_PUMP, 2, DeviceModel::NONE), nullptr);
  EXPECT_EQ(find_heating_profile(DeviceType::EXTERIOR_HEATING, 0, DeviceModel::NONE), nullptr);
  EXPECT_EQ(find_heating_profile(DeviceType::ROLLER_SHUTTER, 0, DeviceModel::NONE), nullptr);
  EXPECT_EQ(find_heating_profile(DeviceType::UNKNOWN, 0, DeviceModel::NONE), nullptr);
}

TEST(HeatingProfiles, RegisterFramesAreForRegisterHeatersOnly) {
  EXPECT_TRUE(device_uses_heating_registers(DeviceType::ELECTRICAL_HEATER, 1));
  EXPECT_FALSE(device_uses_heating_registers(DeviceType::ELECTRICAL_HEATER, 0))
      << "a heater that takes the standard EXECUTE must never be sent a register write";
  EXPECT_FALSE(device_uses_heating_registers(DeviceType::ELECTRICAL_HEATER, 9));
  EXPECT_FALSE(device_uses_heating_registers(DeviceType::HEAT_PUMP, 0));
  EXPECT_TRUE(device_uses_heating_levels(DeviceType::ELECTRICAL_HEATER, 0));
  EXPECT_FALSE(device_uses_heating_levels(DeviceType::ELECTRICAL_HEATER, 1));
}

TEST(HeatingProfiles, ClimateGateAndTypeTableAgreeForEveryRow) {
  // One source: a climate-class row a platform drives has a profile, and a profile exists nowhere else.
  for (const DeviceType type : {DeviceType::ELECTRICAL_HEATER, DeviceType::HEAT_PUMP, DeviceType::EXTERIOR_HEATING,
                                DeviceType::HEATING_TEMPERATURE_INTERFACE, DeviceType::ROLLER_SHUTTER}) {
    for (const uint8_t subtype : {0, 1, 2, 58}) {
      const bool table_says_climate_platform =
          device_capability_class(type, subtype) == DeviceCapabilityClass::CLIMATE &&
          device_has_platform(type, subtype);
      EXPECT_EQ(find_heating_profile(type, subtype, DeviceModel::NONE) != nullptr, table_says_climate_platform)
          << static_cast<unsigned>(type) << "/" << +subtype;
      EXPECT_EQ(device_supports_climate_control(type, subtype), table_says_climate_platform);
    }
  }
}

TEST(HeatingProfiles, EveryProfileNamesARowOfTheTypeTable) {
  // find_heating_profile() falls back differently from the type table for a named subtype, so the two
  // lookups stay apart; this keeps a profile from being keyed to a device the table does not know.
  ASSERT_FALSE(heating_profiles().empty());
  for (const HeatingProfile &profile : heating_profiles()) {
    const DeviceTypeInfo *row = profile.model == DeviceModel::NONE
                                    ? find_exact_device_type_info(profile.type, profile.subtype)
                                    : find_device_model_info(profile.type, profile.subtype, profile.model);
    EXPECT_NE(row, nullptr) << format_device_ids(profile.type, profile.subtype) << " model "
                            << static_cast<unsigned>(profile.model);
  }
}

TEST(HeatingProfiles, TheTemperatureRangeCapabilityMarksTheProfilesThatTakeATemperatureWithoutAFixedRange) {
  // The YAML `min_temperature:` / `max_temperature:` keys are accepted by this capability, so it must be set
  // exactly where the climate platform uses the configured range (platform_climate.cpp reads
  // `fixed_range`).
  const auto check = [](const DeviceTypeInfo &row) {
    const HeatingProfile *profile = find_heating_profile(row.type, row.subtype, row.model);
    const bool takes_range = profile != nullptr && profile->path == HeatingControlPath::TEMPERATURE_EXECUTE &&
                             profile->features.target_temperature && !profile->fixed_range;
    EXPECT_EQ(has_feature(row.features, device_feature::TEMPERATURE_RANGE), takes_range)
        << format_device_ids(row.type, row.subtype);
  };
  test::for_each_table_row(check);
  test::for_each_model_row(check);
  EXPECT_TRUE(has_feature(device_features(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0, DeviceModel::NONE),
                          device_feature::TEMPERATURE_RANGE));
  EXPECT_FALSE(
      has_feature(device_features(DeviceType::HEAT_PUMP, 0, DeviceModel::NONE), device_feature::TEMPERATURE_RANGE));
}

// === Profile contents ===

TEST(HeatingProfiles, SetpointHeaterOffersTheCardItsRegistersCanDrive) {
  const HeatingProfile &p = test::radiator_profile();
  EXPECT_TRUE(p.features.target_temperature);
  EXPECT_TRUE(p.features.current_temperature);
  EXPECT_TRUE(p.features.action);
  const auto polled = pinned_poll(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE).every_poll;
  EXPECT_TRUE(std::find(polled.begin(), polled.end(), DeviceRegister::WINDOW_OPEN) != polled.end())
      << "the window sensor is polled with every read";

  ASSERT_EQ(p.modes.size(), 3u);
  EXPECT_EQ(p.modes[0].hvac, HeatingHvacMode::OFF);
  EXPECT_EQ(p.modes[0].wire, 4);
  EXPECT_EQ(p.modes[1].hvac, HeatingHvacMode::HEAT);
  EXPECT_EQ(p.modes[1].wire, 1);
  EXPECT_EQ(p.modes[2].hvac, HeatingHvacMode::AUTO);
  EXPECT_EQ(p.modes[2].wire, 0);

  // Both automatic modes are custom presets that write the mode register.
  ASSERT_EQ(p.presets.size(), 2u);
  EXPECT_EQ(p.presets[0].id, HeatingPresetId::CUSTOM);
  EXPECT_STREQ(p.presets[0].custom_name, "Program");
  EXPECT_EQ(p.preset_fn, DeviceRegister::OPERATING_MODE);
  EXPECT_EQ(p.presets[0].wire, 2);
  EXPECT_EQ(p.presets[1].id, HeatingPresetId::CUSTOM);
  EXPECT_STREQ(p.presets[1].custom_name, "Auto");
  EXPECT_EQ(p.presets[1].wire, 0);
}

TEST(HeatingProfiles, SetpointHeaterModeRegisterMapsBackToTheCard) {
  const HeatingProfile &p = test::radiator_profile();
  const auto *auto_mode = find_observed_state(p, 0);
  ASSERT_NE(auto_mode, nullptr);
  EXPECT_EQ(auto_mode->hvac, HeatingHvacMode::AUTO);
  ASSERT_GE(auto_mode->preset, 0);
  EXPECT_STREQ(p.presets[auto_mode->preset].custom_name, "Auto");
  EXPECT_EQ(find_observed_state(p, 1)->hvac, HeatingHvacMode::HEAT);
  const auto *program = find_observed_state(p, 2);
  ASSERT_NE(program, nullptr);
  EXPECT_EQ(program->hvac, HeatingHvacMode::AUTO) << "AUTO and PROG both show as HVAC auto";
  ASSERT_GE(program->preset, 0);
  EXPECT_STREQ(p.presets[program->preset].custom_name, "Program");
  EXPECT_EQ(find_observed_state(p, 3)->hvac, HeatingHvacMode::HEAT) << "external is shown as heat";
  EXPECT_EQ(find_observed_state(p, 4)->hvac, HeatingHvacMode::OFF);
  EXPECT_EQ(find_observed_state(p, 5), nullptr);
}

TEST(HeatingProfiles, PlainHeaterOffersLevelsAsModesAndPresets) {
  const HeatingProfile &p = test::level_heater_profile();
  EXPECT_FALSE(p.features.target_temperature);
  EXPECT_FALSE(p.features.current_temperature);
  EXPECT_FALSE(p.features.action);
  EXPECT_TRUE(pinned_poll(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE).every_poll.empty())
      << "a level heater is polled with the status read";

  ASSERT_EQ(p.modes.size(), 2u);
  EXPECT_EQ(p.modes[0].hvac, HeatingHvacMode::OFF);
  EXPECT_EQ(p.modes[0].wire, heating_level::OFF);
  EXPECT_EQ(p.modes[1].hvac, HeatingHvacMode::HEAT);
  EXPECT_EQ(p.modes[1].wire, heating_level::COMFORT);

  // Exact preset set: the standard presets and the comfort offsets; the secured level is reported, never a preset.
  ASSERT_EQ(p.presets.size(), 6u);
  EXPECT_EQ(p.presets[0].id, HeatingPresetId::AWAY);
  EXPECT_EQ(p.presets[0].wire, heating_level::FROST_PROTECTION);
  EXPECT_EQ(p.presets[1].id, HeatingPresetId::ECO);
  EXPECT_EQ(p.presets[2].id, HeatingPresetId::COMFORT);
  EXPECT_EQ(p.presets[3].id, HeatingPresetId::BOOST);
  EXPECT_EQ(p.presets[3].wire, heating_level::BOOST);
  EXPECT_STREQ(p.presets[4].custom_name, "Comfort -1");
  EXPECT_EQ(p.presets[4].wire, heating_level::COMFORT_MINUS_1);
  EXPECT_STREQ(p.presets[5].custom_name, "Comfort -2");
  EXPECT_EQ(p.presets[5].wire, heating_level::COMFORT_MINUS_2);
}

TEST(HeatingProfiles, TheSecuredLevelIsReadAsHeatWithNoPreset) {
  for (const DeviceModel model : {DeviceModel::NONE, DeviceModel::PILOT_WIRE_INTERFACE}) {
    const HeatingProfile &p = test::profile_of(DeviceType::ELECTRICAL_HEATER, 0, model);
    const HeatingObservedState *secured = find_observed_state(p, heating_level::SECURED);
    ASSERT_NE(secured, nullptr);
    EXPECT_EQ(secured->hvac, HeatingHvacMode::HEAT);
    EXPECT_EQ(secured->preset, -1) << "no preset: the heater reports it and a controller cannot set it";
  }
}

TEST(HeatingProfiles, EveryLevelTheProfileCanCommandIsReadBackAsItself) {
  const HeatingProfile &p = test::level_heater_profile();
  for (size_t i = 0; i < p.presets.size(); i++) {
    const HeatingObservedState *state = find_observed_state(p, p.presets[i].wire);
    ASSERT_NE(state, nullptr) << "preset " << i;
    EXPECT_EQ(state->hvac, HeatingHvacMode::HEAT);
    EXPECT_EQ(state->preset, static_cast<int8_t>(i)) << "level 0x" << std::hex << +p.presets[i].wire;
  }
  for (size_t i = 0; i < p.modes.size(); i++)
    EXPECT_NE(find_observed_state(p, p.modes[i].wire), nullptr) << "mode " << i;
  EXPECT_EQ(find_observed_state(p, heating_level::OFF)->hvac, HeatingHvacMode::OFF);
  EXPECT_EQ(find_observed_state(p, heating_level::OFF)->preset, -1);
  EXPECT_EQ(find_observed_state(p, 0x06), nullptr) << "a level nobody defined";
}

TEST(HeatingProfiles, ObservedTablesAndPresetsAgreeInEveryProfile) {
  const struct {
    DeviceType type;
    uint8_t subtype;
  } profiles[] = {{DeviceType::ELECTRICAL_HEATER, 0},
                  {DeviceType::ELECTRICAL_HEATER, 1},
                  {DeviceType::HEATING_TEMPERATURE_INTERFACE, 0},
                  {DeviceType::HEAT_PUMP, 0}};
  for (const auto &kind : profiles) {
    const std::string where = std::to_string(static_cast<unsigned>(kind.type)) + "/" + std::to_string(+kind.subtype);
    const HeatingProfile *p = find_heating_profile(kind.type, kind.subtype, DeviceModel::NONE);
    ASSERT_NE(p, nullptr) << where;
    for (size_t r = 0; r < p->observed.size(); r++) {
      const HeatingObservedState &row = p->observed[r];
      ASSERT_LT(row.preset, static_cast<int8_t>(p->presets.size())) << where << " row " << r;
      if (row.preset >= 0) {
        EXPECT_EQ(p->presets[row.preset].wire, row.wire) << where << " row " << r << ": names a preset of its wire";
      }
    }
    // A commanded preset is shown through the observed table, so each one needs exactly one row naming it.
    for (size_t i = 0; i < p->presets.size(); i++) {
      size_t rows = 0;
      for (size_t r = 0; r < p->observed.size(); r++) {
        if (p->observed[r].wire == p->presets[i].wire) {
          rows++;
          EXPECT_EQ(p->observed[r].preset, static_cast<int8_t>(i)) << where << " preset " << i << " row " << r;
        }
      }
      EXPECT_EQ(rows, 1u) << where << " preset " << i;
    }
  }
}

// === Register-read probe gate ===

TEST(HeatingProfiles, RegisterProbeIsAllowedOnlyForRegisterHeatersAndHotWater) {
  EXPECT_TRUE(device_accepts_register_probe(DeviceType::ELECTRICAL_HEATER, 1)) << "register-protocol heater";
  EXPECT_TRUE(device_accepts_register_probe(DeviceType::DOMESTIC_HOT_WATER, 0));
  EXPECT_TRUE(device_accepts_register_probe(DeviceType::DOMESTIC_HOT_WATER, 58)) << "any subtype of the type";

  EXPECT_FALSE(device_accepts_register_probe(DeviceType::ELECTRICAL_HEATER, 0)) << "takes the standard EXECUTE";
  EXPECT_FALSE(device_accepts_register_probe(DeviceType::ROLLER_SHUTTER, 0));
  EXPECT_FALSE(device_accepts_register_probe(DeviceType::VERTICAL_EXTERIOR_AWNING, 0));
  EXPECT_FALSE(device_accepts_register_probe(DeviceType::LIGHT, 0));
  EXPECT_FALSE(device_accepts_register_probe(DeviceType::LOCK, 0));
  EXPECT_FALSE(device_accepts_register_probe(DeviceType::ON_OFF_SWITCH, 0));
  EXPECT_FALSE(device_accepts_register_probe(DeviceType::UNKNOWN, 0));
}

// === Towel dryer (52/1, model TOWEL_DRYER) ===

namespace {

bool lists(std::span<const DeviceRegister> list, DeviceRegister fn) {
  return std::find(list.begin(), list.end(), fn) != list.end();
}

}  // namespace

TEST(HeatingProfilesTowelDryer, TheModelSelectsItsOwnProfileAndNoModelKeepsTheRadiator) {
  const HeatingProfile *radiator = find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE);
  const HeatingProfile *towel = find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER);
  ASSERT_NE(radiator, nullptr);
  ASSERT_NE(towel, nullptr);
  EXPECT_NE(towel, radiator);
  EXPECT_EQ(radiator->model, DeviceModel::NONE);
  EXPECT_EQ(towel->model, DeviceModel::TOWEL_DRYER);
  EXPECT_EQ(find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::TOWEL_DRYER),
            find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE))
      << "a model the (type, subtype) does not offer falls back to its profile";

  IoDevice dev{};
  dev.type = DeviceType::ELECTRICAL_HEATER;
  dev.subtype = 1;
  EXPECT_EQ(find_heating_profile(dev), radiator);
  dev.model = DeviceModel::TOWEL_DRYER;
  EXPECT_EQ(find_heating_profile(dev), towel) << "the device overload passes the declared model";
}

TEST(HeatingProfilesTowelDryer, AModelProfileKeepsTheControlPathOfTheProfileAboveIt) {
  // The path predicates (device_uses_heating_registers() and friends) take no model: this is why.
  for (const auto model :
       {DeviceModel::TOWEL_DRYER, DeviceModel::HEATER_WITH_LIGHT, DeviceModel::PILOT_WIRE_INTERFACE}) {
    for (unsigned type = 0; type <= 0x3FF; type++) {
      for (unsigned subtype = 0; subtype <= 0x3F; subtype++) {
        const auto t = static_cast<DeviceType>(type);
        const auto s = static_cast<uint8_t>(subtype);
        const HeatingProfile *with = find_heating_profile(t, s, model);
        const HeatingProfile *without = find_heating_profile(t, s, DeviceModel::NONE);
        EXPECT_EQ(with != nullptr, without != nullptr) << format_device_ids(t, s);
        if (with != nullptr && without != nullptr) {
          EXPECT_EQ(with->path, without->path) << format_device_ids(t, s);
        }
      }
    }
  }
}

TEST(HeatingProfilesTowelDryer, ModesMapToTheTowelModeRegisterInBothDirections) {
  const HeatingProfile &p = test::towel_dryer_profile();
  EXPECT_EQ(p.path, HeatingControlPath::PRIVATE_REGISTERS);
  EXPECT_EQ(p.mode_fn, DeviceRegister::TOWEL_MODE);
  ASSERT_EQ(p.modes.size(), 3u);
  const struct {
    HeatingHvacMode hvac;
    TowelDryerMode wire;
  } expected[] = {{HeatingHvacMode::OFF, TowelDryerMode::OFF},
                  {HeatingHvacMode::HEAT, TowelDryerMode::MANUAL},
                  {HeatingHvacMode::AUTO, TowelDryerMode::PROG}};
  for (size_t i = 0; i < 3; i++) {
    EXPECT_EQ(p.modes[i].hvac, expected[i].hvac) << i;
    EXPECT_EQ(p.modes[i].wire, static_cast<uint8_t>(expected[i].wire)) << i;
    const HeatingObservedState *seen = find_observed_state(p, p.modes[i].wire);
    ASSERT_NE(seen, nullptr) << i;
    EXPECT_EQ(seen->hvac, expected[i].hvac) << "what is written reads back as the same mode";
    EXPECT_EQ(seen->preset, -1) << "the mode register never names a preset";
  }
  EXPECT_EQ(find_observed_state(p, static_cast<uint8_t>(TowelDryerMode::AUTO)), nullptr)
      << "value 1 has never been seen";
}

TEST(HeatingProfilesTowelDryer, PresetsWriteTheTemporaryStateAndReadBackAsThemselves) {
  const HeatingProfile &p = test::towel_dryer_profile();
  EXPECT_EQ(p.preset_fn, DeviceRegister::TEMPORARY_STATE);
  ASSERT_FALSE(p.preset_observed.empty());
  ASSERT_EQ(p.presets.size(), 3u) << "none, boost and drying: comfort and eco are no presets";
  EXPECT_EQ(p.presets[0].id, HeatingPresetId::NONE);
  EXPECT_EQ(p.presets[1].id, HeatingPresetId::BOOST);
  EXPECT_EQ(p.presets[2].id, HeatingPresetId::CUSTOM);
  EXPECT_STREQ(p.presets[2].custom_name, "Drying");
  // The timed runs are commanded and read back through the temporary state.
  for (size_t i = 0; i < 3; i++) {
    const HeatingObservedState *seen = find_observed_preset(p, p.presets[i].wire);
    ASSERT_NE(seen, nullptr) << i;
    EXPECT_EQ(seen->preset, static_cast<int8_t>(i)) << "a written preset reads back as itself";
  }
  EXPECT_EQ(p.presets[1].wire, static_cast<uint8_t>(HeatingTemporaryState::BOOST));
  EXPECT_EQ(p.presets[2].wire, static_cast<uint8_t>(HeatingTemporaryState::DRYING));
  EXPECT_EQ(find_observed_preset(p, 3), nullptr);
}

TEST(HeatingProfilesTowelDryer, TheStatusActionComesFromTheWorkingRateAndTheWritesAreNotExperimental) {
  const HeatingProfile &p = test::towel_dryer_profile();
  EXPECT_EQ(p.action_source, HeatingActionSource::WORKING_RATE);
  EXPECT_FALSE(p.experimental_writes);
}

TEST(HeatingProfilesTowelDryer, TheProgrammePeriodNamesTheRegistersOfTheProgramme) {
  const HeatingProfile &p = test::towel_dryer_profile();
  ASSERT_NE(p.programme_period, nullptr);
  const HeatingProgrammePeriod &period = *p.programme_period;
  EXPECT_EQ(period.idle_preset_state, static_cast<uint8_t>(HeatingTemporaryState::NONE));
  EXPECT_EQ(period.in_force_fn, DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE);
  EXPECT_EQ(period.derogation_fn, DeviceRegister::DEROGATED_SETPOINT) << "a target set in auto is written here";
  EXPECT_TRUE(period.reports_level) << "the dryer answers the status read with its level";
  for (const HeatingPresetEntry &preset : p.presets) {
    EXPECT_NE(preset.id, HeatingPresetId::COMFORT) << "the period is shown by the level sensor, not a preset";
    EXPECT_NE(preset.id, HeatingPresetId::ECO);
  }
}

TEST(HeatingProfilesProgramme, TheRadiatorFamilyNamesItsOwnRegistersAndTheModelsInheritThem) {
  const HeatingProfile *radiator = find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE);
  ASSERT_NE(radiator->programme_period, nullptr);
  EXPECT_EQ(radiator->programme_period->in_force_fn, DeviceRegister::TEMPERATURE_IN_FORCE);
  EXPECT_EQ(radiator->programme_period->derogation_fn, DeviceRegister::DEROGATED_SETPOINT);
  EXPECT_FALSE(radiator->programme_period->reports_level)
      << "no radiator has answered the status read with a level, so none is asked";
  EXPECT_EQ(find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::HEATER_WITH_LIGHT)->programme_period,
            radiator->programme_period);
  EXPECT_NE(radiator->programme_period, test::towel_dryer_profile().programme_period);
}

TEST(HeatingProfilesProgramme, ProfilesWithoutAProgrammeHaveNoPeriod) {
  EXPECT_EQ(find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE)->programme_period, nullptr);
  EXPECT_EQ(find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::PILOT_WIRE_INTERFACE)->programme_period,
            nullptr);
  EXPECT_EQ(find_heating_profile(DeviceType::HEAT_PUMP, 0, DeviceModel::NONE)->programme_period, nullptr);
  EXPECT_EQ(find_heating_profile(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0, DeviceModel::NONE)->programme_period,
            nullptr);
}

namespace {

/// A towel dryer's readings in the state of the display: mode, temporary state, comfort 20.0.
IoDevice towel_readings(TowelDryerMode mode, HeatingTemporaryState state, float in_force_c) {
  IoDevice r;
  test::store_reading(r, DeviceReading::TOWEL_MODE, static_cast<int16_t>(mode));
  test::store_reading(r, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(state));
  test::store_reading(r, DeviceReading::SETPOINT, 20.0F);
  test::store_reading(r, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE, in_force_c);
  return r;
}

}  // namespace

TEST(HeatingProfilesTowelDryer, TheTemperatureInForceIsTheProgrammesTarget) {
  EXPECT_FLOAT_EQ(
      programme_temperature_in_force(test::towel_dryer_profile(),
                                     towel_readings(TowelDryerMode::PROG, HeatingTemporaryState::NONE, 20.0F)),
      20.0F);
  EXPECT_FLOAT_EQ(
      programme_temperature_in_force(test::towel_dryer_profile(),
                                     towel_readings(TowelDryerMode::PROG, HeatingTemporaryState::NONE, 16.5F)),
      16.5F)
      << "an eco period: whatever the heater holds, with no setback arithmetic";
  EXPECT_FLOAT_EQ(
      programme_temperature_in_force(test::towel_dryer_profile(),
                                     towel_readings(TowelDryerMode::PROG, HeatingTemporaryState::NONE, 7.0F)),
      7.0F);
}

TEST(HeatingProfilesTowelDryer, WithoutTheTemperatureInForceThereIsNoTargetToShow) {
  IoDevice r = towel_readings(TowelDryerMode::PROG, HeatingTemporaryState::NONE, 20.0F);
  test::clear_reading(r, DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE);
  EXPECT_TRUE(std::isnan(programme_temperature_in_force(test::towel_dryer_profile(), r)))
      << "the programme runs, it is just not read yet";
}

// --- the programme runs while the card shows auto -------------------------------------------

namespace {

/// A radiator's readings: the mode register and the temperature in force.
IoDevice radiator_readings(HeatingMode mode, float in_force_c) {
  IoDevice r;
  test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(mode));
  test::store_reading(r, DeviceReading::SETPOINT, 20.0F);
  test::store_reading(r, DeviceReading::TEMPERATURE_IN_FORCE, in_force_c);
  return r;
}

}  // namespace

TEST(HeatingProfilesProgramme, ARadiatorRunsItsProgrammeInProgAndInAutoButNotInManualOffOrExternal) {
  for (const HeatingMode mode : {HeatingMode::PROG, HeatingMode::AUTO}) {
    const IoDevice r = radiator_readings(mode, 17.5F);
    EXPECT_TRUE(programme_period_active(test::radiator_profile(), r)) << static_cast<int>(mode);
    EXPECT_FLOAT_EQ(programme_temperature_in_force(test::radiator_profile(), r), 17.5F) << static_cast<int>(mode);
  }
  for (const HeatingMode mode : {HeatingMode::MANUAL, HeatingMode::OFF, HeatingMode::EXTERNAL_SETPOINT}) {
    const IoDevice r = radiator_readings(mode, 17.5F);
    EXPECT_FALSE(programme_period_active(test::radiator_profile(), r)) << static_cast<int>(mode);
    EXPECT_TRUE(std::isnan(programme_temperature_in_force(test::radiator_profile(), r))) << static_cast<int>(mode);
  }
  IoDevice unread;
  EXPECT_FALSE(programme_period_active(test::radiator_profile(), unread)) << "an unread mode is not the programme";
}

TEST(HeatingProfilesProgramme, ARadiatorsModeRegisterValueIsNotMistakenForATimedRun) {
  // The radiator has no register for timed runs; its preset register defaults to the mode register, whose
  // AUTO value 0 equals the idle value a towel dryer's temporary state reads.
  EXPECT_TRUE(programme_period_active(test::radiator_profile(), radiator_readings(HeatingMode::PROG, 17.5F)));
}

TEST(HeatingProfilesTowelDryer, TheProgrammeIsActiveOnlyInProgWithNoTimedRunOnTop) {
  const HeatingProfile &p = test::towel_dryer_profile();
  const struct {
    TowelDryerMode mode;
    HeatingTemporaryState state;
    bool active;
  } cases[] = {
      {TowelDryerMode::PROG, HeatingTemporaryState::NONE, true},
      {TowelDryerMode::PROG, HeatingTemporaryState::BOOST, false},
      {TowelDryerMode::PROG, HeatingTemporaryState::DRYING, false},
      {TowelDryerMode::MANUAL, HeatingTemporaryState::NONE, false},
      {TowelDryerMode::OFF, HeatingTemporaryState::NONE, false},
      {TowelDryerMode::AUTO, HeatingTemporaryState::NONE, false},
  };
  for (const auto &c : cases) {
    // 0x0137 reads 1 in boost and 2 in drying: a value that must never be taken for a temperature.
    const IoDevice r = towel_readings(c.mode, c.state, 0.1F);
    EXPECT_EQ(programme_period_active(p, r), c.active)
        << "mode " << static_cast<int>(c.mode) << " state " << static_cast<int>(c.state);
    if (!c.active) {
      EXPECT_TRUE(std::isnan(programme_temperature_in_force(p, r)))
          << "no temperature is reported outside the programme";
    }
  }
  IoDevice unread;
  EXPECT_FALSE(programme_period_active(p, unread)) << "an unread mode is not the programme";
  EXPECT_FALSE(programme_period_active(*find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE),
                                       towel_readings(TowelDryerMode::PROG, HeatingTemporaryState::NONE, 20.0F)))
      << "a profile without a programme period has none";
}

TEST(HeatingProfilesTowelDryer, TheTemperatureInForceIsDroppedWhenTheProgrammeStopsAndKeptWhileItRuns) {
  const HeatingProfile &p = test::towel_dryer_profile();
  IoDevice r = towel_readings(TowelDryerMode::PROG, HeatingTemporaryState::NONE, 20.0F);
  clear_inactive_programme_reading(p, r);
  EXPECT_TRUE(test::has_reading(r, DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE)) << "kept while the programme runs";

  test::store_reading(r, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::BOOST));
  clear_inactive_programme_reading(p, r);
  EXPECT_FALSE(test::has_reading(r, DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE)) << "dropped in boost";
  EXPECT_TRUE(test::has_reading(r, DeviceRegister::COMFORT_SETPOINT)) << "the comfort setpoint is not part of it";

  // Back in the programme the old value must not come back.
  test::store_reading(r, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::NONE));
  EXPECT_TRUE(std::isnan(programme_temperature_in_force(p, r)));

  IoDevice plain;
  test::store_reading(plain, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE, 5.0F);
  clear_inactive_programme_reading(*find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE), plain);
  EXPECT_TRUE(test::has_reading(plain, DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE))
      << "a profile without a period never touches it";

  IoDevice manual = radiator_readings(HeatingMode::MANUAL, 17.5F);
  clear_inactive_programme_reading(test::radiator_profile(), manual);
  EXPECT_FALSE(test::has_reading(manual, DeviceRegister::TEMPERATURE_IN_FORCE)) << "a radiator in heat holds none";
}

TEST(HeatingProfiles, OnlyAPrivateRegisterProfileNamesItsModeAndPresetRegisters) {
  for (const HeatingProfile &p : heating_profiles()) {
    const bool private_registers = p.path == HeatingControlPath::PRIVATE_REGISTERS;
    EXPECT_EQ(p.mode_fn.has_value(), private_registers)
        << static_cast<int>(p.type) << "/" << static_cast<int>(p.subtype);
    EXPECT_EQ(p.preset_fn.has_value(), private_registers)
        << static_cast<int>(p.type) << "/" << static_cast<int>(p.subtype);
  }
  for (const DeviceModel model : {DeviceModel::NONE, DeviceModel::PILOT_WIRE_INTERFACE}) {
    const HeatingProfile &level = test::profile_of(DeviceType::ELECTRICAL_HEATER, 0, model);
    EXPECT_FALSE(level.mode_fn.has_value());
    EXPECT_FALSE(level.preset_fn.has_value());
  }
  for (const DeviceType type : {DeviceType::HEAT_PUMP, DeviceType::HEATING_TEMPERATURE_INTERFACE}) {
    const HeatingProfile &temperature = test::profile_of(type, 0, DeviceModel::NONE);
    EXPECT_FALSE(temperature.mode_fn.has_value());
    EXPECT_FALSE(temperature.preset_fn.has_value());
  }
}

TEST(HeatingProfilesTowelDryer, TheRadiatorProfileNeverTouchesTheTowelRegisters) {
  const HeatingProfile &p = test::radiator_profile();
  EXPECT_EQ(p.mode_fn, DeviceRegister::OPERATING_MODE);
  EXPECT_TRUE(p.preset_observed.empty());
  EXPECT_EQ(p.action_source, HeatingActionSource::REGULATOR);
  EXPECT_FALSE(p.experimental_writes);
  const PinnedPoll radiator_poll = pinned_poll(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE);
  for (const DeviceRegister towel :
       {DeviceRegister::TOWEL_MODE, DeviceRegister::TEMPORARY_STATE, DeviceRegister::WORKING_RATE,
        DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE, DeviceRegister::ECO_SETBACK}) {
    EXPECT_FALSE(lists(radiator_poll.every_poll, towel)) << device_register_name(towel);
    EXPECT_FALSE(lists(radiator_poll.once_after_boot, towel)) << device_register_name(towel);
    EXPECT_NE(p.preset_fn, towel) << device_register_name(towel);
  }
  for (size_t i = 0; i < p.presets.size(); i++)
    EXPECT_NE(p.presets[i].id, HeatingPresetId::NONE) << "the radiator offers no 'none' preset";
}

// === Heater with a night light (52/1, model HEATER_WITH_LIGHT) ===

TEST(HeatingProfilesHeaterWithLight, HasNoProfileOfItsOwn) {
  const HeatingProfile &radiator = test::radiator_profile();
  EXPECT_EQ(find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::HEATER_WITH_LIGHT), &radiator)
      << "the model drives the card exactly as the radiator does";
}

// === Pilot-wire interface (52/0, model PILOT_WIRE_INTERFACE) ===

TEST(HeatingProfilesPilotWireInterface, OffersTheCommandablePilotWireOrdersAndNoBoost) {
  const HeatingProfile &plain = test::level_heater_profile();
  const HeatingProfile *pilot =
      find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::PILOT_WIRE_INTERFACE);
  ASSERT_NE(pilot, nullptr);
  ASSERT_NE(pilot, &plain);
  EXPECT_EQ(pilot->model, DeviceModel::PILOT_WIRE_INTERFACE);

  // Away, Eco, Comfort, Comfort -1 and Comfort -2; Off is the mode.
  ASSERT_EQ(pilot->presets.size(), 5u);
  const uint8_t commandable[] = {heating_level::FROST_PROTECTION, heating_level::ECO, heating_level::COMFORT,
                                 heating_level::COMFORT_MINUS_1, heating_level::COMFORT_MINUS_2};
  for (size_t i = 0; i < std::size(commandable); i++) {
    EXPECT_EQ(pilot->presets[i].wire, commandable[i]) << i;
    EXPECT_NE(pilot->presets[i].id, HeatingPresetId::BOOST) << i;
  }
  for (const HeatingPresetEntry &preset : pilot->presets)
    EXPECT_NE(preset.wire, heating_level::BOOST);
  for (const HeatingObservedState &row : pilot->observed)
    EXPECT_NE(row.wire, heating_level::BOOST);
  EXPECT_EQ(pilot->modes.size(), 2u);
}

TEST(HeatingProfilesPilotWireInterface, EveryObservedLevelNamesTheSamePresetAsThePlainHeater) {
  const HeatingProfile &plain = test::level_heater_profile();
  const HeatingProfile &pilot =
      *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::PILOT_WIRE_INTERFACE);
  for (const HeatingObservedState &row : pilot.observed) {
    const auto same = std::find_if(plain.observed.begin(), plain.observed.end(),
                                   [&](const HeatingObservedState &p) { return p.wire == row.wire; });
    ASSERT_NE(same, plain.observed.end()) << static_cast<int>(row.wire);
    EXPECT_EQ(same->hvac, row.hvac);
    if (row.preset == -1) {
      EXPECT_EQ(same->preset, -1);
    } else {
      const HeatingPresetEntry &a = pilot.presets[static_cast<size_t>(row.preset)];
      const HeatingPresetEntry &b = plain.presets[static_cast<size_t>(same->preset)];
      EXPECT_EQ(a.id, b.id);
      EXPECT_EQ(a.wire, b.wire);
    }
  }
}

TEST(HeatingProfilesPilotWireInterface, KeepsThePlainHeatersControlPathAndMainParameterPrefix) {
  const HeatingProfile &plain = test::level_heater_profile();
  const HeatingProfile &pilot =
      *find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::PILOT_WIRE_INTERFACE);
  EXPECT_EQ(pilot.path, HeatingControlPath::HEATING_LEVEL);
  EXPECT_EQ(pilot.path, plain.path);
  EXPECT_EQ(pilot.mp_prefix, plain.mp_prefix);
  EXPECT_EQ(pilot.modes.data(), plain.modes.data());
  EXPECT_EQ(pilot.features.mode_observed, plain.features.mode_observed);
  EXPECT_EQ(find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::PILOT_WIRE_INTERFACE),
            find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE))
      << "a subtype that does not offer the model falls back to its profile";
}

TEST(HeatingProfilesPilotWireInterface, APlainHeaterKeepsItsBoost) {
  const HeatingProfile &plain = test::level_heater_profile();
  EXPECT_EQ(plain.presets.size(), 6u);
  EXPECT_EQ(plain.presets[3].id, HeatingPresetId::BOOST);
}

namespace {

/// @return Whether changing one register of a device moves the card of its profile, on a device whose mode shows
/// `shown`
///         (the programme's temperature in force replaces the setpoint on the card while the programme runs, so a
///         register may matter in one state only).
bool card_moves_with(const HeatingProfile &profile, DeviceRegister changed_fn, HeatingHvacMode shown) {
  const auto mode = std::ranges::find(profile.observed, shown, &HeatingObservedState::hvac);
  EXPECT_NE(mode, profile.observed.end());
  IoDevice base;
  for (const DeviceReadingRow &row : device_readings()) {
    if (const auto *source = std::get_if<RegisterSource>(&row.source))
      test::store_reading(base, source->fn, 1.0F);
  }
  test::store_reading(base, *profile.mode_fn, static_cast<float>(mode->wire));
  if (!profile.preset_observed.empty())
    test::store_reading(base, *profile.preset_fn, static_cast<float>(profile.programme_period->idle_preset_state));
  const HeatingCardState before = reduce_heating_card(profile, base, initial_heating_card(profile));
  for (const float value : {0.0F, 2.0F, 18.0F, 22.5F}) {
    IoDevice changed = base;
    test::store_reading(changed, changed_fn, value);
    if (!reduce_heating_card(profile, changed, initial_heating_card(profile)).shows_same_as(before))
      return true;
  }
  return false;
}

}  // namespace

TEST(HeatingProfileReads, TheRegistersTheCardReadsAreTheOnesTheProfileSays) {
  // The card moves with a register exactly when the profile says it reads it, apart from the programme's override,
  // which is read for the write that goes to it and not shown.
  for (const HeatingProfile &profile : heating_profiles()) {
    if (profile.path != HeatingControlPath::PRIVATE_REGISTERS)
      continue;
    for (const DeviceReadingRow &row : device_readings()) {
      const auto *source = std::get_if<RegisterSource>(&row.source);
      if (source == nullptr)
        continue;
      const bool card_reads = card_moves_with(profile, source->fn, HeatingHvacMode::AUTO) ||
                              card_moves_with(profile, source->fn, HeatingHvacMode::HEAT);
      const bool override_register =
          profile.programme_period != nullptr && source->fn == profile.programme_period->derogation_fn;
      EXPECT_EQ(heating_profile_reads(profile, row.reading), card_reads || override_register)
          << row.name << " on " << static_cast<int>(profile.type) << "/" << static_cast<int>(profile.subtype);
    }
  }
}

TEST(HeatingProfileReads, AProfileOfAnotherControlPathReadsNoRegister) {
  for (const HeatingProfile &profile : heating_profiles()) {
    if (profile.path == HeatingControlPath::PRIVATE_REGISTERS)
      continue;
    for (const DeviceReadingRow &row : device_readings())
      EXPECT_FALSE(heating_profile_reads(profile, row.reading)) << row.name;
  }
  EXPECT_FALSE(heating_profile_reads(*find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE),
                                     DeviceReading::HEATING_LEVEL))
      << "a status reading is no register";
  EXPECT_FALSE(heating_profile_reads(*find_heating_profile(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE),
                                     DeviceReading::COUNT));
}

// === Every preset is a command ===

TEST(HeatingProfiles, EveryPresetOfEveryProfileSendsACommand) {
  for (const HeatingProfile &profile : heating_profiles()) {
    for (const HeatingPresetEntry &preset : profile.presets) {
      const std::string label = std::string(preset.custom_name != nullptr ? preset.custom_name : "standard") + " of " +
                                std::to_string(static_cast<int>(profile.type)) + "/" + std::to_string(profile.subtype);
      switch (profile.path) {
        case HeatingControlPath::PRIVATE_REGISTERS:
          EXPECT_TRUE(heating_write_accepts(*profile.preset_fn, static_cast<float>(preset.wire)))
              << label << ": the register's rule takes the value the preset writes";
          break;
        case HeatingControlPath::HEATING_LEVEL:
          EXPECT_NE(preset.wire, heating_level::SECURED) << label << ": a level a heater reports and never takes";
          break;
        case HeatingControlPath::TEMPERATURE_EXECUTE:
          break;
      }
      // A selected preset becomes a write, never a refusal.
      HeatingWriteRequest request;
      request.kind = HeatingWriteKind::PRESET;
      request.preset = preset.id;
      request.custom_name = preset.custom_name;
      const HeatingWritePlan plan = plan_heating_write(profile, request, HeatingDeviceFacts{.has_record = true});
      EXPECT_TRUE(plan.is_write()) << label;
    }
  }
}

TEST(HeatingProfiles, EveryModeAProfileWritesToTheModeRegisterIsWritable) {
  for (const HeatingProfile &p : heating_profiles()) {
    if (p.path != HeatingControlPath::PRIVATE_REGISTERS)
      continue;
    for (const HeatingModeEntry &m : p.modes)
      EXPECT_TRUE(heating_write_accepts(*p.mode_fn, static_cast<float>(m.wire)))
          << "mode " << static_cast<int>(m.wire) << " of profile " << static_cast<int>(p.type) << "/"
          << static_cast<int>(p.subtype) << ": the register's rule takes it";
    // A profile whose mode register is another one (the towel dryer's) has its own value set.
    if (p.mode_fn == DeviceRegister::OPERATING_MODE) {
      for (const HeatingModeEntry &m : p.modes)
        EXPECT_TRUE(heating_mode_is_writable(static_cast<HeatingMode>(m.wire)))
            << "mode " << static_cast<int>(m.wire) << " of profile " << static_cast<int>(p.type) << "/"
            << static_cast<int>(p.subtype);
    }
    for (const HeatingPresetEntry &pr : p.presets) {
      if (p.preset_fn != DeviceRegister::OPERATING_MODE)
        continue;
      EXPECT_TRUE(heating_mode_is_writable(static_cast<HeatingMode>(pr.wire)))
          << "preset " << static_cast<int>(pr.wire) << " of profile " << static_cast<int>(p.type) << "/"
          << static_cast<int>(p.subtype);
    }
  }
}

// === The temperature in force and the programme registers ===

TEST(HeatingProfileInForce, TheStateIsTheStoresAbsentAndReportedMarksOfTheInForceReading) {
  IoDevice dev;
  dev.type = DeviceType::ELECTRICAL_HEATER;
  dev.subtype = 1;
  const HeatingProfile *profile = find_heating_profile(dev);
  ASSERT_NE(profile, nullptr);
  ASSERT_NE(profile->programme_period, nullptr);
  const DeviceReading in_force = find_register_reading(profile->programme_period->in_force_fn)->reading;
  EXPECT_EQ(in_force_support(*profile, dev), InForceSupport::UNKNOWN) << "nothing answered yet";
  mark_reading_absent(dev, in_force);
  EXPECT_EQ(in_force_support(*profile, dev), InForceSupport::ABSENT);
  store_reading(dev, in_force, 17.0F);
  EXPECT_EQ(in_force_support(*profile, dev), InForceSupport::REPORTED) << "a value lifts the absent mark";
  clear_reading(dev, in_force);
  EXPECT_EQ(in_force_support(*profile, dev), InForceSupport::REPORTED) << "kept until the next boot";
}

TEST(HeatingProfileInForce, AProfileWithoutAProgrammeHasNoInForceState) {
  const HeatingProfile *profile = find_heating_profile(DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::NONE);
  ASSERT_NE(profile, nullptr);
  ASSERT_EQ(profile->programme_period, nullptr);
  EXPECT_EQ(in_force_support(*profile, IoDevice{}), InForceSupport::UNKNOWN);
  EXPECT_FALSE(is_programme_register(*profile, DeviceRegister::DEROGATED_SETPOINT));
}

TEST(HeatingProfileInForce, TheProgrammeRegistersAreTheOnesAProfileNames) {
  for (const HeatingProfile &profile : heating_profiles()) {
    if (profile.programme_period == nullptr)
      continue;
    EXPECT_TRUE(is_programme_register(profile, profile.programme_period->in_force_fn));
    EXPECT_TRUE(is_programme_register(profile, profile.programme_period->derogation_fn));
    EXPECT_FALSE(is_programme_register(profile, *profile.mode_fn));
  }
  EXPECT_TRUE(is_programme_register_of_any_profile(DeviceRegister::DEROGATED_SETPOINT));
  EXPECT_FALSE(is_programme_register_of_any_profile(DeviceRegister::OPERATING_MODE));
}
