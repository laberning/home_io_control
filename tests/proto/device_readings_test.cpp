/// @file device_readings_test.cpp
/// @brief Tests for the table of named device readings: its rows, the lookup of a value per source kind, the
/// registers a reading makes the poll read, and the conditional reads of a towel dryer's runs.

#include "proto_device_readings.h"
#include "proto_heating_profiles.h"
#include "reading_store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

using namespace esphome::home_io_control;

namespace {

/// A towel dryer whose temporary state has been read.
IoDevice readings_in_state(HeatingTemporaryState state) {
  IoDevice readings;
  test::store_reading(readings, HeatingFunction::TEMPORARY_STATE, static_cast<float>(state));
  return readings;
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
                                            .poll = ReadingPoll::WHEN_ASKED,
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

// === The reading store ===

TEST(DeviceReadingStore, AValueStoredAgainAfterBeingAbsentIsHeld) {
  DeviceReadingValues values;
  values.mark_absent(3);
  values.store(3, 7.0F);
  EXPECT_FALSE(values.absent(3));
  EXPECT_EQ(values.value(3), 7.0F);
  EXPECT_FALSE(values.value(DeviceReadingValues::SLOTS)) << "out of range reads as nothing";
}

TEST(DeviceReadingStore, ANaNIsAValueTheDeviceAnswered) {
  // A room-temperature sensor fault decodes as NaN: the heater did answer, so the store holds it.
  DeviceReadingValues values;
  EXPECT_FALSE(values.value(0).has_value());
  values.store(0, NAN);
  ASSERT_TRUE(values.value(0).has_value());
  EXPECT_TRUE(std::isnan(*values.value(0)));

  IoDevice dev;
  test::store_reading(dev, DeviceReading::ROOM_TEMPERATURE, NAN);
  EXPECT_EQ(reading_wire(dev, DeviceReading::ROOM_TEMPERATURE), -1) << "a NaN has no whole number";
}

TEST(DeviceReadingStore, ClearForgetsTheValueAndLeavesTheFlagsAlone) {
  DeviceReadingValues values;
  values.ask(5);
  values.store(5, 1.0F);
  values.clear(5);
  EXPECT_FALSE(values.value(5).has_value());
  EXPECT_TRUE(values.asked(5)) << "an entity still wants it";
  EXPECT_FALSE(values.absent(5)) << "a cleared reading is not an absent one";
  values.clear(DeviceReadingValues::SLOTS);  // out of range: nothing happens
}

TEST(DeviceReadingStore, AbsentDropsTheValueAndAskedSurvivesIt) {
  DeviceReadingValues values;
  values.ask(9);
  values.store(9, 4.0F);
  values.mark_absent(9);
  EXPECT_TRUE(values.absent(9));
  EXPECT_FALSE(values.value(9).has_value());
  EXPECT_TRUE(values.asked(9));
  EXPECT_FALSE(values.asked(10)) << "another slot is untouched";
}

TEST(DeviceReadingStore, HoldsEveryReadingOfTheTable) {
  // A reading past the store's last slot would silently keep nothing.
  for (const DeviceReadingRow &row : device_readings())
    EXPECT_LT(reading_slot(row.reading), DeviceReadingValues::SLOTS) << row.name;
}

TEST(DeviceReadingStore, ARegisterIsKeptAskedAndAbsentThroughItsReading) {
  IoDevice dev;
  EXPECT_FALSE(register_reading_wanted(dev, HeatingFunction::ENERGY_TOTAL));
  ask_register_reading(dev, HeatingFunction::ENERGY_TOTAL);
  EXPECT_TRUE(register_reading_wanted(dev, HeatingFunction::ENERGY_TOTAL));
  EXPECT_TRUE(store_register_reading(dev, HeatingFunction::ENERGY_TOTAL, 3422.0F));
  EXPECT_EQ(register_value(dev, HeatingFunction::ENERGY_TOTAL), 3422.0F);
  EXPECT_EQ(reading_value(dev, DeviceReading::ENERGY_TOTAL), 3422.0F) << "the register is the reading";

  mark_register_reading_absent(dev, HeatingFunction::ENERGY_TOTAL);
  EXPECT_FALSE(register_reading_wanted(dev, HeatingFunction::ENERGY_TOTAL)) << "no longer read";
  EXPECT_FALSE(register_value(dev, HeatingFunction::ENERGY_TOTAL).has_value());

  EXPECT_FALSE(store_register_reading(dev, HeatingFunction::POWER_ON, 1.0F)) << "a logged register keeps nothing";
  EXPECT_FALSE(clear_register_reading(dev, HeatingFunction::MIDNIGHT_SYNC));
  EXPECT_FALSE(register_value(dev, HeatingFunction::POWER_ON).has_value());
  EXPECT_EQ(register_wire(dev, HeatingFunction::POWER_ON), -1);
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
    // A status reading answers the status read and has no register; every other kind of the table that is polled
    // names registers.
    if (std::holds_alternative<StatusMainParameterSource>(row.source)) {
      EXPECT_TRUE(reading_polled_registers(row).empty()) << row.name;
      continue;
    }
    EXPECT_FALSE(reading_polled_registers(row).empty()) << row.name;
    for (const HeatingFunction fn : reading_polled_registers(row))
      EXPECT_TRUE(heating_function_is_readable(fn)) << row.name;
  }
}

TEST(DeviceReadings, TheRunTimersNeedTheRunTimersCapability) {
  for (const DeviceReading reading :
       {DeviceReading::BOOST_REMAINING, DeviceReading::BOOST_DURATION, DeviceReading::DRYING_REMAINING,
        DeviceReading::DRYING_DURATION, DeviceReading::REMAINING_TIME}) {
    EXPECT_EQ(find_device_reading(reading)->feature, std::optional(DeviceFeature::RUN_TIMERS))
        << static_cast<int>(reading);
  }
}

namespace {

/// A reply to a read of `fn` that carries the value 1 in the register's own byte layout.
IoFrame reply_with_one(HeatingFunction fn) {
  uint8_t low = 0;
  EXPECT_TRUE(heating_function_register(fn, low));
  IoFrame reply{};
  reply.cmd = CMD_WRITE_PRIVATE_ACK;
  const uint8_t data[] = {0x0C, 0x60, 0x01, low, 0x01, 0x00};
  std::copy(std::begin(data), std::end(data), reply.data);
  reply.data_len = sizeof(data);
  return reply;
}

}  // namespace

TEST(DeviceReadings, EveryKeptHeaterRegisterIsTheSourceOfExactlyOneRow) {
  // A register that decodes to a value is one the hub keeps, so it has a row; the registers that are logged, never
  // decoded, have none. ECO_SETBACK decodes but nothing reads it, so it is not kept.
  constexpr auto COUNT = static_cast<unsigned>(HeatingFunction::COUNT);
  for (unsigned i = 0; i < COUNT; i++) {
    const auto fn = static_cast<HeatingFunction>(i);
    ASSERT_STRNE(heating_function_name(fn), "unknown") << i << " has no descriptor row";
    float value = 0.0F;
    const bool decodes = heating_function_is_readable(fn) && decode_heating_reply(reply_with_one(fn), fn, value);
    size_t rows = 0;
    for (const DeviceReadingRow &row : device_readings()) {
      const auto *source = std::get_if<HeatingRegisterSource>(&row.source);
      rows += source != nullptr && source->fn == fn ? 1 : 0;
    }
    const bool kept = decodes && fn != HeatingFunction::ECO_SETBACK;
    EXPECT_EQ(rows, kept ? 1u : 0u) << heating_function_name(fn);
  }
}

TEST(DeviceReadings, ARegisterReadOnlyWhileAnotherHoldsAValueIsPolledAfterThatRegister) {
  // The poll reads the asked registers in the order of HeatingFunction, so a condition is judged on the value read
  // earlier in the same poll.
  for (const DeviceReadingRow &row : device_readings()) {
    const auto *source = std::get_if<HeatingRegisterSource>(&row.source);
    if (source == nullptr || !source->only_while)
      continue;
    EXPECT_LT(static_cast<unsigned>(source->only_while->fn), static_cast<unsigned>(source->fn)) << row.name;
  }
}

TEST(DeviceReadings, TheStatusReadingsHaveNoRegisterAndReadThroughTheStore) {
  for (const DeviceReading reading : {DeviceReading::HEATING_LEVEL, DeviceReading::STATUS_SETPOINT}) {
    const DeviceReadingRow *row = find_device_reading(reading);
    ASSERT_NE(row, nullptr);
    EXPECT_TRUE(std::holds_alternative<StatusMainParameterSource>(row->source)) << row->name;
    EXPECT_FALSE(row->feature.has_value()) << "no key of the YAML declares it";
    IoDevice dev;
    EXPECT_FALSE(reading_value(dev, reading).has_value()) << "unread";
    test::store_reading(dev, reading, 5.0F);
    EXPECT_EQ(reading_value(dev, reading), 5.0F);
  }
  EXPECT_FALSE(find_register_reading(HeatingFunction::SET_TEMPERATURE) == nullptr);
  EXPECT_EQ(find_register_reading(HeatingFunction::SET_TEMPERATURE)->reading, DeviceReading::SETPOINT)
      << "the comfort-setpoint register is a reading of its own, apart from the status setpoint";
}

TEST(DeviceReadings, AStatusMainParameterIsDecodedByItsForm) {
  const auto decode = [](DeviceReading reading, uint8_t high, uint8_t low) {
    const std::array<uint8_t, 2> bytes = {high, low};
    return decode_status_reading(reading, bytes, 0xFC, -40.0F, 80.0F);
  };
  EXPECT_EQ(decode(DeviceReading::HEATING_LEVEL, 0xFC, 0x02), 2.0F) << "the low byte under the prefix";
  EXPECT_FALSE(decode(DeviceReading::HEATING_LEVEL, 0xC8, 0x00)) << "another prefix is no level";
  // Half way over -40..80 is 20 degrees.
  const auto half = decode(DeviceReading::STATUS_SETPOINT, 0x64, 0x00);
  ASSERT_TRUE(half.has_value());
  EXPECT_NEAR(*half, 20.0F, 0.01F);
  EXPECT_FALSE(decode(DeviceReading::STATUS_SETPOINT, 0xD8, 0x00)) << "an alias is no temperature";
  EXPECT_FALSE(decode(DeviceReading::ROOM_TEMPERATURE, 0xFC, 0x02)) << "a register reading has no status form";
  const std::array<uint8_t, 1> short_bytes = {0xFC};
  EXPECT_FALSE(decode_status_reading(DeviceReading::HEATING_LEVEL, short_bytes, 0xFC, -40.0F, 80.0F));
}

TEST(DeviceReadings, EveryRowStatesWhenItIsPolledAndTheProfilesListsAgreeWithIt) {
  // The poll lists of the heating profiles and the cadence on the rows are two statements of one fact until the
  // poll reads the rows; they must agree.
  const auto poll_of = [](HeatingFunction fn) {
    const DeviceReadingRow *row = find_register_reading(fn);
    return row != nullptr ? std::optional(row->poll) : std::nullopt;
  };
  for (const HeatingProfile &profile : heating_profiles()) {
    for (const HeatingFunction fn : profile.poll_registers)
      EXPECT_EQ(poll_of(fn), std::optional(ReadingPoll::EVERY_POLL)) << heating_function_name(fn);
    for (const HeatingFunction fn : profile.extra_poll_registers)
      EXPECT_EQ(poll_of(fn), std::optional(ReadingPoll::EVERY_POLL)) << heating_function_name(fn);
    for (const HeatingFunction fn : profile.boot_registers)
      EXPECT_EQ(poll_of(fn), std::optional(ReadingPoll::ONCE_AFTER_BOOT)) << heating_function_name(fn);
  }
  for (const DeviceReadingRow &row : device_readings()) {
    if (row.poll == ReadingPoll::WHEN_ASKED) {
      EXPECT_TRUE(row.feature.has_value()) << row.name << ": an entity asks for it through a capability";
    }
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
  test::store_reading(dev, HeatingFunction::ENERGY_TOTAL, 40000.0F);
  test::store_reading(dev, HeatingFunction::CENTRAL_HEATING_MODE, 0.0F);
  test::store_reading(dev, HeatingFunction::SET_PRESENCE, 1.0F);
  EXPECT_FLOAT_EQ(*reading_value(dev, DeviceReading::ENERGY_TOTAL), 40000.0F) << "past what a flag byte holds";
  EXPECT_FLOAT_EQ(*reading_value(dev, DeviceReading::CENTRAL_HEATING_MODE), 0.0F) << "off, because the device said so";
  EXPECT_FLOAT_EQ(*reading_value(dev, DeviceReading::OCCUPANCY), 1.0F);
}

TEST(DeviceReadings, ADroppedReadingHasNoValueAgain) {
  IoDevice dev;
  test::store_reading(dev, HeatingFunction::ENERGY_TOTAL, 100.0F);
  test::clear_reading(dev, HeatingFunction::ENERGY_TOTAL);
  EXPECT_FALSE(reading_value(dev, DeviceReading::ENERGY_TOTAL).has_value());
}

TEST(DeviceReadings, TheRemainingTimeFollowsTheTemporaryState) {
  IoDevice readings = readings_in_state(HeatingTemporaryState::BOOST);
  test::store_reading(readings, HeatingFunction::BOOST_REMAINING, 55.0F);
  test::store_reading(readings, HeatingFunction::DRYING_REMAINING,
                      28.0F);  // an older drying run's value stays out of it
  EXPECT_FLOAT_EQ(*reading_value(readings, DeviceReading::REMAINING_TIME), 55.0F);

  test::store_reading(readings, HeatingFunction::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::DRYING));
  EXPECT_FLOAT_EQ(*reading_value(readings, DeviceReading::REMAINING_TIME), 28.0F);

  test::store_reading(readings, HeatingFunction::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::NONE));
  EXPECT_FLOAT_EQ(*reading_value(readings, DeviceReading::REMAINING_TIME), 0.0F)
      << "no run: 0, whatever an earlier run left";
}

