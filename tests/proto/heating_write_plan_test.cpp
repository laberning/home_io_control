/// @file heating_write_plan_test.cpp
/// @brief Tests for plan_heating_write(): what a climate request writes to each kind of heater, without an
/// entity or a hub.

#include "proto_codecs.h"
#include "reading_store.h"
#include "proto_commands.h"
#include "proto_device_readings.h"
#include "proto_device_types.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"

#include <gtest/gtest.h>

#include <cmath>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

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

/// A device the hub knows, with the YAML range and the setpoint it last reported, whose programme-state read
/// was answered.
HeatingDeviceFacts known_device(float setpoint_c = NAN, float min_c = 7.0F, float max_c = 30.0F) {
  return {.has_record = true,
          .range_min_c = min_c,
          .range_max_c = max_c,
          .reported_setpoint_c = setpoint_c,
          .programme_state_answered = true};
}

/// The main parameter of a temperature over a range, as the heater decodes it.
uint16_t encoded(float celsius, float min_c, float max_c) {
  uint16_t main_parameter = 0;
  EXPECT_TRUE(encode_temperature_main_parameter(celsius, min_c, max_c, main_parameter));
  return main_parameter;
}

}  // namespace

// === Which plan holds a register ===

TEST(HeatingWritePlan, OnlyARegisterPlanHoldsARegister) {
  const HeatingWriteRequest target_requests[] = {target_request(20.5F), target_request(NAN)};
  const HeatingDeviceFacts facts[] = {known_device(20.0F), {}, {.has_record = true, .programme_state_answered = false}};
  for (const HeatingProfile &profile : heating_profiles()) {
    std::vector<HeatingWriteRequest> requests(std::begin(target_requests), std::end(target_requests));
    for (const HeatingHvacMode hvac : {HeatingHvacMode::OFF, HeatingHvacMode::HEAT, HeatingHvacMode::AUTO})
      requests.push_back(mode_request(hvac, 21.0F));
    for (const HeatingPresetEntry &entry : profile.presets)
      requests.push_back(preset_request(entry.id, entry.custom_name));
    requests.push_back(preset_request(HeatingPresetId::CUSTOM, "No such preset"));
    for (const HeatingWriteRequest &request : requests) {
      for (const HeatingDeviceFacts &fact : facts) {
        const HeatingWritePlan plan = plan_heating_write(profile, request, fact);
        const std::string label = "profile " + std::to_string(static_cast<int>(profile.type)) + "/" +
                                  std::to_string(profile.subtype) + ", request kind " +
                                  std::to_string(static_cast<int>(request.kind));
        EXPECT_EQ(plan.register_fn.has_value(), plan.kind == HeatingPlanKind::REGISTER) << label;
        EXPECT_FALSE(plan.read_back && !plan.register_fn) << label << ": a read-back names a register";
      }
    }
  }
}

// === Setpoint radiator (private registers) ===

TEST(HeatingWritePlan, RadiatorModeIsARegisterWriteThatIsReadBack) {
  const HeatingProfile &radiator = profile_of(DeviceType::ELECTRICAL_HEATER, 1);

  const HeatingWritePlan plan = plan_heating_write(radiator, mode_request(HeatingHvacMode::HEAT), known_device());

  ASSERT_EQ(plan.kind, HeatingPlanKind::REGISTER);
  EXPECT_EQ(plan.register_fn, radiator.mode_fn);
  EXPECT_FLOAT_EQ(plan.register_value, static_cast<float>(radiator.modes[1].wire));
  EXPECT_TRUE(plan.read_back);
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
  EXPECT_EQ(plan.register_fn, radiator.preset_fn);
  EXPECT_FLOAT_EQ(plan.register_value, static_cast<float>(entry.wire));
  EXPECT_TRUE(plan.read_back);
}

