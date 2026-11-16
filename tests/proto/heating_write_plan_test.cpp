/// @file heating_write_plan_test.cpp
/// @brief Tests for plan_heating_write(): what a climate request writes to each kind of heater, without an
/// entity or a hub.

#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_device_types.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace esphome::home_io_control;

namespace {

const HeatingProfile &profile_of(DeviceType type, uint8_t subtype, DeviceModel model = DeviceModel::NONE) {
  const HeatingProfile *profile = find_heating_profile(type, subtype, model);
  EXPECT_NE(profile, nullptr);
  return *profile;
}

HeatingWriteRequest mode_request(HeatingHvacMode hvac, float shown_target_c = NAN) {
  return {.kind = HeatingWriteKind::MODE, .hvac = hvac, .shown_target_c = shown_target_c};
}

HeatingWriteRequest preset_request(HeatingPresetId preset, const char *custom_name = nullptr) {
  return {.kind = HeatingWriteKind::PRESET, .preset = preset, .custom_name = custom_name};
}

HeatingWriteRequest target_request(float target_c) { return {.kind = HeatingWriteKind::TARGET, .target_c = target_c}; }

/// A device the hub knows, with the YAML range and the setpoint it last reported.
HeatingDeviceFacts known_device(float setpoint_c = NAN, float min_c = 7.0F, float max_c = 30.0F) {
  return {.has_record = true, .range_min_c = min_c, .range_max_c = max_c, .reported_setpoint_c = setpoint_c};
}

/// The main parameter of a temperature over a range, as the heater decodes it.
uint16_t encoded(float celsius, float min_c, float max_c) {
  uint16_t main_parameter = 0;
  EXPECT_TRUE(encode_temperature_main_parameter(celsius, min_c, max_c, main_parameter));
  return main_parameter;
}

}  // namespace

// === Setpoint radiator (private registers) ===

TEST(HeatingWritePlan, RadiatorModeIsARegisterWriteThatIsReadBack) {
  const HeatingProfile &radiator = profile_of(DeviceType::ELECTRICAL_HEATER, 1);

  const HeatingWritePlan plan = plan_heating_write(radiator, mode_request(HeatingHvacMode::HEAT), known_device());

  ASSERT_EQ(plan.kind, HeatingPlanKind::REGISTER);
  EXPECT_EQ(plan.register_fn, radiator.mode_fn);
  EXPECT_FLOAT_EQ(plan.register_value, static_cast<float>(radiator.modes[1].wire));
  EXPECT_TRUE(plan.read_back);
  EXPECT_EQ(plan.read_back_fn, radiator.mode_fn);
  EXPECT_EQ(plan.refusal, HeatingRefusal::NONE);
}

TEST(HeatingWritePlan, RadiatorPresetWritesItsRegisterAndNamesTheMatchedEntry) {
  const HeatingProfile &radiator = profile_of(DeviceType::ELECTRICAL_HEATER, 1);
  const HeatingWritePlan plan =
      plan_heating_write(radiator, preset_request(HeatingPresetId::CUSTOM, "Program"), known_device());

  ASSERT_EQ(plan.kind, HeatingPlanKind::REGISTER);
  ASSERT_GE(plan.preset_index, 0);
  const HeatingPresetEntry &entry = radiator.presets[plan.preset_index];
  EXPECT_STREQ(entry.custom_name, "Program");
  EXPECT_EQ(plan.register_fn, entry.write_fn);
  EXPECT_FLOAT_EQ(plan.register_value, static_cast<float>(entry.wire));
  EXPECT_TRUE(plan.read_back);
  EXPECT_EQ(plan.read_back_fn, entry.write_fn);
}

TEST(HeatingWritePlan, RadiatorTargetIsAnUnencodedSetpointWriteThatIsReadBack) {
  const HeatingProfile &radiator = profile_of(DeviceType::ELECTRICAL_HEATER, 1);

  const HeatingWritePlan plan = plan_heating_write(radiator, target_request(20.5F), HeatingDeviceFacts{});

  ASSERT_EQ(plan.kind, HeatingPlanKind::REGISTER) << "the register protocol needs no device record";
  EXPECT_EQ(plan.register_fn, HeatingFunction::SET_TEMPERATURE);
  EXPECT_FLOAT_EQ(plan.register_value, 20.5F);
  EXPECT_TRUE(plan.read_back);
  EXPECT_EQ(plan.read_back_fn, HeatingFunction::SET_TEMPERATURE);
}

