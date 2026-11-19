/// @file heating_card_state_test.cpp
/// @brief Tests for reduce_heating_card() and command_heating_card(): what a climate card shows from a
/// heater's readings and from the last command, without an entity or a hub.

#include "proto_commands.h"
#include "heating_profiles.h"
#include "reading_store.h"
#include "proto_device_types.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>

using namespace esphome::home_io_control;

namespace {

const HeatingProfile &thermostat() { return test::profile_of(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0); }

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
HeatingCardState reduced(const HeatingProfile &profile, const IoDevice &readings, const HeatingCardState &prior = {}) {
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

}  // namespace

// === Readings: radiator ===

TEST(HeatingCardReduce, RadiatorReadingsGiveTemperaturesModeActionAndMaximum) {
  IoDevice r;
  test::store_reading(r, DeviceReading::ROOM_TEMPERATURE, 20.75F);
  test::store_reading(r, DeviceReading::SETPOINT, 19.5F);
  test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::MANUAL));
  test::store_reading(r, DeviceReading::REGULATOR_STATE, static_cast<int16_t>(HeatingRegulatorState::HEATING));
  test::store_reading(r, DeviceReading::MAX_SETPOINT, 26.0F);

  const HeatingCardState card = reduced(test::radiator_profile(), r);

  EXPECT_FLOAT_EQ(card.current_c, 20.75F);
  EXPECT_FLOAT_EQ(card.target_c, 19.5F);
  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT);
  EXPECT_EQ(card.action, HeatingAction::HEATING);
  EXPECT_FLOAT_EQ(card.max_target_c, 26.0F);
  EXPECT_TRUE(card.observed);
  EXPECT_TRUE(reduced(test::radiator_profile(), r, card).shows_same_as(card)) << "the same readings change nothing";
}

TEST(HeatingCardReduce, ReadingsThatSayNothingLeaveTheCardUnobserved) {
  const HeatingCardState card = reduced(test::radiator_profile(), IoDevice{});

  EXPECT_FALSE(card.observed);
  EXPECT_TRUE(card.shows_same_as(HeatingCardState{}));
}

TEST(HeatingCardReduce, MaximumIsNeverPastWhatCanBeEncodedNorBelowTheMinimum) {
  IoDevice r;
  test::store_reading(r, DeviceReading::MAX_SETPOINT, 35.0F);
  EXPECT_FLOAT_EQ(reduced(test::radiator_profile(), r).max_target_c, test::radiator_profile().visual_max_c);

  test::store_reading(r, DeviceReading::MAX_SETPOINT, HEATING_TEMP_MIN_C);
  const HeatingCardState unchanged = reduced(test::radiator_profile(), r);
  EXPECT_FLOAT_EQ(unchanged.max_target_c, HEATING_TEMP_MAX_C);
  EXPECT_FALSE(unchanged.observed) << "a maximum that is no setpoint is no reading";
}

TEST(HeatingCardReduce, ASensorFaultShowsNoCurrentTemperature) {
  IoDevice r;
  test::store_reading(r, DeviceReading::ROOM_TEMPERATURE, 20.0F);
  const HeatingCardState with_sensor = reduced(test::radiator_profile(), r);

  test::store_reading(r, DeviceReading::ROOM_TEMPERATURE, NAN);
  const HeatingCardState faulty = reduced(test::radiator_profile(), r, with_sensor);

  EXPECT_TRUE(std::isnan(faulty.current_c));
  EXPECT_TRUE(faulty.observed) << "an earlier reading stays observed";
  EXPECT_TRUE(reduced(test::radiator_profile(), r, faulty).shows_same_as(faulty)) << "two NaN are not a change";
}

TEST(HeatingCardReduce, AnOffHeaterIsNotIdleAndTheActionFollowsTheRegulator) {
  IoDevice r;
  test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::MANUAL));
  test::store_reading(r, DeviceReading::REGULATOR_STATE, static_cast<int16_t>(HeatingRegulatorState::STANDBY));
  EXPECT_EQ(reduced(test::radiator_profile(), r).action, HeatingAction::IDLE);

  test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::OFF));
  test::store_reading(r, DeviceReading::REGULATOR_STATE, static_cast<int16_t>(HeatingRegulatorState::HEATING));
  EXPECT_EQ(reduced(test::radiator_profile(), r).action, HeatingAction::OFF);
}

