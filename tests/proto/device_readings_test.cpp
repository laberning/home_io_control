/// @file device_readings_test.cpp
/// @brief Tests for the table of named device readings: its rows, the lookup of a value per source kind, the
/// registers a reading makes the poll read, and the conditional reads of a towel dryer's runs.

#include "proto_device_readings.h"
#include "proto_heating_profiles.h"
#include "device_table_walk.h"
#include "reading_store.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace esphome::home_io_control;

namespace {

/// A towel dryer whose temporary state has been read.
IoDevice readings_in_state(HeatingTemporaryState state) {
  IoDevice readings;
  test::store_reading(readings, DeviceRegister::TEMPORARY_STATE, static_cast<float>(state));
  return readings;
}

}  // namespace

// === A data-object reading ===

namespace {

constexpr DataObjectSource COUNTER_SLICE{
    .recipe = 0, .offset = 0x10, .first = 2, .length = 3, .codec = ValueCodec::MARKED_U16_BE, .max_value = 0};
/// A test table of the tests' own, with a slice that does not start at the first byte.
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

TEST(DeviceReadings, TheHeatPumpEnergyRowsReadTheirEntriesOfTheAtlanticRecipe) {
  // Issue #244: the heating counter at offset 0x0023, the hot-water counter at 0x0025, both three bytes.
  const struct {
    DeviceReading reading;
    uint16_t offset;
  } cases[] = {{DeviceReading::HEAT_PUMP_HEATING_ENERGY, 0x0023}, {DeviceReading::HEAT_PUMP_HOT_WATER_ENERGY, 0x0025}};
  for (const auto &c : cases) {
    const DeviceReadingRow *row = find_device_reading(c.reading);
    ASSERT_NE(row, nullptr);
    SCOPED_TRACE(row->name);
    const auto *source = std::get_if<DataObjectSource>(&row->source);
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->recipe, TRANSFER_RECIPE_ATLANTIC_HEAT_PUMP);
    EXPECT_EQ(source->offset, c.offset);
    EXPECT_TRUE(transfer_recipe_allows_offset(TRANSFER_RECIPES[source->recipe], source->offset));
    EXPECT_EQ(row->feature, DeviceFeature::HEAT_PUMP_ENERGY);
    EXPECT_EQ(row->stale, ReadingStale::KEEP) << "a counter only grows";
    EXPECT_TRUE(reading_is_scheduled(*row));
  }
}

TEST(DeviceReadings, AHeatPumpCounterDecodesTheMarkedAnswerAndRefusesItsNoValuePattern) {
  const DeviceReadingRow *row = find_device_reading(DeviceReading::HEAT_PUMP_HOT_WATER_ENERGY);
  ASSERT_NE(row, nullptr);
  const auto &source = std::get<DataObjectSource>(row->source);
  // The TaHoma's read of the hot-water counter in issue #244.
  EXPECT_EQ(decode_data_object_reading(source, std::vector<uint8_t>{0x80, 0x24, 0xAC}), 9388.0F);
  EXPECT_FALSE(decode_data_object_reading(source, std::vector<uint8_t>{0x81, 0xFF, 0xFF})) << "the entry has no value";
}

TEST(DeviceReadings, TheHeatPumpOutdoorTemperatureRowReadsItsEntryAndDropsAStaleValue) {
  const DeviceReadingRow *row = find_device_reading(DeviceReading::HEAT_PUMP_OUTDOOR_TEMPERATURE);
  ASSERT_NE(row, nullptr);
  const auto *source = std::get_if<DataObjectSource>(&row->source);
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(source->recipe, TRANSFER_RECIPE_ATLANTIC_HEAT_PUMP);
  EXPECT_EQ(source->offset, 0x019B);
  EXPECT_TRUE(transfer_recipe_allows_offset(TRANSFER_RECIPES[source->recipe], source->offset));
  EXPECT_EQ(row->feature, DeviceFeature::HEAT_PUMP_OUTDOOR_TEMPERATURE);
  EXPECT_EQ(row->stale, ReadingStale::DROP) << "a measurement is not shown stale";
  EXPECT_FLOAT_EQ(*decode_data_object_reading(*source, std::vector<uint8_t>{0x80, 0x00, 0x77}), 11.9F);
  EXPECT_FALSE(decode_data_object_reading(*source, std::vector<uint8_t>{0x81, 0xFF, 0xFF})) << "no value";
}