// === A target while the programme runs: a temporary override ===

namespace {

/// Facts of a heater whose programme runs (@p runs) with the comfort setpoint at 20.0.
HeatingDeviceFacts programme_facts(bool runs) {
  HeatingDeviceFacts facts = known_device(20.0F);
  facts.programme_runs = runs;
  return facts;
}

/// A heater with an adjustable setpoint or a towel dryer, by model.
const HeatingProfile &programme_profile(DeviceModel model) {
  return profile_of(DeviceType::ELECTRICAL_HEATER, 1, model);
}

}  // namespace

TEST(HeatingWritePlan, ATargetWhileTheProgrammeRunsIsAnOverrideWithAReadBackOnBothHeaterFamilies) {
  for (const DeviceModel model : {DeviceModel::TOWEL_DRYER, DeviceModel::NONE, DeviceModel::HEATER_WITH_LIGHT}) {
    const HeatingWritePlan plan =
        plan_heating_write(programme_profile(model), target_request(17.5F), programme_facts(true));
    ASSERT_EQ(plan.kind, HeatingPlanKind::REGISTER) << static_cast<int>(model);
    EXPECT_EQ(plan.register_fn, HeatingFunction::DEROGATED_SETPOINT)
        << "the comfort setpoint stays as it is, in every period";
    EXPECT_FLOAT_EQ(plan.register_value, 17.5F);
    EXPECT_TRUE(plan.temporary_override);
    EXPECT_TRUE(plan.read_back);
    EXPECT_EQ(plan.read_back_fn, HeatingFunction::DEROGATED_SETPOINT);
  }
}

TEST(HeatingWritePlan, ATargetWhileTheProgrammeDoesNotRunWritesTheComfortSetpoint) {
  for (const DeviceModel model : {DeviceModel::TOWEL_DRYER, DeviceModel::NONE}) {
    // heat, off, a timed run on top, or no temperature in force reported: no programme the target could override
    const HeatingWritePlan plan =
        plan_heating_write(programme_profile(model), target_request(21.0F), programme_facts(false));
    ASSERT_EQ(plan.kind, HeatingPlanKind::REGISTER);
    EXPECT_EQ(plan.register_fn, HeatingFunction::SET_TEMPERATURE);
    EXPECT_EQ(plan.read_back_fn, HeatingFunction::SET_TEMPERATURE);
    EXPECT_FALSE(plan.temporary_override);
    EXPECT_EQ(plan_heating_write(programme_profile(model), target_request(21.0F), known_device(20.0F)).register_fn,
              HeatingFunction::SET_TEMPERATURE)
        << "facts that say nothing about a programme";
  }
}

TEST(HeatingWritePlan, AProfileWithoutAProgrammeWritesTheSetpointWhateverTheFactsSay) {
  const HeatingProfile &plain = profile_of(DeviceType::ELECTRICAL_HEATER, 0);
  EXPECT_FALSE(plan_heating_write(plain, target_request(21.0F), programme_facts(true)).temporary_override);
  const HeatingProfile &thermostat = profile_of(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0);
  EXPECT_FALSE(plan_heating_write(thermostat, target_request(21.0F), programme_facts(true)).temporary_override);
}

TEST(HeatingWritePlan, ModeAndPresetWritesAreNeverOverrides) {
  const HeatingProfile &dryer = programme_profile(DeviceModel::TOWEL_DRYER);
  EXPECT_FALSE(
      plan_heating_write(dryer, mode_request(HeatingHvacMode::HEAT), programme_facts(true)).temporary_override);
  EXPECT_FALSE(
      plan_heating_write(dryer, preset_request(HeatingPresetId::BOOST), programme_facts(true)).temporary_override);
}