TEST(HeatingWritePlan, RadiatorTargetIsAnUnencodedSetpointWriteThatIsReadBack) {
  const HeatingProfile &radiator = profile_of(DeviceType::ELECTRICAL_HEATER, 1);

  const HeatingWritePlan plan = plan_heating_write(radiator, target_request(20.5F), {.programme_state_answered = true});

  ASSERT_EQ(plan.kind, HeatingPlanKind::REGISTER) << "the register protocol needs no device record";
  EXPECT_EQ(plan.register_fn, DeviceRegister::COMFORT_SETPOINT);
  EXPECT_FLOAT_EQ(plan.register_value, 20.5F);
  EXPECT_TRUE(plan.read_back);
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
    // The dryer takes the override in an eco period; the radiators in every period.
    HeatingDeviceFacts facts = programme_facts(true);
    facts.period = HeatingPeriod::ECO;
    const HeatingWritePlan plan = plan_heating_write(programme_profile(model), target_request(17.5F), facts);
    ASSERT_EQ(plan.kind, HeatingPlanKind::REGISTER) << static_cast<int>(model);
    EXPECT_EQ(plan.register_fn, DeviceRegister::DEROGATED_SETPOINT) << "the comfort setpoint stays as it is";
    EXPECT_FLOAT_EQ(plan.register_value, 17.5F);
    EXPECT_TRUE(plan.temporary_override);
    EXPECT_TRUE(plan.read_back);
  }
}

TEST(HeatingWritePlan, ATargetWhileTheProgrammeDoesNotRunWritesTheComfortSetpoint) {
  for (const DeviceModel model : {DeviceModel::TOWEL_DRYER, DeviceModel::NONE}) {
    // heat, off, a timed run on top, or no temperature in force reported: no programme the target could override
    const HeatingWritePlan plan =
        plan_heating_write(programme_profile(model), target_request(21.0F), programme_facts(false));
    ASSERT_EQ(plan.kind, HeatingPlanKind::REGISTER);
    EXPECT_EQ(plan.register_fn, DeviceRegister::COMFORT_SETPOINT);
    EXPECT_FALSE(plan.temporary_override);
    EXPECT_EQ(plan_heating_write(programme_profile(model), target_request(21.0F), known_device(20.0F)).register_fn,
              DeviceRegister::COMFORT_SETPOINT)
        << "facts that say nothing about a programme";
  }
}

TEST(HeatingWritePlan, ATowelDryerTargetInAComfortPeriodIsTheComfortSetpointAndInAnEcoPeriodTheOverride) {
  const HeatingProfile &dryer = programme_profile(DeviceModel::TOWEL_DRYER);
  HeatingDeviceFacts facts = programme_facts(true);

  facts.period = HeatingPeriod::COMFORT;
  const HeatingWritePlan comfort = plan_heating_write(dryer, target_request(21.5F), facts);
  ASSERT_EQ(comfort.kind, HeatingPlanKind::REGISTER);
  EXPECT_EQ(comfort.register_fn, DeviceRegister::COMFORT_SETPOINT) << "the dryer ignores an override here";
  EXPECT_FLOAT_EQ(comfort.register_value, 21.5F);
  EXPECT_FALSE(comfort.temporary_override);
  EXPECT_TRUE(comfort.read_back);

  facts.period = HeatingPeriod::ECO;
  const HeatingWritePlan eco = plan_heating_write(dryer, target_request(21.5F), facts);
  EXPECT_EQ(eco.register_fn, DeviceRegister::DEROGATED_SETPOINT);
  EXPECT_TRUE(eco.temporary_override);
}

TEST(HeatingWritePlan, ATowelDryerTargetInAutoWithNoPeriodIsRefusedAndWritesNothing) {
  HeatingDeviceFacts facts = programme_facts(true);
  facts.period = HeatingPeriod::UNKNOWN;
  const HeatingWritePlan plan =
      plan_heating_write(programme_profile(DeviceModel::TOWEL_DRYER), target_request(21.0F), facts);
  EXPECT_FALSE(plan.is_write()) << "neither the override nor the comfort write is known to be right";
  EXPECT_EQ(plan.refusal, HeatingRefusal::PERIOD_UNKNOWN);
}