TEST(DeviceReadings, AnAnyNonZeroSliceTheDataCutsOffIsNoValue) {
  // The any-non-zero form would decode the one byte that is there as a value; a cut-off slice is not read at all.
  constexpr DataObjectSource TWO_BYTE_FLAG{
      .recipe = 0, .offset = 0, .first = 1, .length = 2, .codec = ValueCodec::ANY_NONZERO, .max_value = 0};
  EXPECT_EQ(decode_data_object_reading(TWO_BYTE_FLAG, std::vector<uint8_t>{0x00, 0x00, 0x01}), 1.0F);
  EXPECT_FALSE(decode_data_object_reading(TWO_BYTE_FLAG, std::vector<uint8_t>{0x00, 0x01}));
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
  EXPECT_FALSE(register_reading_wanted(dev, DeviceRegister::ENERGY_TOTAL));
  ask_register_reading(dev, DeviceRegister::ENERGY_TOTAL);
  EXPECT_TRUE(register_reading_wanted(dev, DeviceRegister::ENERGY_TOTAL));
  store_reading(dev, DeviceReading::ENERGY_TOTAL, 3422.0F);
  EXPECT_EQ(register_value(dev, DeviceRegister::ENERGY_TOTAL), 3422.0F);
  EXPECT_EQ(reading_value(dev, DeviceReading::ENERGY_TOTAL), 3422.0F) << "the register is the reading";

  mark_register_reading_absent(dev, DeviceRegister::ENERGY_TOTAL);
  EXPECT_FALSE(register_reading_wanted(dev, DeviceRegister::ENERGY_TOTAL)) << "no longer read";
  EXPECT_FALSE(register_value(dev, DeviceRegister::ENERGY_TOTAL).has_value());

  EXPECT_FALSE(find_register_reading(DeviceRegister::PAIRED_DEVICES)) << "a logged register is no reading";
  EXPECT_FALSE(clear_register_reading(dev, DeviceRegister::SETPOINT_BLOCK));
  EXPECT_FALSE(register_value(dev, DeviceRegister::PAIRED_DEVICES).has_value());
  EXPECT_EQ(register_wire(dev, DeviceRegister::PAIRED_DEVICES), -1);
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

TEST(DeviceReadings, ARegisterIsOneReadingOrAllPartsAndThePartsDoNotOverlap) {
  // A register with one value of its own is one reading; a register that holds several facts is only ever read by
  // parts, each of its own bytes.
  std::set<DeviceRegister> whole;
  std::map<DeviceRegister, std::vector<RegisterPart>> parts;
  for (const DeviceReadingRow &row : device_readings()) {
    const auto *source = std::get_if<RegisterSource>(&row.source);
    if (source == nullptr)
      continue;
    if (source->part) {
      EXPECT_GT(source->part->length, 0) << row.name;
      parts[source->fn].push_back(*source->part);
    } else {
      EXPECT_TRUE(whole.insert(source->fn).second) << row.name << ": the register's value is read twice";
    }
  }
  for (const auto &[fn, slices] : parts) {
    EXPECT_FALSE(whole.count(fn)) << device_register_name(fn) << " is read whole and by parts";
    EXPECT_EQ(device_register_codec(fn), ValueCodec::NONE) << device_register_name(fn) << ": a part has its own codec";
    for (size_t i = 0; i < slices.size(); i++) {
      for (size_t j = i + 1; j < slices.size(); j++) {
        const bool overlap = slices[i].first < slices[j].first + slices[j].length &&
                             slices[j].first < slices[i].first + slices[i].length;
        EXPECT_FALSE(overlap) << device_register_name(fn);
      }
    }
  }
}

TEST(DeviceReadings, EveryRegisterOfAReadingIsReadable) {
  for (const DeviceReadingRow &row : device_readings()) {
    // A status reading answers the status read, a data-object, event or limitation reading is read on a schedule of its
    // own, and the battery flag is derived from a status reading: none has a register. Every other kind of the table
    // names registers.
    if (std::holds_alternative<DataObjectSource>(row.source) ||
        std::holds_alternative<StatusMainParameterSource>(row.source) ||
        std::holds_alternative<StatusFunctionalParameterSource>(row.source) ||
        std::holds_alternative<StatusBatterySource>(row.source) || std::holds_alternative<EventSource>(row.source) ||
        std::holds_alternative<LimitationSource>(row.source) || row.reading == DeviceReading::BATTERY_LOW) {
      EXPECT_TRUE(reading_polled_registers(row).empty()) << row.name;
      continue;
    }
    EXPECT_FALSE(reading_polled_registers(row).empty()) << row.name;
    for (const DeviceRegister fn : reading_polled_registers(row))
      EXPECT_TRUE(device_register_is_readable(fn)) << row.name;
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
IoFrame reply_with_one(DeviceRegister fn) {
  uint8_t high = 0;
  uint8_t low = 0;
  EXPECT_TRUE(device_register_bytes(fn, high, low));
  IoFrame reply{};
  reply.cmd = CMD_WRITE_PRIVATE_ACK;
  // Four value bytes: the widest value of a codec that is read in one piece.
  const uint8_t data[] = {0x0C, 0x60, high, low, 0x01, 0x00, 0x00, 0x00};
  std::copy(std::begin(data), std::end(data), reply.data);
  reply.data_len = sizeof(data);
  return reply;
}

}  // namespace

TEST(DeviceReadings, EveryKeptRegisterIsTheSourceOfExactlyOneRowOrOnlyOfParts) {
  // A register that decodes to a value is one the hub keeps, so it has a row; the registers that are logged, never
  // decoded, have none. ECO_SETBACK decodes but nothing reads it, so it is not kept. A register with no value of its
  // own that several readings are slices of (the water heater's heating state) has only rows with a part.
  constexpr auto COUNT = static_cast<unsigned>(DeviceRegister::COUNT);
  for (unsigned i = 0; i < COUNT; i++) {
    const auto fn = static_cast<DeviceRegister>(i);
    ASSERT_STRNE(device_register_name(fn), "unknown") << i << " has no descriptor row";
    float value = 0.0F;
    const bool decodes = device_register_is_readable(fn) && decode_heating_reply(reply_with_one(fn), fn, value);
    size_t whole_rows = 0;
    size_t part_rows = 0;
    for (const DeviceReadingRow &row : device_readings()) {
      const auto *source = std::get_if<RegisterSource>(&row.source);
      if (source == nullptr || source->fn != fn)
        continue;
      (source->part ? part_rows : whole_rows)++;
    }
    const bool kept = decodes && fn != DeviceRegister::ECO_SETBACK;
    EXPECT_EQ(whole_rows, kept ? 1u : 0u) << device_register_name(fn);
    if (part_rows > 0) {
      EXPECT_FALSE(decodes) << device_register_name(fn) << ": a register with parts has no value of its own";
    }
  }
}

namespace {

/// The phases of a heater's poll, in the order the controller runs them (HeaterController::poll_registers_()).
constexpr ReadingPoll POLL_PHASES[] = {ReadingPoll::EVERY_POLL, ReadingPoll::EVERY_POLL_MAY_MISS,
                                       ReadingPoll::WHEN_ASKED, ReadingPoll::ONCE_AFTER_BOOT};

/// @return The position of a register's row in the poll: its phase, then its row.
std::pair<size_t, size_t> poll_position(const DeviceReadingRow &row) {
  const size_t phase = static_cast<size_t>(std::ranges::find(POLL_PHASES, row.poll) - std::begin(POLL_PHASES));
  return {phase, static_cast<size_t>(&row - device_readings().data())};
}

}  // namespace

TEST(DeviceReadings, ARegisterReadOnlyWhileAnotherHoldsAValueIsPolledAfterThatRegister) {
  // A condition is judged on the value read earlier in the same poll.
  for (const DeviceReadingRow &row : device_readings()) {
    const auto *source = std::get_if<RegisterSource>(&row.source);
    if (source == nullptr || !source->only_while)
      continue;
    const DeviceReadingRow *condition = find_register_reading(source->only_while->fn);
    ASSERT_NE(condition, nullptr) << row.name;
    EXPECT_LT(poll_position(*condition), poll_position(row)) << row.name;
  }
}

TEST(DeviceReadings, TheAskedRowsAreInTheOrderOfTheRegisters) {
  // The asked registers go by the registers' enumeration so the wire order of an optional read does not depend on how
  // the YAML lists its entities. The other policies read in the order of the rows, which a row may choose freely;
  // the poll pins (HeaterPollPin) state that order register by register.
  std::optional<unsigned> previous;
  for (const DeviceReadingRow &row : device_readings()) {
    const auto *source = std::get_if<RegisterSource>(&row.source);
    if (source == nullptr || row.poll != ReadingPoll::WHEN_ASKED)
      continue;
    EXPECT_GT(static_cast<unsigned>(source->fn), previous.value_or(0)) << row.name;
    previous = static_cast<unsigned>(source->fn);
  }
}

TEST(DeviceReadings, TheHubKeepsEveryRegisterItReadsAndTheRowsThatReadThem) {
  // Literal counts, so a register the hub keeps or a row that reads one is dropped or added only as a decision and not
  // as a side effect. A register is kept when a row of the table reads it, whatever its codec decodes; the water
  // heater's heating-state register is read by three rows, one per part of its value.
  size_t rows = 0;
  std::set<DeviceRegister> kept;
  for (const DeviceReadingRow &row : device_readings()) {
    if (const auto *source = std::get_if<RegisterSource>(&row.source)) {
      rows++;
      kept.insert(source->fn);
    }
  }
  EXPECT_EQ(kept.size(), 30u) << "registers the hub keeps";
  EXPECT_EQ(rows, 32u) << "rows with a register source";
  EXPECT_EQ(std::ranges::count_if(device_readings(),
                                  [](const DeviceReadingRow &row) {
                                    const auto *source = std::get_if<RegisterSource>(&row.source);
                                    return source != nullptr && source->fn == DeviceRegister::DHW_HEATING;
                                  }),
            3)
      << "the heating-state register is behind three rows";
}

TEST(DeviceReadings, ADerivedReadingPollsOnlyRegistersThatAreRowsOfTheirOwnWithItsPolicyAndCapability) {
  for (const DeviceReadingRow &row : device_readings()) {
    if (!std::holds_alternative<DerivedSource>(row.source))
      continue;
    for (const DeviceRegister fn : reading_polled_registers(row)) {
      const DeviceReadingRow *own = find_register_reading(fn);
      ASSERT_NE(own, nullptr) << row.name << " needs " << device_register_name(fn) << " as a row";
      EXPECT_EQ(own->poll, row.poll) << row.name;
      EXPECT_EQ(own->feature, row.feature) << row.name;
    }
  }
}

TEST(DeviceReadings, AStatusReadingIsNeverPolledAndNoHeaterSelectsAReadingOfNotPolled) {
  for (const DeviceReadingRow &row : device_readings()) {
    if (std::holds_alternative<StatusMainParameterSource>(row.source)) {
      EXPECT_EQ(row.poll, ReadingPoll::NOT_POLLED) << row.name;
    }
  }
  // No heater poll selects a row of NOT_POLLED, whatever the heater is.
  for (const HeatingProfile &profile : heating_profiles()) {
    IoDevice dev;
    dev.type = profile.type;
    dev.subtype = profile.subtype;
    dev.model = profile.model;
    dev.features = device_features(profile.type, profile.subtype, profile.model);
    for (unsigned i = 0; i < static_cast<unsigned>(DeviceRegister::COUNT); i++)
      ask_register_reading(dev, static_cast<DeviceRegister>(i));
    EXPECT_TRUE(device_poll_reads(dev, ReadingPoll::NOT_POLLED).empty());
  }
}

TEST(DeviceReadings, EveryRegisterRowThatIsPolledIsPolledForSomeHeaterOrWaterHeater) {
  // A polled row no device applies to would be a read nothing makes: the capability and the profile rules must meet.
  std::set<DeviceRegister> polled;
  const auto collect = [&polled](DeviceType type, uint8_t subtype, DeviceModel model) {
    IoDevice dev;
    dev.type = type;
    dev.subtype = subtype;
    dev.model = model;
    dev.features = device_features(type, subtype, model);
    for (unsigned i = 0; i < static_cast<unsigned>(DeviceRegister::COUNT); i++)
      ask_register_reading(dev, static_cast<DeviceRegister>(i));
    for (const ReadingPoll policy : POLL_PHASES) {
      for (const DeviceRegister fn : device_poll_reads(dev, policy))
        polled.insert(fn);
    }
  };
  test::for_each_table_row([&](const DeviceTypeInfo &row) { collect(row.type, row.subtype, DeviceModel::NONE); });
  test::for_each_model_row([&](const DeviceTypeInfo &row) { collect(row.type, row.subtype, row.model); });
  for (const DeviceReadingRow &row : device_readings()) {
    const auto *source = std::get_if<RegisterSource>(&row.source);
    if (source != nullptr && row.poll != ReadingPoll::NOT_POLLED) {
      EXPECT_TRUE(polled.count(source->fn)) << row.name << " is read by no device";
    }
  }
}

TEST(DeviceReadings, AReadingNotPolledIsNotSelectedForAnyHeaterEvenWhenAskedFor) {
  IoDevice dev;
  test::set_kind(dev, DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::TOWEL_DRYER);
  ask_register_reading(dev, DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE);
  for (const ReadingPoll policy : POLL_PHASES) {
    const HeaterPollList list = device_poll_reads(dev, policy);
    EXPECT_EQ(std::count(list.begin(), list.end(), DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE), 0);
  }
}

TEST(DeviceReadings, TheStatusReadingsHaveNoRegisterAndReadThroughTheStore) {
  for (const DeviceReading reading : {DeviceReading::HEATING_LEVEL, DeviceReading::STATUS_SETPOINT}) {
    const DeviceReadingRow *row = find_device_reading(reading);
    ASSERT_NE(row, nullptr);
    EXPECT_TRUE(std::holds_alternative<StatusMainParameterSource>(row->source)) << row->name;
    // The heating level is declared by a key of the YAML and needs the capability of the heaters that report it.
    EXPECT_EQ(row->feature.has_value(), reading == DeviceReading::HEATING_LEVEL) << row->name;
    IoDevice dev;
    EXPECT_FALSE(reading_value(dev, reading).has_value()) << "unread";
    test::store_reading(dev, reading, 5.0F);
    EXPECT_EQ(reading_value(dev, reading), 5.0F);
  }
  EXPECT_FALSE(find_register_reading(DeviceRegister::COMFORT_SETPOINT) == nullptr);
  EXPECT_EQ(find_register_reading(DeviceRegister::COMFORT_SETPOINT)->reading, DeviceReading::SETPOINT)
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

TEST(DeviceReadings, ARowNamesItsCapabilityWhereAnEntityAsksForItAndElseTheProfileReadsIt) {
  // WHEN_ASKED needs an entity to ask, and an entity asks through a capability.
  for (const DeviceReadingRow &row : device_readings()) {
    if (row.poll == ReadingPoll::WHEN_ASKED) {
      EXPECT_TRUE(row.feature.has_value()) << row.name << ": an entity asks for it through a capability";
    }
  }
}

// === What kind of value a reading is ===

TEST(DeviceReadings, OnlyAFlagReadingLogsNoUnchangedLine) {
  // Rain is polled often and rarely changes; sun level and battery say something even when they repeat.
  const auto logs = [](DeviceReading reading) { return reading_logs_unchanged(*find_device_reading(reading)); };
  EXPECT_FALSE(logs(DeviceReading::RAIN));
  EXPECT_FALSE(logs(DeviceReading::BATTERY_LOW)) << "a derived flag";
  EXPECT_TRUE(logs(DeviceReading::SUN_LEVEL));
  EXPECT_TRUE(logs(DeviceReading::BATTERY_LEVEL));
  EXPECT_TRUE(logs(DeviceReading::BATTERY_STATE)) << "a named value";
}

TEST(DeviceReadings, AReadingsValueKindFollowsItsCodecOrNames) {
  for (const DeviceReadingRow &row : device_readings()) {
    if (row.value_name != nullptr) {
      EXPECT_EQ(reading_value_kind(row), ReadingValueKind::NAMED) << row.name;
      continue;
    }
    const auto *source = std::get_if<RegisterSource>(&row.source);
    const auto *parameter = std::get_if<StatusFunctionalParameterSource>(&row.source);
    const auto *derived = std::get_if<DerivedSource>(&row.source);
    // A flag is a register read as a flag, a part of a register read as one, a field of a status parameter, a derived
    // reading that says so, or the answer to a limitation read (the rain flag).
    const ValueCodec codec = source == nullptr ? ValueCodec::NONE
                             : source->part    ? source->part->codec
                                               : device_register_codec(source->fn);
    const bool flag = codec == ValueCodec::BINARY || codec == ValueCodec::ANY_NONZERO ||
                      (parameter != nullptr && parameter->form == StatusParameterForm::FIELD_ON) ||
                      (derived != nullptr && derived->codec == ValueCodec::BINARY) ||
                      std::holds_alternative<LimitationSource>(row.source);
    EXPECT_EQ(reading_value_kind(row), flag ? ReadingValueKind::FLAG : ReadingValueKind::NUMBER) << row.name;
  }
  // The values the entities rely on, by name.
  const auto kind = [](DeviceReading reading) { return reading_value_kind(*find_device_reading(reading)); };
  EXPECT_EQ(kind(DeviceReading::WINDOW_OPEN), ReadingValueKind::FLAG);
  EXPECT_EQ(kind(DeviceReading::OPEN_WINDOW_DETECTION), ReadingValueKind::FLAG);
  EXPECT_EQ(kind(DeviceReading::HEATING_LEVEL), ReadingValueKind::NAMED);
  EXPECT_EQ(kind(DeviceReading::ENERGY_TOTAL), ReadingValueKind::NUMBER);
  EXPECT_EQ(kind(DeviceReading::REMAINING_TIME), ReadingValueKind::NUMBER);
  EXPECT_EQ(kind(DeviceReading::HOT_WATER_TEMPERATURE), ReadingValueKind::NUMBER);
  EXPECT_EQ(kind(DeviceReading::HOT_WATER_MODE), ReadingValueKind::NAMED);
  EXPECT_EQ(kind(DeviceReading::HOT_WATER_HEAT_PUMP), ReadingValueKind::FLAG) << "a part read as a flag";
  EXPECT_EQ(kind(DeviceReading::HOT_WATER_BOOST), ReadingValueKind::FLAG) << "a field of a status parameter";
  EXPECT_EQ(kind(DeviceReading::HOT_WATER_CAPABILITIES), ReadingValueKind::NUMBER) << "the whole word";
  EXPECT_EQ(kind(DeviceReading::RAIN), ReadingValueKind::FLAG) << "the answer to a limitation read";
  EXPECT_EQ(kind(DeviceReading::SUN_LEVEL), ReadingValueKind::NUMBER) << "an event's value";
  EXPECT_EQ(kind(DeviceReading::BATTERY_LEVEL), ReadingValueKind::NUMBER);
  EXPECT_EQ(kind(DeviceReading::BATTERY_STATE), ReadingValueKind::NAMED);
  EXPECT_EQ(kind(DeviceReading::BATTERY_LOW), ReadingValueKind::FLAG) << "a derived flag";
}

TEST(DeviceReadings, ADataObjectReadingOfAFlagCodecIsAFlag) {
  const DeviceReadingRow row = {
      .name = "flag",
      .source =
          DataObjectSource{
              .recipe = 0, .offset = 0, .first = 0, .length = 1, .codec = ValueCodec::BINARY, .max_value = 0},
      .reading = DeviceReading::COUNT,
      .poll = ReadingPoll::NOT_POLLED};
  EXPECT_EQ(reading_value_kind(row), ReadingValueKind::FLAG);
}

TEST(DeviceReadings, OnlyTheHeatingLevelTheWaterHeaterModeAndTheBatteryStateHaveValueNames) {
  for (const DeviceReadingRow &row : device_readings()) {
    EXPECT_EQ(row.value_name != nullptr, row.reading == DeviceReading::HEATING_LEVEL ||
                                             row.reading == DeviceReading::HOT_WATER_MODE ||
                                             row.reading == DeviceReading::BATTERY_STATE)
        << row.name;
  }
  const DeviceReadingRow *battery = find_device_reading(DeviceReading::BATTERY_STATE);
  ASSERT_NE(battery, nullptr);
  EXPECT_STREQ(battery->value_name(static_cast<uint8_t>(BatteryState::LOW_CHARGE)), "low");
  EXPECT_STREQ(battery->value_name(static_cast<uint8_t>(BatteryState::NORMAL_CHARGE)), "normal");
  EXPECT_STREQ(battery->value_name(static_cast<uint8_t>(BatteryState::FULL_CHARGE)), "full");
  EXPECT_EQ(battery->value_name(static_cast<uint8_t>(BatteryState::NOT_REPORTED)), nullptr)
      << "no battery is an absent reading, never a stored name";
  EXPECT_EQ(battery->value_name(4), nullptr);
  EXPECT_STREQ(find_device_reading(DeviceReading::HOT_WATER_MODE)->value_name(0), "auto");
  const DeviceReadingRow *level = find_device_reading(DeviceReading::HEATING_LEVEL);
  ASSERT_NE(level, nullptr);
  EXPECT_STREQ(level->value_name(heating_level::COMFORT), "comfort");
  EXPECT_EQ(level->value_name(0x40), nullptr) << "a value of no level has no name";
  EXPECT_EQ(level->feature, DeviceFeature::REPORTS_HEATING_LEVEL);
}

// === The registers a reading makes the poll read ===

TEST(DeviceReadings, ARegisterReadingPollsItsRegisterAndADerivedOneThoseItNames) {
  const auto polled = [](DeviceReading reading) {
    const auto registers = reading_polled_registers(*find_device_reading(reading));
    return std::vector<DeviceRegister>(registers.begin(), registers.end());
  };
  EXPECT_EQ(polled(DeviceReading::ENERGY_TOTAL), std::vector<DeviceRegister>{DeviceRegister::ENERGY_TOTAL});
  EXPECT_EQ(polled(DeviceReading::OCCUPANCY), std::vector<DeviceRegister>{DeviceRegister::PRESENCE})
      << "the reading's name is not its register's";
  EXPECT_EQ(polled(DeviceReading::REMAINING_TIME),
            (std::vector<DeviceRegister>{DeviceRegister::BOOST_REMAINING, DeviceRegister::DRYING_REMAINING}));
}

TEST(DeviceReadings, ASettingIsTheRegisterItWrites) {
  EXPECT_EQ(reading_register(DeviceReading::OCCUPANCY_DETECTION), DeviceRegister::OCCUPANCY_DETECTION);
  EXPECT_EQ(reading_register(DeviceReading::OPEN_WINDOW_DETECTION), DeviceRegister::OPEN_WINDOW_DETECTION);
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
  test::store_reading(dev, DeviceRegister::ENERGY_TOTAL, 40000.0F);
  test::store_reading(dev, DeviceRegister::CENTRAL_HEATING_MODE, 0.0F);
  test::store_reading(dev, DeviceRegister::PRESENCE, 1.0F);
  EXPECT_FLOAT_EQ(*reading_value(dev, DeviceReading::ENERGY_TOTAL), 40000.0F) << "past what a flag byte holds";
  EXPECT_FLOAT_EQ(*reading_value(dev, DeviceReading::CENTRAL_HEATING_MODE), 0.0F) << "off, because the device said so";
  EXPECT_FLOAT_EQ(*reading_value(dev, DeviceReading::OCCUPANCY), 1.0F);
}

TEST(DeviceReadings, ADroppedReadingHasNoValueAgain) {
  IoDevice dev;
  test::store_reading(dev, DeviceRegister::ENERGY_TOTAL, 100.0F);
  test::clear_reading(dev, DeviceRegister::ENERGY_TOTAL);
  EXPECT_FALSE(reading_value(dev, DeviceReading::ENERGY_TOTAL).has_value());
}

TEST(DeviceReadings, TheRemainingTimeFollowsTheTemporaryState) {
  IoDevice readings = readings_in_state(HeatingTemporaryState::BOOST);
  test::store_reading(readings, DeviceRegister::BOOST_REMAINING, 55.0F);
  test::store_reading(readings, DeviceRegister::DRYING_REMAINING,
                      28.0F);  // an older drying run's value stays out of it
  EXPECT_FLOAT_EQ(*reading_value(readings, DeviceReading::REMAINING_TIME), 55.0F);

  test::store_reading(readings, DeviceRegister::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::DRYING));
  EXPECT_FLOAT_EQ(*reading_value(readings, DeviceReading::REMAINING_TIME), 28.0F);

  test::store_reading(readings, DeviceRegister::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::NONE));
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
  test::store_reading(boosting, DeviceRegister::BOOST_REMAINING, 60.0F);
  EXPECT_FLOAT_EQ(*remaining(boosting), 60.0F);
  test::clear_reading(boosting, DeviceRegister::BOOST_REMAINING);
  EXPECT_FALSE(remaining(boosting).has_value()) << "a dropped reading is no value";
}

// === Registers read only while another holds a value ===

TEST(DeviceReadings, TheTimeLeftOfARunIsDueOnlyWhileThatRunIsOn) {
  const IoDevice idle = readings_in_state(HeatingTemporaryState::NONE);
  EXPECT_FALSE(heating_register_due(DeviceRegister::BOOST_REMAINING, idle));
  EXPECT_FALSE(heating_register_due(DeviceRegister::DRYING_REMAINING, idle));

  const IoDevice boosting = readings_in_state(HeatingTemporaryState::BOOST);
  EXPECT_TRUE(heating_register_due(DeviceRegister::BOOST_REMAINING, boosting));
  EXPECT_FALSE(heating_register_due(DeviceRegister::DRYING_REMAINING, boosting));

  const IoDevice drying_run = readings_in_state(HeatingTemporaryState::DRYING);
  EXPECT_FALSE(heating_register_due(DeviceRegister::BOOST_REMAINING, drying_run));
  EXPECT_TRUE(heating_register_due(DeviceRegister::DRYING_REMAINING, drying_run));

  const IoDevice unread;
  EXPECT_FALSE(heating_register_due(DeviceRegister::BOOST_REMAINING, unread)) << "an unread state is no run";
  EXPECT_FALSE(heating_register_due(DeviceRegister::DRYING_REMAINING, unread));
}

TEST(DeviceReadings, ARegisterWithoutAConditionIsAlwaysDue) {
  for (const DeviceRegister fn : {DeviceRegister::ENERGY_TOTAL, DeviceRegister::BOOST_DURATION,
                                  DeviceRegister::DRYING_DURATION, DeviceRegister::CENTRAL_HEATING_MODE}) {
    EXPECT_TRUE(heating_register_due(fn, IoDevice{})) << device_register_name(fn);
  }
  EXPECT_TRUE(heating_register_due(DeviceRegister::ROOM_TEMPERATURE, IoDevice{})) << "a row without a condition";
}

TEST(DeviceReadings, ARegisterWithoutAReadingRowIsAlwaysDue) {
  // The registers the heater poll reads for their own sake (a model name, the heating level) carry no reading row.
  std::optional<DeviceRegister> without_row;
  for (unsigned i = 0; i < static_cast<unsigned>(DeviceRegister::COUNT); ++i) {
    if (find_register_reading(static_cast<DeviceRegister>(i)) == nullptr)
      without_row = static_cast<DeviceRegister>(i);
  }
  ASSERT_TRUE(without_row.has_value()) << "some register is no reading";
  EXPECT_TRUE(heating_register_due(*without_row, IoDevice{})) << device_register_name(*without_row);
}

TEST(DeviceReadings, AWrittenTemporaryStateForgetsTheTimeLeftOfBothRunsAndNothingElse) {
  IoDevice readings = readings_in_state(HeatingTemporaryState::BOOST);
  test::store_reading(readings, DeviceRegister::BOOST_REMAINING, 40.0F);
  test::store_reading(readings, DeviceRegister::DRYING_REMAINING, 12.0F);
  test::store_reading(readings, DeviceRegister::BOOST_DURATION, 60.0F);

  EXPECT_TRUE(clear_readings_read_only_while(DeviceRegister::TEMPORARY_STATE, readings));
  EXPECT_FALSE(test::has_reading(readings, DeviceRegister::BOOST_REMAINING));
  EXPECT_FALSE(test::has_reading(readings, DeviceRegister::DRYING_REMAINING));
  EXPECT_FLOAT_EQ(test::number_read(readings, DeviceRegister::BOOST_DURATION), 60.0F);
  EXPECT_EQ(test::wire_read(readings, DeviceRegister::TEMPORARY_STATE),
            static_cast<int16_t>(HeatingTemporaryState::BOOST));

  EXPECT_FALSE(clear_readings_read_only_while(DeviceRegister::TEMPORARY_STATE, readings)) << "nothing left to clear";
  test::store_reading(readings, DeviceRegister::BOOST_REMAINING, 40.0F);
  EXPECT_FALSE(clear_readings_read_only_while(DeviceRegister::TOWEL_MODE, readings)) << "no row depends on the mode";
  EXPECT_TRUE(test::has_reading(readings, DeviceRegister::BOOST_REMAINING));
}

// === The water heater's rows, selected by capability ===

TEST(DeviceReadings, AWaterHeaterIsPolledByItsRowsInTableOrderWithTheHeatingRegisterOnce) {
  const IoDevice dev = test::water_heater_record();
  ASSERT_TRUE(has_feature(dev.features, device_feature::WATER_HEATER));
  EXPECT_EQ(test::polled_registers(dev, ReadingPoll::EVERY_POLL),
            (std::vector<DeviceRegister>{DeviceRegister::DHW_WATER_TEMPERATURE, DeviceRegister::DHW_MODE,
                                         DeviceRegister::DHW_HEATING, DeviceRegister::DHW_BOOST_DURATION,
                                         DeviceRegister::DHW_AWAY_DURATION, DeviceRegister::DHW_ENERGY}))
      << "three readings are slices of the heating register: one read";
  EXPECT_EQ(test::polled_registers(dev, ReadingPoll::ONCE_AFTER_BOOT),
            (std::vector<DeviceRegister>{DeviceRegister::DHW_BRAND}));
  EXPECT_TRUE(test::polled_registers(dev, ReadingPoll::EVERY_POLL_MAY_MISS).empty());
  EXPECT_TRUE(test::polled_registers(dev, ReadingPoll::WHEN_ASKED).empty());
  EXPECT_TRUE(test::polled_registers(dev, ReadingPoll::NOT_POLLED).empty())
      << "the status readings answer the status read";
}

TEST(DeviceReadings, TheBrandIsReadUntilItHoldsAValueAndNoLonger) {
  IoDevice dev = test::water_heater_record();
  ASSERT_EQ(test::polled_registers(dev, ReadingPoll::ONCE_AFTER_BOOT).size(), 1u);
  test::store_reading(dev, DeviceRegister::DHW_BRAND, 65);
  EXPECT_TRUE(test::polled_registers(dev, ReadingPoll::ONCE_AFTER_BOOT).empty());
}

TEST(DeviceReadings, ADeviceWithoutAHeatingProfileOrTheWaterHeaterCapabilityIsPolledForNoRegister) {
  for (const DeviceType type : {DeviceType::ROLLER_SHUTTER, DeviceType::LIGHT, DeviceType::DOMESTIC_HOT_WATER}) {
    IoDevice dev;
    // The water heater's kind declared on another platform has no capability: it is not a water heater.
    dev.entity_class = DeviceCapabilityClass::SWITCH;
    test::set_kind(dev, type);
    for (const ReadingPoll policy : {ReadingPoll::EVERY_POLL, ReadingPoll::EVERY_POLL_MAY_MISS, ReadingPoll::WHEN_ASKED,
                                     ReadingPoll::ONCE_AFTER_BOOT})
      EXPECT_TRUE(test::polled_registers(dev, policy).empty()) << static_cast<int>(type);
  }
}

TEST(DeviceReadings, AWaterHeaterReplyStoresItsReadingsAndAnEmptyOrWrongOneNone) {
  IoDevice dev = test::water_heater_record();
  EXPECT_TRUE(store_register_reply(dev, DeviceRegister::DHW_MODE, std::vector<uint8_t>{0x02}));
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_MODE), 2);
  EXPECT_FALSE(store_register_reply(dev, DeviceRegister::DHW_MODE, std::vector<uint8_t>{0x07}))
      << "a mode the register does not define";
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_MODE), 2) << "the reading keeps its value";
  EXPECT_FALSE(store_register_reply(dev, DeviceRegister::DHW_MODE, std::vector<uint8_t>{}));
  EXPECT_FALSE(store_register_reply(dev, DeviceRegister::PAIRED_DEVICES, std::vector<uint8_t>{0x01}))
      << "a logged register";
}

