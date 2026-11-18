/// @file device_readings_test.cpp
/// @brief Tests for the table of named device readings: its rows, the lookup of a value per source kind, the
/// registers a reading makes the poll read, and the conditional reads of a towel dryer's runs.

#include "proto_device_readings.h"

#include <gtest/gtest.h>

#include <cmath>
#include <set>
#include <string>
#include <variant>
#include <vector>

using namespace esphome::home_io_control;

namespace {

/// Readings of a towel dryer whose temporary state has been read.
HeatingReadings readings_in_state(HeatingTemporaryState state) {
  HeatingReadings readings;
  readings.store(HeatingFunction::TEMPORARY_STATE, static_cast<float>(state));
  return readings;
}

IoDevice device_with(const HeatingReadings &readings) {
  IoDevice dev;
  dev.heating = readings;
  return dev;
}

}  // namespace

// === A data-object reading ===

namespace {

constexpr DataObjectSource COUNTER_SLICE{
    .recipe = 0, .offset = 0x10, .first = 2, .length = 3, .codec = ValueCodec::MARKED_U16_BE, .max_value = 0};
/// A test table: no production row has a data-object source yet.
constexpr DeviceReadingRow DATA_TABLE[] = {{.name = "energy_total",
                                            .source = COUNTER_SLICE,
                                            .reading = DeviceReading::ENERGY_TOTAL,
                                            .feature = DeviceFeature::ENERGY_METER}};

}  // namespace

TEST(DeviceReadings, ADataObjectSliceIsDecodedFromItsByteRangeOfTheAnswer) {
  const std::vector<uint8_t> data = {0xAA, 0xBB, 0x80, 0x0C, 0xBE, 0xFF};
  EXPECT_EQ(decode_data_object_reading(COUNTER_SLICE, data), 3262.0F) << "the slice starts at byte 2, three long";
}

TEST(DeviceReadings, ADataObjectSliceWithNoValueOrBeyondTheDataIsNoValue) {
  EXPECT_FALSE(decode_data_object_reading(COUNTER_SLICE, std::vector<uint8_t>{0xAA, 0xBB, 0x81, 0xFF, 0xFF}));
  EXPECT_FALSE(decode_data_object_reading(COUNTER_SLICE, std::vector<uint8_t>{0xAA, 0xBB, 0x80, 0x0C}))
      << "a cut-off slice";
  EXPECT_FALSE(decode_data_object_reading(COUNTER_SLICE, std::vector<uint8_t>{0xAA}));
  EXPECT_FALSE(decode_data_object_reading(COUNTER_SLICE, std::vector<uint8_t>{}));
}

TEST(DeviceReadings, ADataObjectReadingIsLookedUpInTheDevicesStoreAndPollsNoRegister) {
  IoDevice dev;
  EXPECT_FALSE(reading_value(dev, DeviceReading::ENERGY_TOTAL, DATA_TABLE)) << "never read";
  dev.reading_values.store(reading_slot(DeviceReading::ENERGY_TOTAL), 3262.0F);
  EXPECT_EQ(reading_value(dev, DeviceReading::ENERGY_TOTAL, DATA_TABLE), 3262.0F);
  dev.reading_values.mark_absent(reading_slot(DeviceReading::ENERGY_TOTAL));
  EXPECT_FALSE(reading_value(dev, DeviceReading::ENERGY_TOTAL, DATA_TABLE)) << "absent";
  EXPECT_TRUE(reading_polled_registers(DATA_TABLE[0]).empty());
}

TEST(DeviceReadings, AValueStoredAgainAfterBeingAbsentIsHeld) {
  DeviceReadingValues values;
  values.mark_absent(3);
  values.store(3, 7.0F);
  EXPECT_FALSE(values.absent(3));
  EXPECT_EQ(values.value(3), 7.0F);
  EXPECT_FALSE(values.value(DeviceReadingValues::SLOTS)) << "out of range reads as nothing";
}

// === The table ===

TEST(DeviceReadings, EveryReadingHasExactlyOneRowAndAUniqueName) {
  std::set<std::string> names;
  for (unsigned i = 0; i < static_cast<unsigned>(DeviceReading::COUNT); i++) {
    const auto reading = static_cast<DeviceReading>(i);
    const DeviceReadingRow *row = find_device_reading(reading);
    ASSERT_NE(row, nullptr) << i;
    EXPECT_EQ(row->reading, reading);
    EXPECT_TRUE(names.insert(row->name).second) << row->name << " is named twice";
  }
  EXPECT_EQ(device_readings().size(), static_cast<size_t>(DeviceReading::COUNT));
  EXPECT_EQ(find_device_reading(DeviceReading::COUNT), nullptr);
}