TEST(HeatingCardReduce, AValueTheProfileDoesNotMapLeavesTheCardAsItWas) {
  HeatingCardState prior;
  prior.mode = HeatingHvacMode::HEAT;
  IoDevice r;
  test::store_reading(r, DeviceReading::MODE, 0x7F);

  const HeatingCardState card = reduced(test::radiator_profile(), r, prior);

  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT);
  EXPECT_TRUE(card.observed) << "the register was read, only its value is unknown";
}

TEST(HeatingCardReduce, RadiatorProgramAndAutoShowAsAutoWithTheirOwnPreset) {
  IoDevice r;
  test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::PROG));
  HeatingCardState card = reduced(test::radiator_profile(), r);
  EXPECT_EQ(card.mode, HeatingHvacMode::AUTO);
  EXPECT_EQ(card.preset_index, preset_of(test::radiator_profile(), HeatingPresetId::CUSTOM, "Program"));

  test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::MANUAL));
  card = reduced(test::radiator_profile(), r, card);
  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT);
  EXPECT_EQ(card.preset_index, -1);
}

// === Readings: towel dryer ===

TEST(HeatingCardReduce, TowelDryerBoostAndDryingComeFromTheTemporaryStateAndSurviveEveryPoll) {
  IoDevice r;
  test::store_reading(r, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::MANUAL));
  test::store_reading(r, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::BOOST));
  HeatingCardState card = reduced(test::towel_dryer_profile(), r);
  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT);
  EXPECT_EQ(card.preset_index, preset_of(test::towel_dryer_profile(), HeatingPresetId::BOOST));

  // The next poll reads the same mode: a mode entry without a preset must not wipe the boost.
  EXPECT_TRUE(reduced(test::towel_dryer_profile(), r, card).shows_same_as(card));

  test::store_reading(r, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::DRYING));
  card = reduced(test::towel_dryer_profile(), r, card);
  EXPECT_EQ(card.preset_index, preset_of(test::towel_dryer_profile(), HeatingPresetId::CUSTOM, "Drying"));
  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT) << "drying does not change the base mode";

  test::store_reading(r, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::NONE));
  card = reduced(test::towel_dryer_profile(), r, card);
  EXPECT_EQ(card.preset_index, preset_of(test::towel_dryer_profile(), HeatingPresetId::NONE));
}

TEST(HeatingCardReduce, TowelDryerProgrammeShowsTheTemperatureInForceAndNoPresetForThePeriod) {
  IoDevice r;
  test::store_programme_readings(r, TowelDryerMode::PROG, HeatingTemporaryState::NONE, 20.0F);
  test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_level::COMFORT);
  HeatingCardState card = reduced(test::towel_dryer_profile(), r);
  EXPECT_EQ(card.mode, HeatingHvacMode::AUTO);
  EXPECT_EQ(card.preset_index, preset_of(test::towel_dryer_profile(), HeatingPresetId::NONE))
      << "comfort is a level, not a preset";
  EXPECT_FLOAT_EQ(card.target_c, 20.0F);

  test::store_reading(r, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE, 16.5F);
  test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_level::ECO);
  card = reduced(test::towel_dryer_profile(), r, card);
  EXPECT_EQ(card.preset_index, preset_of(test::towel_dryer_profile(), HeatingPresetId::NONE));
  EXPECT_FLOAT_EQ(card.target_c, 16.5F) << "the temperature the dryer heats to now";

  // A frost or holiday value: the temperature, and no preset.
  test::store_reading(r, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE, 7.0F);
  card = reduced(test::towel_dryer_profile(), r, card);
  EXPECT_EQ(card.preset_index, preset_of(test::towel_dryer_profile(), HeatingPresetId::NONE));
  EXPECT_FLOAT_EQ(card.target_c, 7.0F);
}

