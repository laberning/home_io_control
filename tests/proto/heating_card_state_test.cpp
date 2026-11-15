/// @file heating_card_state_test.cpp
/// @brief Tests for reduce_heating_card() and command_heating_card(): what a climate card shows from a
/// heater's readings and from the last command, without an entity or a hub.

#include "proto_commands.h"
#include "proto_device_types.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

using namespace esphome::home_io_control;

namespace {

const HeatingProfile &profile_of(DeviceType type, uint8_t subtype, DeviceModel model = DeviceModel::NONE) {
  const HeatingProfile *profile = find_heating_profile(type, subtype, model);
  EXPECT_NE(profile, nullptr);
  return *profile;
}

const HeatingProfile &radiator() { return profile_of(DeviceType::ELECTRICAL_HEATER, 1); }
const HeatingProfile &towel_dryer() { return profile_of(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER); }
const HeatingProfile &level_heater() { return profile_of(DeviceType::ELECTRICAL_HEATER, 0); }
const HeatingProfile &thermostat() { return profile_of(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0); }

/// Index of a preset in the profile's list; a custom one by its name.
int8_t preset_of(const HeatingProfile &profile, HeatingPresetId id, const char *custom_name = nullptr) {
  for (size_t i = 0; i < profile.presets.size(); i++) {
    const HeatingPresetEntry &entry = profile.presets[i];
    if (entry.id == id && (custom_name == nullptr || std::strcmp(entry.custom_name, custom_name) == 0))
      return static_cast<int8_t>(i);
  }
  ADD_FAILURE() << "profile has no such preset";
  return -1;
}

/// The card after a heater's readings arrived.
HeatingCardState reduced(const HeatingProfile &profile, const HeatingReadings &readings,
                         const HeatingCardState &prior = {}) {
  return reduce_heating_card(profile, readings, prior);
}

/// A card a first reading has been folded into: the heater's report now decides.
HeatingCardState observed_card() {
  HeatingCardState card;
  card.observed = true;
  return card;
}

HeatingWriteRequest mode_request(HeatingHvacMode hvac) { return {.kind = HeatingWriteKind::MODE, .hvac = hvac}; }

/// A preset request and the plan it matched, as plan_heating_write() reports its index.
struct PresetCommand {
  HeatingWriteRequest request{};
  HeatingWritePlan plan{};
};

PresetCommand preset_command(const HeatingProfile &profile, int8_t index) {
  const HeatingPresetEntry &entry = profile.presets[index];
  PresetCommand command;
  command.request = {.kind = HeatingWriteKind::PRESET, .preset = entry.id, .custom_name = entry.custom_name};
  command.plan.kind = HeatingPlanKind::REGISTER;
  command.plan.preset_index = index;
  return command;
}

HeatingCardCommand commanded(const HeatingProfile &profile, const PresetCommand &command,
                             const HeatingCardState &prior) {
  return command_heating_card(profile, command.request, command.plan, prior);
}

HeatingCardCommand commanded_mode(const HeatingProfile &profile, HeatingHvacMode hvac, const HeatingCardState &prior) {
  return command_heating_card(profile, mode_request(hvac), HeatingWritePlan{}, prior);
}

HeatingCardCommand commanded_target(const HeatingProfile &profile, float target_c, HeatingPlanKind kind,
                                    const HeatingCardState &prior) {
  HeatingWritePlan plan;
  plan.kind = kind;
  return command_heating_card(profile, {.kind = HeatingWriteKind::TARGET, .target_c = target_c}, plan, prior);
}

/// The readings of a towel dryer with comfort setpoint 20.0 and setback 3.5, `in_force` in 0x0137.
void programme_readings(HeatingReadings &r, TowelDryerMode mode, HeatingTemporaryState state, float in_force) {
  r.towel_mode = static_cast<int16_t>(mode);
  r.temporary_state = static_cast<int16_t>(state);
  r.setpoint_c = 20.0F;
  r.eco_setback_k = 3.5F;
  r.in_force_temperature_c = in_force;
}

}  // namespace

// === Readings: radiator ===

