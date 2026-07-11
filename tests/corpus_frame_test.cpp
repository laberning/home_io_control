/// @file corpus_frame_test.cpp
/// @brief Universal wire invariants over every frame of every golden-frame corpus capture.
///
/// Needs no per-frame expectations: parse()/serialize() round-trip, CRC-CCITT (where
/// captured), and CTRL0 length agreement hold for every frame regardless of protocol
/// content. This is the pipeline's own regression test — an issue-derived capture becomes a
/// permanent parser fixture the day it's ingested. Where per-frame expectations exist (cmd,
/// start/end/1W flags), they are checked too.

#include "corpus_generated.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace esphome::home_io_control;

namespace {

std::vector<const corpus::CorpusCapture *> all_captures() {
  std::vector<const corpus::CorpusCapture *> result;
  for (size_t i = 0; i < corpus::CAPTURE_COUNT; i++)
    result.push_back(&corpus::CAPTURES[i]);
  return result;
}

// Bits with a known meaning today (proto_frame.h); anything else is a reserved/unknown bit.
constexpr uint8_t CTRL1_KNOWN_MASK =
    CTRL1_VERSION_MASK | CTRL1_PRIORITY | CTRL1_ACK | CTRL1_LOW_POWER | CTRL1_ROUTED | CTRL1_BEACON;

}  // namespace

class CorpusFrameRoundTrip : public ::testing::TestWithParam<const corpus::CorpusCapture *> {};

TEST_P(CorpusFrameRoundTrip, WireInvariantsHoldForEveryFrame) {
  const corpus::CorpusCapture *capture = GetParam();

  for (uint8_t i = 0; i < capture->frame_count; i++) {
    const corpus::CorpusFrame &cf = capture->frames[i];
    SCOPED_TRACE(::testing::Message() << "capture=" << capture->id << " frame=" << static_cast<int>(i));

    const uint8_t non_crc_len = cf.crc_present ? static_cast<uint8_t>(cf.len - 2) : cf.len;

    // 1. parse() must succeed on the CRC-stripped bytes.
    IoFrame parsed{};
    ASSERT_TRUE(parse(cf.bytes, non_crc_len, parsed)) << "parse() failed on captured frame bytes";

    // 2. serialize(parse(bytes)) must be byte-identical to the CRC-stripped input.
    uint8_t serialized[FRAME_MAX_SIZE] = {0};
    const uint8_t serialized_len = serialize(parsed, serialized, sizeof(serialized));
    ASSERT_EQ(serialized_len, non_crc_len) << "serialize() length does not match captured length";
    EXPECT_EQ(serialized_len, frame_length(parsed)) << "frame_length() disagrees with serialize() length";
    EXPECT_EQ(memcmp(serialized, cf.bytes, non_crc_len), 0) << "round-trip did not reproduce the captured bytes";

    // 3. CRC-CCITT over the CRC-stripped bytes must match the trailing 2 bytes (little-endian,
    // per the software-CRC TX path in radio_sx1262.cpp: low byte first, then high byte).
    if (cf.crc_present) {
      const uint16_t computed = crc_ccitt(cf.bytes, non_crc_len);
      const uint16_t captured_crc =
          static_cast<uint16_t>(cf.bytes[non_crc_len]) | (static_cast<uint16_t>(cf.bytes[non_crc_len + 1]) << 8);
      EXPECT_EQ(computed, captured_crc) << "CRC-CCITT mismatch";
    }

    // 4. Per-frame expectations, where present.
    if (cf.has_cmd)
      EXPECT_EQ(parsed.cmd, cf.cmd) << "cmd expectation mismatch";
    if (cf.has_flags) {
      EXPECT_EQ(is_start(parsed), cf.start) << "start-flag expectation mismatch";
      EXPECT_EQ(is_end(parsed), cf.end) << "end-flag expectation mismatch";
      EXPECT_EQ((parsed.ctrl0 & CTRL0_PROTOCOL_1W) != 0, cf.oneway) << "1W-bit expectation mismatch";
    }

    // 5. CTRL1 reserved/unknown bits are logged, never a failure — that's how new protocol
    // facts get noticed instead of silently breaking captures from newer devices.
    const uint8_t reserved_bits = parsed.ctrl1 & static_cast<uint8_t>(~CTRL1_KNOWN_MASK);
    if (reserved_bits != 0) {
      GTEST_LOG_(INFO) << capture->id << " frame " << static_cast<int>(i) << ": CTRL1 reserved bits set: 0x" << std::hex
                       << static_cast<int>(reserved_bits);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(CorpusFrame, CorpusFrameRoundTrip, ::testing::ValuesIn(all_captures()),
                         [](const ::testing::TestParamInfo<const corpus::CorpusCapture *> &info) {
                           return std::string(info.param->id);
                         });
