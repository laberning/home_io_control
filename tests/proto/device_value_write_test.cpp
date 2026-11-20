/// @file device_value_write_test.cpp
/// @brief Tests for a device value's write side and the rows several models share (ADR 0110): the capability groups,
/// the row a device's capabilities select, a main-parameter target over the range of its row, a register write judged
/// by the register's rule, and the status-parameter form "field equals N".
///
/// The production table has one row of a water heater's target; the rows a second heater model needs (the Atlantic
/// Explorer V4's 20..70 degrees) are rows of a test table here, gated by capabilities that production gives other
/// meanings, so the selection and the write are tested without a production row that no hardware has confirmed.

#include "proto_device_features.h"
#include "proto_device_readings.h"
#include "proto_device_types.h"
#include "proto_dhw.h"
#include "device_table_walk.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <vector>

using namespace esphome::home_io_control;

namespace {

// === A test group and a test table of the target's rows ===

/// Stand-ins for the members of a test group: capabilities no water heater of production carries.
constexpr DeviceFeature RANGE_20_62 = DeviceFeature::TILT;
constexpr DeviceFeature RANGE_20_70 = DeviceFeature::VENT;
constexpr DeviceFeature FOREIGN = DeviceFeature::PEDESTRIAN;

constexpr DeviceFeature TEST_MEMBERS[] = {RANGE_20_62, RANGE_20_70};
constexpr FeatureGroup TEST_GROUP{.name = "TEST_TARGET", .base = DeviceFeature::WATER_HEATER, .members = TEST_MEMBERS};

/// A row of the water heater's target with its own range, written as a main parameter.
constexpr DeviceReadingRow target_row(DeviceFeature feature, ValueRange range) {
  return {.name = "hot_water_target",
          .source = StatusMainParameterSource{.form = StatusValueForm::TEMPERATURE, .range = range},
          .reading = DeviceReading::HOT_WATER_TARGET,
          .poll = ReadingPoll::NOT_POLLED,
          .feature = feature,
          .write = ValueWrite{MainParameterWrite{}}};
}

constexpr DeviceReadingRow ROW_20_62 = target_row(RANGE_20_62, {20.0F, 62.0F});
constexpr DeviceReadingRow ROW_20_70 = target_row(RANGE_20_70, {20.0F, 70.0F});
constexpr DeviceReadingRow TEST_TABLE[] = {ROW_20_62, ROW_20_70};

/// A water heater with no reading yet, for a write whose guard is none or unread.
const IoDevice &heater() {
  static const IoDevice dev = test::water_heater_record();
  return dev;
}

/// A water heater record carrying exactly the given capability next to the water heater's own.
IoDevice heater_with(DeviceFeatures extra) {
  IoDevice dev = test::water_heater_record();
  dev.features = DeviceFeatures{device_feature::WATER_HEATER} | extra;
  return dev;
}

/// What is wrong with a capability set against a group: nothing, no member, or several.
enum class GroupFault { NONE, NO_MEMBER, SEVERAL_MEMBERS };

GroupFault group_fault(DeviceFeatures kind, const FeatureGroup &group) {
  if (!has_feature(kind, group.base))
    return GroupFault::NONE;
  const size_t members = group_member_count(kind, group);
  return members == 0 ? GroupFault::NO_MEMBER : members == 1 ? GroupFault::NONE : GroupFault::SEVERAL_MEMBERS;
}

/// Whether every row of a reading with several rows is gated by a member of one group; a row without a capability or
/// gated outside the group fails.
bool rows_follow_group(std::span<const DeviceReadingRow> table, DeviceReading reading, const FeatureGroup &group) {
  size_t rows = 0;
  for (const DeviceReadingRow &row : table)
    rows += row.reading == reading ? 1U : 0U;
  if (rows <= 1)
    return true;
  for (const DeviceReadingRow &row : table) {
    if (row.reading != reading)
      continue;
    if (!row.feature || std::ranges::find(group.members, *row.feature) == group.members.end())
      return false;
  }
  return true;
}

}  // namespace

// === Capability groups ===

TEST(FeatureGroups, ADeviceWithNoMemberOfAGroupItsBaseNeedsFailsTheCheck) {
  EXPECT_EQ(group_fault(DeviceFeature::WATER_HEATER, TEST_GROUP), GroupFault::NO_MEMBER);
}

