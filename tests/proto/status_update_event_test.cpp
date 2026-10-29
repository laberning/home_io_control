/// @file status_update_event_test.cpp
/// @brief decode_status_update_event() and decisions::is_device_event_report() on the edges of the
/// well-formed test; the corpus side lives in corpus_status_update_event_test.cpp.

#include "hub_decisions.h"
#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

#include <vector>

using namespace esphome::home_io_control;

namespace {

/// A frame with the given command and payload; endpoints are irrelevant to the decode.
IoFrame frame_with(uint8_t cmd, const std::vector<uint8_t> &payload) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  EXPECT_TRUE(set_cmd(f, cmd, payload.data(), static_cast<uint8_t>(payload.size())));
  return f;
}

}  // namespace

TEST(StatusUpdateEvent, DecodesTheHeaderOfAWellFormedReport) {
  const IoFrame f = frame_with(CMD_STATUS_UPDATE, {0x05, 0x60, 0x20, 0x05, 0x02, 0x06, 0x2A});
  StatusUpdateEvent event;
  ASSERT_TRUE(decode_status_update_event(f, event));
  EXPECT_EQ(event.event_id, EVENT_LOCAL_SENSOR);
  EXPECT_EQ(event.data_offset, STATUS_UPDATE_EVENT_DATA_OFFSET);
  EXPECT_EQ(event.data_len, 2);
  EXPECT_TRUE(decisions::is_device_event_report(f));
}

TEST(StatusUpdateEvent, AcceptsAnEmptyEventBody) {
  const IoFrame f = frame_with(CMD_STATUS_UPDATE, {0x05, 0x60, 0x20, 0x01, 0x00});
  StatusUpdateEvent event;
  ASSERT_TRUE(decode_status_update_event(f, event));
  EXPECT_EQ(event.event_id, EVENT_BATTERY_LEVEL_CHANGE);
  EXPECT_EQ(event.data_len, 0);
}

TEST(StatusUpdateEvent, RejectsALengthByteThatDoesNotMatchTheFrame) {
  // One byte short and one byte long: neither is well formed, so both keep the position path.
  for (const uint8_t length : {uint8_t{0x01}, uint8_t{0x03}}) {
    SCOPED_TRACE(static_cast<int>(length));
    const IoFrame f = frame_with(CMD_STATUS_UPDATE, {0x05, 0x60, 0x20, 0x05, length, 0x06, 0x2A});
    StatusUpdateEvent event;
    EXPECT_FALSE(decode_status_update_event(f, event));
    EXPECT_FALSE(decisions::is_device_event_report(f));
  }
}

TEST(StatusUpdateEvent, RejectsAPayloadShorterThanTheHeader) {
  const IoFrame f = frame_with(CMD_STATUS_UPDATE, {0x05, 0x60, 0x20, 0x05});
  StatusUpdateEvent event;
  EXPECT_FALSE(decode_status_update_event(f, event));
}

TEST(StatusUpdateEvent, RejectsAnyOtherCommand) {
  const IoFrame f = frame_with(CMD_PRIVATE_RESP, {0x05, 0x60, 0x20, 0x05, 0x00});
  StatusUpdateEvent event;
  EXPECT_FALSE(decode_status_update_event(f, event));
  EXPECT_FALSE(decisions::is_device_event_report(f));
}

TEST(StatusUpdateEvent, PositionUpdateIsNotAnEventReport) {
  const IoFrame f = frame_with(CMD_STATUS_UPDATE, {0x05, 0x60, 0x10, 0x0A, 0x01, 0x00});
  StatusUpdateEvent event;
  ASSERT_TRUE(decode_status_update_event(f, event));
  EXPECT_EQ(event.event_id, EVENT_STATUS_CHANGE);
  EXPECT_FALSE(decisions::is_device_event_report(f));
}

TEST(StatusUpdateEvent, AnyConfigResponseIsAnEventReport) {
  EXPECT_TRUE(decisions::is_device_event_report(frame_with(CMD_SET_CONFIG1_RESP, {0x01, 0x02})));
}