TEST(DeviceReadings, ARegisterThatIsReadByPartsIsAskedClearedAndMarkedAbsentAsAWhole) {
  IoDevice dev = test::water_heater_record();
  ASSERT_TRUE(store_register_reply(dev, DeviceRegister::DHW_HEATING,
                                   std::vector<uint8_t>{0x01, 0xFF, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00}));
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_HEAT_PUMP), 1);
  EXPECT_TRUE(clear_register_reading(dev, DeviceRegister::DHW_HEATING));
  for (const DeviceReading reading :
       {DeviceReading::HOT_WATER_HEATING, DeviceReading::HOT_WATER_ELECTRIC_BACKUP, DeviceReading::HOT_WATER_HEAT_PUMP})
    EXPECT_EQ(reading_wire(dev, reading), -1);
  ask_register_reading(dev, DeviceRegister::DHW_HEATING);
  EXPECT_TRUE(register_reading_wanted(dev, DeviceRegister::DHW_HEATING));
  mark_register_reading_absent(dev, DeviceRegister::DHW_HEATING);
  EXPECT_FALSE(register_reading_wanted(dev, DeviceRegister::DHW_HEATING));
  EXPECT_TRUE(dev.reading_values.absent(reading_slot(DeviceReading::HOT_WATER_HEAT_PUMP)));
  EXPECT_TRUE(dev.reading_values.absent(reading_slot(DeviceReading::HOT_WATER_HEATING)));
}