TEST(HeatingCardReduce, TowelDryerOutsideTheProgrammeNeverShowsTheTemperatureInForce) {
  HeatingCardState card;
  for (const auto state : {HeatingTemporaryState::BOOST, HeatingTemporaryState::DRYING}) {
    IoDevice r;
    test::store_programme_readings(r, TowelDryerMode::PROG, state, 0.1F);
    card = reduced(test::towel_dryer_profile(), r, card);
    EXPECT_FLOAT_EQ(card.target_c, 20.0F);
  }
  IoDevice manual;
  test::store_programme_readings(manual, TowelDryerMode::MANUAL, HeatingTemporaryState::NONE, 16.5F);
  card = reduced(test::towel_dryer_profile(), manual, card);
  EXPECT_FLOAT_EQ(card.target_c, 20.0F) << "manual holds the comfort setpoint";
  EXPECT_EQ(card.preset_index, preset_of(test::towel_dryer_profile(), HeatingPresetId::NONE));
}

TEST(HeatingCardReduce, TowelDryerTargetIsTheComfortSetpointUntilTheTemperatureInForceIsRead) {
  IoDevice r;
  test::store_programme_readings(r, TowelDryerMode::PROG, HeatingTemporaryState::NONE, 16.5F);
  HeatingCardState card = reduced(test::towel_dryer_profile(), r);
  ASSERT_FLOAT_EQ(card.target_c, 16.5F);

  clear_reading(r, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE);
  card = reduced(test::towel_dryer_profile(), r, card);
  EXPECT_FLOAT_EQ(card.target_c, 20.0F);
}

TEST(HeatingCardReduce, TowelDryerActionFollowsTheWorkingRateNotTheRegulator) {
  IoDevice r;
  test::store_reading(r, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::MANUAL));
  test::store_reading(r, DeviceReading::REGULATOR_STATE, 0);
  test::store_reading(r, DeviceReading::WORKING_RATE, 100);
  EXPECT_EQ(reduced(test::towel_dryer_profile(), r).action, HeatingAction::HEATING);

  test::store_reading(r, DeviceReading::WORKING_RATE, 0);
  EXPECT_EQ(reduced(test::towel_dryer_profile(), r).action, HeatingAction::IDLE);
}

// === Readings: level heater, thermostat ===

TEST(HeatingCardReduce, ObservedLevelsDriveModeAndPreset) {
  IoDevice r;
  test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_level::OFF);
  EXPECT_EQ(reduced(test::level_heater_profile(), r).mode, HeatingHvacMode::OFF);

  test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_level::ECO);
  HeatingCardState card = reduced(test::level_heater_profile(), r);
  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT);
  EXPECT_EQ(card.preset_index, preset_of(test::level_heater_profile(), HeatingPresetId::ECO));

  test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_level::SECURED);
  card = reduced(test::level_heater_profile(), r, card);
  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT);
  EXPECT_EQ(card.preset_index, -1) << "secured is reported, not a preset";
  EXPECT_EQ(card.action, HeatingAction::OFF) << "a level heater has no action";
  EXPECT_TRUE(std::isnan(card.target_c));
}

TEST(HeatingCardReduce, ALevelHeaterWhoseStatusCarriesNoLevelKeepsShowingTheCommand) {
  // A status main parameter outside the level prefix (0xC800, issue #228) leaves the level unread.
  const int8_t eco = preset_of(test::level_heater_profile(), HeatingPresetId::ECO);
  HeatingCardState card =
      commanded(test::level_heater_profile(), preset_command(test::level_heater_profile(), eco), HeatingCardState{})
          .card;
  const std::array<uint8_t, 2> secured_position = {0xC8, 0x00};
  EXPECT_FALSE(decode_status_reading(DeviceReading::HEATING_LEVEL, secured_position, HEATING_LEVEL_MP_PREFIX,
                                     HEATING_TEMPERATURE_RANGE_MIN_C, HEATING_TEMPERATURE_RANGE_MAX_C))
      << "the status decoder stores no level for it";
  IoDevice r;

  card = reduced(test::level_heater_profile(), r, card);
  EXPECT_FALSE(card.observed) << "no level was read, so the commands keep deciding";
  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT);
  EXPECT_EQ(card.preset_index, eco);

  const HeatingCardCommand off = commanded_mode(test::level_heater_profile(), HeatingHvacMode::OFF, card);
  EXPECT_TRUE(off.shown);
  EXPECT_EQ(off.card.mode, HeatingHvacMode::OFF);
  EXPECT_EQ(off.card.preset_index, -1);
  EXPECT_EQ(initial_heating_card(test::level_heater_profile()).mode, HeatingHvacMode::OFF) << "off after a reboot";
}

