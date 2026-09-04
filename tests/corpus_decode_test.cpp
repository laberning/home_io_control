/// @file corpus_decode_test.cpp
/// @brief Codec-layer expectations driven by golden-frame corpus captures.
///
/// Every assertion here is optional per-capture: only fields present in a capture's `expect:`
/// block are checked, so a keyless community capture that only verified a decoded intent still
/// exercises exactly that and nothing more. Address classification (`classify_address()` /
/// `broadcast_target_type()`) has no dedicated expectation field of its own — it is exercised
/// transitively through `decode_1w_frame()`, which calls both internally to populate
/// `OneWayFrameInfo::address_class` / `::target_type`.

#include "corpus_generated.h"
#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_device_model.h"
#include "proto_frame.h"

#include "corpus_test_helpers.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>

using namespace esphome::home_io_control;

namespace {

// Mirrors PRIVATE_RESPONSE_TARGET_OFFSET / PRIVATE_RESPONSE_CURRENT_OFFSET /
// STATUS_STOPPED_FLAGS_OFFSET in hub_status.cpp — those constants are file-local (anonymous
// namespace), so the documented CMD_PRIVATE_RESP (0x04) byte layout is reproduced here rather
// than extracted out of components/ — this test deliberately does not modify component code.
// decode_position_report() itself IS a callable pure function
// (proto_device_model.h) and is exercised for real, not reimplemented.
constexpr uint8_t PRIVATE_RESP_STOPPED_FLAGS_OFFSET = 0;
constexpr uint8_t PRIVATE_RESP_TARGET_OFFSET = 2;
constexpr uint8_t PRIVATE_RESP_CURRENT_OFFSET = 4;

// Mirrors EXTENDED_TILT_RESPONSE_MIN_DATA_LEN / EXTENDED_TILT_SELECTOR_OFFSET / EXTENDED_TILT_MSB_OFFSET /
// EXTENDED_TILT_LSB_OFFSET in hub_status.cpp — same file-local-constant situation as the position offsets
// above. Only a status-poll reply (not our own EXECUTE-tilt ack) is long enough to carry this extended
// block; hub_status.cpp additionally gates on device_supports_tilt(dev.type), which this codec-layer test
// has no device record to check, so it relies on the capture author only setting reported_tilt on frames
// that are genuinely status-poll replies (see tests/corpus/README.md).
constexpr uint8_t EXTENDED_TILT_RESPONSE_MIN_DATA_LEN = 15;
constexpr uint8_t EXTENDED_TILT_SELECTOR_OFFSET = 12;
constexpr uint8_t EXTENDED_TILT_MSB_OFFSET = 13;
constexpr uint8_t EXTENDED_TILT_LSB_OFFSET = 14;

}  // namespace

class CorpusDecode : public ::testing::TestWithParam<const corpus::CorpusCapture *> {};