TEST(HeatingWritePlan, ThePeriodDecidesNothingOutsideTheProgrammeOrOnARadiator) {
  for (const HeatingPeriod period : {HeatingPeriod::UNKNOWN, HeatingPeriod::COMFORT, HeatingPeriod::ECO}) {
    HeatingDeviceFacts facts = programme_facts(true);
    facts.period = period;
    for (const DeviceModel model : {DeviceModel::NONE, DeviceModel::HEATER_WITH_LIGHT}) {
      const HeatingWritePlan plan = plan_heating_write(programme_profile(model), target_request(21.0F), facts);
      EXPECT_EQ(plan.register_fn, DeviceRegister::DEROGATED_SETPOINT)
          << "a radiator keeps the override in every period, " << static_cast<int>(period);
    }
    facts.programme_runs = false;
    EXPECT_EQ(plan_heating_write(programme_profile(DeviceModel::TOWEL_DRYER), target_request(21.0F), facts).register_fn,
              DeviceRegister::COMFORT_SETPOINT)
        << "heat, or a timed run on top: the comfort setpoint without any period";
  }
}

TEST(HeatingWritePlan, ATargetInAutoBeforeTheHeaterAnsweredTheTemperatureInForceReadIsRefusedUntilItDoes) {
  for (const DeviceModel model : {DeviceModel::TOWEL_DRYER, DeviceModel::NONE}) {
    HeatingDeviceFacts facts = known_device(20.0F);
    facts.in_force_unresolved = true;
    const HeatingWritePlan plan = plan_heating_write(programme_profile(model), target_request(21.0F), facts);
    EXPECT_FALSE(plan.is_write()) << static_cast<int>(model)
                                  << ": neither an override nor a comfort write is known right";
    EXPECT_EQ(plan.refusal, HeatingRefusal::IN_FORCE_UNKNOWN);
  }
  HeatingDeviceFacts facts = known_device(20.0F);
  facts.in_force_unresolved = true;
  EXPECT_NE(plan_heating_write(profile_of(DeviceType::ELECTRICAL_HEATER, 0), target_request(21.0F), facts).refusal,
            HeatingRefusal::IN_FORCE_UNKNOWN)
      << "a profile without a programme never waits for a temperature in force";
}

TEST(HeatingWritePlan, ATargetOnAHeaterThatDidNotSayWhatItRunsIsRefusedOnBothHeaterFamilies) {
  for (const DeviceModel model : {DeviceModel::TOWEL_DRYER, DeviceModel::NONE, DeviceModel::HEATER_WITH_LIGHT}) {
    // Whatever the record holds: a stored mode that shows the programme, a reported temperature in force, none
    // of it decides without the read made for this request.
    HeatingDeviceFacts facts = programme_facts(true);
    facts.programme_state_answered = false;
    const HeatingWritePlan plan = plan_heating_write(programme_profile(model), target_request(21.0F), facts);
    EXPECT_FALSE(plan.is_write()) << static_cast<int>(model) << ": no register is known to be the right one";
    EXPECT_EQ(plan.refusal, HeatingRefusal::PROGRAMME_STATE_UNANSWERED);

    facts = programme_facts(false);
    facts.programme_state_answered = false;
    EXPECT_EQ(plan_heating_write(programme_profile(model), target_request(21.0F), facts).refusal,
              HeatingRefusal::PROGRAMME_STATE_UNANSWERED)
        << "also when the stored mode shows heat";
    facts.in_force_unresolved = true;
    EXPECT_EQ(plan_heating_write(programme_profile(model), target_request(21.0F), facts).refusal,
              HeatingRefusal::PROGRAMME_STATE_UNANSWERED)
        << "the mode is asked before the temperature in force";
  }
}

TEST(HeatingWritePlan, AModeOrPresetNeverWaitsForTheProgrammeStateRead) {
  for (const DeviceModel model : {DeviceModel::TOWEL_DRYER, DeviceModel::NONE}) {
    HeatingDeviceFacts facts = known_device(20.0F);
    facts.programme_state_answered = false;
    EXPECT_TRUE(plan_heating_write(programme_profile(model), mode_request(HeatingHvacMode::AUTO), facts).is_write());
  }
  HeatingDeviceFacts facts = known_device(20.0F);
  facts.programme_state_answered = false;
  for (const HeatingWriteRequest &request :
       {preset_request(HeatingPresetId::BOOST), preset_request(HeatingPresetId::CUSTOM, "Drying")}) {
    const HeatingWritePlan plan = plan_heating_write(programme_profile(DeviceModel::TOWEL_DRYER), request, facts);
    EXPECT_TRUE(plan.is_write()) << "a boost or drying run is a write of its own register, whatever the mode";
    EXPECT_EQ(plan.register_fn, DeviceRegister::TEMPORARY_STATE);
  }
}

