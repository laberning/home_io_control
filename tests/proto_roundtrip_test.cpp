/// @file proto_roundtrip_test.cpp
/// @brief Serialize/parse round-trip and CRC pinning regression tests.
///
/// This suite is a safety net for later protocol-layer refactorings: it locks
/// the observable behavior of @ref esphome::home_io_control::serialize /
/// @ref esphome::home_io_control::parse and @ref esphome::home_io_control::crc_ccitt
/// against a large, deterministically generated frame corpus. All randomness uses
/// a fixed seed so CI runs are bit-for-bit reproducible.

#include "proto_frame.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <random>
#include <vector>

using namespace esphome::home_io_control;

namespace {

/// Every command ID defined in proto_frame.h, so the generated corpus exercises
/// each real CMD_* value in the command byte position (not just random bytes).
constexpr std::array<uint8_t, 34> kAllCommands = {
    CMD_EXECUTE,
    CMD_ACTIVATE_MODE,
    CMD_PRIVATE,
    CMD_PRIVATE_RESP,
    CMD_SET_SENSOR,
    CMD_SET_SENSOR_ACK,
    CMD_WRITE_PRIVATE,
    CMD_WRITE_PRIVATE_ACK,
    CMD_DISCOVER_REQ,
    CMD_DISCOVER_RESP,
    CMD_DISCOVER_SPE_REQ,
    CMD_DISCOVER_SPE_RESP,
    CMD_DISCOVER_CONFIRM,
    CMD_DISCOVER_CONFIRM_ACK,
    CMD_DISCOVER_ALT_REQ,
    CMD_KEY_INIT,
    CMD_KEY_TRANSFER,
    CMD_KEY_CONFIRM,
    CMD_ADDRESS_REQ,
    CMD_ADDRESS_RESP,
    CMD_LAUNCH_KEY_TRANSFER,
    CMD_CHALLENGE_REQ,
    CMD_CHALLENGE_RESP,
    CMD_GET_NAME,
    CMD_GET_NAME_RESP,
    CMD_SET_NAME,
    CMD_SET_NAME_RESP,
    CMD_GET_INFO2,
    CMD_GET_INFO2_RESP,
    CMD_SET_CONFIG1,
    CMD_SET_CONFIG1_RESP,
    CMD_STATUS_UPDATE,
    CMD_STATUS_UPDATE_RESP,
    CMD_ERROR_RESP,
};

/// Interesting node-address patterns: both broadcast targets, the all-zero and
/// all-ones edge IDs, plus a couple of arbitrary unicast addresses.
constexpr std::array<std::array<uint8_t, NODE_ID_SIZE>, 6> kSpecialAddresses = {{
    {0x00, 0x00, 0x3B},  // BROADCAST_DISCOVER
    {0x00, 0x00, 0x3F},  // BROADCAST_DISCOVER_ALT
    {0x00, 0x00, 0x00},  // all-zero edge ID
    {0xFF, 0xFF, 0xFF},  // all-ones edge ID
    {0xC0, 0xFF, 0xEE},  // arbitrary unicast
    {0x9C, 0xA3, 0x9C},  // arbitrary unicast
}};

/// Pick a node address: mostly one of the interesting patterns, sometimes fully random.
void pick_address(uint8_t out[NODE_ID_SIZE], std::mt19937 &rng) {
  std::uniform_int_distribution<int> choice(0, 9);
  const int pick = choice(rng);
  if (pick < static_cast<int>(kSpecialAddresses.size())) {
    std::memcpy(out, kSpecialAddresses[static_cast<size_t>(pick)].data(), NODE_ID_SIZE);
    return;
  }
  std::uniform_int_distribution<int> byte(0, 255);
  for (uint8_t i = 0; i < NODE_ID_SIZE; i++)
    out[i] = static_cast<uint8_t>(byte(rng));
}

/// Pick a command byte: usually a defined CMD_* value, occasionally a raw random byte.
uint8_t pick_command(std::mt19937 &rng) {
  std::uniform_int_distribution<int> use_defined(0, 3);
  if (use_defined(rng) != 0) {
    std::uniform_int_distribution<size_t> idx(0, kAllCommands.size() - 1);
    return kAllCommands[idx(rng)];
  }
  std::uniform_int_distribution<int> byte(0, 255);
  return static_cast<uint8_t>(byte(rng));
}

/// Build a wire-consistent frame with the given flags and payload length; all
/// other fields (ctrl1, addresses, command, data) come from the seeded RNG.
IoFrame build_frame(std::mt19937 &rng, bool start, bool end, bool oneway, uint8_t data_len) {
  IoFrame f{};
  std::uniform_int_distribution<int> byte(0, 255);

  uint8_t flags = 0;
  if (start)
    flags |= CTRL0_START;
  if (end)
    flags |= CTRL0_END;
  if (oneway)
    flags |= CTRL0_PROTOCOL_1W;
  // The wire length field is (frame length - 1); frame length = 9 header bytes + payload.
  const uint8_t length_field = static_cast<uint8_t>((FRAME_MIN_SIZE + data_len - 1) & CTRL0_LENGTH_MASK);
  f.ctrl0 = static_cast<uint8_t>(flags | length_field);
  f.ctrl1 = static_cast<uint8_t>(byte(rng));
  pick_address(f.dst, rng);
  pick_address(f.src, rng);
  f.cmd = pick_command(rng);
  f.data_len = data_len;
  for (uint8_t i = 0; i < data_len; i++)
    f.data[i] = static_cast<uint8_t>(byte(rng));
  return f;
}

/// Serialize @p original, parse it back, and assert every field survives the round-trip.
void expect_round_trip(const IoFrame &original) {
  uint8_t buf[FRAME_MAX_SIZE] = {0};
  const uint8_t len = serialize(original, buf, sizeof(buf));
  ASSERT_GT(len, 0) << "well-formed frame should serialize";
  ASSERT_EQ(len, frame_length(original)) << "serialized length must equal frame_length()";

  IoFrame parsed{};
  ASSERT_TRUE(parse(buf, len, parsed)) << "serialized frame should parse back";
  EXPECT_EQ(parsed.ctrl0, original.ctrl0);
  EXPECT_EQ(parsed.ctrl1, original.ctrl1);
  EXPECT_EQ(0, std::memcmp(parsed.dst, original.dst, NODE_ID_SIZE));
  EXPECT_EQ(0, std::memcmp(parsed.src, original.src, NODE_ID_SIZE));
  EXPECT_EQ(parsed.cmd, original.cmd);
  EXPECT_EQ(parsed.data_len, original.data_len);
  EXPECT_EQ(0, std::memcmp(parsed.data, original.data, original.data_len)) << "payload bytes must round-trip";
}

}  // namespace