TEST_P(CorpusDecode, ExpectationsMatchDecodedFrames) {
  const corpus::CorpusCapture *capture = GetParam();
  SCOPED_TRACE(::testing::Message() << "capture=" << capture->id);

  // --- 1W remote frame decode --------------------------------------------------------------
  const bool has_oneway_expectation = capture->has_oneway_intent || capture->has_oneway_target_type ||
                                      capture->has_oneway_originator || capture->has_oneway_acei;
  if (has_oneway_expectation) {
    int oneway_frames_seen = 0;
    for (uint8_t i = 0; i < capture->frame_count; i++) {
      const corpus::CorpusFrame &cf = capture->frames[i];
      if (!cf.has_flags || !cf.oneway)
        continue;
      oneway_frames_seen++;
      IoFrame frame = corpus_test::parse_capture_frame(cf);
      OneWayFrameInfo info = decode_1w_frame(frame);
      if (capture->has_oneway_intent) {
        EXPECT_STREQ(info.intent, capture->oneway_intent) << "1W intent mismatch on frame " << static_cast<int>(i);
      }
      if (capture->has_oneway_target_type) {
        EXPECT_EQ(static_cast<uint16_t>(info.target_type), capture->oneway_target_type)
            << "1W target_type mismatch on frame " << static_cast<int>(i);
      }
      if (capture->has_oneway_originator) {
        EXPECT_EQ(info.originator, capture->oneway_originator)
            << "1W originator mismatch on frame " << static_cast<int>(i);
      }
      if (capture->has_oneway_acei) {
        EXPECT_EQ(info.acei_level, capture->oneway_acei) << "1W ACEI level mismatch on frame " << static_cast<int>(i);
      }
    }
    EXPECT_GT(oneway_frames_seen, 0) << "capture has oneway expectations but no 1W-flagged frame was found";
  }

  // --- Device name decode --------------------------------------------------------------------
  if (capture->has_device_name) {
    int name_frames_seen = 0;
    for (uint8_t i = 0; i < capture->frame_count; i++) {
      const corpus::CorpusFrame &cf = capture->frames[i];
      if (!cf.has_cmd || cf.cmd != CMD_GET_NAME_RESP)
        continue;
      name_frames_seen++;
      IoFrame frame = corpus_test::parse_capture_frame(cf);
      std::string decoded = decode_device_name_payload(frame.data, frame.data_len);
      EXPECT_EQ(decoded, capture->device_name) << "device name mismatch on frame " << static_cast<int>(i);
    }
    EXPECT_GT(name_frames_seen, 0) << "capture has device.name expectation but no CMD_GET_NAME_RESP frame found";
  }

  // --- Status-response position decode --------------------------------------------------------
  if (capture->has_reported_position) {
    int status_frames_seen = 0;
    for (uint8_t i = 0; i < capture->frame_count; i++) {
      const corpus::CorpusFrame &cf = capture->frames[i];
      if (!cf.has_cmd || cf.cmd != CMD_PRIVATE_RESP)
        continue;
      IoFrame frame = corpus_test::parse_capture_frame(cf);
      if (frame.data_len <= PRIVATE_RESP_CURRENT_OFFSET + 1)
        continue;  // too short to carry target/current — not a position-bearing 0x04 reply
      status_frames_seen++;

      // Bitwise, not equality — mirrors hub_status.cpp :: apply_private_response_status()
      // (`(frame.data[0] & STATUS_STOPPED) != 0`); STATUS_STOPPED is one flag bit among others.
      const bool is_stopped = (frame.data[PRIVATE_RESP_STOPPED_FLAGS_OFFSET] & STATUS_STOPPED) != 0;
      const uint16_t target_raw = (static_cast<uint16_t>(frame.data[PRIVATE_RESP_TARGET_OFFSET]) << 8) |
                                  frame.data[PRIVATE_RESP_TARGET_OFFSET + 1];
      const uint16_t current_raw = (static_cast<uint16_t>(frame.data[PRIVATE_RESP_CURRENT_OFFSET]) << 8) |
                                   frame.data[PRIVATE_RESP_CURRENT_OFFSET + 1];
      float target = 0.0F;
      float position = 0.0F;
      decode_position_report(target_raw, current_raw, is_stopped, target, position);
      // `reported_position` is a whole-percent uint8_t in the schema (a human-verified reading,
      // e.g. "the log says position=1%"), but decode_position_report() returns sub-percent-
      // precision floats on real hardware (dev.position is carried as a float throughout
      // components/ — see platform_cover.cpp's `dev.position / 100.0F`). Comparing those directly
      // with EXPECT_FLOAT_EQ only ever passed by luck, on synthetic/zeroed captures whose raw
      // bytes happen to decode to an exact integer; round to the nearest percent first so a real
      // capture's genuinely fractional decode (e.g. 25.08) matches its human-verified "25%".
      EXPECT_EQ(std::lround(position), static_cast<long>(capture->reported_position))
          << "reported_position mismatch on frame " << static_cast<int>(i) << " (raw decoded position=" << position
          << ")";
    }
    EXPECT_GT(status_frames_seen, 0)
        << "capture has device.reported_position expectation but no position-bearing CMD_PRIVATE_RESP frame found";
  }

  // --- Status-poll extended tilt decode --------------------------------------------------------
  // Only the extended (>=15-byte) CMD_PRIVATE_RESP layout used for status-poll replies carries a
  // real slat-angle reading (selector 0x20 at offset 12, 16-bit angle at 13..14) — the immediate
  // ack to our own EXECUTE-tilt echoes a *different*, pre-command tilt block at offset 4..6 (see
  // tests/corpus/captures/exchange/tilt_cover_exchange_ack_tilt_block*.yaml and
  // tilt_cover_exchange_ack_echoes_precommand_tilt.yaml) and must never be checked here.
  if (capture->has_reported_tilt) {
    int tilt_frames_seen = 0;
    for (uint8_t i = 0; i < capture->frame_count; i++) {
      const corpus::CorpusFrame &cf = capture->frames[i];
      if (!cf.has_cmd || cf.cmd != CMD_PRIVATE_RESP)
        continue;
      IoFrame frame = corpus_test::parse_capture_frame(cf);
      if (frame.data_len < EXTENDED_TILT_RESPONSE_MIN_DATA_LEN ||
          frame.data[EXTENDED_TILT_SELECTOR_OFFSET] != STATUS_TILT_SELECTOR)
        continue;  // not an extended tilt-bearing reply
      tilt_frames_seen++;

      const uint16_t tilt_raw =
          (static_cast<uint16_t>(frame.data[EXTENDED_TILT_MSB_OFFSET]) << 8) | frame.data[EXTENDED_TILT_LSB_OFFSET];
      const float tilt = decode_tilt_report(tilt_raw);
      // Whole-percent uint8_t in the schema, same rounding rationale as reported_position above.
      EXPECT_EQ(std::lround(tilt), static_cast<long>(capture->reported_tilt))
          << "reported_tilt mismatch on frame " << static_cast<int>(i) << " (raw decoded tilt=" << tilt << ")";
    }
    EXPECT_GT(tilt_frames_seen, 0)
        << "capture has device.reported_tilt expectation but no extended tilt-bearing CMD_PRIVATE_RESP frame found";
  }
}

