/// @file proto_event_reading_test.cpp
/// @brief Shape checks of decode_sun_level_event() and decode_battery_level_event().
///
/// The captured answers themselves are checked in corpus_status_update_event_test.cpp. These tests
/// pin what the decoders refuse: any shape other than the one captured stays undecoded, so a device
/// that answers differently leaves its sensor unknown instead of showing a misread value.

#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

#include <cstring>
#include <initializer_list>

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

}  // namespace

TEST(ProtoEventReading, SunLevelReadsTheBigEndianWordAfterTheTypeAndOneByte) {
  uint16_t value = 0;
  ASSERT_TRUE(decode_sun_level_event(event_report(EVENT_LOCAL_SENSOR, {0x06, 0x00, 0x02, 0xE8, 0xB9}), value));
  EXPECT_EQ(value, 744);
  ASSERT_TRUE(decode_sun_level_event(event_report(EVENT_LOCAL_SENSOR, {0x06, 0x7F, 0xFF, 0xFF, 0x00}), value));
  EXPECT_EQ(value, 0xFFFF) << "the surrounding bytes do not enter the value";
}

TEST(ProtoEventReading, SunLevelRefusesEveryOtherShape) {
  uint16_t value = 0;
  EXPECT_FALSE(decode_sun_level_event(event_report(EVENT_LOCAL_SENSOR, {0x05, 0x00, 0x02, 0xE8, 0xB9}), value))
      << "another sensor type byte";
  EXPECT_FALSE(decode_sun_level_event(event_report(EVENT_LOCAL_SENSOR, {0x06, 0x00, 0x02, 0xE8}), value))
      << "four data bytes";
  EXPECT_FALSE(decode_sun_level_event(event_report(EVENT_LOCAL_SENSOR, {0x06, 0x00, 0x02, 0xE8, 0xB9, 0x00}), value))
      << "six data bytes";
  EXPECT_FALSE(decode_sun_level_event(event_report(EVENT_STATUS_CHANGE, {0x06, 0x00, 0x02, 0xE8, 0xB9}), value))
      << "a position update's event";

  IoFrame wrong_length = event_report(EVENT_LOCAL_SENSOR, {0x06, 0x00, 0x02, 0xE8, 0xB9});
  wrong_length.data[STATUS_UPDATE_EVENT_LENGTH_OFFSET] = 4;
  EXPECT_FALSE(decode_sun_level_event(wrong_length, value)) << "a length byte that does not match the frame";

  IoFrame other_cmd = event_report(EVENT_LOCAL_SENSOR, {0x06, 0x00, 0x02, 0xE8, 0xB9});
  other_cmd.cmd = CMD_PRIVATE_RESP;
  EXPECT_FALSE(decode_sun_level_event(other_cmd, value)) << "not a CMD_STATUS_UPDATE";
}

TEST(ProtoEventReading, BatteryLevelReadsOneByteUpToOneHundred) {
  uint8_t level = 0;
  ASSERT_TRUE(decode_battery_level_event(event_report(EVENT_BATTERY_LEVEL_CHANGE, {100}), level));
  EXPECT_EQ(level, 100);
  ASSERT_TRUE(decode_battery_level_event(event_report(EVENT_BATTERY_LEVEL_CHANGE, {0}), level));
  EXPECT_EQ(level, 0);
}

TEST(ProtoEventReading, BatteryLevelRefusesEveryOtherShape) {
  uint8_t level = 0;
  EXPECT_FALSE(decode_battery_level_event(event_report(EVENT_BATTERY_LEVEL_CHANGE, {101}), level))
      << "not a percentage";
  EXPECT_FALSE(decode_battery_level_event(event_report(EVENT_BATTERY_LEVEL_CHANGE, {0x00, 0x64}), level))
      << "two data bytes";
  EXPECT_FALSE(decode_battery_level_event(event_report(EVENT_BATTERY_LEVEL_CHANGE, {}), level)) << "no data";
  EXPECT_FALSE(decode_battery_level_event(event_report(EVENT_LOCAL_SENSOR, {100}), level)) << "another event";
}
