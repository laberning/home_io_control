/// @file limitation_status_test.cpp
/// @brief Tests for the limitation read decoders (proto_codecs.h).

#include "proto_codecs.h"
#include "proto_frame.h"
#include "payload_frame.h"

#include <gtest/gtest.h>

#include <cstdint>

using namespace esphome::home_io_control;

// ============================================================================
// LimitationStatus test suite
// ============================================================================
// decode_limitation_status() / decode_limitation_request() / limitation_time_seconds() against the
// two replies and two requests heard from a VELUX KLF 200 and a window (issue #98), plus the
// layout's boundaries. The rain-limited reply is a prediction from the KLF 200 API specification,
// not a capture, and its test names say so.

TEST(LimitationStatus, DecodesHeardReplyForUnlimitedMinimum) {
  LimitationStatus s;
  ASSERT_TRUE(decode_limitation_status(
      test::make_payload_frame(CMD_LIMITATION_STATUS_RESP, {0x00, 0x00, 0x00, 0x00, 0x00}), s));
  EXPECT_EQ(s.parameter_id, LIMITATION_PARAM_MP);
  EXPECT_EQ(s.value_raw, 0x0000);
  EXPECT_EQ(s.originator, ORIGINATOR_LOCAL_USER);
  EXPECT_EQ(s.time_raw, 0x00);
}

TEST(LimitationStatus, DecodesHeardReplyForUnlimitedMaximum) {
  LimitationStatus s;
  ASSERT_TRUE(decode_limitation_status(
      test::make_payload_frame(CMD_LIMITATION_STATUS_RESP, {0x00, 0xC8, 0x00, 0x00, 0x00}), s));
  EXPECT_EQ(s.value_raw, STATUS_POS_MAX);
}

TEST(LimitationStatus, DecodesPredictedRainLayout) {
  // Prediction, not a capture: minimum read of a rain-limited window, 93 %, rain sensor, timer running.
  LimitationStatus s;
  ASSERT_TRUE(decode_limitation_status(
      test::make_payload_frame(CMD_LIMITATION_STATUS_RESP, {0x00, 0xBA, 0x00, 0x02, 0x1D}), s));
  EXPECT_EQ(s.value_raw, 0xBA00);
  EXPECT_EQ(s.originator, ORIGINATOR_RAIN_SENSOR);
  EXPECT_EQ(s.time_raw, 0x1D);
}

TEST(LimitationStatus, ReplyRejectsWrongLength) {
  LimitationStatus s;
  EXPECT_FALSE(
      decode_limitation_status(test::make_payload_frame(CMD_LIMITATION_STATUS_RESP, {0x00, 0xC8, 0x00, 0x00}), s));
  EXPECT_FALSE(decode_limitation_status(
      test::make_payload_frame(CMD_LIMITATION_STATUS_RESP, {0x00, 0xC8, 0x00, 0x00, 0x00, 0x00}), s));
  EXPECT_FALSE(decode_limitation_status(test::make_payload_frame(CMD_LIMITATION_STATUS_RESP, {}), s));
}

TEST(LimitationStatus, ReplyRejectsOtherCommand) {
  LimitationStatus s;
  EXPECT_FALSE(
      decode_limitation_status(test::make_payload_frame(CMD_LIMITATION_STATUS_REQ, {0x00, 0xC8, 0x00, 0x00, 0x00}), s));
}

TEST(LimitationStatus, DecodesHeardRequests) {
  LimitationType type = LimitationType::MAXIMUM;
  uint8_t param = 0xFF;
  ASSERT_TRUE(
      decode_limitation_request(test::make_payload_frame(CMD_LIMITATION_STATUS_REQ, {0x80, 0x00, 0x00}), type, param));
  EXPECT_EQ(type, LimitationType::MINIMUM);
  EXPECT_EQ(param, LIMITATION_PARAM_MP);
  ASSERT_TRUE(
      decode_limitation_request(test::make_payload_frame(CMD_LIMITATION_STATUS_REQ, {0xC0, 0x00, 0x00}), type, param));
  EXPECT_EQ(type, LimitationType::MAXIMUM);
}

TEST(LimitationStatus, RequestRejectsUnknownSelectorWrongLengthAndOtherCommand) {
  LimitationType type;
  uint8_t param = 0;
  EXPECT_FALSE(
      decode_limitation_request(test::make_payload_frame(CMD_LIMITATION_STATUS_REQ, {0x40, 0x00, 0x00}), type, param));
  EXPECT_FALSE(
      decode_limitation_request(test::make_payload_frame(CMD_LIMITATION_STATUS_REQ, {0x80, 0x00}), type, param));
  EXPECT_FALSE(
      decode_limitation_request(test::make_payload_frame(CMD_LIMITATION_STATUS_RESP, {0x80, 0x00, 0x00}), type, param));
}

TEST(LimitationStatus, TimeCodesCountThirtySecondSteps) {
  EXPECT_EQ(limitation_time_seconds(0), 30u);
  EXPECT_EQ(limitation_time_seconds(0x1D), 900u);
  EXPECT_EQ(limitation_time_seconds(LIMITATION_TIME_MAX_COUNTED), 253u * 30u);
}

TEST(LimitationStatus, TimeCodesAboveTheCountedRangeAreNotDurations) {
  EXPECT_EQ(limitation_time_seconds(LIMITATION_TIME_UNLIMITED), UINT32_MAX);
  EXPECT_EQ(limitation_time_seconds(254), 0u);
  EXPECT_EQ(limitation_time_seconds(255), 0u);
}