TEST(DeviceReadings, TheRemainingTimeHasNoValueUntilItIsKnown) {
  const auto remaining = [](const IoDevice &readings) {
    return reading_value(readings, DeviceReading::REMAINING_TIME);
  };
  EXPECT_FALSE(remaining(IoDevice{}).has_value()) << "state not read";

  IoDevice boosting = readings_in_state(HeatingTemporaryState::BOOST);
  EXPECT_FALSE(remaining(boosting).has_value()) << "the run's register not read yet";
  test::store_reading(boosting, HeatingFunction::BOOST_REMAINING, 60.0F);
  EXPECT_FLOAT_EQ(*remaining(boosting), 60.0F);
  test::clear_reading(boosting, HeatingFunction::BOOST_REMAINING);
  EXPECT_FALSE(remaining(boosting).has_value()) << "a dropped reading is no value";
}

// === Registers read only while another holds a value ===

TEST(DeviceReadings, TheTimeLeftOfARunIsDueOnlyWhileThatRunIsOn) {
  const IoDevice idle = readings_in_state(HeatingTemporaryState::NONE);
  EXPECT_FALSE(heating_register_due(HeatingFunction::BOOST_REMAINING, idle));
  EXPECT_FALSE(heating_register_due(HeatingFunction::DRYING_REMAINING, idle));

  const IoDevice boosting = readings_in_state(HeatingTemporaryState::BOOST);
  EXPECT_TRUE(heating_register_due(HeatingFunction::BOOST_REMAINING, boosting));
  EXPECT_FALSE(heating_register_due(HeatingFunction::DRYING_REMAINING, boosting));

  const IoDevice drying_run = readings_in_state(HeatingTemporaryState::DRYING);
  EXPECT_FALSE(heating_register_due(HeatingFunction::BOOST_REMAINING, drying_run));
  EXPECT_TRUE(heating_register_due(HeatingFunction::DRYING_REMAINING, drying_run));

  const IoDevice unread;
  EXPECT_FALSE(heating_register_due(HeatingFunction::BOOST_REMAINING, unread)) << "an unread state is no run";
  EXPECT_FALSE(heating_register_due(HeatingFunction::DRYING_REMAINING, unread));
}

