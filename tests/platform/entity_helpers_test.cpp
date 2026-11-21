/// @file entity_helpers_test.cpp
/// @brief Tests for the conversions and renderers in entity_helpers.h.

#include "entity_helpers.h"
#include "payload_frame.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

using namespace esphome::home_io_control;

// ============================================================================
// EntityHelpers test suite
// ============================================================================
// Percent rounding and the "Last Commanded By" / "Last Command Source" renderers. Includes only
// entity_helpers.h, so it also proves that header compiles without the hub.

TEST(EntityHelpers, RoundPercentRoundsRatherThanTruncates) {
  // HA's actual "50%" (128/255) truncates to 49 but should round to 50 — the real-hardware
  // regression this helper exists to fix (see platform_cover.cpp / platform_light.cpp).
  EXPECT_EQ(detail::round_percent(128.0F / 255.0F), 50) << "128/255 should round to 50, not truncate to 49";
  EXPECT_EQ(detail::round_percent(0.0F), 0);
  EXPECT_EQ(detail::round_percent(1.0F), 100);
  EXPECT_EQ(detail::round_percent(0.504F), 50) << "just past the rounding boundary should round up";
  EXPECT_EQ(detail::round_percent(0.494F), 49) << "just below the rounding boundary should round down";
}

TEST(EntityHelpers, IoFromFractionMapsOpenToZeroUnlessInverted) {
  EXPECT_EQ(detail::io_from_fraction(1.0F, false), 0) << "standard: HA fully open is IO 0";
  EXPECT_EQ(detail::io_from_fraction(0.0F, false), 100);
  EXPECT_EQ(detail::io_from_fraction(1.0F, true), 100) << "inverted: HA fully open is IO 100";
  EXPECT_EQ(detail::io_from_fraction(0.0F, true), 0);
}

TEST(EntityHelpers, IoFromFractionRoundsQuantizedValues) {
  // HA's "50%" arrives as 128/255; both directions must round to 50, not truncate to 49.
  EXPECT_EQ(detail::io_from_fraction(128.0F / 255.0F, true), 50);
  EXPECT_EQ(detail::io_from_fraction(1.0F - 128.0F / 255.0F, false), 50);
  EXPECT_EQ(detail::io_from_fraction(0.504F, true), 50);
  EXPECT_EQ(detail::io_from_fraction(0.494F, true), 49);
}

TEST(EntityHelpers, FractionFromIoInvertsTheMapping) {
  EXPECT_FLOAT_EQ(detail::fraction_from_io(0.0F, false), 1.0F);
  EXPECT_FLOAT_EQ(detail::fraction_from_io(100.0F, false), 0.0F);
  EXPECT_FLOAT_EQ(detail::fraction_from_io(30.0F, false), 0.7F);
  EXPECT_FLOAT_EQ(detail::fraction_from_io(100.0F, true), 1.0F);
  EXPECT_FLOAT_EQ(detail::fraction_from_io(30.0F, true), 0.3F);
}

TEST(EntityHelpers, FractionFromIoKeepsTheDevicesResolution) {
  // The conversion does not round: a half-percent position stays a half-percent fraction, and a caller
  // that wants whole percent (the cover) rounds first.
  EXPECT_FLOAT_EQ(detail::fraction_from_io(33.5F, true), 0.335F);
  EXPECT_FLOAT_EQ(detail::fraction_from_io(detail::whole_percent(99.68F), false), 0.0F)
      << "a cover resting at 99.68 % closed publishes exactly closed";
}

TEST(EntityHelpers, ReadingChangedTreatsTwoNansAsEqual) {
  EXPECT_FALSE(detail::reading_changed(NAN, NAN)) << "never read twice is no change";
  EXPECT_TRUE(detail::reading_changed(NAN, 21.5F));
  EXPECT_TRUE(detail::reading_changed(21.5F, NAN)) << "a sensor fault after a value is a change";
  EXPECT_TRUE(detail::reading_changed(21.0F, 21.5F));
  EXPECT_FALSE(detail::reading_changed(21.5F, 21.5F));
}