TEST(FeatureGroups, ADeviceWithTwoMembersOfAGroupFailsTheCheck) {
  EXPECT_EQ(group_fault(DeviceFeature::WATER_HEATER | RANGE_20_62 | RANGE_20_70, TEST_GROUP),
            GroupFault::SEVERAL_MEMBERS);
}

TEST(FeatureGroups, ADeviceWithOneMemberPassesAndAKindWithoutTheBaseNeedsNone) {
  EXPECT_EQ(group_fault(DeviceFeature::WATER_HEATER | RANGE_20_70, TEST_GROUP), GroupFault::NONE);
  EXPECT_EQ(group_fault(DeviceFeatures{}, TEST_GROUP), GroupFault::NONE);
}

TEST(FeatureGroups, ARowOfASharedReadingGatedOutsideTheGroupOrNotAtAllFailsTheCheck) {
  EXPECT_TRUE(rows_follow_group(TEST_TABLE, DeviceReading::HOT_WATER_TARGET, TEST_GROUP));

  const DeviceReadingRow foreign_gate[] = {ROW_20_62, target_row(FOREIGN, {20.0F, 70.0F})};
  EXPECT_FALSE(rows_follow_group(foreign_gate, DeviceReading::HOT_WATER_TARGET, TEST_GROUP))
      << "a capability outside the group would let two rows apply";

  DeviceReadingRow ungated = ROW_20_70;
  ungated.feature = std::nullopt;
  const DeviceReadingRow no_gate[] = {ROW_20_62, ungated};
  EXPECT_FALSE(rows_follow_group(no_gate, DeviceReading::HOT_WATER_TARGET, TEST_GROUP))
      << "a row of a shared reading needs a capability";
}

TEST(FeatureGroups, EveryKindWithAGroupsBaseCarriesExactlyOneMemberModelsIncluded) {
  const auto check = [](const DeviceTypeInfo &row) {
    for (const FeatureGroup &group : FEATURE_GROUPS) {
      EXPECT_EQ(group_fault(row.features, group), GroupFault::NONE)
          << row.name << " (" << static_cast<unsigned>(row.type) << "/" << static_cast<unsigned>(row.subtype)
          << ") must carry exactly one member of " << group.name;
    }
  };
  test::for_each_table_row(check);
  test::for_each_model_row(check);
}

TEST(FeatureGroups, EveryMemberOfAGroupIsAFurtherCapabilityThanItsBase) {
  for (const FeatureGroup &group : FEATURE_GROUPS) {
    EXPECT_FALSE(group.members.empty()) << group.name;
    for (const DeviceFeature member : group.members)
      EXPECT_NE(member, group.base) << group.name;
  }
}

TEST(FeatureGroups, EveryReadingWithSeveralRowsIsGatedByTheMembersOfOneGroup) {
  for (const DeviceReadingRow &row : device_readings()) {
    size_t rows = 0;
    for (const DeviceReadingRow &other : device_readings())
      rows += other.reading == row.reading ? 1U : 0U;
    if (rows <= 1)
      continue;
    bool in_a_group = false;
    for (const FeatureGroup &group : FEATURE_GROUPS)
      in_a_group = in_a_group || rows_follow_group(device_readings(), row.reading, group);
    EXPECT_TRUE(in_a_group) << row.name << ": the rows of a shared reading are gated by members of one group";
  }
}

TEST(FeatureGroups, TheProductionWaterHeaterTargetIsOneRowGatedByAMemberOfItsGroup) {
  const DeviceReadingRow *row = find_device_reading(DeviceReading::HOT_WATER_TARGET);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(row->feature.has_value());
  EXPECT_EQ(*row->feature, DeviceFeature::HOT_WATER_TARGET_20_62);
  EXPECT_EQ(group_member_count(*row->feature, FEATURE_GROUPS[0]), 1u);
}