TEST(HeatingCardReduce, RadiatorReadingsGiveTemperaturesModeActionAndMaximum) {
  HeatingReadings r;
  r.room_temperature_c = 20.75F;
  r.setpoint_c = 19.5F;
  r.mode = static_cast<int16_t>(HeatingMode::MANUAL);
  r.regulator = static_cast<int16_t>(HeatingRegulatorState::HEATING);
  r.max_setpoint_c = 26.0F;

  const HeatingCardState card = reduced(radiator(), r);

  EXPECT_FLOAT_EQ(card.current_c, 20.75F);
  EXPECT_FLOAT_EQ(card.target_c, 19.5F);
  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT);
  EXPECT_EQ(card.action, HeatingAction::HEATING);
  EXPECT_FLOAT_EQ(card.max_target_c, 26.0F);
  EXPECT_TRUE(card.observed);
  EXPECT_TRUE(reduced(radiator(), r, card).shows_same_as(card)) << "the same readings change nothing";
}

TEST(HeatingCardReduce, ReadingsThatSayNothingLeaveTheCardUnobserved) {
  const HeatingCardState card = reduced(radiator(), HeatingReadings{});

  EXPECT_FALSE(card.observed);
  EXPECT_TRUE(card.shows_same_as(HeatingCardState{}));
}

TEST(HeatingCardReduce, MaximumIsNeverPastWhatCanBeEncodedNorBelowTheMinimum) {
  HeatingReadings r;
  r.max_setpoint_c = 35.0F;
  EXPECT_FLOAT_EQ(reduced(radiator(), r).max_target_c, radiator().visual_max_c);

  r.max_setpoint_c = HEATING_TEMP_MIN_C;
  const HeatingCardState unchanged = reduced(radiator(), r);
  EXPECT_FLOAT_EQ(unchanged.max_target_c, HEATING_TEMP_MAX_C);
  EXPECT_FALSE(unchanged.observed) << "a maximum that is no setpoint is no reading";
}

TEST(HeatingCardReduce, ASensorFaultShowsNoCurrentTemperature) {
  HeatingReadings r;
  r.room_temperature_c = 20.0F;
  const HeatingCardState with_sensor = reduced(radiator(), r);

  r.room_temperature_c = NAN;
  const HeatingCardState faulty = reduced(radiator(), r, with_sensor);

  EXPECT_TRUE(std::isnan(faulty.current_c));
  EXPECT_TRUE(faulty.observed) << "an earlier reading stays observed";
  EXPECT_TRUE(reduced(radiator(), r, faulty).shows_same_as(faulty)) << "two NaN are not a change";
}

TEST(HeatingCardReduce, AnOffHeaterIsNotIdleAndTheActionFollowsTheRegulator) {
  HeatingReadings r;
  r.mode = static_cast<int16_t>(HeatingMode::MANUAL);
  r.regulator = static_cast<int16_t>(HeatingRegulatorState::STANDBY);
  EXPECT_EQ(reduced(radiator(), r).action, HeatingAction::IDLE);

  r.mode = static_cast<int16_t>(HeatingMode::OFF);
  r.regulator = static_cast<int16_t>(HeatingRegulatorState::HEATING);
  EXPECT_EQ(reduced(radiator(), r).action, HeatingAction::OFF);
}

TEST(HeatingCardReduce, AValueTheProfileDoesNotMapLeavesTheCardAsItWas) {
  HeatingCardState prior;
  prior.mode = HeatingHvacMode::HEAT;
  HeatingReadings r;
  r.mode = 0x7F;

  const HeatingCardState card = reduced(radiator(), r, prior);

  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT);
  EXPECT_TRUE(card.observed) << "the register was read, only its value is unknown";
}

TEST(HeatingCardReduce, RadiatorProgramAndAutoShowAsAutoWithTheirOwnPreset) {
  HeatingReadings r;
  r.mode = static_cast<int16_t>(HeatingMode::PROG);
  HeatingCardState card = reduced(radiator(), r);
  EXPECT_EQ(card.mode, HeatingHvacMode::AUTO);
  EXPECT_EQ(card.preset_index, preset_of(radiator(), HeatingPresetId::CUSTOM, "Program"));

  r.mode = static_cast<int16_t>(HeatingMode::MANUAL);
  card = reduced(radiator(), r, card);
  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT);
  EXPECT_EQ(card.preset_index, -1);
}

// === Readings: towel dryer ===