namespace {

/// A heater record whose mode register shows @p mode_wire and whose temperature in force is @p in_force_c.
IoDevice heater_record(DeviceModel model, int16_t mode_wire, float in_force_c) {
  IoDevice dev;
  dev.type = DeviceType::ELECTRICAL_HEATER;
  dev.subtype = 1;
  dev.model = model;
  dev.heating.towel_mode = model == DeviceModel::TOWEL_DRYER ? mode_wire : -1;
  dev.heating.mode = model == DeviceModel::TOWEL_DRYER ? -1 : mode_wire;
  dev.heating.temporary_state = static_cast<int16_t>(HeatingTemporaryState::NONE);
  dev.heating.setpoint_c = 20.0F;
  dev.heating.in_force_temperature_c = in_force_c;
  return dev;
}

}  // namespace

TEST(HeatingWritePlan, DeviceFactsSayTheProgrammeRunsWhenTheCardShowsAutoAndATemperatureInForceIsKnown) {
  IoDevice dryer = heater_record(DeviceModel::TOWEL_DRYER, static_cast<int16_t>(TowelDryerMode::PROG), 16.5F);
  EXPECT_TRUE(heating_device_facts(&dryer).programme_runs) << "in an eco period";
  dryer.heating.in_force_temperature_c = 20.0F;
  EXPECT_TRUE(heating_device_facts(&dryer).programme_runs) << "in a comfort period: the level plays no part";
  dryer.heating.level = heating_level::COMFORT;
  EXPECT_TRUE(heating_device_facts(&dryer).programme_runs);
  dryer.heating.in_force_temperature_c = NAN;
  EXPECT_FALSE(heating_device_facts(&dryer).programme_runs) << "a heater that reports no temperature in force";
  dryer.heating.in_force_temperature_c = 20.0F;
  dryer.heating.towel_mode = static_cast<int16_t>(TowelDryerMode::MANUAL);
  EXPECT_FALSE(heating_device_facts(&dryer).programme_runs) << "MANUEL: no programme";
  dryer.heating.towel_mode = static_cast<int16_t>(TowelDryerMode::PROG);
  dryer.heating.temporary_state = static_cast<int16_t>(HeatingTemporaryState::BOOST);
  EXPECT_FALSE(heating_device_facts(&dryer).programme_runs) << "a timed run is on top";

  for (const HeatingMode mode : {HeatingMode::PROG, HeatingMode::AUTO}) {
    IoDevice radiator = heater_record(DeviceModel::NONE, static_cast<int16_t>(mode), 17.0F);
    EXPECT_TRUE(heating_device_facts(&radiator).programme_runs) << static_cast<int>(mode);
  }
  IoDevice manual = heater_record(DeviceModel::NONE, static_cast<int16_t>(HeatingMode::MANUAL), 17.0F);
  EXPECT_FALSE(heating_device_facts(&manual).programme_runs);
  EXPECT_FALSE(heating_device_facts(nullptr).programme_runs);
}

TEST(HeatingWritePlan, AnOverrideIsTakenWhenTheRegisterHoldsTheRequestToATenth) {
  EXPECT_TRUE(heating_override_taken(17.5F, 17.5F));
  EXPECT_TRUE(heating_override_taken(17.5F, 17.5001F));
  EXPECT_FALSE(heating_override_taken(17.5F, 17.6F)) << "the heater clamped or changed it";
  EXPECT_FALSE(heating_override_taken(17.5F, 0.0F)) << "no override holds";
  EXPECT_FALSE(heating_override_taken(17.5F, NAN)) << "no read-back";
}