TEST(FeatureGroups, ADeviceThatLosesTheBaseLosesTheGroupsMemberToo) {
  IoDevice dev;
  dev.type = DeviceType::DOMESTIC_HOT_WATER;
  dev.entity_class = DeviceCapabilityClass::SWITCH;  // bound to another platform: the water heater path is dropped
  resolve_device_features(dev);
  EXPECT_FALSE(has_feature(dev.features, DeviceFeature::WATER_HEATER));
  EXPECT_FALSE(has_feature(dev.features, DeviceFeature::HOT_WATER_TARGET_20_62));
}

// === The row a device's capabilities select ===

TEST(DeviceReadingRows, ADeviceGetsTheRowOfItsOwnCapability) {
  const IoDevice calypso = heater_with(RANGE_20_62);
  const IoDevice explorer = heater_with(RANGE_20_70);
  const DeviceReadingRow *row62 = device_reading_row(calypso, DeviceReading::HOT_WATER_TARGET, TEST_TABLE);
  const DeviceReadingRow *row70 = device_reading_row(explorer, DeviceReading::HOT_WATER_TARGET, TEST_TABLE);
  ASSERT_NE(row62, nullptr);
  ASSERT_NE(row70, nullptr);
  EXPECT_EQ(row62->feature, RANGE_20_62);
  EXPECT_EQ(row70->feature, RANGE_20_70);
}

TEST(DeviceReadingRows, ADeviceThatWouldMatchNoRowOrTwoGetsNone) {
  const IoDevice none = heater_with(DeviceFeatures{});
  EXPECT_EQ(device_reading_row_matches(none, DeviceReading::HOT_WATER_TARGET, TEST_TABLE), 0u);
  EXPECT_EQ(device_reading_row(none, DeviceReading::HOT_WATER_TARGET, TEST_TABLE), nullptr);

  const IoDevice both = heater_with(RANGE_20_62 | RANGE_20_70);
  EXPECT_EQ(device_reading_row_matches(both, DeviceReading::HOT_WATER_TARGET, TEST_TABLE), 2u);
  EXPECT_EQ(device_reading_row(both, DeviceReading::HOT_WATER_TARGET, TEST_TABLE), nullptr);
}

TEST(DeviceReadingRows, EveryKindOfTheTypeTableThatHasAGroupsBaseMatchesExactlyOneRowOfItsReading) {
  const auto check = [](const DeviceTypeInfo &row) {
    if (!has_feature(row.features, DeviceFeature::WATER_HEATER))
      return;
    IoDevice dev;
    dev.features = row.features;
    EXPECT_EQ(device_reading_row_matches(dev, DeviceReading::HOT_WATER_TARGET, device_readings()), 1u) << row.name;
    EXPECT_NE(device_reading_row(dev, DeviceReading::HOT_WATER_TARGET), nullptr) << row.name;
  };
  test::for_each_table_row(check);
  test::for_each_model_row(check);
}

TEST(DeviceReadingRows, AReadingWithOneRowIsThatRowWhateverTheDeviceHas) {
  const IoDevice cover;
  EXPECT_EQ(device_reading_row_matches(cover, DeviceReading::HEATING_LEVEL, device_readings()), 1u);
  EXPECT_EQ(device_reading_row(cover, DeviceReading::HEATING_LEVEL), find_device_reading(DeviceReading::HEATING_LEVEL));
  EXPECT_EQ(device_reading_row_matches(cover, DeviceReading::COUNT, device_readings()), 0u);
}

// === The target of a model with its own range ===

TEST(DeviceValueWrite, ATwentyToSeventyTargetDecodesTheCapturedMainParameters) {
  const struct {
    uint8_t high;
    uint8_t low;
    float celsius;
  } captured[] = {{0x7C, 0x00, 51.0F}, {0x8C, 0x00, 55.0F}, {0x9C, 0x00, 59.0F}};
  for (const auto &c : captured) {
    const uint8_t bytes[] = {c.high, c.low};
    // The device's own range is the Calypso's; the row's range wins.
    const auto value = decode_status_reading(ROW_20_70, bytes, 0, 20.0F, 62.0F);
    ASSERT_TRUE(value.has_value());
    EXPECT_FLOAT_EQ(*value, c.celsius) << std::hex << ((c.high << 8) | c.low);
  }
}

