/// @file value_write_side_test.cpp
/// @brief Tests for the parts of a value's write side beyond its target (ADR 0110): a functional-parameter target, a
/// guard on another reading, a write sent ahead, and the rounding policy of a target that takes only steps.
///
/// The Atlantic Calypso's boost and away rows are the production rows under test. The stepped target is a row of
/// the test-only shape in stepped_water_heater_rows.h (a one-byte register counting showers), because no production
/// heater takes steps yet.

#include "proto_device_features.h"
#include "proto_device_readings.h"
#include "proto_dhw.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"
#include "reading_store.h"
#include "stepped_water_heater_rows.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace esphome::home_io_control;

namespace {

/// A water heater whose FP15 has been read as `capabilities`.
IoDevice heater_reporting(uint16_t capabilities) {
  IoDevice dev = test::water_heater_record();
  test::store_reading(dev, DeviceReading::HOT_WATER_CAPABILITIES, capabilities);
  return dev;
}

/// A water heater in the given mode.
IoDevice heater_in_mode(DhwMode mode) {
  IoDevice dev = test::water_heater_record();
  test::store_reading(dev, DeviceReading::HOT_WATER_MODE, static_cast<int>(mode));
  return dev;
}

const DeviceReadingRow &row_of(DeviceReading reading) { return *find_device_reading(reading); }

}  // namespace

// === A functional-parameter target ===

TEST(ValueWriteSide, AFlagOnTheOperatingModeWordSendsTheOnOrTheOffWordOfItsRow) {
  const IoDevice dev = test::water_heater_record();
  const struct {
    DeviceReading reading;
    uint16_t on_word;
  } features[] = {{DeviceReading::HOT_WATER_BOOST, 0x4900}, {DeviceReading::HOT_WATER_AWAY, 0x4600}};
  for (const auto &feature : features) {
    const ValueWritePlan on = plan_value_write(dev, row_of(feature.reading), 1.0F);
    ASSERT_EQ(on.refusal, ValueWriteRefusal::NONE);
    EXPECT_TRUE(on.is_execute());
    EXPECT_EQ(on.functional_parameter, DHW_FP_OPERATING_MODE);
    EXPECT_EQ(on.functional_word, feature.on_word);

    const ValueWritePlan off = plan_value_write(dev, row_of(feature.reading), 0.0F);
    ASSERT_EQ(off.refusal, ValueWriteRefusal::NONE);
    EXPECT_EQ(off.functional_word, 0x4A00) << "the off word ends boost and away alike";
  }
}

TEST(ValueWriteSide, AValueThatIsNoFlagIsRefusedForAFunctionalParameter) {
  const IoDevice dev = test::water_heater_record();
  for (const float value : {0.5F, 2.0F, -1.0F, NAN})
    EXPECT_EQ(plan_value_write(dev, row_of(DeviceReading::HOT_WATER_BOOST), value).refusal,
              ValueWriteRefusal::UNENCODABLE)
        << value;
}

// === A write sent ahead ===

TEST(ValueWriteSide, TheDurationGoesAheadOfTheOnWordAndNothingGoesAheadOfTheOffWord) {
  const IoDevice dev = test::water_heater_record();
  const ValueWritePlan boost = plan_value_write(dev, row_of(DeviceReading::HOT_WATER_BOOST), 1.0F);
  ASSERT_TRUE(boost.before.has_value());
  EXPECT_EQ(boost.before->fn, DeviceRegister::DHW_BOOST_DURATION);
  EXPECT_EQ(boost.before->value, 1.0F);
  const ValueWritePlan away = plan_value_write(dev, row_of(DeviceReading::HOT_WATER_AWAY), 1.0F);
  ASSERT_TRUE(away.before.has_value());
  EXPECT_EQ(away.before->fn, DeviceRegister::DHW_AWAY_DURATION);
  EXPECT_EQ(away.before->value, 255.0F);

  EXPECT_FALSE(plan_value_write(dev, row_of(DeviceReading::HOT_WATER_BOOST), 0.0F).before.has_value());
  EXPECT_FALSE(plan_value_write(dev, row_of(DeviceReading::HOT_WATER_AWAY), 0.0F).before.has_value());
}