TEST(HeatingCardReduce, WithoutACommandedModeAThermostatThatReportsIsShownAsHeating) {
  IoDevice r;
  test::store_reading(r, DeviceReading::STATUS_SETPOINT, 21.0F);

  const HeatingCardState card = reduced(thermostat(), r);

  EXPECT_EQ(card.mode, HeatingHvacMode::HEAT);
  EXPECT_FLOAT_EQ(card.target_c, 21.0F);
}

TEST(HeatingCardReduce, ACommandedModeIsNotOverwrittenByWhatAThermostatReports) {
  HeatingCardState prior;
  prior.mode_commanded = true;
  IoDevice r;
  test::store_reading(r, DeviceReading::STATUS_SETPOINT, 21.0F);

  EXPECT_EQ(reduced(thermostat(), r, prior).mode, HeatingHvacMode::OFF);
}

TEST(HeatingCardReduce, ThermostatAliasShowsItsPresetAndAPlainTemperatureShowsNone) {
  IoDevice r;
  test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_alias::THERMOSTAT_ECO);
  test::store_reading(r, DeviceReading::STATUS_SETPOINT, 18.0F);
  HeatingCardState card = reduced(thermostat(), r);
  EXPECT_EQ(card.preset_index, preset_of(thermostat(), HeatingPresetId::ECO));
  EXPECT_FLOAT_EQ(card.target_c, 18.0F);

  clear_reading(r, DeviceReading::HEATING_LEVEL);  // a reply that carried a plain temperature
  test::store_reading(r, DeviceReading::STATUS_SETPOINT, 19.0F);
  card = reduced(thermostat(), r, card);
  EXPECT_EQ(card.preset_index, -1);
  EXPECT_FLOAT_EQ(card.target_c, 19.0F);
}

// === Commands against readings ===

TEST(HeatingCardCommand, BeforeAReadAModeIsShownAndSupersedesThePresetOnARegisterHeater) {
  HeatingCardState prior;
  prior.preset_index = preset_of(test::radiator_profile(), HeatingPresetId::CUSTOM, "Program");

  const HeatingCardCommand result = commanded_mode(test::radiator_profile(), HeatingHvacMode::HEAT, prior);

  EXPECT_TRUE(result.shown);
  EXPECT_EQ(result.card.mode, HeatingHvacMode::HEAT);
  EXPECT_EQ(result.card.preset_index, -1);
  EXPECT_TRUE(result.card.mode_commanded);
}

TEST(HeatingCardCommand, AfterAReadAModeIsNotShownButIsRemembered) {
  HeatingCardState prior = observed_card();
  prior.mode = HeatingHvacMode::AUTO;

  const HeatingCardCommand result = commanded_mode(test::radiator_profile(), HeatingHvacMode::HEAT, prior);

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
  towel.preset_index = preset_of(test::towel_dryer_profile(), HeatingPresetId::BOOST);
  EXPECT_EQ(commanded_mode(test::towel_dryer_profile(), HeatingHvacMode::HEAT, towel).card.preset_index,
            towel.preset_index)
      << "a towel dryer's boost outlives a mode change";

  HeatingCardState alias;
  alias.preset_index = preset_of(thermostat(), HeatingPresetId::ECO);
  EXPECT_EQ(commanded_mode(thermostat(), HeatingHvacMode::HEAT, alias).card.preset_index, alias.preset_index)
      << "a temperature heater's preset follows its status reply";
}

TEST(HeatingCardCommand, ARadiatorPresetBeforeAReadShowsAsTheReadBackWould) {
  const int8_t program = preset_of(test::radiator_profile(), HeatingPresetId::CUSTOM, "Program");

  const HeatingCardCommand result =
      commanded(test::radiator_profile(), preset_command(test::radiator_profile(), program), HeatingCardState{});

  EXPECT_TRUE(result.shown);
  EXPECT_EQ(result.card.mode, HeatingHvacMode::AUTO);
  EXPECT_EQ(result.card.preset_index, program);
}