TEST(HeatingWritePlan, TowelDryerModeAndTemporaryStateAreSeparateRegisters) {
  const HeatingProfile &dryer = profile_of(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER);

  const HeatingWritePlan mode = plan_heating_write(dryer, mode_request(HeatingHvacMode::AUTO), known_device());
  ASSERT_EQ(mode.kind, HeatingPlanKind::REGISTER);
  EXPECT_EQ(mode.register_fn, HeatingFunction::TOWEL_MODE);
  EXPECT_EQ(mode.read_back_fn, HeatingFunction::TOWEL_MODE);

  const HeatingWritePlan boost = plan_heating_write(dryer, preset_request(HeatingPresetId::BOOST), known_device());
  ASSERT_EQ(boost.kind, HeatingPlanKind::REGISTER);
  EXPECT_EQ(boost.register_fn, HeatingFunction::TEMPORARY_STATE);
  EXPECT_FLOAT_EQ(boost.register_value, static_cast<float>(HeatingTemporaryState::BOOST));
  EXPECT_EQ(boost.read_back_fn, HeatingFunction::TEMPORARY_STATE);

  const HeatingWritePlan drying =
      plan_heating_write(dryer, preset_request(HeatingPresetId::CUSTOM, "Drying"), known_device());
  ASSERT_EQ(drying.kind, HeatingPlanKind::REGISTER);
  EXPECT_FLOAT_EQ(drying.register_value, static_cast<float>(HeatingTemporaryState::DRYING));

  const HeatingWritePlan none = plan_heating_write(dryer, preset_request(HeatingPresetId::NONE), known_device());
  ASSERT_EQ(none.kind, HeatingPlanKind::REGISTER);
  EXPECT_FLOAT_EQ(none.register_value, static_cast<float>(HeatingTemporaryState::NONE));
}

TEST(HeatingWritePlan, ComfortAndEcoAreNoPresetOfATowelDryer) {
  const HeatingProfile &dryer = profile_of(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER);

  for (const HeatingPresetId preset : {HeatingPresetId::COMFORT, HeatingPresetId::ECO}) {
    const HeatingWritePlan plan = plan_heating_write(dryer, preset_request(preset), known_device());
    EXPECT_FALSE(plan.is_write());
    EXPECT_EQ(plan.refusal, HeatingRefusal::NO_MATCH) << "the card never offers them";
  }
}

// === Plain heater (heating levels) ===

TEST(HeatingWritePlan, PlainHeaterModesAndPresetsAreLevelsWithoutAReadBack) {
  const HeatingProfile &plain = profile_of(DeviceType::ELECTRICAL_HEATER, 0);

  const HeatingWritePlan off = plan_heating_write(plain, mode_request(HeatingHvacMode::OFF), known_device());
  ASSERT_EQ(off.kind, HeatingPlanKind::LEVEL);
  EXPECT_EQ(off.level, heating_level::OFF);
  EXPECT_FALSE(off.read_back) << "the settle poll reads a level back";

  const HeatingWritePlan heat = plan_heating_write(plain, mode_request(HeatingHvacMode::HEAT), known_device());
  EXPECT_EQ(heat.level, heating_level::COMFORT);

  const HeatingWritePlan away = plan_heating_write(plain, preset_request(HeatingPresetId::AWAY), known_device());
  ASSERT_EQ(away.kind, HeatingPlanKind::LEVEL);
  EXPECT_EQ(away.level, heating_level::FROST_PROTECTION);

  const HeatingWritePlan minus1 =
      plan_heating_write(plain, preset_request(HeatingPresetId::CUSTOM, "Comfort -1"), known_device());
  ASSERT_EQ(minus1.kind, HeatingPlanKind::LEVEL);
  EXPECT_EQ(minus1.level, heating_level::COMFORT_MINUS_1);
}

TEST(HeatingWritePlan, PlainHeaterHasNoSetpointAndNoSecuredPreset) {
  const HeatingProfile &plain = profile_of(DeviceType::ELECTRICAL_HEATER, 0);

  EXPECT_EQ(plan_heating_write(plain, target_request(20.0F), known_device()).refusal, HeatingRefusal::NO_SETPOINT);
  EXPECT_EQ(plan_heating_write(plain, preset_request(HeatingPresetId::CUSTOM, "Secured"), known_device()).refusal,
            HeatingRefusal::NO_MATCH);
}

// === Thermostat (FP9) ===