TEST(DeviceValueWrite, TheSameWordIsAnotherTemperatureOnAnotherRowsRange) {
  const uint8_t bytes[] = {0x7C, 0x00};
  EXPECT_FLOAT_EQ(*decode_status_reading(ROW_20_62, bytes, 0, 0.0F, 0.0F), 46.0F)
      << "0x7C00 over 20..62 is 46.04 degrees, shown as 46.0, and 51 over 20..70";
}

TEST(DeviceValueWrite, FiftyFourDegreesOnTheTwentyToSeventyRowEncodesAsEightEightHundred) {
  const ValueWritePlan plan = plan_value_write(heater(), ROW_20_70, 54.0F);
  ASSERT_EQ(plan.refusal, ValueWriteRefusal::NONE);
  EXPECT_TRUE(plan.is_execute());
  EXPECT_EQ(plan.main_parameter, 0x8800);
}

TEST(DeviceValueWrite, TheEndsOfTheRowsRangeEncodeAndAValueBeyondThemIsRefusedNotClamped) {
  EXPECT_EQ(plan_value_write(heater(), ROW_20_70, 20.0F).main_parameter, 0x0000);
  EXPECT_EQ(plan_value_write(heater(), ROW_20_70, 70.0F).main_parameter, 0xC800);
  EXPECT_EQ(plan_value_write(heater(), ROW_20_70, 70.1F).refusal, ValueWriteRefusal::OUT_OF_RANGE);
  EXPECT_EQ(plan_value_write(heater(), ROW_20_70, 19.9F).refusal, ValueWriteRefusal::OUT_OF_RANGE);
  EXPECT_EQ(plan_value_write(heater(), ROW_20_62, 70.0F).refusal, ValueWriteRefusal::OUT_OF_RANGE)
      << "70 degrees is outside the 20..62 row";
  EXPECT_EQ(plan_value_write(heater(), ROW_20_70, NAN).refusal, ValueWriteRefusal::UNENCODABLE);
}

TEST(DeviceValueWrite, AMainParameterWriteOnARowWithoutARangeCannotBeEncoded) {
  DeviceReadingRow row = ROW_20_70;
  row.source = StatusMainParameterSource{.form = StatusValueForm::TEMPERATURE};
  EXPECT_EQ(plan_value_write(heater(), row, 54.0F).refusal, ValueWriteRefusal::UNENCODABLE);
}

TEST(DeviceValueWrite, TheCalypsosProductionRowEncodesLikeTheCaptures) {
  const DeviceReadingRow *row = find_device_reading(DeviceReading::HOT_WATER_TARGET);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(plan_value_write(heater(), *row, 54.0F).main_parameter, 0xA1E8);
  EXPECT_EQ(plan_value_write(heater(), *row, 62.0F).main_parameter, 0xC800);
  EXPECT_EQ(plan_value_write(heater(), *row, 62.5F).refusal, ValueWriteRefusal::OUT_OF_RANGE);
}

TEST(DeviceValueWrite, ADevicesTemperatureRangeIsTheRangeOfItsTargetRow) {
  const IoDevice dev = test::water_heater_record();
  const DeviceReadingRow *row = device_reading_row(dev, DeviceReading::HOT_WATER_TARGET);
  ASSERT_NE(row, nullptr);
  const auto *source = std::get_if<StatusMainParameterSource>(&row->source);
  ASSERT_NE(source, nullptr);
  ASSERT_TRUE(source->range.has_value());
  const HeatingCardRange range = device_temperature_range(dev);
  EXPECT_FLOAT_EQ(range.min_c, source->range->min);
  EXPECT_FLOAT_EQ(range.max_c, source->range->max);
}

// === A register write ===