TEST(HeatingCardCommand, APresetAfterAReadChangesNothing) {
  const HeatingCardState prior = observed_card();

  const HeatingCardCommand result = commanded(
      test::radiator_profile(),
      preset_command(test::radiator_profile(), preset_of(test::radiator_profile(), HeatingPresetId::CUSTOM, "Program")),
      prior);

  EXPECT_FALSE(result.shown);
  EXPECT_TRUE(result.card.shows_same_as(prior));
}

TEST(HeatingCardCommand, ATowelDryerPresetIsShownThroughItsOwnTableAndLeavesTheMode) {
  HeatingCardState prior;
  prior.mode = HeatingHvacMode::AUTO;
  const int8_t boost = preset_of(test::towel_dryer_profile(), HeatingPresetId::BOOST);

  const HeatingCardCommand result =
      commanded(test::towel_dryer_profile(), preset_command(test::towel_dryer_profile(), boost), prior);

  EXPECT_TRUE(result.shown);
  EXPECT_EQ(result.card.preset_index, boost);
  EXPECT_EQ(result.card.mode, HeatingHvacMode::AUTO);
}

TEST(HeatingCardCommand, ALevelPresetShowsAsTheReadBackWould) {
  const int8_t boost = preset_of(test::level_heater_profile(), HeatingPresetId::BOOST);

  const HeatingCardCommand result =
      commanded(test::level_heater_profile(), preset_command(test::level_heater_profile(), boost), HeatingCardState{});

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
  const HeatingCardCommand result =
      commanded_target(test::radiator_profile(), 21.5F, HeatingPlanKind::REGISTER, HeatingCardState{});

  EXPECT_TRUE(result.shown);
  EXPECT_FLOAT_EQ(result.card.target_c, 21.5F);
}

TEST(HeatingCardCommand, ATargetAfterAReadChangesNothing) {
  HeatingCardState prior = observed_card();
  prior.target_c = 19.0F;

  const HeatingCardCommand result = commanded_target(test::radiator_profile(), 21.5F, HeatingPlanKind::REGISTER, prior);

  EXPECT_FALSE(result.shown);
  EXPECT_FLOAT_EQ(result.card.target_c, 19.0F);
}

TEST(HeatingCardCommand, ATargetEndsAnAliasPresetOnATemperatureHeaterOnly) {
  HeatingCardState alias;
  alias.preset_index = preset_of(thermostat(), HeatingPresetId::ECO);
  EXPECT_EQ(commanded_target(thermostat(), 20.0F, HeatingPlanKind::TEMPERATURE, alias).card.preset_index, -1);

  HeatingCardState program;
  program.preset_index = preset_of(test::radiator_profile(), HeatingPresetId::CUSTOM, "Program");
  EXPECT_EQ(commanded_target(test::radiator_profile(), 20.0F, HeatingPlanKind::REGISTER, program).card.preset_index,
            program.preset_index);
}

// === Starting state and hardware status ===

TEST(HeatingCard, ACardStartsOffWithNothingKnownAndTheProfilesOwnRange) {
  const HeatingCardState radiator_card = initial_heating_card(test::radiator_profile());
  EXPECT_EQ(radiator_card.mode, HeatingHvacMode::OFF);
  EXPECT_EQ(radiator_card.preset_index, -1);
  EXPECT_TRUE(std::isnan(radiator_card.target_c));
  EXPECT_TRUE(std::isnan(radiator_card.current_c));
  EXPECT_FALSE(radiator_card.observed);
  EXPECT_FLOAT_EQ(radiator_card.max_target_c, test::radiator_profile().visual_max_c);
  EXPECT_FLOAT_EQ(initial_heating_card(thermostat(), 10.0F, 25.0F).max_target_c, 25.0F)
      << "a narrower YAML encoding range narrows the thermostat's card";
  EXPECT_FLOAT_EQ(initial_heating_card(thermostat()).max_target_c, thermostat().visual_max_c)
      << "the default encoding range holds the whole visual range";
  EXPECT_FLOAT_EQ(initial_heating_card(test::level_heater_profile()).max_target_c, HEATING_TEMP_MAX_C)
      << "no setpoint, no range";
}

