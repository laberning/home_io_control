/// @file
/// @brief Bridle trace line tests: the two examples of spec §12.1 and the split rule.

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "radio_bridle_codec.h"
#include "radio_bridle_trace.h"
#include "bridle_vectors.h"

using namespace esphome::home_io_control::bridle;

namespace {

/// The decoded bytes of a vector.
std::vector<uint8_t> bytes_of(const Vector &v) { return {v.decoded, v.decoded + v.decoded_len}; }

const Vector &vector_named(const char *name) {
  for (const Vector &v : VECTORS) {
    if (std::string(name) == v.name)
      return v;
  }
  ADD_FAILURE() << "no vector named " << name;
  return VECTORS[0];
}

std::string hex_of(const std::vector<uint8_t> &bytes) {
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string text;
  for (uint8_t b : bytes) {
    text += DIGITS[b >> 4];
    text += DIGITS[b & 0x0F];
  }
  return text;
}

std::string line(TraceDirection dir, uint32_t ms, const std::vector<uint8_t> &msg, size_t part) {
  char out[TRACE_LINE_MAX];
  size_t n = format_trace_part(out, sizeof(out), dir, ms, msg.data(), msg.size(), part);
  return std::string(out, n);
}

}  // namespace

TEST(RadioBridleTrace, HostCommandIsOneLineWithDirectionClockAndHex) {
  // spec §12.1: a get_rssi, its bytes taken from the vector
  const Vector &v = vector_named("get_rssi");
  EXPECT_EQ(line(TraceDirection::HOST_TO_COPROC, 1234, bytes_of(v), 0), "[bridle] > 1234 " + hex_of(bytes_of(v)));
}

TEST(RadioBridleTrace, CoprocessorMessageUsesTheOtherArrow) {
  // spec §12.1: the rssi reply, from the vector
  const Vector &v = vector_named("rssi");
  EXPECT_EQ(line(TraceDirection::COPROC_TO_HOST, 1236, bytes_of(v), 0), "[bridle] < 1236 " + hex_of(bytes_of(v)));
}

TEST(RadioBridleTrace, MessageOver128BytesIsSplitIntoNumberedParts) {
  // spec §12.1: a 145-byte log, split 128 + 17; the message is encoded by the codec, never typed in
  std::string const text(140, 'x');
  Log log;
  log.level = LogLevel::INFO;
  log.text = {reinterpret_cast<const uint8_t *>(text.data()), text.size()};
  uint8_t buffer[MAX_MESSAGE_SIZE];
  size_t const n = encode_message(0x2B, log, buffer, sizeof(buffer));
  ASSERT_EQ(n, 145u);
  std::vector<uint8_t> const msg(buffer, buffer + n);
  ASSERT_EQ(trace_part_count(msg.size()), 2u);
  std::string const all = hex_of(msg);
  std::string const first = line(TraceDirection::COPROC_TO_HOST, 1240, msg, 0);
  std::string const second = line(TraceDirection::COPROC_TO_HOST, 1240, msg, 1);
  EXPECT_EQ(first, "[bridle] < 1240 1/2 " + all.substr(0, 256));
  EXPECT_EQ(first.size(), std::strlen("[bridle] < 1240 1/2 ") + 256);
  EXPECT_EQ(second, "[bridle] < 1240 2/2 " + all.substr(256));
}

TEST(RadioBridleTrace, MessageOfExactly128BytesIsNotSplit) {
  std::vector<uint8_t> msg(TRACE_PART_BYTES, 0xAB);
  EXPECT_EQ(trace_part_count(msg.size()), 1u);
  std::string const one = line(TraceDirection::HOST_TO_COPROC, 5, msg, 0);
  EXPECT_EQ(one.find('/'), std::string::npos);
  msg.push_back(0xCD);
  EXPECT_EQ(trace_part_count(msg.size()), 2u);
}

TEST(RadioBridleTrace, LongestLineFitsTheBufferAndOutOfRangePartsWriteNothing) {
  std::vector<uint8_t> msg(255, 0xFF);
  char out[TRACE_LINE_MAX];
  size_t const n =
      format_trace_part(out, sizeof(out), TraceDirection::HOST_TO_COPROC, 0xFFFFFFFFu, msg.data(), msg.size(), 0);
  EXPECT_GT(n, 0u);
  EXPECT_LT(n, sizeof(out));
  EXPECT_EQ(format_trace_part(out, sizeof(out), TraceDirection::HOST_TO_COPROC, 1, msg.data(), msg.size(), 2), 0u);
  char small[8];
  EXPECT_EQ(format_trace_part(small, sizeof(small), TraceDirection::HOST_TO_COPROC, 1, msg.data(), msg.size(), 0), 0u);
}

TEST(RadioBridleTrace, EmptyMessageStillProducesOneLine) {
  EXPECT_EQ(trace_part_count(0), 1u);
  EXPECT_EQ(line(TraceDirection::HOST_TO_COPROC, 7, {}, 0), "[bridle] > 7 ");
}