TEST(HeatingWritePlan, AProfileWithoutAProgrammeIsNeverRefusedForTheProgrammeState) {
  HeatingDeviceFacts facts = known_device(20.0F);
  facts.programme_state_answered = false;
  const HeatingProfile &thermostat = profile_of(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0);
  EXPECT_TRUE(plan_heating_write(thermostat, target_request(21.0F), facts).is_write());
  EXPECT_NE(plan_heating_write(profile_of(DeviceType::ELECTRICAL_HEATER, 0), target_request(21.0F), facts).refusal,
            HeatingRefusal::PROGRAMME_STATE_UNANSWERED);
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

/// A heater record whose mode register shows @p mode_wire and whose temperature in force is @p in_force_c; a
/// number marks it reported in this boot.
IoDevice heater_record(DeviceModel model, int16_t mode_wire, float in_force_c) {
  IoDevice dev;
  dev.type = DeviceType::ELECTRICAL_HEATER;
  dev.subtype = 1;
  dev.model = model;
  const bool towel = model == DeviceModel::TOWEL_DRYER;
  test::store_wire(dev, DeviceReading::TOWEL_MODE, towel ? mode_wire : -1);
  test::store_wire(dev, DeviceReading::MODE, towel ? -1 : mode_wire);
  test::store_reading(dev, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::NONE));
  test::store_reading(dev, DeviceReading::SETPOINT, 20.0F);
  if (!std::isnan(in_force_c))
    test::store_reading(dev, towel ? DeviceReading::TOWEL_TEMPERATURE_IN_FORCE : DeviceReading::TEMPERATURE_IN_FORCE,
                        in_force_c);
  if (!std::isnan(in_force_c))
    test::set_in_force_support(dev, InForceSupport::REPORTED);
  return dev;
}

}  // namespace

TEST(HeatingWritePlan, DeviceFactsSayTheProgrammeRunsWhenTheModeShowsAutoOnAHeaterThatReportedATemperatureInForce) {
  IoDevice dryer = heater_record(DeviceModel::TOWEL_DRYER, static_cast<int16_t>(TowelDryerMode::PROG), 16.5F);
  EXPECT_TRUE(heating_device_facts(&dryer, true).programme_runs) << "in an eco period";
  test::store_reading(dryer, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE, 20.0F);
  EXPECT_TRUE(heating_device_facts(&dryer, true).programme_runs) << "in a comfort period: the level plays no part";
  test::store_reading(dryer, DeviceReading::HEATING_LEVEL, heating_level::COMFORT);
  EXPECT_TRUE(heating_device_facts(&dryer, true).programme_runs);
  clear_reading(dryer, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE);
  EXPECT_TRUE(heating_device_facts(&dryer, true).programme_runs)
      << "a read missed since the heater reported one: the target still leaves the comfort setpoint alone";
  test::set_in_force_support(dryer, InForceSupport::UNKNOWN);
  EXPECT_FALSE(heating_device_facts(&dryer, true).programme_runs) << "not reported yet in this boot";
  test::set_in_force_support(dryer, InForceSupport::ABSENT);
  EXPECT_FALSE(heating_device_facts(&dryer, true).programme_runs) << "answered empty: no such register";
  test::set_in_force_support(dryer, InForceSupport::REPORTED);
  test::store_reading(dryer, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE, 20.0F);
  test::store_reading(dryer, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::MANUAL));
  EXPECT_FALSE(heating_device_facts(&dryer, true).programme_runs) << "MANUEL: no programme";
  test::store_reading(dryer, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::PROG));
  test::store_reading(dryer, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::BOOST));
  EXPECT_FALSE(heating_device_facts(&dryer, true).programme_runs) << "a timed run is on top";

  for (const HeatingMode mode : {HeatingMode::PROG, HeatingMode::AUTO}) {
    IoDevice radiator = heater_record(DeviceModel::NONE, static_cast<int16_t>(mode), 17.0F);
    EXPECT_TRUE(heating_device_facts(&radiator, true).programme_runs) << static_cast<int>(mode);
  }
  IoDevice manual = heater_record(DeviceModel::NONE, static_cast<int16_t>(HeatingMode::MANUAL), 17.0F);
  EXPECT_FALSE(heating_device_facts(&manual, true).programme_runs);
  EXPECT_FALSE(heating_device_facts(nullptr, true).programme_runs);
}

