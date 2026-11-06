/// @file corpus_status_update_event_test.cpp
/// @brief The CMD_STATUS_UPDATE (0x71) event header, checked against every 0x71 in the corpus.
///
/// Every captured 0x71 is `<flags> <status> <event id> <length> <event data>` with a length byte that
/// names exactly the bytes that follow. Position updates carry EVENT_STATUS_CHANGE with 11 data bytes;
/// the RS100 Solar io's answers to a subscription carry EVENT_LOCAL_SENSOR with 5 or
/// EVENT_BATTERY_LEVEL_CHANGE with 1 (issue #156). The
/// sweep pins the header on every 0x71 the corpus holds, so a capture of another shape fails here
/// before the hub misroutes it.

#include "corpus_test_helpers.h"
#include "hub_decisions.h"
#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

#include <iterator>
#include <string>

using namespace esphome::home_io_control;

namespace {

/// Position updates in the corpus all carry this many event-data bytes.
constexpr uint8_t STATUS_CHANGE_EVENT_DATA_LEN = 11;
/// Fewest 0x71 frames the sweep must see: the 9 position updates plus the four subscription answers.
constexpr int MIN_STATUS_UPDATES_IN_CORPUS = 13;
/// The same motor's two daylight answers to the subscription (11:45 and 13:41, issue #156).
constexpr const char *DAYLIGHT_SENSOR_EVENT_CAPTURE = "somfy_rs100_probe_subscribe_event_daylight_sx1262";
/// Sun-sensor subscription answers in the corpus: one at night, two in daylight.
constexpr int SENSOR_EVENT_REPORTS_IN_CORPUS = 3;
/// The same motor's answer to the battery-level subscription (20:53, issue #156).
constexpr const char *BATTERY_EVENT_CAPTURE = "somfy_rs100_probe_subscribe_event_battery_sx1262";
/// The level that answer carries.
constexpr uint8_t CAPTURED_BATTERY_LEVEL = 100;
/// The daylight answers' values, in capture order: 0x00B7 at 11:45, 0x02E8 at 13:41.
constexpr uint16_t DAYLIGHT_VALUES[] = {0x00B7, 0x02E8};

/// True for a capture that holds the RS100's answers to the 0x2005 subscription.
bool is_sensor_event_capture(const char *id) {
  return std::string(id) == corpus_test::SENSOR_EVENT_REPORT_CAPTURE ||
         std::string(id) == DAYLIGHT_SENSOR_EVENT_CAPTURE;
}

}  // namespace

TEST(CorpusStatusUpdateEvent, EveryCaptured0x71HasAWellFormedEventHeader) {
  int status_updates = 0;
  int event_reports = 0;
  int battery_reports = 0;
  for (const corpus::CorpusCapture *capture : corpus_test::all_captures()) {
    for (uint8_t i = 0; i < capture->frame_count; i++) {
      const IoFrame frame = corpus_test::parse_capture_frame(capture->frames[i]);
      if (frame.cmd != CMD_STATUS_UPDATE)
        continue;
      SCOPED_TRACE(std::string(capture->id) + " frame " + std::to_string(i));
      status_updates++;
      StatusUpdateEvent event;
      ASSERT_TRUE(decode_status_update_event(frame, event)) << "length byte must name the rest of the payload";
      EXPECT_EQ(event.data_offset, STATUS_UPDATE_EVENT_DATA_OFFSET);
      if (std::string(capture->id) == BATTERY_EVENT_CAPTURE) {
        battery_reports++;
        EXPECT_EQ(event.event_id, EVENT_BATTERY_LEVEL_CHANGE);
        EXPECT_EQ(event.data_len, BATTERY_LEVEL_EVENT_DATA_LEN);
        EXPECT_TRUE(decisions::is_device_event_report(frame));
      } else if (is_sensor_event_capture(capture->id)) {
        event_reports++;
        EXPECT_EQ(event.event_id, EVENT_LOCAL_SENSOR);
        EXPECT_EQ(event.data_len, LOCAL_SENSOR_EVENT_DATA_LEN);
        EXPECT_EQ(frame.data[event.data_offset], LOCAL_SENSOR_TYPE_SUN);
        EXPECT_TRUE(decisions::is_device_event_report(frame));
      } else {
        EXPECT_EQ(event.event_id, EVENT_STATUS_CHANGE);
        EXPECT_EQ(event.data_len, STATUS_CHANGE_EVENT_DATA_LEN);
        EXPECT_FALSE(decisions::is_device_event_report(frame)) << "a position update must reach the position decode";
      }
    }
  }
  EXPECT_GE(status_updates, MIN_STATUS_UPDATES_IN_CORPUS);
  EXPECT_EQ(event_reports, SENSOR_EVENT_REPORTS_IN_CORPUS);
  EXPECT_EQ(battery_reports, 1);
}