TEST(EntityHelpers, FormatOriginatorRendersNameAndHex) {
  EXPECT_EQ(detail::format_originator(ORIGINATOR_USER_REMOTE), "user_remote(0x01)");
  EXPECT_EQ(detail::format_originator(0x0A), "unknown(0x0A)") << "an undecoded byte must keep its raw hex";
}

TEST(EntityHelpers, DescribeLastCommanderQualifiesThisHub) {
  IoDevice dev{};
  dev.node_id[0] = 0xAB;
  dev.node_id[1] = 0xC1;
  dev.node_id[2] = 0x23;
  dev.last_commander[0] = 0xC0;
  dev.last_commander[1] = 0xFF;
  dev.last_commander[2] = 0xEE;
  dev.has_last_command = true;
  const uint8_t hub_id[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};

  EXPECT_EQ(detail::describe_last_commander(dev, hub_id), "C0FFEE (this hub)");
}

TEST(EntityHelpers, DescribeLastCommanderQualifiesTheDeviceItself) {
  IoDevice dev{};
  dev.node_id[0] = 0x58;
  dev.node_id[1] = 0x6E;
  dev.node_id[2] = 0x35;
  dev.last_commander[0] = 0x58;
  dev.last_commander[1] = 0x6E;
  dev.last_commander[2] = 0x35;
  dev.has_last_command = true;
  const uint8_t hub_id[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};

  EXPECT_EQ(detail::describe_last_commander(dev, hub_id), "586E35 (this device)");
}

TEST(EntityHelpers, DescribeLastCommanderIsPlainForAForeignController) {
  IoDevice dev{};
  dev.node_id[0] = 0xAB;
  dev.node_id[1] = 0xC1;
  dev.node_id[2] = 0x23;
  dev.last_commander[0] = 0x3B;
  dev.last_commander[1] = 0x74;
  dev.last_commander[2] = 0xDC;
  dev.has_last_command = true;
  const uint8_t hub_id[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};

  EXPECT_EQ(detail::describe_last_commander(dev, hub_id), "3B74DC");
}

TEST(EntityHelpers, DescribeLastCommanderIsEmptyBeforeAnyRecord) {
  IoDevice dev{};  // has_last_command == false
  const uint8_t hub_id[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};

  EXPECT_TRUE(detail::describe_last_commander(dev, hub_id).empty());
}

TEST(EntityHelpers, DescribeLastCommandSourceRendersNameAndHex) {
  IoDevice dev{};
  dev.has_last_command = true;
  dev.last_command_originator = 0x01;  // ORIGINATOR_USER_REMOTE

  EXPECT_EQ(detail::describe_last_command_source(dev), "user_remote(0x01)");
}

TEST(EntityHelpers, DescribeLastCommandSourceRendersUndecodedBytes) {
  // 0x0A is a genuine gap in the ORIGINATOR_* table (a mains gate reported it) — must self-describe
  // rather than being invented a name or silently dropped.
  IoDevice dev{};
  dev.has_last_command = true;
  dev.last_command_originator = 0x0A;

  EXPECT_EQ(detail::describe_last_command_source(dev), "unknown(0x0A)");
}

TEST(EntityHelpers, DescribeLastCommandSourceIsEmptyBeforeAnyRecord) {
  IoDevice dev{};  // has_last_command == false

  EXPECT_TRUE(detail::describe_last_command_source(dev).empty());
}

// ============================================================================
// Limitation read rendering
// ============================================================================

TEST(EntityHelpers, DescribeLimitationReplyRendersPredictedRainLayout) {
  // Prediction from the KLF 200 API specification, not a capture.
  EXPECT_EQ(detail::describe_limitation_reply(
                test::make_payload_frame(CMD_LIMITATION_STATUS_RESP, {0x00, 0xBA, 0x00, 0x02, 0x1D})),
            "limitation (assumed layout): status=0x00 (code 0, state 0) value=93% (BA 00) originator=rain_sensor(0x02) "
            "time=0x1D (900 s)");
}