TEST(HeatingCardReduce, TowelDryerBoostAndDryingComeFromTheTemporaryStateAndSurviveEveryPoll) {
  HeatingReadings r;
  r.towel_mode = static_cast<int16_t>(TowelDryerMode::MANUAL);
  r.temporary_state = static_cast<int16_t>(HeatingTemporaryState::BOOST);
  HeatingCardState card = reduced(towel_dryer(), r);
  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT);
  EXPECT_EQ(card.preset_index, preset_of(towel_dryer(), HeatingPresetId::BOOST));

  // The next poll reads the same mode: a mode entry without a preset must not wipe the boost.
  EXPECT_TRUE(reduced(towel_dryer(), r, card).shows_same_as(card));

  r.temporary_state = static_cast<int16_t>(HeatingTemporaryState::DRYING);
  card = reduced(towel_dryer(), r, card);
  EXPECT_EQ(card.preset_index, preset_of(towel_dryer(), HeatingPresetId::CUSTOM, "Drying"));
  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT) << "drying does not change the base mode";

  r.temporary_state = static_cast<int16_t>(HeatingTemporaryState::NONE);
  card = reduced(towel_dryer(), r, card);
  EXPECT_EQ(card.preset_index, preset_of(towel_dryer(), HeatingPresetId::NONE));
}

TEST(HeatingCardReduce, TowelDryerProgrammeShowsComfortOrEcoAndTheTemperatureInForce) {
  HeatingReadings r;
  programme_readings(r, TowelDryerMode::PROG, HeatingTemporaryState::NONE, 20.0F);
  HeatingCardState card = reduced(towel_dryer(), r);
  EXPECT_EQ(card.mode, HeatingHvacMode::AUTO);
  EXPECT_EQ(card.preset_index, preset_of(towel_dryer(), HeatingPresetId::COMFORT));
  EXPECT_FLOAT_EQ(card.target_c, 20.0F);

  r.in_force_temperature_c = 16.5F;
  card = reduced(towel_dryer(), r, card);
  EXPECT_EQ(card.preset_index, preset_of(towel_dryer(), HeatingPresetId::ECO));
  EXPECT_FLOAT_EQ(card.target_c, 16.5F) << "the temperature the dryer heats to now";

  // A frost or holiday value: the temperature, and no period.
  r.in_force_temperature_c = 7.0F;
  card = reduced(towel_dryer(), r, card);
  EXPECT_EQ(card.preset_index, preset_of(towel_dryer(), HeatingPresetId::NONE));
  EXPECT_FLOAT_EQ(card.target_c, 7.0F);
}

TEST(HeatingCardReduce, TowelDryerOutsideTheProgrammeNeverShowsTheTemperatureInForce) {
  HeatingCardState card;
  for (const auto state : {HeatingTemporaryState::BOOST, HeatingTemporaryState::DRYING}) {
    HeatingReadings r;
    programme_readings(r, TowelDryerMode::PROG, state, 0.1F);
    card = reduced(towel_dryer(), r, card);
    EXPECT_FLOAT_EQ(card.target_c, 20.0F);
  }
  HeatingReadings manual;
  programme_readings(manual, TowelDryerMode::MANUAL, HeatingTemporaryState::NONE, 16.5F);
  card = reduced(towel_dryer(), manual, card);
  EXPECT_FLOAT_EQ(card.target_c, 20.0F) << "manual holds the comfort setpoint";
  EXPECT_EQ(card.preset_index, preset_of(towel_dryer(), HeatingPresetId::NONE));
}

TEST(HeatingCardReduce, TowelDryerPeriodLosesTheOldPeriodWhenTheTemperatureInForceIsNotReadYet) {
  HeatingReadings r;
  programme_readings(r, TowelDryerMode::PROG, HeatingTemporaryState::NONE, 16.5F);
  HeatingCardState card = reduced(towel_dryer(), r);
  ASSERT_EQ(card.preset_index, preset_of(towel_dryer(), HeatingPresetId::ECO));

  // Back in the programme before the temperature in force is read again: no preset of the old period.
  r.in_force_temperature_c = NAN;
  card = reduced(towel_dryer(), r, card);
  EXPECT_EQ(card.preset_index, preset_of(towel_dryer(), HeatingPresetId::NONE));
  EXPECT_FLOAT_EQ(card.target_c, 20.0F);
}