TEST(HeatingWritePlan,
     DeviceFactsLeaveTheTemperatureInForceUnresolvedOnlyWhileTheProgrammeRunsAndTheHeaterHasNotAnswered) {
  IoDevice dryer = heater_record(DeviceModel::TOWEL_DRYER, static_cast<int16_t>(TowelDryerMode::PROG), NAN);
  ASSERT_EQ(test::in_force_of(dryer), InForceSupport::UNKNOWN);
  EXPECT_TRUE(heating_device_facts(&dryer, true).in_force_unresolved);
  EXPECT_FALSE(heating_device_facts(&dryer, true).programme_runs);
  test::set_in_force_support(dryer, InForceSupport::REPORTED);
  EXPECT_FALSE(heating_device_facts(&dryer, true).in_force_unresolved);
  test::set_in_force_support(dryer, InForceSupport::ABSENT);
  EXPECT_FALSE(heating_device_facts(&dryer, true).in_force_unresolved);
  test::set_in_force_support(dryer, InForceSupport::UNKNOWN);
  test::store_reading(dryer, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::MANUAL));
  EXPECT_FALSE(heating_device_facts(&dryer, true).in_force_unresolved) << "heat: a target is the comfort setpoint";
  test::store_reading(dryer, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::PROG));
  test::store_reading(dryer, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::DRYING));
  EXPECT_FALSE(heating_device_facts(&dryer, true).in_force_unresolved) << "a timed run is on top";
  EXPECT_FALSE(heating_device_facts(nullptr, true).in_force_unresolved);
}

TEST(HeatingWritePlan, DeviceFactsNameThePeriodFromTheHeatingLevelOnAProfileThatNeedsIt) {
  IoDevice dryer = heater_record(DeviceModel::TOWEL_DRYER, static_cast<int16_t>(TowelDryerMode::PROG), 17.0F);
  EXPECT_EQ(heating_device_facts(&dryer, true).period, HeatingPeriod::UNKNOWN) << "no level read";
  test::store_reading(dryer, DeviceReading::HEATING_LEVEL, heating_level::COMFORT);
  EXPECT_EQ(heating_device_facts(&dryer, true).period, HeatingPeriod::COMFORT);
  test::store_reading(dryer, DeviceReading::HEATING_LEVEL, heating_level::ECO);
  EXPECT_EQ(heating_device_facts(&dryer, true).period, HeatingPeriod::ECO);
  for (const int16_t other : {static_cast<int16_t>(heating_level::FROST_PROTECTION),
                              static_cast<int16_t>(heating_level::BOOST), static_cast<int16_t>(-1)}) {
    test::store_reading(dryer, DeviceReading::HEATING_LEVEL, other);
    EXPECT_EQ(heating_device_facts(&dryer, true).period, HeatingPeriod::UNKNOWN) << other;
  }
  IoDevice radiator = heater_record(DeviceModel::NONE, static_cast<int16_t>(HeatingMode::PROG), 17.0F);
  test::store_reading(radiator, DeviceReading::HEATING_LEVEL, heating_level::ECO);
  EXPECT_EQ(heating_device_facts(&radiator, true).period, HeatingPeriod::UNKNOWN) << "a radiator needs no period";
}

TEST(HeatingWritePlan, DeviceFactsCarryWhetherTheProgrammeStateReadWasAnswered) {
  IoDevice dryer = heater_record(DeviceModel::TOWEL_DRYER, static_cast<int16_t>(TowelDryerMode::PROG), 16.5F);
  EXPECT_TRUE(heating_device_facts(&dryer, true).programme_state_answered);
  EXPECT_FALSE(heating_device_facts(&dryer, false).programme_state_answered)
      << "the record alone says nothing: it may be stale";
  EXPECT_FALSE(heating_device_facts(nullptr, true).has_record);
}

namespace {

/// The one-step rule from a record: a target on @p dev, planned from the facts of a read that was answered.
HeatingWritePlan plan_target_from_record(const IoDevice &dev, float target_c = 21.0F) {
  return plan_heating_write(*find_heating_profile(dev), target_request(target_c), heating_device_facts(&dev, true));
}

}  // namespace