INSTANTIATE_TEST_SUITE_P(CorpusDecode, CorpusDecode, ::testing::ValuesIn(corpus_test::all_captures()),
                         corpus_test::capture_name_generator);

namespace {

// Mirrors detail::STATUS_UPDATE_ORIGINATOR_OFFSET (hub_internal.h). Reproduced rather than
// included: this is a codec-layer file that deliberately pulls in only the proto_* headers, and
// the constant is what the corpus evidence below is about, not a value borrowed from production.
// hub_status_test.cpp covers the production call site (detail::describe_status_update_originator);
// what this test adds is the corpus-wide evidence for the offset — data[14] names a real
// originator on every captured 0x71, data[1] never does.
constexpr uint8_t STATUS_UPDATE_ORIGINATOR_OFFSET = 14;

}  // namespace

TEST(CorpusStatusUpdateOriginator, OriginatorLivesAtOffset14NotOffset1) {
  int frames_seen = 0;
  for (const corpus::CorpusCapture *capture :
       corpus_test::captures_where([](const corpus::CorpusCapture *) { return true; })) {
    SCOPED_TRACE(::testing::Message() << "capture=" << capture->id);
    for (uint8_t i = 0; i < capture->frame_count; i++) {
      const corpus::CorpusFrame &cf = capture->frames[i];
      // No corpus fixture attaches a `cmd:` expectation to its 0x71 frames, so filter on the
      // parsed command byte rather than cf.has_cmd/cf.cmd (which would match nothing).
      IoFrame frame = corpus_test::parse_capture_frame(cf);
      if (frame.cmd != CMD_STATUS_UPDATE)
        continue;
      frames_seen++;
      ASSERT_GT(frame.data_len, STATUS_UPDATE_ORIGINATOR_OFFSET)
          << "0x71 frame " << static_cast<int>(i) << " too short to carry an originator byte";
      EXPECT_EQ(frame.data[STATUS_UPDATE_ORIGINATOR_OFFSET], ORIGINATOR_USER_REMOTE)
          << "0x71 frame " << static_cast<int>(i) << ": data[14] is not ORIGINATOR_USER_REMOTE";
      EXPECT_NE(std::strcmp(originator_name(frame.data[STATUS_UPDATE_ORIGINATOR_OFFSET]), "unknown"), 0)
          << "0x71 frame " << static_cast<int>(i) << ": data[14] does not name a known originator";
      EXPECT_EQ(std::strcmp(originator_name(frame.data[1]), "unknown"), 0)
          << "0x71 frame " << static_cast<int>(i)
          << ": data[1] coincidentally aliases an ORIGINATOR_* value on this fixture — the two "
             "assertions above (data[14] carries the real originator) are the load-bearing ones";
    }
  }
  EXPECT_GT(frames_seen, 0) << "no CMD_STATUS_UPDATE frame found in the corpus — test matched nothing";
}