TEST(HeatingCardReduce, TowelDryerActionFollowsTheWorkingRateNotTheRegulator) {
  HeatingReadings r;
  r.towel_mode = static_cast<int16_t>(TowelDryerMode::MANUAL);
  r.regulator = 0;
  r.working_rate = 100;
  EXPECT_EQ(reduced(towel_dryer(), r).action, HeatingAction::HEATING);

  r.working_rate = 0;
  EXPECT_EQ(reduced(towel_dryer(), r).action, HeatingAction::IDLE);
}

// === Readings: level heater, thermostat ===

TEST(HeatingCardReduce, ObservedLevelsDriveModeAndPreset) {
  HeatingReadings r;
  r.level = heating_level::OFF;
  EXPECT_EQ(reduced(level_heater(), r).mode, HeatingHvacMode::OFF);

  r.level = heating_level::ECO;
  HeatingCardState card = reduced(level_heater(), r);
  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT);
  EXPECT_EQ(card.preset_index, preset_of(level_heater(), HeatingPresetId::ECO));

  r.level = heating_level::SECURED;
  card = reduced(level_heater(), r, card);
  EXPECT_EQ(card.preset_index, preset_of(level_heater(), HeatingPresetId::CUSTOM, "Secured"));
  EXPECT_EQ(card.action, HeatingAction::OFF) << "a level heater has no action";
  EXPECT_TRUE(std::isnan(card.target_c));
}

TEST(HeatingCardReduce, ALevelHeaterWhoseStatusCarriesNoLevelKeepsShowingTheCommand) {
  // A status main parameter outside the level prefix (0xC800, issue #228) leaves the level unread.
  const int8_t eco = preset_of(level_heater(), HeatingPresetId::ECO);
  HeatingCardState card = commanded(level_heater(), preset_command(level_heater(), eco), HeatingCardState{}).card;
  HeatingReadings r;
  r.main_parameter = 0xC800;

  card = reduced(level_heater(), r, card);
  EXPECT_FALSE(card.observed) << "no level was read, so the commands keep deciding";
  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT);
  EXPECT_EQ(card.preset_index, eco);

  const HeatingCardCommand off = commanded_mode(level_heater(), HeatingHvacMode::OFF, card);
  EXPECT_TRUE(off.shown);
  EXPECT_EQ(off.card.mode, HeatingHvacMode::OFF);
  EXPECT_EQ(off.card.preset_index, -1);
  EXPECT_EQ(initial_heating_card(level_heater()).mode, HeatingHvacMode::OFF) << "off after a reboot";
}

TEST(HeatingCardReduce, WithoutACommandedModeAThermostatThatReportsIsShownAsHeating) {
  HeatingReadings r;
  r.setpoint_c = 21.0F;

  const HeatingCardState card = reduced(thermostat(), r);

  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT);
  EXPECT_FLOAT_EQ(card.target_c, 21.0F);
}

TEST(HeatingCardReduce, ACommandedModeIsNotOverwrittenByWhatAThermostatReports) {
  HeatingCardState prior;
  prior.mode_commanded = true;
  HeatingReadings r;
  r.setpoint_c = 21.0F;

  EXPECT_EQ(reduced(thermostat(), r, prior).mode, HeatingHvacMode::OFF);
}

TEST(HeatingCardReduce, ThermostatAliasShowsItsPresetAndAPlainTemperatureShowsNone) {
  HeatingReadings r;
  r.level = heating_alias::THERMOSTAT_ECO;
  r.setpoint_c = 18.0F;
  HeatingCardState card = reduced(thermostat(), r);
  EXPECT_EQ(card.preset_index, preset_of(thermostat(), HeatingPresetId::ECO));
  EXPECT_FLOAT_EQ(card.target_c, 18.0F);

  r.level = -1;  // a reply that carried a plain temperature
  r.setpoint_c = 19.0F;
  card = reduced(thermostat(), r, card);
  EXPECT_EQ(card.preset_index, -1);
  EXPECT_FLOAT_EQ(card.target_c, 19.0F);
}

// === Commands against readings ===