TEST(DeviceReadings, EveryStatusSetpointIsRoundedToTenthsOfADegree) {
  // 26027 steps of the 51200-step scale over -40..80 is 21.00078 degrees: the scale resolves finer than a tenth, and
  // the reading shows the tenth.
  const uint8_t bytes[] = {0x65, 0xAB};
  ASSERT_EQ((bytes[0] << 8) | bytes[1], 26027);
  const auto setpoint = decode_status_reading(DeviceReading::STATUS_SETPOINT, bytes, 0, -40.0F, 80.0F);
  ASSERT_TRUE(setpoint.has_value());
  EXPECT_EQ(*setpoint, 21.0F);
  // A main parameter beyond the scale is no temperature, and the level reading is not rounded: it is not a temperature.
  const uint8_t alias[] = {0xD8, 0x12};
  EXPECT_FALSE(decode_status_reading(DeviceReading::STATUS_SETPOINT, alias, 0, -40.0F, 80.0F).has_value());
}

// === A cover's readings ===

TEST(DeviceReadings, ACoversReadingsNeedNoCapabilityAndTheScheduledOnesDropTheirValueWhenStale) {
  IoDevice cover{};
  cover.type = DeviceType::ROLLER_SHUTTER;
  resolve_device_features(cover);
  for (const DeviceReading reading : {DeviceReading::RAIN, DeviceReading::SUN_LEVEL, DeviceReading::BATTERY_LEVEL,
                                      DeviceReading::BATTERY_STATE, DeviceReading::BATTERY_LOW}) {
    const DeviceReadingRow *row = find_device_reading(reading);
    ASSERT_NE(row, nullptr);
    EXPECT_FALSE(row->feature.has_value()) << row->name << ": a YAML key of the cover declares it";
    EXPECT_TRUE(device_can_have_reading(cover, *row)) << row->name;
    EXPECT_EQ(row->poll, ReadingPoll::NOT_POLLED) << row->name << ": no heater poll reads it";
  }
  for (const DeviceReading reading : {DeviceReading::RAIN, DeviceReading::SUN_LEVEL, DeviceReading::BATTERY_LEVEL}) {
    const DeviceReadingRow *row = find_device_reading(reading);
    EXPECT_TRUE(reading_is_scheduled(*row)) << row->name;
    EXPECT_EQ(row->stale, ReadingStale::DROP) << row->name << ": a state that changes is not kept past the misses";
  }
  for (const DeviceReading reading : {DeviceReading::BATTERY_STATE, DeviceReading::BATTERY_LOW})
    EXPECT_FALSE(reading_is_scheduled(*find_device_reading(reading))) << "a status reply answers it";
}