TEST(ValueWriteSide, ARowWithoutABeforeWriteHasNoneInItsPlan) {
  const IoDevice dev = test::water_heater_record();
  EXPECT_FALSE(plan_value_write(dev, row_of(DeviceReading::HOT_WATER_TARGET), 54.0F).before.has_value());
}

// === A guard ===

TEST(ValueWriteSide, AGuardThatHasBeenReadAndFailsRefusesTheWriteOnOrOff) {
  const IoDevice only_away = heater_reporting(DHW_CAPABILITY_AWAY);
  for (const float flag : {1.0F, 0.0F})
    EXPECT_EQ(plan_value_write(only_away, row_of(DeviceReading::HOT_WATER_BOOST), flag).refusal,
              ValueWriteRefusal::GUARD)
        << "the heater does not report boost as supported: " << flag;
  const IoDevice only_boost = heater_reporting(DHW_CAPABILITY_BOOST);
  EXPECT_EQ(plan_value_write(only_boost, row_of(DeviceReading::HOT_WATER_AWAY), 1.0F).refusal,
            ValueWriteRefusal::GUARD);
  const IoDevice none = heater_reporting(0);
  EXPECT_EQ(plan_value_write(none, row_of(DeviceReading::HOT_WATER_AWAY), 1.0F).refusal, ValueWriteRefusal::GUARD);
}

TEST(ValueWriteSide, AGuardThatPassesLetsTheWriteThrough) {
  const IoDevice both = heater_reporting(DHW_CAPABILITY_AWAY | DHW_CAPABILITY_BOOST);
  EXPECT_EQ(plan_value_write(both, row_of(DeviceReading::HOT_WATER_BOOST), 1.0F).refusal, ValueWriteRefusal::NONE);
  EXPECT_EQ(plan_value_write(both, row_of(DeviceReading::HOT_WATER_AWAY), 1.0F).refusal, ValueWriteRefusal::NONE);
}

TEST(ValueWriteSide, AGuardWhoseReadingHasNotBeenReadLetsTheWriteTry) {
  const IoDevice unread = test::water_heater_record();
  ASSERT_EQ(reading_wire(unread, DeviceReading::HOT_WATER_CAPABILITIES), -1);
  EXPECT_EQ(plan_value_write(unread, row_of(DeviceReading::HOT_WATER_BOOST), 1.0F).refusal, ValueWriteRefusal::NONE);
}

TEST(ValueWriteSide, AnEqualsGuardAllowsTheWriteOnlyInTheModeItNames) {
  const test::stepped::SteppedHeaterTables tables;
  const DeviceReadingRow target = test::stepped::target_row();
  EXPECT_EQ(plan_value_write(heater_in_mode(DhwMode::MANUAL_ECO_ON), target, 60.0F).refusal, ValueWriteRefusal::NONE);
  EXPECT_EQ(plan_value_write(heater_in_mode(DhwMode::AUTO), target, 60.0F).refusal, ValueWriteRefusal::GUARD);
  EXPECT_EQ(plan_value_write(heater_in_mode(DhwMode::MANUAL_ECO_OFF), target, 60.0F).refusal, ValueWriteRefusal::GUARD);
  EXPECT_EQ(plan_value_write(test::water_heater_record(), target, 60.0F).refusal, ValueWriteRefusal::NONE)
      << "the mode has not been read yet";
}

// === A stepped target ===

namespace {

/// The plan of `celsius` on the shower-count target under `policy`, for a heater in manual mode.
ValueWritePlan stepped_plan(RoundingPolicy policy, float celsius) {
  return plan_value_write(heater_in_mode(DhwMode::MANUAL_ECO_ON), test::stepped::target_row(policy), celsius);
}

}  // namespace

TEST(SteppedTarget, ARegisterTargetWritesToTheRegisterWithTheDegreesTheScaleEncodes) {
  const test::stepped::SteppedHeaterTables tables;
  for (const float celsius : {50.0F, 60.0F, 70.0F}) {
    const ValueWritePlan plan = stepped_plan(RoundingPolicy::EXACT, celsius);
    ASSERT_EQ(plan.refusal, ValueWriteRefusal::NONE) << celsius;
    EXPECT_FALSE(plan.is_execute());
    EXPECT_EQ(plan.register_fn, test::stepped::STEP_REGISTER);
    EXPECT_FLOAT_EQ(plan.register_value, celsius);
  }
}