TEST(HeatingCardCommand, BeforeAReadAModeIsShownAndSupersedesThePresetOnARegisterHeater) {
  HeatingCardState prior;
  prior.preset_index = preset_of(radiator(), HeatingPresetId::CUSTOM, "Program");

  const HeatingCardCommand result = commanded_mode(radiator(), HeatingHvacMode::HEAT, prior);

  EXPECT_TRUE(result.shown);
  EXPECT_EQ(result.card.mode, HeatingHvacMode::HEAT);
  EXPECT_EQ(result.card.preset_index, -1);
  EXPECT_TRUE(result.card.mode_commanded);
}

TEST(HeatingCardCommand, AfterAReadAModeIsNotShownButIsRemembered) {
  HeatingCardState prior = observed_card();
  prior.mode = HeatingHvacMode::AUTO;

  const HeatingCardCommand result = commanded_mode(radiator(), HeatingHvacMode::HEAT, prior);

  EXPECT_FALSE(result.shown) << "the read-back decides";
  EXPECT_EQ(result.card.mode, HeatingHvacMode::AUTO);
  EXPECT_TRUE(result.card.mode_commanded);
}

TEST(HeatingCardCommand, AModeNothingReadsBackStaysTheLastCommandEvenAfterARead) {
  const HeatingCardCommand result = commanded_mode(thermostat(), HeatingHvacMode::AUTO, observed_card());

  EXPECT_TRUE(result.shown);
  EXPECT_EQ(result.card.mode, HeatingHvacMode::AUTO);
}

TEST(HeatingCardCommand, AModeKeepsThePresetWhereItIsKeptElsewhere) {
  HeatingCardState towel;
  towel.preset_index = preset_of(towel_dryer(), HeatingPresetId::BOOST);
  EXPECT_EQ(commanded_mode(towel_dryer(), HeatingHvacMode::HEAT, towel).card.preset_index, towel.preset_index)
      << "a towel dryer's boost outlives a mode change";

  HeatingCardState alias;
  alias.preset_index = preset_of(thermostat(), HeatingPresetId::ECO);
  EXPECT_EQ(commanded_mode(thermostat(), HeatingHvacMode::HEAT, alias).card.preset_index, alias.preset_index)
      << "a temperature heater's preset follows its status reply";
}

TEST(HeatingCardCommand, ARadiatorPresetBeforeAReadShowsAsTheReadBackWould) {
  const int8_t program = preset_of(radiator(), HeatingPresetId::CUSTOM, "Program");

  const HeatingCardCommand result = commanded(radiator(), preset_command(radiator(), program), HeatingCardState{});

  EXPECT_TRUE(result.shown);
  EXPECT_EQ(result.card.mode, HeatingHvacMode::AUTO);
  EXPECT_EQ(result.card.preset_index, program);
}

TEST(HeatingCardCommand, APresetAfterAReadChangesNothing) {
  const HeatingCardState prior = observed_card();

  const HeatingCardCommand result = commanded(
      radiator(), preset_command(radiator(), preset_of(radiator(), HeatingPresetId::CUSTOM, "Program")), prior);

  EXPECT_FALSE(result.shown);
  EXPECT_TRUE(result.card.shows_same_as(prior));
}

TEST(HeatingCardCommand, ATowelDryerPresetIsShownThroughItsOwnTableAndLeavesTheMode) {
  HeatingCardState prior;
  prior.mode = HeatingHvacMode::AUTO;
  const int8_t boost = preset_of(towel_dryer(), HeatingPresetId::BOOST);

  const HeatingCardCommand result = commanded(towel_dryer(), preset_command(towel_dryer(), boost), prior);

  EXPECT_TRUE(result.shown);
  EXPECT_EQ(result.card.preset_index, boost);
  EXPECT_EQ(result.card.mode, HeatingHvacMode::AUTO);
}

TEST(HeatingCardCommand, ALevelPresetShowsAsTheReadBackWould) {
  const int8_t boost = preset_of(level_heater(), HeatingPresetId::BOOST);

  const HeatingCardCommand result =
      commanded(level_heater(), preset_command(level_heater(), boost), HeatingCardState{});

  EXPECT_EQ(result.card.mode, HeatingHvacMode::HEAT);
  EXPECT_EQ(result.card.preset_index, boost);
}