TEST(EntityHelpers, DescribeLimitationReplyRendersHeardReplies) {
  EXPECT_EQ(detail::describe_limitation_reply(
                test::make_payload_frame(CMD_LIMITATION_STATUS_RESP, {0x00, 0xC8, 0x00, 0x00, 0x00})),
            "limitation (assumed layout): status=0x00 (code 0, state 0) value=100% (C8 00) originator=local_user(0x00) "
            "time=0x00 (30 s)");
  EXPECT_EQ(detail::describe_limitation_reply(
                test::make_payload_frame(CMD_LIMITATION_STATUS_RESP, {0x00, 0x00, 0x00, 0x00, 0x00})),
            "limitation (assumed layout): status=0x00 (code 0, state 0) value=0% (00 00) originator=local_user(0x00) "
            "time=0x00 (30 s)");
}

TEST(EntityHelpers, DescribeLimitationReplyKeepsTheRawStatusByte) {
  EXPECT_EQ(detail::describe_limitation_reply(
                test::make_payload_frame(CMD_LIMITATION_STATUS_RESP, {0x05, 0x00, 0x00, 0x00, 0x00})),
            "limitation (assumed layout): status=0x05 (code 0, state 5) value=0% (00 00) originator=local_user(0x00) "
            "time=0x00 (30 s)");
}

TEST(EntityHelpers, DescribeLimitationReplyReportsUnexpectedLength) {
  EXPECT_EQ(detail::describe_limitation_reply(test::make_payload_frame(CMD_LIMITATION_STATUS_RESP, {0x00, 0xC8, 0x00})),
            "limitation (assumed layout): unexpected length 3");
}

TEST(EntityHelpers, LimitationValueShowsSelectorsAndFractionsHonestly) {
  EXPECT_EQ(detail::format_limitation_value(0xD801), "selector (D8 01)")
      << "a selector must never read as a percentage";
  EXPECT_EQ(detail::format_limitation_value(0x5D00), "46.5% (5D 00)") << "a non-whole percentage keeps one decimal";
  EXPECT_EQ(detail::format_limitation_value(STATUS_POS_MAX), "100% (C8 00)") << "the boundary is still a position";
  EXPECT_EQ(detail::format_limitation_value(STATUS_POS_MAX + 1), "selector (C8 01)");
}

TEST(EntityHelpers, LimitationParamNamesMainAndFunctionalParameters) {
  EXPECT_EQ(detail::format_limitation_param(0), "MP");
  EXPECT_EQ(detail::format_limitation_param(1), "FP1");
  EXPECT_EQ(detail::format_limitation_param(16), "FP16");
  EXPECT_EQ(detail::format_limitation_param(17), "0x11");
}

TEST(EntityHelpers, LimitationTimeRendersEveryCodeClass) {
  EXPECT_EQ(detail::format_limitation_time(0), "0x00 (30 s)");
  EXPECT_EQ(detail::format_limitation_time(252), "0xFC (7590 s)");
  EXPECT_EQ(detail::format_limitation_time(253), "0xFD (unlimited)");
  EXPECT_EQ(detail::format_limitation_time(254), "0xFE (code 254)");
  EXPECT_EQ(detail::format_limitation_time(255), "0xFF (code 255)");
}

TEST(EntityHelpers, DescribeLimitationRequestNamesTypeAndParameter) {
  EXPECT_EQ(
      detail::describe_limitation_request(test::make_payload_frame(CMD_LIMITATION_STATUS_REQ, {0x80, 0x00, 0x00})),
      "limitation request (assumed layout): type=minimum param=MP");
  EXPECT_EQ(
      detail::describe_limitation_request(test::make_payload_frame(CMD_LIMITATION_STATUS_REQ, {0xC0, 0x00, 0x00})),
      "limitation request (assumed layout): type=maximum param=MP");
  EXPECT_EQ(
      detail::describe_limitation_request(test::make_payload_frame(CMD_LIMITATION_STATUS_REQ, {0x40, 0x00, 0x00})),
      "limitation request (assumed layout): unexpected payload");
}
