/// @file entity_helpers_test.cpp
/// @brief Tests for the conversions and renderers in entity_helpers.h.

#include "entity_helpers.h"

#include <gtest/gtest.h>

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