TEST(SteppedTarget, ExactRefusesAValueBetweenTheStepsAndOutsideThem) {
  const test::stepped::SteppedHeaterTables tables;
  for (const float celsius : {55.0F, 65.0F, 50.5F})
    EXPECT_EQ(stepped_plan(RoundingPolicy::EXACT, celsius).refusal, ValueWriteRefusal::OFF_STEP) << celsius;
  for (const float celsius : {49.9F, 70.1F, 20.0F})
    EXPECT_EQ(stepped_plan(RoundingPolicy::EXACT, celsius).refusal, ValueWriteRefusal::OUT_OF_RANGE) << celsius;
  EXPECT_EQ(stepped_plan(RoundingPolicy::EXACT, NAN).refusal, ValueWriteRefusal::UNENCODABLE);
}

TEST(SteppedTarget, NearestDownAndUpPickTheStepTheirPolicyNames) {
  const test::stepped::SteppedHeaterTables tables;
  const struct {
    float asked;
    float nearest;
    float down;
    float up;
  } cases[] = {{55.0F, 60.0F, 50.0F, 60.0F},  // halfway: nearest takes the higher
               {54.0F, 50.0F, 50.0F, 60.0F}, {56.0F, 60.0F, 50.0F, 60.0F}, {65.0F, 70.0F, 60.0F, 70.0F},
               {51.0F, 50.0F, 50.0F, 60.0F}, {69.0F, 70.0F, 60.0F, 70.0F}};
  for (const auto &c : cases) {
    EXPECT_FLOAT_EQ(stepped_plan(RoundingPolicy::NEAREST, c.asked).register_value, c.nearest) << c.asked;
    EXPECT_FLOAT_EQ(stepped_plan(RoundingPolicy::DOWN, c.asked).register_value, c.down) << c.asked;
    EXPECT_FLOAT_EQ(stepped_plan(RoundingPolicy::UP, c.asked).register_value, c.up) << c.asked;
  }
}

TEST(SteppedTarget, AStepIsKeptWhateverThePolicyAndRoundingNeverClampsOutsideTheScale) {
  const test::stepped::SteppedHeaterTables tables;
  for (const RoundingPolicy policy : {RoundingPolicy::NEAREST, RoundingPolicy::DOWN, RoundingPolicy::UP}) {
    EXPECT_FLOAT_EQ(stepped_plan(policy, 60.0F).register_value, 60.0F);
    EXPECT_EQ(stepped_plan(policy, 45.0F).refusal, ValueWriteRefusal::OUT_OF_RANGE)
        << "below the first step, not rounded up to it";
    EXPECT_EQ(stepped_plan(policy, 75.0F).refusal, ValueWriteRefusal::OUT_OF_RANGE);
  }
}

TEST(SteppedTarget, TheCardOffersTheRangeFromTheFirstToTheLastStepAndTheDistanceOfTheSteps) {
  const test::stepped::SteppedHeaterTables tables;
  const DeviceReadingRow target = test::stepped::target_row();
  const std::optional<ValueRange> range = value_write_range(target);
  ASSERT_TRUE(range.has_value());
  EXPECT_FLOAT_EQ(range->min, 50.0F);
  EXPECT_FLOAT_EQ(range->max, 70.0F);
  EXPECT_EQ(value_write_step(target), 10.0F);
}

TEST(SteppedTarget, ATargetThatTakesAnyValueHasNoStep) {
  EXPECT_FALSE(value_write_step(row_of(DeviceReading::HOT_WATER_TARGET)).has_value()) << "the Calypso's";
  EXPECT_FALSE(value_write_step(row_of(DeviceReading::HOT_WATER_MODE)).has_value());
  EXPECT_FALSE(value_write_step(row_of(DeviceReading::HOT_WATER_ENERGY)).has_value()) << "no write side";
  const std::optional<ValueRange> range = value_write_range(row_of(DeviceReading::HOT_WATER_TARGET));
  ASSERT_TRUE(range.has_value());
  EXPECT_FLOAT_EQ(range->min, DHW_TARGET_MIN_C);
  EXPECT_FLOAT_EQ(range->max, DHW_TARGET_MAX_C);
}

// === A main-parameter target with a step ===