TEST(DeviceReadings, OnlyAnEventOrLimitationReadingIsScheduledWithoutACapability) {
  // device_can_have_reading() lets a row without a capability onto any device: that is right only for a cover's
  // readings, which a cover's own YAML key declares. A data-object reading needs its capability, so a row that forgot
  // it would be read on every device that asks.
  for (const DeviceReadingRow &row : device_readings()) {
    if (reading_is_scheduled(row) && !row.feature.has_value()) {
      EXPECT_TRUE(std::holds_alternative<EventSource>(row.source) ||
                  std::holds_alternative<LimitationSource>(row.source))
          << row.name;
    }
  }
}

TEST(DeviceReadings, ARowThatNamesACapabilityStaysOutOfADeviceThatLacksIt) {
  IoDevice cover{};
  cover.type = DeviceType::ROLLER_SHUTTER;
  resolve_device_features(cover);
  const DeviceReadingRow *energy = find_device_reading(DeviceReading::ENERGY_TOTAL);
  ASSERT_NE(energy, nullptr);
  EXPECT_FALSE(device_can_have_reading(cover, *energy));
}

TEST(DeviceReadings, TheBatteryFlagIsOnForALowBatteryOffForOtherLevelsAndNoneWithoutALevel) {
  IoDevice dev{};
  const size_t slot = reading_slot(DeviceReading::BATTERY_STATE);
  EXPECT_FALSE(reading_value(dev, DeviceReading::BATTERY_LOW).has_value()) << "nothing read yet";
  EXPECT_EQ(battery_state_of(dev), BatteryState::NOT_REPORTED);

  dev.reading_values.store(slot, static_cast<float>(BatteryState::LOW_CHARGE));
  EXPECT_EQ(reading_value(dev, DeviceReading::BATTERY_LOW), std::optional<float>(1.0F));
  EXPECT_EQ(battery_state_of(dev), BatteryState::LOW_CHARGE);
  dev.reading_values.store(slot, static_cast<float>(BatteryState::NORMAL_CHARGE));
  EXPECT_EQ(reading_value(dev, DeviceReading::BATTERY_LOW), std::optional<float>(0.0F));
  dev.reading_values.store(slot, static_cast<float>(BatteryState::FULL_CHARGE));
  EXPECT_EQ(reading_value(dev, DeviceReading::BATTERY_LOW), std::optional<float>(0.0F));

  dev.reading_values.mark_absent(slot);
  EXPECT_FALSE(reading_value(dev, DeviceReading::BATTERY_LOW).has_value()) << "off must mean a device said so";
  EXPECT_EQ(battery_state_of(dev), BatteryState::NOT_REPORTED);
}