TEST(HeatingWritePlan, ThermostatOffAndAutoSendTheirMarkerWithFp9) {
  const HeatingProfile &thermostat = profile_of(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0);

  const HeatingWritePlan off = plan_heating_write(thermostat, mode_request(HeatingHvacMode::OFF), known_device());
  ASSERT_EQ(off.kind, HeatingPlanKind::TEMPERATURE);
  EXPECT_EQ(off.main_parameter, HEATING_OFF_MP);
  EXPECT_EQ(off.fp.kind, HeatingFpKind::FP9);
  EXPECT_EQ(off.fp.value, heating_mode_flags::OFF);
  EXPECT_FALSE(off.read_back);

  const HeatingWritePlan automatic =
      plan_heating_write(thermostat, mode_request(HeatingHvacMode::AUTO), known_device());
  EXPECT_EQ(automatic.main_parameter, heating_prefixed_main_parameter(HEATING_ALIAS_MP_PREFIX, heating_alias::COMFORT));
  EXPECT_EQ(automatic.fp.value, heating_mode_flags::AUTO);
}

TEST(HeatingWritePlan, ThermostatHeatSendsTheShownTargetOverTheYamlRange) {
  const HeatingProfile &thermostat = profile_of(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0);

  const HeatingWritePlan plan =
      plan_heating_write(thermostat, mode_request(HeatingHvacMode::HEAT, 21.0F), known_device(19.0F, 7.0F, 30.0F));

  ASSERT_EQ(plan.kind, HeatingPlanKind::TEMPERATURE);
  EXPECT_EQ(plan.main_parameter, encoded(21.0F, 7.0F, 30.0F)) << "the shown target wins over the reported setpoint";
  EXPECT_EQ(plan.fp.kind, HeatingFpKind::FP9);
  EXPECT_EQ(plan.fp.value, heating_mode_flags::MANUAL);
}

TEST(HeatingWritePlan, ThermostatHeatFallsBackToTheReportedSetpointAndRefusesWithNeither) {
  const HeatingProfile &thermostat = profile_of(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0);

  const HeatingWritePlan reported =
      plan_heating_write(thermostat, mode_request(HeatingHvacMode::HEAT), known_device(19.5F, 7.0F, 30.0F));
  ASSERT_EQ(reported.kind, HeatingPlanKind::TEMPERATURE);
  EXPECT_EQ(reported.main_parameter, encoded(19.5F, 7.0F, 30.0F));

  const HeatingWritePlan neither = plan_heating_write(thermostat, mode_request(HeatingHvacMode::HEAT), known_device());
  EXPECT_EQ(neither.kind, HeatingPlanKind::NONE);
  EXPECT_EQ(neither.refusal, HeatingRefusal::NO_TARGET);

  const HeatingWritePlan no_record =
      plan_heating_write(thermostat, mode_request(HeatingHvacMode::HEAT, 21.0F), HeatingDeviceFacts{});
  EXPECT_EQ(no_record.refusal, HeatingRefusal::NO_TARGET) << "a target cannot be encoded without the device's range";
}

TEST(HeatingWritePlan, ThermostatTargetAndAliasPresetsCarryTheirFunctionalParameter) {
  const HeatingProfile &thermostat = profile_of(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0);

  const HeatingWritePlan target = plan_heating_write(thermostat, target_request(22.0F), known_device(NAN, 5.0F, 35.0F));
  ASSERT_EQ(target.kind, HeatingPlanKind::TEMPERATURE);
  EXPECT_EQ(target.main_parameter, encoded(22.0F, 5.0F, 35.0F));
  EXPECT_EQ(target.fp.kind, thermostat.setpoint_fp.kind);
  EXPECT_EQ(target.fp.value, thermostat.setpoint_fp.value);

  const HeatingWritePlan eco = plan_heating_write(thermostat, preset_request(HeatingPresetId::ECO), known_device());
  ASSERT_EQ(eco.kind, HeatingPlanKind::TEMPERATURE);
  EXPECT_EQ(eco.main_parameter,
            heating_prefixed_main_parameter(HEATING_ALIAS_MP_PREFIX, heating_alias::THERMOSTAT_ECO));
  EXPECT_EQ(eco.fp.kind, HeatingFpKind::NONE);
  EXPECT_FALSE(eco.read_back);
}

// === Heat pump (FP16, fixed range) ===