TEST(DeviceReadings, ARegisterWithoutAConditionIsAlwaysDue) {
  for (const HeatingFunction fn : {HeatingFunction::ENERGY_TOTAL, HeatingFunction::BOOST_DURATION,
                                   HeatingFunction::DRYING_DURATION, HeatingFunction::CENTRAL_HEATING_MODE}) {
    EXPECT_TRUE(heating_register_due(fn, IoDevice{})) << heating_function_name(fn);
  }
  EXPECT_TRUE(heating_register_due(HeatingFunction::ROOM_TEMPERATURE, IoDevice{}))
      << "a register no reading is has no condition";
}

TEST(DeviceReadings, AWrittenTemporaryStateForgetsTheTimeLeftOfBothRunsAndNothingElse) {
  IoDevice readings = readings_in_state(HeatingTemporaryState::BOOST);
  test::store_reading(readings, HeatingFunction::BOOST_REMAINING, 40.0F);
  test::store_reading(readings, HeatingFunction::DRYING_REMAINING, 12.0F);
  test::store_reading(readings, HeatingFunction::BOOST_DURATION, 60.0F);

  EXPECT_TRUE(clear_readings_read_only_while(HeatingFunction::TEMPORARY_STATE, readings));
  EXPECT_FALSE(test::has_reading(readings, HeatingFunction::BOOST_REMAINING));
  EXPECT_FALSE(test::has_reading(readings, HeatingFunction::DRYING_REMAINING));
  EXPECT_FLOAT_EQ(test::number_read(readings, HeatingFunction::BOOST_DURATION), 60.0F);
  EXPECT_EQ(test::wire_read(readings, HeatingFunction::TEMPORARY_STATE),
            static_cast<int16_t>(HeatingTemporaryState::BOOST));

  EXPECT_FALSE(clear_readings_read_only_while(HeatingFunction::TEMPORARY_STATE, readings)) << "nothing left to clear";
  test::store_reading(readings, HeatingFunction::BOOST_REMAINING, 40.0F);
  EXPECT_FALSE(clear_readings_read_only_while(HeatingFunction::TOWEL_MODE, readings)) << "no row depends on the mode";
  EXPECT_TRUE(test::has_reading(readings, HeatingFunction::BOOST_REMAINING));
}