TEST(DeviceReadings, TheSunLevelRowAppliesTheEstimatedLuxFactorWhenItDecodes) {
  const DeviceReadingRow *row = find_device_reading(DeviceReading::SUN_LEVEL);
  ASSERT_NE(row, nullptr);
  const auto *source = std::get_if<EventSource>(&row->source);
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(source->event, EVENT_LOCAL_SENSOR);
  EXPECT_EQ(source->factor, 100) << "one step of the device's value is 100 lux";
  EXPECT_EQ(source->type_byte, std::optional<uint8_t>(LOCAL_SENSOR_TYPE_SUN));
}

// === The store door ===

TEST(DeviceReadingStore, TheDoorStoresClearsAndMarksAbsentByReading) {
  IoDevice dev;
  mark_reading_absent(dev, DeviceReading::ENERGY_TOTAL);
  EXPECT_TRUE(reading_absent(dev, DeviceReading::ENERGY_TOTAL));
  store_reading(dev, DeviceReading::ENERGY_TOTAL, 12.0F);
  EXPECT_FALSE(reading_absent(dev, DeviceReading::ENERGY_TOTAL)) << "a stored value lifts the absent mark";
  EXPECT_EQ(reading_value(dev, DeviceReading::ENERGY_TOTAL), 12.0F);
  clear_reading(dev, DeviceReading::ENERGY_TOTAL);
  EXPECT_FALSE(reading_value(dev, DeviceReading::ENERGY_TOTAL).has_value());
  mark_reading_absent(dev, DeviceReading::ENERGY_TOTAL);
  EXPECT_TRUE(reading_absent(dev, DeviceReading::ENERGY_TOTAL));
  EXPECT_FALSE(reading_asked(dev, DeviceReading::BATTERY_STATE));
  ask_reading(dev, DeviceReading::BATTERY_STATE);
  EXPECT_TRUE(reading_asked(dev, DeviceReading::BATTERY_STATE));
}