TEST(DeviceValueWrite, ARegisterWriteIsJudgedByTheRegistersOwnRule) {
  const DeviceReadingRow *mode = find_device_reading(DeviceReading::HOT_WATER_MODE);
  ASSERT_NE(mode, nullptr);
  for (const DhwMode value : {DhwMode::AUTO, DhwMode::MANUAL_ECO_ON, DhwMode::MANUAL_ECO_OFF}) {
    const ValueWritePlan plan = plan_value_write(heater(), *mode, static_cast<float>(value));
    ASSERT_EQ(plan.refusal, ValueWriteRefusal::NONE);
    EXPECT_FALSE(plan.is_execute());
    EXPECT_EQ(plan.register_fn, DeviceRegister::DHW_MODE);
    EXPECT_FLOAT_EQ(plan.register_value, static_cast<float>(value));
  }
  EXPECT_EQ(plan_value_write(heater(), *mode, 3.0F).refusal, ValueWriteRefusal::REGISTER_RULE)
      << "a mode the heater has never shown, by the register's rule";
  EXPECT_EQ(plan_value_write(heater(), *mode, 1.5F).refusal, ValueWriteRefusal::REGISTER_RULE);
  EXPECT_EQ(plan_value_write(heater(), *mode, NAN).refusal, ValueWriteRefusal::REGISTER_RULE);
}

TEST(DeviceValueWrite, ARowWithoutAWriteSideIsRefused) {
  const DeviceReadingRow *energy = find_device_reading(DeviceReading::HOT_WATER_ENERGY);
  ASSERT_NE(energy, nullptr);
  EXPECT_FALSE(energy->write.has_value());
  EXPECT_EQ(plan_value_write(heater(), *energy, 1.0F).refusal, ValueWriteRefusal::NO_WRITE_SIDE);
}

TEST(DeviceValueWrite, OnlyTheRowsThatSayHowAreWritable) {
  for (const DeviceReadingRow &row : device_readings()) {
    if (!row.write)
      continue;
    EXPECT_TRUE(row.reading == DeviceReading::HOT_WATER_MODE || row.reading == DeviceReading::HOT_WATER_TARGET ||
                row.reading == DeviceReading::HOT_WATER_BOOST || row.reading == DeviceReading::HOT_WATER_AWAY)
        << row.name << ": a write side is a deliberate addition";
  }
}

// === A field that equals N ===

TEST(StatusParameterForm, AFieldEqualsTheValueTheRowNamesAndNothingElse) {
  // The Calypso reports boost as field 1 of FP16 bits 8-9 (4A00 at rest, 4900 boost); the Explorer V4's word is the
  // other way round (4900 at rest, 4A00 boost), which is field 2.
  const StatusFunctionalParameterSource boost_is_one{.fp = DHW_FP_OPERATING_MODE,
                                                     .form = StatusParameterForm::FIELD_EQUALS,
                                                     .shift = DHW_FP16_BOOST_SHIFT,
                                                     .equals = 1};
  const StatusFunctionalParameterSource boost_is_two{.fp = DHW_FP_OPERATING_MODE,
                                                     .form = StatusParameterForm::FIELD_EQUALS,
                                                     .shift = DHW_FP16_BOOST_SHIFT,
                                                     .equals = 2};
  EXPECT_EQ(decode_status_parameter(boost_is_one, 0x4900), 1.0F);
  EXPECT_EQ(decode_status_parameter(boost_is_one, 0x4A00), 0.0F);
  EXPECT_EQ(decode_status_parameter(boost_is_two, 0x4900), 0.0F);
  EXPECT_EQ(decode_status_parameter(boost_is_two, 0x4A00), 1.0F);
  EXPECT_EQ(decode_status_parameter(boost_is_two, 0x4B00), 0.0F) << "field 3 is neither";
  EXPECT_EQ(decode_status_parameter(boost_is_two, 0x4800), 0.0F) << "field 0 is neither";
}

TEST(StatusParameterForm, TheWordFormKeepsTheWholeValue) {
  const StatusFunctionalParameterSource word{.fp = DHW_FP_CAPABILITIES, .form = StatusParameterForm::WORD};
  EXPECT_EQ(decode_status_parameter(word, 0x000C), 12.0F);
}

TEST(StatusParameterForm, TheCalypsosBoostAndAwayRowsAreFieldsEqualToOne) {
  for (const DeviceReading reading : {DeviceReading::HOT_WATER_BOOST, DeviceReading::HOT_WATER_AWAY}) {
    const DeviceReadingRow *row = find_device_reading(reading);
    ASSERT_NE(row, nullptr);
    const auto *source = std::get_if<StatusFunctionalParameterSource>(&row->source);
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->form, StatusParameterForm::FIELD_EQUALS);
    EXPECT_EQ(source->equals, 1) << row->name;
  }
}