TEST(DeviceReadings, NoRegisterIsTheSourceOfTwoReadings) {
  std::set<HeatingFunction> registers;
  for (const DeviceReadingRow &row : device_readings()) {
    if (const auto *source = std::get_if<HeatingRegisterSource>(&row.source)) {
      EXPECT_TRUE(registers.insert(source->fn).second) << row.name;
    }
  }
}

TEST(DeviceReadings, EveryRegisterOfAReadingIsReadable) {
  for (const DeviceReadingRow &row : device_readings()) {
    EXPECT_FALSE(reading_polled_registers(row).empty()) << row.name;
    for (const HeatingFunction fn : reading_polled_registers(row))
      EXPECT_TRUE(heating_function_is_readable(fn)) << row.name;
  }
}

TEST(DeviceReadings, TheRunTimersNeedTheRunTimersCapability) {
  for (const DeviceReading reading :
       {DeviceReading::BOOST_REMAINING, DeviceReading::BOOST_DURATION, DeviceReading::DRYING_REMAINING,
        DeviceReading::DRYING_DURATION, DeviceReading::REMAINING_TIME}) {
    EXPECT_EQ(find_device_reading(reading)->feature, DeviceFeature::RUN_TIMERS) << static_cast<int>(reading);
  }
}

// === The registers a reading makes the poll read ===

TEST(DeviceReadings, ARegisterReadingPollsItsRegisterAndADerivedOneThoseItNames) {
  const auto polled = [](DeviceReading reading) {
    const auto registers = reading_polled_registers(*find_device_reading(reading));
    return std::vector<HeatingFunction>(registers.begin(), registers.end());
  };
  EXPECT_EQ(polled(DeviceReading::ENERGY_TOTAL), std::vector<HeatingFunction>{HeatingFunction::ENERGY_TOTAL});
  EXPECT_EQ(polled(DeviceReading::OCCUPANCY), std::vector<HeatingFunction>{HeatingFunction::SET_PRESENCE})
      << "the reading's name is not its register's";
  EXPECT_EQ(polled(DeviceReading::REMAINING_TIME),
            (std::vector<HeatingFunction>{HeatingFunction::BOOST_REMAINING, HeatingFunction::DRYING_REMAINING}));
}

TEST(DeviceReadings, ASettingIsTheRegisterItWrites) {
  EXPECT_EQ(reading_register(DeviceReading::OCCUPANCY_DETECTION), HeatingFunction::OCCUPANCY_DETECTION);
  EXPECT_EQ(reading_register(DeviceReading::OPEN_WINDOW_DETECTION), HeatingFunction::SET_OPEN_WINDOW_DETECTION);
  EXPECT_FALSE(reading_register(DeviceReading::REMAINING_TIME).has_value()) << "a derived value is no register";
  EXPECT_FALSE(reading_register(DeviceReading::COUNT).has_value());
}

// === Reading a value, per source kind ===

TEST(DeviceReadings, ARegisterReadingHasNoValueUntilTheRegisterIsRead) {
  IoDevice dev;
  EXPECT_FALSE(reading_value(dev, DeviceReading::ENERGY_TOTAL).has_value());
  EXPECT_FALSE(reading_value(dev, DeviceReading::CENTRAL_HEATING_MODE).has_value());
  EXPECT_FALSE(reading_value(dev, DeviceReading::COUNT).has_value());
}

TEST(DeviceReadings, ANumberAndAFlagBothComeBackAsTheirValue) {
  IoDevice dev;
  dev.heating.store(HeatingFunction::ENERGY_TOTAL, 40000.0F);
  dev.heating.store(HeatingFunction::CENTRAL_HEATING_MODE, 0.0F);
  dev.heating.store(HeatingFunction::SET_PRESENCE, 1.0F);
  EXPECT_FLOAT_EQ(*reading_value(dev, DeviceReading::ENERGY_TOTAL), 40000.0F) << "past what a flag byte holds";
  EXPECT_FLOAT_EQ(*reading_value(dev, DeviceReading::CENTRAL_HEATING_MODE), 0.0F) << "off, because the device said so";
  EXPECT_FLOAT_EQ(*reading_value(dev, DeviceReading::OCCUPANCY), 1.0F);
}

TEST(DeviceReadings, ADroppedReadingHasNoValueAgain) {
  IoDevice dev;
  dev.heating.store(HeatingFunction::ENERGY_TOTAL, 100.0F);
  dev.heating.clear(HeatingFunction::ENERGY_TOTAL);
  EXPECT_FALSE(reading_value(dev, DeviceReading::ENERGY_TOTAL).has_value());
}