TEST(CorpusStatusUpdateEvent, DaylightAnswersCarryAValueTheNightAnswerDoesNot) {
  // The night answer's value bytes are all zero; in daylight two of them are not. Their meaning is
  // undecoded, so only the bytes are pinned, read from the frame through the event header.
  const corpus::CorpusCapture *night = corpus_test::capture_by_id(corpus_test::SENSOR_EVENT_REPORT_CAPTURE);
  const corpus::CorpusCapture *day = corpus_test::capture_by_id(DAYLIGHT_SENSOR_EVENT_CAPTURE);
  ASSERT_NE(night, nullptr);
  ASSERT_NE(day, nullptr);
  ASSERT_EQ(day->frame_count, std::size(DAYLIGHT_VALUES));

  const auto value_at = [](const IoFrame &frame) {
    StatusUpdateEvent event;
    EXPECT_TRUE(decode_status_update_event(frame, event));
    const uint8_t at = event.data_offset + LOCAL_SENSOR_VALUE_OFFSET;
    return static_cast<uint16_t>((frame.data[at] << 8) | frame.data[at + 1]);
  };
  EXPECT_EQ(value_at(corpus_test::parse_capture_frame(night->frames[0])), 0);
  for (uint8_t i = 0; i < day->frame_count; i++) {
    SCOPED_TRACE("daylight answer " + std::to_string(i));
    EXPECT_EQ(value_at(corpus_test::parse_capture_frame(day->frames[i])), DAYLIGHT_VALUES[i]);
  }
}

TEST(CorpusStatusUpdateEvent, SubscriptionAnswerCarriesTheBatteryByteOfAPositionUpdate) {
  // data[1] is header, not event data: the solar motor's battery bits read full in the
  // subscription answer as in its status replies.
  const corpus::CorpusCapture *capture = corpus_test::capture_by_id(corpus_test::SENSOR_EVENT_REPORT_CAPTURE);
  ASSERT_NE(capture, nullptr);
  const IoFrame frame = corpus_test::parse_capture_frame(capture->frames[0]);
  EXPECT_EQ(decode_battery_state(frame.data[1]), BatteryState::FULL_CHARGE);
}

TEST(CorpusStatusUpdateEvent, SunLevelDecodesFromEveryCapturedSensorAnswer) {
  const corpus::CorpusCapture *night = corpus_test::capture_by_id(corpus_test::SENSOR_EVENT_REPORT_CAPTURE);
  const corpus::CorpusCapture *day = corpus_test::capture_by_id(DAYLIGHT_SENSOR_EVENT_CAPTURE);
  ASSERT_NE(night, nullptr);
  ASSERT_NE(day, nullptr);
  uint16_t value = 1;
  ASSERT_TRUE(decode_sun_level_event(corpus_test::parse_capture_frame(night->frames[0]), value));
  EXPECT_EQ(value, 0) << "night";
  for (uint8_t i = 0; i < day->frame_count; i++) {
    SCOPED_TRACE("daylight answer " + std::to_string(i));
    ASSERT_TRUE(decode_sun_level_event(corpus_test::parse_capture_frame(day->frames[i]), value));
    EXPECT_EQ(value, DAYLIGHT_VALUES[i]);
  }
  uint8_t level = 0;
  EXPECT_FALSE(decode_battery_level_event(corpus_test::parse_capture_frame(day->frames[0]), level))
      << "a sun answer is not a battery level";
}

TEST(CorpusStatusUpdateEvent, BatteryLevelDecodesFromTheCapturedBatteryAnswer) {
  const corpus::CorpusCapture *capture = corpus_test::capture_by_id(BATTERY_EVENT_CAPTURE);
  ASSERT_NE(capture, nullptr);
  const IoFrame frame = corpus_test::parse_capture_frame(capture->frames[0]);
  uint8_t level = 0;
  ASSERT_TRUE(decode_battery_level_event(frame, level));
  EXPECT_EQ(level, CAPTURED_BATTERY_LEVEL);
  uint16_t value = 0;
  EXPECT_FALSE(decode_sun_level_event(frame, value)) << "a battery answer is not a sun value";
  EXPECT_EQ(decode_battery_state(frame.data[1]), BatteryState::FULL_CHARGE) << "the status byte agrees: full";
}