TEST(HeatingWritePlan, HeatPumpOffAndOnCarryFp16) {
  const HeatingProfile &pump = profile_of(DeviceType::HEAT_PUMP, 0);

  const HeatingWritePlan off = plan_heating_write(pump, mode_request(HeatingHvacMode::OFF), known_device());
  ASSERT_EQ(off.kind, HeatingPlanKind::TEMPERATURE);
  EXPECT_EQ(off.main_parameter, HEATING_OFF_MP);
  EXPECT_EQ(off.fp.kind, HeatingFpKind::FP16);
  EXPECT_EQ(off.fp.value, heating_fp16::OFF);

  const HeatingWritePlan setback =
      plan_heating_write(pump, preset_request(HeatingPresetId::CUSTOM, "Setback"), known_device());
  ASSERT_EQ(setback.kind, HeatingPlanKind::TEMPERATURE);
  EXPECT_EQ(setback.main_parameter,
            heating_prefixed_main_parameter(HEATING_ALIAS_MP_PREFIX, heating_alias::HEAT_PUMP_SETBACK));
  EXPECT_EQ(setback.fp.value, heating_fp16::ON);
}

TEST(HeatingWritePlan, AFixedRangeProfileIgnoresTheYamlRange) {
  const HeatingProfile &pump = profile_of(DeviceType::HEAT_PUMP, 0);
  ASSERT_TRUE(pump.fixed_range);

  const HeatingWritePlan plan = plan_heating_write(pump, target_request(21.0F), known_device(NAN, 7.0F, 30.0F));

  ASSERT_EQ(plan.kind, HeatingPlanKind::TEMPERATURE);
  EXPECT_EQ(plan.main_parameter, encoded(21.0F, HEATING_TEMPERATURE_RANGE_MIN_C, HEATING_TEMPERATURE_RANGE_MAX_C));
  EXPECT_NE(plan.main_parameter, encoded(21.0F, 7.0F, 30.0F));
}

// === Refusals ===

TEST(HeatingWritePlan, ATargetNeedsADeviceRecordAndAFiniteTemperature) {
  const HeatingProfile &thermostat = profile_of(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0);

  EXPECT_EQ(plan_heating_write(thermostat, target_request(21.0F), HeatingDeviceFacts{}).refusal,
            HeatingRefusal::UNENCODABLE);
  EXPECT_EQ(plan_heating_write(thermostat, target_request(NAN), known_device()).refusal, HeatingRefusal::UNENCODABLE);
  EXPECT_EQ(plan_heating_write(thermostat, target_request(21.0F), known_device(NAN, 30.0F, 30.0F)).refusal,
            HeatingRefusal::UNENCODABLE)
      << "an empty range encodes nothing";
}

TEST(HeatingWritePlan, AModeOrPresetTheProfileDoesNotOfferMatchesNothing) {
  const HeatingProfile &pump = profile_of(DeviceType::HEAT_PUMP, 0);

  EXPECT_EQ(plan_heating_write(pump, mode_request(HeatingHvacMode::AUTO), known_device()).refusal,
            HeatingRefusal::NO_MATCH);
  EXPECT_EQ(plan_heating_write(pump, preset_request(HeatingPresetId::BOOST), known_device()).refusal,
            HeatingRefusal::NO_MATCH);
  EXPECT_EQ(plan_heating_write(pump, preset_request(HeatingPresetId::CUSTOM, "Nope"), known_device()).refusal,
            HeatingRefusal::NO_MATCH);
  EXPECT_EQ(plan_heating_write(pump, preset_request(HeatingPresetId::CUSTOM, nullptr), known_device()).refusal,
            HeatingRefusal::NO_MATCH);
}

TEST(HeatingWritePlan, DeviceFactsCarryTheRecordsRangeAndReportedSetpoint) {
  EXPECT_FALSE(heating_device_facts(nullptr).has_record);

  IoDevice dev;
  dev.temperature_range_min_c = 6.0F;
  dev.temperature_range_max_c = 28.0F;
  dev.heating.setpoint_c = 19.0F;
  const HeatingDeviceFacts facts = heating_device_facts(&dev);
  EXPECT_TRUE(facts.has_record);
  EXPECT_FLOAT_EQ(facts.range_min_c, 6.0F);
  EXPECT_FLOAT_EQ(facts.range_max_c, 28.0F);
  EXPECT_FLOAT_EQ(facts.reported_setpoint_c, 19.0F);
}