// ============================================================================
// Exhaustive coverage: every payload length crossed with every flag combination
// ============================================================================

TEST(ProtoRoundTrip, AllLengthsAndFlagCombinations) {
  std::mt19937 rng(0xC0FFEE);
  for (uint8_t data_len = 0; data_len <= FRAME_MAX_DATA_SIZE; data_len++) {
    for (int flags = 0; flags < 8; flags++) {
      const bool start = (flags & 0x01) != 0;
      const bool end = (flags & 0x02) != 0;
      const bool oneway = (flags & 0x04) != 0;
      const IoFrame frame = build_frame(rng, start, end, oneway, data_len);
      ASSERT_EQ(is_start(frame), start);
      ASSERT_EQ(is_end(frame), end);
      expect_round_trip(frame);
    }
  }
}

// ============================================================================
// Large deterministic fuzz corpus (> 1000 frames)
// ============================================================================

TEST(ProtoRoundTrip, DeterministicFuzzCorpus) {
  std::mt19937 rng(0xC0FFEE);
  std::uniform_int_distribution<int> len_dist(0, FRAME_MAX_DATA_SIZE);
  std::uniform_int_distribution<int> flag_bit(0, 1);
  constexpr int kFrameCount = 2000;
  for (int i = 0; i < kFrameCount; i++) {
    const auto data_len = static_cast<uint8_t>(len_dist(rng));
    const IoFrame frame = build_frame(rng, flag_bit(rng) != 0, flag_bit(rng) != 0, flag_bit(rng) != 0, data_len);
    expect_round_trip(frame);
  }
}

// ============================================================================
// Malformed-input rejection: assert only what the current contract guarantees
// ============================================================================

TEST(ProtoRoundTrip, ParseRejectsNullAndShortBuffers) {
  IoFrame parsed{};
  EXPECT_FALSE(parse(nullptr, 0, parsed)) << "null buffer must be rejected";
  EXPECT_FALSE(parse(nullptr, FRAME_MIN_SIZE, parsed)) << "null buffer must be rejected regardless of claimed length";

  // Any buffer shorter than the 9-byte minimum frame is rejected before field access.
  const uint8_t junk[FRAME_MIN_SIZE] = {0};
  for (uint8_t len = 0; len < FRAME_MIN_SIZE; len++)
    EXPECT_FALSE(parse(junk, len, parsed)) << "buffers shorter than FRAME_MIN_SIZE must be rejected, len=" << +len;
}