TEST(HeatingCardCommand, AStandardAliasPresetHeatsAndACustomOneLeavesTheMode) {
  const int8_t eco = preset_of(thermostat(), HeatingPresetId::ECO);
  HeatingCardCommand result = commanded(thermostat(), preset_command(thermostat(), eco), HeatingCardState{});
  EXPECT_EQ(result.card.preset_index, eco);
  EXPECT_EQ(result.card.mode, HeatingHvacMode::HEAT);

  const int8_t secured = preset_of(thermostat(), HeatingPresetId::CUSTOM, "Secured");
  HeatingCardState off;
  result = commanded(thermostat(), preset_command(thermostat(), secured), off);
  EXPECT_EQ(result.card.preset_index, secured);
  EXPECT_EQ(result.card.mode, HeatingHvacMode::OFF);
}

TEST(HeatingCardCommand, ATargetBeforeAReadIsShown) {
  const HeatingCardCommand result = commanded_target(radiator(), 21.5F, HeatingPlanKind::REGISTER, HeatingCardState{});

  EXPECT_TRUE(result.shown);
  EXPECT_FLOAT_EQ(result.card.target_c, 21.5F);
}

TEST(HeatingCardCommand, ATargetAfterAReadChangesNothing) {
  HeatingCardState prior = observed_card();
  prior.target_c = 19.0F;

  const HeatingCardCommand result = commanded_target(radiator(), 21.5F, HeatingPlanKind::REGISTER, prior);

  EXPECT_FALSE(result.shown);
  EXPECT_FLOAT_EQ(result.card.target_c, 19.0F);
}

TEST(HeatingCardCommand, ATargetEndsAnAliasPresetOnATemperatureHeaterOnly) {
  HeatingCardState alias;
  alias.preset_index = preset_of(thermostat(), HeatingPresetId::ECO);
  EXPECT_EQ(commanded_target(thermostat(), 20.0F, HeatingPlanKind::TEMPERATURE, alias).card.preset_index, -1);

  HeatingCardState program;
  program.preset_index = preset_of(radiator(), HeatingPresetId::CUSTOM, "Program");
  EXPECT_EQ(commanded_target(radiator(), 20.0F, HeatingPlanKind::REGISTER, program).card.preset_index,
            program.preset_index);
}

// === Starting state and hardware status ===

TEST(HeatingCard, ACardStartsOffWithNothingKnownAndTheProfilesOwnRange) {
  const HeatingCardState radiator_card = initial_heating_card(radiator());
  EXPECT_EQ(radiator_card.mode, HeatingHvacMode::OFF);
  EXPECT_EQ(radiator_card.preset_index, -1);
  EXPECT_TRUE(std::isnan(radiator_card.target_c));
  EXPECT_TRUE(std::isnan(radiator_card.current_c));
  EXPECT_FALSE(radiator_card.observed);
  EXPECT_FLOAT_EQ(radiator_card.max_target_c, radiator().visual_max_c);
  EXPECT_FLOAT_EQ(initial_heating_card(thermostat()).max_target_c, thermostat().visual_max_c);
  EXPECT_FLOAT_EQ(initial_heating_card(level_heater()).max_target_c, HEATING_TEMP_MAX_C) << "no setpoint, no range";
}

TEST(HeatingCard, TwoCardsDifferingInABookkeepingFlagShowTheSame) {
  HeatingCardState a;
  HeatingCardState b;
  b.observed = true;
  b.mode_commanded = true;
  EXPECT_TRUE(a.shows_same_as(b));

  b.target_c = 20.0F;
  EXPECT_FALSE(a.shows_same_as(b));
}

TEST(HeatingCard, TheRegisterAndLevelProfilesHaveBeenRunOnHardware) {
  EXPECT_EQ(heating_hardware_status(nullptr), HeatingHardwareStatus::UNVALIDATED);
  EXPECT_EQ(heating_hardware_status(&thermostat()), HeatingHardwareStatus::UNVALIDATED);
  EXPECT_EQ(heating_hardware_status(&level_heater()), HeatingHardwareStatus::LEVELS_CONFIRMED);
  EXPECT_EQ(heating_hardware_status(&radiator()), HeatingHardwareStatus::CONFIRMED);
  EXPECT_EQ(heating_hardware_status(&towel_dryer()), HeatingHardwareStatus::CONFIRMED);

  HeatingProfile unconfirmed_writes = radiator();
  unconfirmed_writes.experimental_writes = true;
  EXPECT_EQ(heating_hardware_status(&unconfirmed_writes), HeatingHardwareStatus::WRITES_EXPERIMENTAL);
}
