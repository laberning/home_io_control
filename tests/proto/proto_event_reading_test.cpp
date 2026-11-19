/// @file proto_event_reading_test.cpp
/// @brief Shape checks of decode_event_reading() on the sun level and battery level rows.
///
/// The captured answers themselves are checked in corpus_status_update_event_test.cpp. These tests
/// pin what the decoder reads and refuses: any shape other than the one captured stays undecoded, so a
/// device that answers differently leaves its sensor unknown instead of showing a misread value.

#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_device_readings.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

#include <cstring>
#include <initializer_list>
#include <optional>
#include <variant>

using namespace esphome::home_io_control;

namespace {

/// A CMD_STATUS_UPDATE with the captured header (flags 05, status 60) and the given event and data.
IoFrame event_report(uint16_t event_id, std::initializer_list<uint8_t> event_data) {
  IoFrame f{};
  f.cmd = CMD_STATUS_UPDATE;
  uint8_t n = 0;
  f.data[n++] = 0x05;  // flags byte, as captured
  f.data[n++] = 0x60;  // status byte: battery full, as captured
  f.data[n++] = static_cast<uint8_t>(event_id >> 8);
  f.data[n++] = static_cast<uint8_t>(event_id & 0xFF);
  f.data[n++] = static_cast<uint8_t>(event_data.size());
  for (uint8_t b : event_data)
    f.data[n++] = b;
  f.data_len = n;
  return f;
}

/// What the reading `reading` (a row with an EventSource) makes of a frame.
std::optional<float> read_event(DeviceReading reading, const IoFrame &frame) {
  const DeviceReadingRow *row = find_device_reading(reading);
  const auto *source = row != nullptr ? std::get_if<EventSource>(&row->source) : nullptr;
  EXPECT_NE(source, nullptr) << "the reading is not an event reading";
  return source != nullptr ? decode_event_reading(*source, frame) : std::nullopt;
}

std::optional<float> sun_of(const IoFrame &frame) { return read_event(DeviceReading::SUN_LEVEL, frame); }
std::optional<float> battery_of(const IoFrame &frame) { return read_event(DeviceReading::BATTERY_LEVEL, frame); }

}  // namespace

TEST(ProtoEventReading, SunLevelReadsTheBigEndianWordAfterTheTypeAndOneByteInLux) {
  EXPECT_EQ(sun_of(event_report(EVENT_LOCAL_SENSOR, {0x06, 0x00, 0x02, 0xE8, 0xB9})), std::optional<float>(74400.0F))
      << "raw 744 at 100 lx per step";
  EXPECT_EQ(sun_of(event_report(EVENT_LOCAL_SENSOR, {0x06, 0x7F, 0xFF, 0xFF, 0x00})), std::optional<float>(6553500.0F))
      << "the surrounding bytes do not enter the value, and the widest raw value stays exact in a float";
  EXPECT_EQ(sun_of(event_report(EVENT_LOCAL_SENSOR, {0x06, 0x00, 0x00, 0x00, 0x00})), std::optional<float>(0.0F))
      << "0 is a reading (night), not no reading";
  EXPECT_EQ(sun_of(event_report(EVENT_LOCAL_SENSOR, {0x06, 0x00, 0x00, 0x3D, 0x00})), std::optional<float>(6100.0F));
}

TEST(ProtoEventReading, SunLevelRefusesEveryOtherShape) {
  EXPECT_FALSE(sun_of(event_report(EVENT_LOCAL_SENSOR, {0x05, 0x00, 0x02, 0xE8, 0xB9}))) << "another sensor type byte";
  EXPECT_FALSE(sun_of(event_report(EVENT_LOCAL_SENSOR, {0x06, 0x00, 0x02, 0xE8}))) << "four data bytes";
  EXPECT_FALSE(sun_of(event_report(EVENT_LOCAL_SENSOR, {0x06, 0x00, 0x02, 0xE8, 0xB9, 0x00}))) << "six data bytes";
  EXPECT_FALSE(sun_of(event_report(EVENT_STATUS_CHANGE, {0x06, 0x00, 0x02, 0xE8, 0xB9})))
      << "a position update's event";

  IoFrame wrong_length = event_report(EVENT_LOCAL_SENSOR, {0x06, 0x00, 0x02, 0xE8, 0xB9});
  wrong_length.data[STATUS_UPDATE_EVENT_LENGTH_OFFSET] = 4;
  EXPECT_FALSE(sun_of(wrong_length)) << "a length byte that does not match the frame";

  IoFrame other_cmd = event_report(EVENT_LOCAL_SENSOR, {0x06, 0x00, 0x02, 0xE8, 0xB9});
  other_cmd.cmd = CMD_PRIVATE_RESP;
  EXPECT_FALSE(sun_of(other_cmd)) << "not a CMD_STATUS_UPDATE";
}

TEST(ProtoEventReading, BatteryLevelReadsOneByteUpToOneHundred) {
  EXPECT_EQ(battery_of(event_report(EVENT_BATTERY_LEVEL_CHANGE, {100})), std::optional<float>(100.0F));
  EXPECT_EQ(battery_of(event_report(EVENT_BATTERY_LEVEL_CHANGE, {0})), std::optional<float>(0.0F));
}

TEST(ProtoEventReading, BatteryLevelRefusesEveryOtherShape) {
  EXPECT_FALSE(battery_of(event_report(EVENT_BATTERY_LEVEL_CHANGE, {101}))) << "not a percentage";
  EXPECT_FALSE(battery_of(event_report(EVENT_BATTERY_LEVEL_CHANGE, {0x00, 0x64}))) << "two data bytes";
  EXPECT_FALSE(battery_of(event_report(EVENT_BATTERY_LEVEL_CHANGE, {}))) << "no data";
  EXPECT_FALSE(battery_of(event_report(EVENT_LOCAL_SENSOR, {100}))) << "another event";
}