TEST(HeatingWritePlan, ATargetOnAProgrammeHeaterGoesWhereWhatTheHeaterJustSaidSendsIt) {
  struct Family {
    DeviceModel model;
    int16_t programme_mode;
    int16_t other_mode;
  };
  const Family families[] = {
      {DeviceModel::TOWEL_DRYER, static_cast<int16_t>(TowelDryerMode::PROG),
       static_cast<int16_t>(TowelDryerMode::MANUAL)},
      {DeviceModel::NONE, static_cast<int16_t>(HeatingMode::PROG), static_cast<int16_t>(HeatingMode::MANUAL)},
  };
  for (const Family &family : families) {
    const int m = static_cast<int>(family.model);
    // Not the programme (heat): the comfort setpoint, whatever the temperature-in-force state.
    for (const InForceSupport support : {InForceSupport::UNKNOWN, InForceSupport::REPORTED, InForceSupport::ABSENT}) {
      IoDevice heat = heater_record(family.model, family.other_mode, NAN);
      test::set_in_force_support(heat, support);
      EXPECT_EQ(plan_target_from_record(heat).register_fn, DeviceRegister::COMFORT_SETPOINT) << m;
    }
    // The programme runs and the heater reports a temperature in force: an override with a read-back (a dryer in
    // an eco period; in a comfort period it is the comfort setpoint, tested on its own).
    IoDevice reported = heater_record(family.model, family.programme_mode, 17.0F);
    test::store_reading(reported, DeviceReading::HEATING_LEVEL, heating_level::ECO);
    const HeatingWritePlan override_plan = plan_target_from_record(reported);
    EXPECT_EQ(override_plan.register_fn, DeviceRegister::DEROGATED_SETPOINT) << m;
    EXPECT_TRUE(override_plan.temporary_override);
    EXPECT_TRUE(override_plan.read_back);
    // The programme runs and the heater has no such register: the comfort setpoint.
    IoDevice absent = heater_record(family.model, family.programme_mode, NAN);
    test::set_in_force_support(absent, InForceSupport::ABSENT);
    EXPECT_EQ(plan_target_from_record(absent).register_fn, DeviceRegister::COMFORT_SETPOINT) << m;
    // The programme runs and the heater has not answered the temperature in force: refused.
    IoDevice unknown = heater_record(family.model, family.programme_mode, NAN);
    EXPECT_EQ(plan_target_from_record(unknown).refusal, HeatingRefusal::IN_FORCE_UNKNOWN) << m;
    // A mode value the profile does not know is not the programme: the comfort setpoint.
    IoDevice unmapped = heater_record(family.model, 99, 17.0F);
    EXPECT_EQ(plan_target_from_record(unmapped).register_fn, DeviceRegister::COMFORT_SETPOINT) << m;
  }
  // A towel dryer's boost or drying run on top of the programme: the comfort setpoint.
  for (const HeatingTemporaryState run : {HeatingTemporaryState::BOOST, HeatingTemporaryState::DRYING}) {
    IoDevice dryer = heater_record(DeviceModel::TOWEL_DRYER, static_cast<int16_t>(TowelDryerMode::PROG), 17.0F);
    test::store_reading(dryer, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(run));
    EXPECT_EQ(plan_target_from_record(dryer).register_fn, DeviceRegister::COMFORT_SETPOINT);
  }
}

TEST(HeatingWritePlan, ATimedRunRegisterMattersOnlyWhileTheModeShowsTheProgrammeOnAProfileThatHasOne) {
  const HeatingProfile &dryer = programme_profile(DeviceModel::TOWEL_DRYER);
  IoDevice readings;
  EXPECT_EQ(timed_run_register(dryer, readings), std::nullopt) << "an unread mode is not the programme";
  test::store_reading(readings, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::MANUAL));
  EXPECT_EQ(timed_run_register(dryer, readings), std::nullopt) << "MANUEL: no programme, whatever runs on top";
  test::store_reading(readings, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::PROG));
  EXPECT_EQ(timed_run_register(dryer, readings), DeviceRegister::TEMPORARY_STATE);
  test::store_reading(readings, DeviceReading::TOWEL_MODE, 99);
  EXPECT_EQ(timed_run_register(dryer, readings), std::nullopt) << "a mode the table does not know";

  const HeatingProfile &radiator = programme_profile(DeviceModel::NONE);
  IoDevice radiator_readings;
  test::store_reading(radiator_readings, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::PROG));
  EXPECT_EQ(timed_run_register(radiator, radiator_readings), std::nullopt) << "a radiator has no timed run";
  EXPECT_EQ(timed_run_register(profile_of(DeviceType::ELECTRICAL_HEATER, 0), radiator_readings), std::nullopt);
}

