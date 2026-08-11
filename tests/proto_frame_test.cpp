#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_constants.h"

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
  ASSERT_TRUE(create_execute_position(frame, own, dst, true, 100)) << "create_execute_position should succeed";
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

TEST(ProtoFrame, ParseRejectsLengthOutsideDeclaredOrTrailerShape) {
  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};

  IoFrame frame{};
  ASSERT_TRUE(create_get_status(frame, own, dst)) << "status request should be created for length-shape test";

  uint8_t serialized[FRAME_MAX_SIZE] = {0};
  const uint8_t serialized_len = serialize(frame, serialized, sizeof(serialized));
  ASSERT_GT(serialized_len, 0) << "status request should serialize for length-shape test";

  // Neither the declared length nor declared_len + HMAC_SIZE — a length in between (or beyond)
  // must never be accepted, whatever the command.
  uint8_t padded[FRAME_MAX_WIRE_SIZE] = {0};
  memcpy(padded, serialized, serialized_len);
  IoFrame parsed{};
  EXPECT_FALSE(parse(padded, static_cast<uint8_t>(serialized_len + 1), parsed))
      << "parse should reject a length that is neither declared_len nor declared_len + HMAC_SIZE";
  EXPECT_FALSE(parse(padded, static_cast<uint8_t>(serialized_len + HMAC_SIZE - 1), parsed))
      << "parse should reject a length one short of declared_len + HMAC_SIZE";
}

TEST(ProtoFrame, ParseRejectsMacTrailerShapeForNonTrailerCommand) {
  // frame_carries_mac_trailer() gates the wider declared_len + HMAC_SIZE shape by command
  // (proto_frame.cpp); this pins that gate so a regression back to a length-only check (which
  // would let any ordinary frame arriving with 6 extra trailing bytes be misread as carrying a
  // fabricated MAC) is caught here rather than surfacing as a ~1-in-65536 CRC coincidence later.
  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};

  IoFrame frame{};
  ASSERT_TRUE(create_get_status(frame, own, dst)) << "status request should be created for the non-trailer test";
  ASSERT_NE(frame.cmd, CMD_ONEWAY_ADD_CONTROLLER) << "test fixture must be a command that carries no trailer";
  ASSERT_FALSE(frame_carries_mac_trailer(frame.cmd)) << "CMD_PRIVATE must not be treated as trailer-bearing";

  uint8_t serialized[FRAME_MAX_WIRE_SIZE] = {0};
  const uint8_t serialized_len = serialize(frame, serialized, sizeof(serialized));
  ASSERT_GT(serialized_len, 0) << "status request should serialize for the non-trailer test";

  // Append 6 arbitrary trailing bytes — the exact shape a genuine MAC trailer would have.
  for (uint8_t i = 0; i < HMAC_SIZE; i++)
    serialized[serialized_len + i] = static_cast<uint8_t>(0xA0 + i);

  IoFrame parsed{};
  EXPECT_FALSE(parse(serialized, static_cast<uint8_t>(serialized_len + HMAC_SIZE), parsed))
      << "declared_len + HMAC_SIZE must be rejected for a command that does not carry a trailer";
}

TEST(ProtoFrame, SerializeRefusesTrailerOnNonTrailerCommand) {
  // The mirror of ParseRejectsMacTrailerShapeForNonTrailerCommand: serialize() applies the same
  // frame_carries_mac_trailer() gate, so it can never emit a frame parse() would reject. Without
  // that guard a caller could set has_mac on any command and put 6 trailing bytes on air that no
  // receiver would ever read back.
  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0x9C, 0xA3, 0x9C};

  IoFrame frame{};
  ASSERT_TRUE(create_get_status(frame, own, dst)) << "status request should be created for the non-trailer test";
  ASSERT_FALSE(frame_carries_mac_trailer(frame.cmd)) << "test fixture must be a command that carries no trailer";

  uint8_t serialized[FRAME_MAX_WIRE_SIZE] = {0};
  ASSERT_GT(serialize(frame, serialized, sizeof(serialized)), 0) << "the frame must serialize while has_mac is false";

  frame.has_mac = true;
  for (uint8_t i = 0; i < HMAC_SIZE; i++)
    frame.mac[i] = static_cast<uint8_t>(0xA0 + i);
  EXPECT_EQ(serialize(frame, serialized, sizeof(serialized)), 0)
      << "serialize() must refuse a trailer on a command that does not carry one";
}