TEST(DeviceReadingStore, AValueStaysReportedThroughAClearAndAnAbsentMarkUntilTheNextBoot) {
  IoDevice dev;
  EXPECT_FALSE(reading_reported(dev, DeviceReading::ENERGY_TOTAL));
  store_reading(dev, DeviceReading::ENERGY_TOTAL, 1.0F);
  EXPECT_TRUE(reading_reported(dev, DeviceReading::ENERGY_TOTAL));
  clear_reading(dev, DeviceReading::ENERGY_TOTAL);
  EXPECT_TRUE(reading_reported(dev, DeviceReading::ENERGY_TOTAL)) << "a clear drops the value, not the fact";
  mark_reading_absent(dev, DeviceReading::ENERGY_TOTAL);
  EXPECT_TRUE(reading_reported(dev, DeviceReading::ENERGY_TOTAL));
  EXPECT_FALSE(reading_reported(dev, DeviceReading::OCCUPANCY)) << "per reading";
  DeviceReadingValues values;
  EXPECT_FALSE(values.reported(DeviceReadingValues::SLOTS)) << "out of range reads as nothing";
}

TEST(DeviceReadingStore, AMainParameterIsStoredOnlyWhenItIsAValueOfTheReading) {
  IoDevice dev;
  const std::vector<uint8_t> level{HEATING_LEVEL_MP_PREFIX, heating_level::ECO};
  const StatusStore first =
      store_status_main_parameter(dev, DeviceReading::HEATING_LEVEL, level, HEATING_LEVEL_MP_PREFIX);
  ASSERT_TRUE(first.value.has_value());
  EXPECT_TRUE(first.changed);
  EXPECT_EQ(test::wire_read(dev, DeviceReading::HEATING_LEVEL), heating_level::ECO);
  EXPECT_FALSE(store_status_main_parameter(dev, DeviceReading::HEATING_LEVEL, level, HEATING_LEVEL_MP_PREFIX).changed)
      << "the same value again is no change";
  const std::vector<uint8_t> other_prefix{0x12, 0x34};
  const StatusStore none =
      store_status_main_parameter(dev, DeviceReading::HEATING_LEVEL, other_prefix, HEATING_LEVEL_MP_PREFIX);
  EXPECT_FALSE(none.value.has_value());
  EXPECT_EQ(test::wire_read(dev, DeviceReading::HEATING_LEVEL), heating_level::ECO)
      << "a main parameter that is none of the reading leaves it to the caller";
}

// === The card role of a reading ===

TEST(DeviceReadings, OnlyTheRoomTemperatureAndTheSetpointsServeTheCard) {
  for (const DeviceReadingRow &row : device_readings()) {
    const bool serves_card = row.reading == DeviceReading::ROOM_TEMPERATURE || row.reading == DeviceReading::SETPOINT ||
                             row.reading == DeviceReading::MAX_SETPOINT;
    EXPECT_EQ(row.card != CardUse::NONE, serves_card) << row.name;
  }
  EXPECT_EQ(find_device_reading(DeviceReading::ROOM_TEMPERATURE)->card, CardUse::CURRENT_TEMPERATURE);
  EXPECT_EQ(find_device_reading(DeviceReading::SETPOINT)->card, CardUse::TARGET_TEMPERATURE);
  EXPECT_EQ(find_device_reading(DeviceReading::MAX_SETPOINT)->card, CardUse::TARGET_TEMPERATURE);
}
