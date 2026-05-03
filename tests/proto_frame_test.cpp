#include "proto_frame.h"
#include "proto_commands.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;

// ============================================================================
// ProtoFrame test suite
// ============================================================================
// Tests for frame construction, parsing, CRC-CCITT, hex utilities, and device
// profile classification. Focus on round-trip integrity and edge-case handling.
// Frame round-trip tests
// ========================================================================================

TEST(ProtoFrame, ExecuteRoundTrip) {
  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};

  IoFrame frame{};
  ASSERT_TRUE(create_execute(frame, own, dst, true, 100)) << "create_execute should succeed";
  EXPECT_TRUE(is_start(frame)) << "execute frame should be marked start";
  EXPECT_FALSE(is_end(frame)) << "execute frame should remain open for the authenticated response path";
  EXPECT_EQ(frame.cmd, CMD_EXECUTE) << "execute frame command should match";
  EXPECT_EQ(frame.data_len, 8) << "execute frame should use 8-byte payload for position commands";

  uint8_t serialized[FRAME_MAX_SIZE] = {0};
  uint8_t serialized_len = serialize(frame, serialized, sizeof(serialized));
  EXPECT_GT(serialized_len, 0) << "execute frame should serialize";

  IoFrame parsed{};
  EXPECT_TRUE(parse(serialized, serialized_len, parsed)) << "serialized execute frame should parse";
  EXPECT_EQ(parsed.cmd, CMD_EXECUTE) << "parsed execute frame command should match";
  EXPECT_EQ(parsed.data_len, 8) << "parsed execute payload length should match";
  EXPECT_EQ(0, memcmp(parsed.src, own, NODE_ID_SIZE)) << "parsed execute src should match";
  EXPECT_EQ(0, memcmp(parsed.dst, dst, NODE_ID_SIZE)) << "parsed execute dst should match";
}

TEST(ProtoFrame, StatusRequestRoundTrip) {
  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};

  IoFrame frame{};
  ASSERT_TRUE(create_get_status(frame, own, dst)) << "create_get_status should succeed";
  EXPECT_EQ(frame.cmd, CMD_PRIVATE) << "status request command should match";
  EXPECT_EQ(frame.data_len, 3) << "status request payload length should match";

  uint8_t serialized[FRAME_MAX_SIZE] = {0};
  uint8_t serialized_len = serialize(frame, serialized, sizeof(serialized));
  EXPECT_GT(serialized_len, 0) << "status request should serialize";

  IoFrame parsed{};
  EXPECT_TRUE(parse(serialized, serialized_len, parsed)) << "serialized status request should parse";
  EXPECT_EQ(parsed.cmd, CMD_PRIVATE) << "parsed status request command should match";
}

TEST(ProtoFrame, MaxFrameRoundTrip) {
  IoFrame frame{};
  init_frame(frame, true, true, true, false);
  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};
  set_src(frame, own);
  set_dst(frame, dst);
  uint8_t payload[FRAME_MAX_DATA_SIZE] = {0};
  for (uint8_t i = 0; i < FRAME_MAX_DATA_SIZE; i++)
    payload[i] = i;
  ASSERT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload))) << "max frame should be constructible";

  uint8_t serialized[FRAME_MAX_SIZE] = {0};
  const uint8_t serialized_len = serialize(frame, serialized, sizeof(serialized));
  EXPECT_EQ(serialized_len, FRAME_MAX_SIZE) << "max frame should serialize to full frame size";

  IoFrame parsed{};
  EXPECT_TRUE(parse(serialized, serialized_len, parsed)) << "max frame should parse";
  EXPECT_EQ(parsed.data_len, FRAME_MAX_DATA_SIZE) << "max frame payload length should round-trip";
}

TEST(ProtoFrame, SerializeRejectsInconsistentMetadata) {
  IoFrame frame{};
  init_frame(frame, true, true, false, false);
  frame.cmd = CMD_PRIVATE;
  frame.data_len = 3;
  frame.ctrl0 =
      static_cast<uint8_t>((frame.ctrl0 & ~CTRL0_LENGTH_MASK) | ((FRAME_MIN_SIZE + 1 - 1) & CTRL0_LENGTH_MASK));

  uint8_t serialized[FRAME_MAX_SIZE] = {0};
  EXPECT_EQ(serialize(frame, serialized, sizeof(serialized)), 0)
      << "serialize should reject inconsistent CTRL0/data_len metadata";
}