namespace {

/// A main-parameter target over 20 to 70 degrees that takes steps of 5 degrees, under `policy`.
DeviceReadingRow five_degree_target(RoundingPolicy policy) {
  return {.name = "hot_water_target",
          .source = StatusMainParameterSource{.form = StatusValueForm::TEMPERATURE, .range = ValueRange{20.0F, 70.0F}},
          .reading = DeviceReading::HOT_WATER_TARGET,
          .poll = ReadingPoll::NOT_POLLED,
          .feature = DeviceFeature::WATER_HEATER,
          .write = ValueWrite{.target = MainParameterWrite{}, .rounding = ValueRounding{policy, 5.0F}}};
}

}  // namespace

TEST(SteppedTarget, AMainParameterStepIsMeasuredFromTheLowEndOfTheRange) {
  const IoDevice dev = test::water_heater_record();
  EXPECT_EQ(plan_value_write(dev, five_degree_target(RoundingPolicy::EXACT), 55.0F).refusal, ValueWriteRefusal::NONE);
  EXPECT_EQ(plan_value_write(dev, five_degree_target(RoundingPolicy::EXACT), 54.0F).refusal,
            ValueWriteRefusal::OFF_STEP);
  EXPECT_EQ(plan_value_write(dev, five_degree_target(RoundingPolicy::EXACT), 70.0F).main_parameter, 0xC800);
  EXPECT_EQ(plan_value_write(dev, five_degree_target(RoundingPolicy::EXACT), 20.0F).main_parameter, 0x0000);
  EXPECT_EQ(plan_value_write(dev, five_degree_target(RoundingPolicy::EXACT), 75.0F).refusal,
            ValueWriteRefusal::OUT_OF_RANGE);
  EXPECT_EQ(value_write_step(five_degree_target(RoundingPolicy::EXACT)), 5.0F);
  EXPECT_EQ(plan_value_write(dev, five_degree_target(RoundingPolicy::NEAREST), 52.0F).main_parameter,
            plan_value_write(dev, five_degree_target(RoundingPolicy::EXACT), 50.0F).main_parameter);
  EXPECT_EQ(plan_value_write(dev, five_degree_target(RoundingPolicy::UP), 52.0F).main_parameter,
            plan_value_write(dev, five_degree_target(RoundingPolicy::EXACT), 55.0F).main_parameter);
}

// === The groups ===

TEST(ValueWriteSide, TheCalypsosBoostAndAwayRowsAreGatedByTheMemberOfTheirOwnGroup) {
  EXPECT_EQ(row_of(DeviceReading::HOT_WATER_BOOST).feature, DeviceFeature::HOT_WATER_BOOST_FP16);
  EXPECT_EQ(row_of(DeviceReading::HOT_WATER_AWAY).feature, DeviceFeature::HOT_WATER_AWAY_FP16);
  const DeviceFeatures calypso = device_features(DeviceType::DOMESTIC_HOT_WATER, 0, DeviceModel::NONE);
  EXPECT_TRUE(has_feature(calypso, DeviceFeature::HOT_WATER_BOOST_FP16));
  EXPECT_TRUE(has_feature(calypso, DeviceFeature::HOT_WATER_AWAY_FP16));
}

// The FP16 boost and away rows guard on FP15, so the FP15 row must not depend on either member: a model whose boost
// reads otherwise but whose away is the Calypso's still needs FP15 for the away guard.
TEST(ValueWriteSide, TheCapabilityWordIsReadByAnyHeaterWithTheOperatingModeWordWhateverItsBoostMember) {
  EXPECT_EQ(row_of(DeviceReading::HOT_WATER_CAPABILITIES).feature, DeviceFeature::HOT_WATER_OPERATING_MODE_WORD);
  IoDevice dev;
  dev.features = DeviceFeatures{} | device_feature::WATER_HEATER | device_feature::HOT_WATER_AWAY_FP16 |
                 device_feature::HOT_WATER_OPERATING_MODE_WORD;
  EXPECT_TRUE(device_can_have_reading(dev, row_of(DeviceReading::HOT_WATER_CAPABILITIES)));
  EXPECT_TRUE(device_has_status_reading(dev));
  const DeviceFeatures calypso = device_features(DeviceType::DOMESTIC_HOT_WATER, 0, DeviceModel::NONE);
  EXPECT_TRUE(has_feature(calypso, DeviceFeature::HOT_WATER_OPERATING_MODE_WORD));
}