TEST(HeatingWritePlan, OnlyATargetOnAProfileWithAProgrammeReadsTheProgrammeStateFirst) {
  for (const DeviceModel model : {DeviceModel::TOWEL_DRYER, DeviceModel::NONE, DeviceModel::HEATER_WITH_LIGHT}) {
    const HeatingProfile &profile = programme_profile(model);
    EXPECT_TRUE(heating_request_reads_programme_state(profile, target_request(21.0F)));
    EXPECT_FALSE(heating_request_reads_programme_state(profile, mode_request(HeatingHvacMode::AUTO)));
    EXPECT_FALSE(heating_request_reads_programme_state(profile, preset_request(HeatingPresetId::BOOST)));
  }
  EXPECT_FALSE(
      heating_request_reads_programme_state(profile_of(DeviceType::ELECTRICAL_HEATER, 0), target_request(21.0F)));
  EXPECT_FALSE(heating_request_reads_programme_state(profile_of(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0),
                                                     target_request(21.0F)));
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
  EXPECT_EQ(mode.register_fn, DeviceRegister::TOWEL_MODE);

  const HeatingWritePlan boost = plan_heating_write(dryer, preset_request(HeatingPresetId::BOOST), known_device());
  ASSERT_EQ(boost.kind, HeatingPlanKind::REGISTER);
  EXPECT_EQ(boost.register_fn, DeviceRegister::TEMPORARY_STATE);
  EXPECT_FLOAT_EQ(boost.register_value, static_cast<float>(HeatingTemporaryState::BOOST));

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

TEST(HeatingWritePlan, HeatPlansTheSetpointAHeaterReportsAtEitherEndOfARangeThatIsNoWholeTenth) {
  // A range of 45 to 80 degrees Fahrenheit: its ends are no whole tenth of a degree Celsius, and the setpoint a status
  // reply decodes to stays inside the range, so heat is planned and not refused as out of range.
  const float min_c = 7.2222F;
  const float max_c = 26.6667F;
  const HeatingProfile &thermostat = profile_of(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0);
  for (const uint16_t word : {static_cast<uint16_t>(0), TEMPERATURE_MP_SPAN}) {
    const std::vector<uint8_t> main_parameter = {static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word & 0xFF)};
    const std::optional<float> setpoint =
        decode_status_reading(DeviceReading::STATUS_SETPOINT, main_parameter, 0, min_c, max_c);
    ASSERT_TRUE(setpoint.has_value()) << word;
    EXPECT_GE(*setpoint, min_c) << word;
    EXPECT_LE(*setpoint, max_c) << word;

    const HeatingWritePlan plan =
        plan_heating_write(thermostat, mode_request(HeatingHvacMode::HEAT), known_device(*setpoint, min_c, max_c));
    EXPECT_EQ(plan.kind, HeatingPlanKind::TEMPERATURE) << "heat on a setpoint at the end " << word;
    EXPECT_EQ(plan.main_parameter, word);
  }
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
  EXPECT_FALSE(heating_device_facts(nullptr, true).has_record);

  IoDevice dev;
  dev.type = DeviceType::ELECTRICAL_HEATER;
  dev.subtype = 1;
  dev.temperature_range_min_c = 6.0F;
  dev.temperature_range_max_c = 28.0F;
  test::store_reading(dev, DeviceReading::SETPOINT, 19.0F);
  const HeatingDeviceFacts facts = heating_device_facts(&dev, true);
  EXPECT_TRUE(facts.has_record);
  EXPECT_FLOAT_EQ(facts.range_min_c, 6.0F);
  EXPECT_FLOAT_EQ(facts.range_max_c, 28.0F);
  EXPECT_FLOAT_EQ(facts.reported_setpoint_c, 19.0F);
}