// ========================================================================================
// Parsing rejection tests
// ========================================================================================

TEST(ProtoFrame, ParseRejectsLengthMismatch) {
  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};

  IoFrame frame{};
  ASSERT_TRUE(create_get_status(frame, own, dst)) << "status request should be created for malformed-length test";

  uint8_t serialized[FRAME_MAX_SIZE] = {0};
  uint8_t serialized_len = serialize(frame, serialized, sizeof(serialized));
  EXPECT_GT(serialized_len, 0) << "status request should serialize for malformed-length test";

  // Tamper with CTRL0 length to mismatch actual data_len
  serialized[0] =
      static_cast<uint8_t>((serialized[0] & ~CTRL0_LENGTH_MASK) | ((serialized_len - 2 - 1) & CTRL0_LENGTH_MASK));

  IoFrame parsed{};
  EXPECT_FALSE(parse(serialized, serialized_len, parsed)) << "parse should reject mismatched CTRL0 length metadata";
}

TEST(ProtoFrame, ParseRejectsNullAndTruncatedInputs) {
  IoFrame parsed{};
  EXPECT_FALSE(parse(nullptr, 0, parsed)) << "parse should reject null input";
  EXPECT_FALSE(parse(reinterpret_cast<const uint8_t *>(""), 0, parsed)) << "parse should reject zero-length input";

  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};
  IoFrame frame{};
  ASSERT_TRUE(create_get_status(frame, own, dst)) << "status request should be created for truncation test";

  uint8_t serialized[FRAME_MAX_SIZE] = {0};
  uint8_t serialized_len = serialize(frame, serialized, sizeof(serialized));
  EXPECT_GT(serialized_len, 0) << "status request should serialize for truncation test";
  EXPECT_FALSE(parse(serialized, serialized_len - 1, parsed)) << "parse should reject truncated input";
}

// ========================================================================================
// Utility function tests
// ========================================================================================

TEST(ProtoFrame, HexToBytesRejectsInvalidInput) {
  uint8_t out[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  EXPECT_FALSE(hex_to_bytes("C0FFE", out, NODE_ID_SIZE)) << "odd-length hex should be rejected";
  EXPECT_EQ(out[0], 0) << "failed hex decode should zero the output buffer (byte 0)";
  EXPECT_EQ(out[1], 0) << "failed hex decode should zero the output buffer (byte 1)";
  EXPECT_EQ(out[2], 0) << "failed hex decode should zero the output buffer (byte 2)";

  EXPECT_FALSE(hex_to_bytes("GGGGGG", out, NODE_ID_SIZE)) << "non-hex characters should be rejected";
}

TEST(ProtoFrame, StatusPositionDecoding) {
  float target = UNKNOWN_POSITION;
  float position = UNKNOWN_POSITION;

  decode_position_report(STATUS_POS_MAX, STATUS_POS_MAX / 2, false, target, position);
  EXPECT_FLOAT_EQ(target, 100.0f) << "max target raw value should decode to 100 percent";
  EXPECT_FLOAT_EQ(position, 50.0f) << "half current raw value should decode to 50 percent";

  decode_position_report(STATUS_POS_MAX / 4, STATUS_POS_MAX + 1, true, target, position);
  EXPECT_FLOAT_EQ(target, 25.0f) << "quarter target raw value should decode to 25 percent";
  EXPECT_FLOAT_EQ(position, 25.0f) << "stopped device with invalid current raw value should fall back to target";

  decode_position_report(STATUS_POS_MAX + 1, STATUS_POS_MAX + 1, false, target, position);
  EXPECT_FLOAT_EQ(target, UNKNOWN_POSITION) << "invalid target raw value should stay unknown";
  EXPECT_FLOAT_EQ(position, UNKNOWN_POSITION) << "invalid current raw value should stay unknown while moving";
}

TEST(ProtoFrame, CrcCcittKnownVector) {
  const uint8_t sample[] = {0x40, 0x20, 0x9C, 0xA3, 0x9C, 0xC0, 0xFF, 0xEE, 0x03, 0x03, 0x00, 0x00};
  EXPECT_EQ(crc_ccitt(sample, sizeof(sample)), 0x6E2C) << "CRC-CCITT should match the known IO-homecontrol vector";
}