TEST(ProtoFrame, AddControllerMacTrailerRoundTrip) {
  // Published worked example (reference/iown-homecontrol's docs/linklayer.md, CC0-1.0), captured
  // verbatim as tests/corpus/captures/reference_1w_vectors/oneway_add_controller_kat.yaml. 29
  // declared bytes (9 header + enc_key[16] + man_id[1] + data[1] + sequence[2] = 20 data bytes)
  // plus a genuine 6-byte MAC that does not fit inside CTRL0's 5-bit length field alongside the
  // declared payload (29 + 6 = 35, unrepresentable in 5 bits) — the MAC rides after the declared
  // length instead, still under the CRC. The corpus suite (corpus_frame_test.cpp) round-trips
  // this capture generically; this test pins the actual decoded field values.
  // Layout: ctrl0, ctrl1, dst[3], src[3], cmd, enc_key[16], man_id, data, sequence[2], mac[6].
  const uint8_t non_crc[] = {0xFC, 0x00, 0x00, 0x00, 0x3F, 0xAB, 0xCD, 0xEF, 0x30, 0x7E, 0x60, 0x49,
                             0x1F, 0x97, 0x6A, 0xDF, 0x65, 0x3D, 0xB0, 0xED, 0x78, 0x5E, 0x49, 0xA2,
                             0x01, 0x02, 0x01, 0x12, 0x34, 0x19, 0xE8, 0x1E, 0xC4, 0x3D, 0x5E};
  const uint8_t expected_mac[HMAC_SIZE] = {0x19, 0xE8, 0x1E, 0xC4, 0x3D, 0x5E};

  ASSERT_EQ(sizeof(non_crc), 35u) << "fixture should be the 29-declared + 6-MAC on-air body, CRC excluded";

  IoFrame parsed{};
  ASSERT_TRUE(parse(non_crc, sizeof(non_crc), parsed))
      << "declared_len + HMAC_SIZE should parse for CMD_ONEWAY_ADD_CONTROLLER";
  EXPECT_TRUE(parsed.has_mac) << "add-controller frame should be recognized as MAC-trailer-bearing";
  EXPECT_EQ(memcmp(parsed.mac, expected_mac, HMAC_SIZE), 0) << "parsed MAC should match the published trailer";
  EXPECT_EQ(parsed.cmd, CMD_ONEWAY_ADD_CONTROLLER) << "parsed command should be the add-controller command";
  EXPECT_EQ(parsed.data_len, 20) << "declared payload (enc_key+man_id+data+sequence) is 20 bytes";
  EXPECT_EQ(frame_length(parsed), 29) << "frame_length() reports only the CTRL0-declared portion, not the trailer";

  uint8_t serialized[FRAME_MAX_WIRE_SIZE] = {0};
  const uint8_t serialized_len = serialize(parsed, serialized, sizeof(serialized));
  ASSERT_EQ(serialized_len, sizeof(non_crc)) << "serialize() should re-emit the declared payload plus the trailer";
  EXPECT_EQ(memcmp(serialized, non_crc, sizeof(non_crc)), 0)
      << "round-trip should reproduce the published bytes exactly";

  // CRC-CCITT is computed over all 35 non-CRC bytes, including the trailer — the published on-air
  // CRC is little-endian "9B F2", i.e. 0xF29B.
  EXPECT_EQ(crc_ccitt(non_crc, sizeof(non_crc)), 0xF29B) << "CRC-CCITT must cover the trailer, per the published frame";
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

  decode_position_report(POS_UNKNOWN << 8, STATUS_POS_MAX / 2, true, target, position);
  EXPECT_FLOAT_EQ(target, 50.0f) << "marker target should fall back to current position when stopped";
  EXPECT_FLOAT_EQ(position, 50.0f) << "valid current raw value should still decode normally";

  decode_position_report(STATUS_POS_MAX + 1, STATUS_POS_MAX + 1, false, target, position);
  EXPECT_FLOAT_EQ(target, UNKNOWN_POSITION) << "invalid target raw value should stay unknown";
  EXPECT_FLOAT_EQ(position, UNKNOWN_POSITION) << "invalid current raw value should stay unknown while moving";
}

TEST(ProtoFrame, ReachedTargetTolerance) {
  EXPECT_TRUE(has_reached_target_position(50.0f, 50.1f))
      << "small differences inside tolerance should count as reached";
  EXPECT_FALSE(has_reached_target_position(50.0f, 51.0f)) << "larger differences should not count as reached";
  EXPECT_FALSE(has_reached_target_position(UNKNOWN_POSITION, 50.0f)) << "unknown target should never count as reached";
}

TEST(ProtoFrame, CommandResultDecodeHelpers) {
  EXPECT_STREQ(command_result_name(RESULT_LIMITATION_BY_RAIN), "LIMITATION_BY_RAIN");
  EXPECT_STREQ(command_result_description(RESULT_LIMITATION_BY_RAIN), "parameter was limited by a rain sensor");
  EXPECT_TRUE(is_limitation_result(RESULT_LIMITATION_BY_RAIN)) << "rain lockouts should be classified as limitations";

  EXPECT_STREQ(command_result_name(RESULT_MOTION_TIME_TOO_LONG), "MOTION_TIME_TOO_LONG");
  EXPECT_STREQ(command_result_description(RESULT_MOTION_TIME_TOO_LONG), "target was not reached in time");

  EXPECT_STREQ(command_result_name(RESULT_THERMAL_PROTECTION), "THERMAL_PROTECTION");
  EXPECT_STREQ(command_result_description(RESULT_THERMAL_PROTECTION), "node has gone into thermal protection mode");
  EXPECT_FALSE(is_limitation_result(RESULT_THERMAL_PROTECTION))
      << "thermal protection should stay in the generic error bucket";

  EXPECT_STREQ(command_result_name(0xFF), "UNKNOWN_RESULT_CODE");
  EXPECT_STREQ(command_result_description(0xFF), "unknown result code");
}

TEST(ProtoFrame, CrcCcittKnownVector) {
  const uint8_t sample[] = {0x40, 0x20, 0x9C, 0xA3, 0x9C, 0xC0, 0xFF, 0xEE, 0x03, 0x03, 0x00, 0x00};
  EXPECT_EQ(crc_ccitt(sample, sizeof(sample)), 0x6E2C) << "CRC-CCITT should match the known IO-homecontrol vector";
}