namespace {

/// @brief A card spans `min_c` to `max_c`.
void expect_card_range(const HeatingCardRange &range, float min_c, float max_c) {
  EXPECT_FLOAT_EQ(range.min_c, min_c);
  EXPECT_FLOAT_EQ(range.max_c, max_c);
}

}  // namespace

TEST(HeatingCard, AMainParameterCardOffersTheVisualRangeInsideTheEncodingRange) {
  const HeatingProfile &pump = test::profile_of(DeviceType::HEAT_PUMP, 0);
  SCOPED_TRACE("thermostat: the visual range, cut to the YAML encoding range");
  expect_card_range(heating_card_range(thermostat(), HEATING_TEMPERATURE_RANGE_MIN_C, HEATING_TEMPERATURE_RANGE_MAX_C),
                    thermostat().visual_min_c, thermostat().visual_max_c);
  expect_card_range(heating_card_range(thermostat(), 10.0F, 25.0F), 10.0F, 25.0F);
  expect_card_range(heating_card_range(thermostat(), 0.0F, 25.0F), thermostat().visual_min_c, 25.0F);
  expect_card_range(heating_card_range(thermostat(), 10.0F, 60.0F), 10.0F, thermostat().visual_max_c);
  // An encoding range beside the visual range leaves the card the device's own range.
  expect_card_range(heating_card_range(thermostat(), 35.0F, 60.0F), 35.0F, 60.0F);
  SCOPED_TRACE("heat pump: a fixed encoding range, whatever the YAML says");
  expect_card_range(heating_card_range(pump, 10.0F, 25.0F), pump.visual_min_c, pump.visual_max_c);
  SCOPED_TRACE("register profiles: the visual range");
  expect_card_range(heating_card_range(test::radiator_profile(), 10.0F, 25.0F), test::radiator_profile().visual_min_c,
                    test::radiator_profile().visual_max_c);
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
  EXPECT_EQ(heating_hardware_status(&test::level_heater_profile()), HeatingHardwareStatus::LEVELS_CONFIRMED);
  EXPECT_EQ(heating_hardware_status(&test::radiator_profile()), HeatingHardwareStatus::CONFIRMED);
  EXPECT_EQ(heating_hardware_status(&test::towel_dryer_profile()), HeatingHardwareStatus::CONFIRMED);

  HeatingProfile unconfirmed_writes = test::radiator_profile();
  unconfirmed_writes.experimental_writes = true;
  EXPECT_EQ(heating_hardware_status(&unconfirmed_writes), HeatingHardwareStatus::WRITES_EXPERIMENTAL);
}

// === The target of a radiator in auto is the temperature in force ===

TEST(HeatingCard, ARadiatorInProgOrAutoShowsTheTemperatureInForceAsItsTargetAndInHeatTheComfortSetpoint) {
  for (const HeatingMode mode : {HeatingMode::PROG, HeatingMode::AUTO}) {
    IoDevice readings;
    test::store_reading(readings, DeviceReading::MODE, static_cast<int16_t>(mode));
    test::store_reading(readings, DeviceReading::SETPOINT, 19.0F);
    test::store_reading(readings, DeviceReading::TEMPERATURE_IN_FORCE, 16.5F);
    const HeatingCardState card = reduced(test::radiator_profile(), readings);
    EXPECT_EQ(card.mode, HeatingHvacMode::AUTO);
    EXPECT_FLOAT_EQ(card.target_c, 16.5F) << static_cast<int>(mode);
  }
  IoDevice heat;
  test::store_reading(heat, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::MANUAL));
  test::store_reading(heat, DeviceReading::SETPOINT, 19.0F);
  test::store_reading(heat, DeviceReading::TEMPERATURE_IN_FORCE,
                      16.5F);  // a stale value is never shown outside the programme
  EXPECT_FLOAT_EQ(reduced(test::radiator_profile(), heat).target_c, 19.0F);

  IoDevice silent;
  test::store_reading(silent, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::PROG));
  test::store_reading(silent, DeviceReading::SETPOINT, 19.0F);
  EXPECT_FLOAT_EQ(reduced(test::radiator_profile(), silent).target_c, 19.0F)
      << "a heater without the register: today's target";
}