TEST(ProtoRoundTrip, ParseRejectsLengthInconsistentWithCtrl0) {
  std::mt19937 rng(0xC0FFEE);
  const IoFrame frame = build_frame(rng, /*start=*/true, /*end=*/false, /*oneway=*/false, /*data_len=*/4);
  uint8_t buf[FRAME_MAX_SIZE] = {0};
  const uint8_t len = serialize(frame, buf, sizeof(buf));
  ASSERT_GT(len, 0);

  IoFrame parsed{};
  // parse() requires buf_len to exactly equal the length encoded in CTRL0.
  EXPECT_FALSE(parse(buf, static_cast<uint8_t>(len - 1), parsed)) << "under-length buffer must be rejected";
  if (len < FRAME_MAX_SIZE) {
    EXPECT_FALSE(parse(buf, static_cast<uint8_t>(len + 1), parsed)) << "over-length buffer must be rejected";
  }
}

TEST(ProtoRoundTrip, SerializeRejectsMalformedFrames) {
  uint8_t buf[FRAME_MAX_SIZE] = {0};
  std::mt19937 rng(0xC0FFEE);

  // A well-formed baseline frame (data_len 0 => 9-byte wire frame).
  const IoFrame valid = build_frame(rng, /*start=*/true, /*end=*/true, /*oneway=*/false, /*data_len=*/0);
  ASSERT_GT(serialize(valid, buf, sizeof(buf)), 0) << "baseline frame should serialize";

  // Null output buffer.
  EXPECT_EQ(serialize(valid, nullptr, sizeof(buf)), 0) << "null output buffer must be rejected";

  // Output buffer smaller than the frame length.
  EXPECT_EQ(serialize(valid, buf, static_cast<uint8_t>(FRAME_MIN_SIZE - 1)), 0)
      << "output buffer smaller than the frame must be rejected";

  // data_len exceeding FRAME_MAX_DATA_SIZE while CTRL0 still encodes a legal wire length.
  IoFrame oversized = build_frame(rng, true, true, false, FRAME_MAX_DATA_SIZE);  // CTRL0 length => 32-byte frame
  oversized.data_len = FRAME_MAX_DATA_SIZE + 1;
  EXPECT_EQ(serialize(oversized, buf, sizeof(buf)), 0) << "oversized payload must be rejected";

  // CTRL0 length field encoding a frame shorter than FRAME_MIN_SIZE.
  IoFrame too_short = valid;
  too_short.ctrl0 = static_cast<uint8_t>(too_short.ctrl0 & ~CTRL0_LENGTH_MASK);  // length field 0 => frame length 1
  EXPECT_EQ(serialize(too_short, buf, sizeof(buf)), 0) << "sub-minimum frame length must be rejected";

  // CTRL0 wire length inconsistent with the explicit data_len (partially initialized frame).
  IoFrame inconsistent = valid;
  inconsistent.data_len = 5;  // CTRL0 still encodes a 9-byte frame (data_len 0)
  EXPECT_EQ(serialize(inconsistent, buf, sizeof(buf)), 0) << "CTRL0/data_len mismatch must be rejected";
}

// ============================================================================
// CRC-CCITT pinning: lock the current implementation against fixed vectors
// ============================================================================
//
// These outputs were computed once from the current crc_ccitt() implementation
// (poly 0x8408, init 0x0000). They pin behavior; a mismatch means the CRC
// routine changed, not that these expectations are wrong.

TEST(ProtoRoundTrip, CrcCcittPinnedVectors) {
  const uint8_t known[] = {0x40, 0x20, 0x9C, 0xA3, 0x9C, 0xC0, 0xFF, 0xEE, 0x03, 0x03, 0x00, 0x00};
  EXPECT_EQ(crc_ccitt(known, sizeof(known)), 0x6E2C) << "known IO-homecontrol frame vector";

  const uint8_t mixed[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
  EXPECT_EQ(crc_ccitt(mixed, sizeof(mixed)), 0xD147) << "mixed 8-byte vector";

  const uint8_t all_ones[] = {0xFF, 0xFF, 0xFF, 0xFF};
  EXPECT_EQ(crc_ccitt(all_ones, sizeof(all_ones)), 0xF399) << "all-ones 4-byte vector";

  const uint8_t single[] = {0xA5};
  EXPECT_EQ(crc_ccitt(single, sizeof(single)), 0xF2A7) << "single-byte vector";

  // Empty input leaves the CRC at its initial value.
  EXPECT_EQ(crc_ccitt(nullptr, 0), 0x0000) << "empty input yields the initial CRC value";
}