TEST(DeviceReadings, TheRemainingTimeFollowsTheTemporaryState) {
  HeatingReadings readings = readings_in_state(HeatingTemporaryState::BOOST);
  readings.store(HeatingFunction::BOOST_REMAINING, 55.0F);
  readings.store(HeatingFunction::DRYING_REMAINING, 28.0F);  // an older drying run's value stays out of it
  EXPECT_FLOAT_EQ(*reading_value(device_with(readings), DeviceReading::REMAINING_TIME), 55.0F);

  readings.store(HeatingFunction::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::DRYING));
  EXPECT_FLOAT_EQ(*reading_value(device_with(readings), DeviceReading::REMAINING_TIME), 28.0F);

  readings.store(HeatingFunction::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::NONE));
  EXPECT_FLOAT_EQ(*reading_value(device_with(readings), DeviceReading::REMAINING_TIME), 0.0F)
      << "no run: 0, whatever an earlier run left";
}

TEST(DeviceReadings, TheRemainingTimeHasNoValueUntilItIsKnown) {
  const auto remaining = [](const HeatingReadings &readings) {
    return reading_value(device_with(readings), DeviceReading::REMAINING_TIME);
  };
  EXPECT_FALSE(remaining(HeatingReadings{}).has_value()) << "state not read";

  HeatingReadings boosting = readings_in_state(HeatingTemporaryState::BOOST);
  EXPECT_FALSE(remaining(boosting).has_value()) << "the run's register not read yet";
  boosting.store(HeatingFunction::BOOST_REMAINING, 60.0F);
  EXPECT_FLOAT_EQ(*remaining(boosting), 60.0F);
  boosting.clear(HeatingFunction::BOOST_REMAINING);
  EXPECT_FALSE(remaining(boosting).has_value()) << "a dropped reading is no value";
}

// === Registers read only while another holds a value ===

TEST(DeviceReadings, TheTimeLeftOfARunIsDueOnlyWhileThatRunIsOn) {
  const HeatingReadings idle = readings_in_state(HeatingTemporaryState::NONE);
  EXPECT_FALSE(heating_register_due(HeatingFunction::BOOST_REMAINING, idle));
  EXPECT_FALSE(heating_register_due(HeatingFunction::DRYING_REMAINING, idle));

  const HeatingReadings boosting = readings_in_state(HeatingTemporaryState::BOOST);
  EXPECT_TRUE(heating_register_due(HeatingFunction::BOOST_REMAINING, boosting));
  EXPECT_FALSE(heating_register_due(HeatingFunction::DRYING_REMAINING, boosting));

  const HeatingReadings drying_run = readings_in_state(HeatingTemporaryState::DRYING);
  EXPECT_FALSE(heating_register_due(HeatingFunction::BOOST_REMAINING, drying_run));
  EXPECT_TRUE(heating_register_due(HeatingFunction::DRYING_REMAINING, drying_run));

  const HeatingReadings unread;
  EXPECT_FALSE(heating_register_due(HeatingFunction::BOOST_REMAINING, unread)) << "an unread state is no run";
  EXPECT_FALSE(heating_register_due(HeatingFunction::DRYING_REMAINING, unread));
}

TEST(DeviceReadings, ARegisterWithoutAConditionIsAlwaysDue) {
  for (const HeatingFunction fn : {HeatingFunction::ENERGY_TOTAL, HeatingFunction::BOOST_DURATION,
                                   HeatingFunction::DRYING_DURATION, HeatingFunction::CENTRAL_HEATING_MODE}) {
    EXPECT_TRUE(heating_register_due(fn, HeatingReadings{})) << heating_function_name(fn);
  }
  EXPECT_TRUE(heating_register_due(HeatingFunction::ROOM_TEMPERATURE, HeatingReadings{}))
      << "a register no reading is has no condition";
}

TEST(DeviceReadings, AWrittenTemporaryStateForgetsTheTimeLeftOfBothRunsAndNothingElse) {
  HeatingReadings readings = readings_in_state(HeatingTemporaryState::BOOST);
  readings.store(HeatingFunction::BOOST_REMAINING, 40.0F);
  readings.store(HeatingFunction::DRYING_REMAINING, 12.0F);
  readings.store(HeatingFunction::BOOST_DURATION, 60.0F);

  EXPECT_TRUE(clear_readings_read_only_while(HeatingFunction::TEMPORARY_STATE, readings));
  EXPECT_FALSE(readings.has(HeatingFunction::BOOST_REMAINING));
  EXPECT_FALSE(readings.has(HeatingFunction::DRYING_REMAINING));
  EXPECT_FLOAT_EQ(readings.number_of(HeatingFunction::BOOST_DURATION), 60.0F);
  EXPECT_EQ(readings.value_of(HeatingFunction::TEMPORARY_STATE), static_cast<int16_t>(HeatingTemporaryState::BOOST));

  EXPECT_FALSE(clear_readings_read_only_while(HeatingFunction::TEMPORARY_STATE, readings)) << "nothing left to clear";
  readings.store(HeatingFunction::BOOST_REMAINING, 40.0F);
  EXPECT_FALSE(clear_readings_read_only_while(HeatingFunction::TOWEL_MODE, readings)) << "no row depends on the mode";
  EXPECT_TRUE(readings.has(HeatingFunction::BOOST_REMAINING));
}
