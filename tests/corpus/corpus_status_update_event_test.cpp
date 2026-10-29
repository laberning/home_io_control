/// @file corpus_status_update_event_test.cpp
/// @brief The CMD_STATUS_UPDATE (0x71) event header, checked against every 0x71 in the corpus.
///
/// Every captured 0x71 is `<flags> <status> <event id> <length> <event data>` with a length byte that
/// names exactly the bytes that follow. Position updates carry EVENT_STATUS_CHANGE with 11 data bytes;
/// the RS100 Solar io's answer to a subscription carries EVENT_LOCAL_SENSOR with 5 (issue #156). The
/// sweep pins the header on every 0x71 the corpus holds, so a capture of another shape fails here
/// before the hub misroutes it.

#include "corpus_test_helpers.h"
#include "hub_decisions.h"
#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

namespace {

/// Position updates in the corpus all carry this many event-data bytes.
constexpr uint8_t STATUS_CHANGE_EVENT_DATA_LEN = 11;
/// The subscription answer's event-data length and first data byte.
constexpr uint8_t LOCAL_SENSOR_EVENT_DATA_LEN = 5;
constexpr uint8_t LOCAL_SENSOR_FIRST_DATA_BYTE = 0x06;
/// Fewest 0x71 frames the sweep must see: the 9 position updates plus the subscription answer.
constexpr int MIN_STATUS_UPDATES_IN_CORPUS = 10;

}  // namespace

TEST(CorpusStatusUpdateEvent, EveryCaptured0x71HasAWellFormedEventHeader) {
  int status_updates = 0;
  int event_reports = 0;
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
      if (std::string(capture->id) == corpus_test::SENSOR_EVENT_REPORT_CAPTURE) {
        event_reports++;
        EXPECT_EQ(event.event_id, EVENT_LOCAL_SENSOR);
        EXPECT_EQ(event.data_len, LOCAL_SENSOR_EVENT_DATA_LEN);
        EXPECT_EQ(frame.data[event.data_offset], LOCAL_SENSOR_FIRST_DATA_BYTE);
        EXPECT_TRUE(decisions::is_device_event_report(frame));
      } else {
        EXPECT_EQ(event.event_id, EVENT_STATUS_CHANGE);
        EXPECT_EQ(event.data_len, STATUS_CHANGE_EVENT_DATA_LEN);
        EXPECT_FALSE(decisions::is_device_event_report(frame)) << "a position update must reach the position decode";
      }
    }
  }
  EXPECT_GE(status_updates, MIN_STATUS_UPDATES_IN_CORPUS);
  EXPECT_EQ(event_reports, 1);
}

TEST(CorpusStatusUpdateEvent, SubscriptionAnswerCarriesTheBatteryByteOfAPositionUpdate) {
  // data[1] is header, not event data: the solar motor's battery bits read full in the
  // subscription answer as in its status replies.
  const corpus::CorpusCapture *capture = corpus_test::capture_by_id(corpus_test::SENSOR_EVENT_REPORT_CAPTURE);
  ASSERT_NE(capture, nullptr);
  const IoFrame frame = corpus_test::parse_capture_frame(capture->frames[0]);
  EXPECT_EQ(decode_battery_state(frame.data[1]), BatteryState::FULL_CHARGE);
}
