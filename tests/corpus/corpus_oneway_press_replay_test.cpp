/// @file corpus_oneway_press_replay_test.cpp
/// @brief Real one-way remote captures replayed through the press filter and the decoder.
///
/// A remote counts one rolling sequence across all of its frames: the copies of a short press share
/// it, the next press and every frame of a held press carry the following values. The captures pin
/// how many presses the hub counts, on the timeline the remote used.
/// @ingroup hioc_hub

#include "oneway_receiver.h"
#include "proto_codecs.h"
#include "proto_frame.h"

#include "corpus_generated.h"
#include "corpus_test_helpers.h"
#include "test_helpers.h"

#include <cstdint>
#include <vector>

using namespace esphome::home_io_control;

namespace {

constexpr uint32_t REPLAY_START_MS = 100000;

/// Counts the frames of a capture that the filter lets through as a new press and that carry a command
/// (the frames that fire a sender event).
struct ReplayResult {
  std::vector<uint16_t> command_sequences;  ///< Sequence of each command press, in order.
  int command_presses{0};
};

ReplayResult replay_presses(const char *id) {
  ReplayResult result;
  const auto *capture = corpus_test::capture_by_id(id);
  EXPECT_NE(capture, nullptr) << id << " is missing from the corpus";
  if (capture == nullptr)
    return result;
  OneWayReceiver rx;
  for (uint8_t i = 0; i < capture->frame_count; i++) {
    const auto &cf = capture->frames[i];
    const IoFrame frame = corpus_test::parse_capture_frame(cf);
    const OneWayFrameInfo info = decode_1w_frame(frame);
    if (rx.is_new_press(frame, info, REPLAY_START_MS + cf.t_ms) && info.has_intent) {
      result.command_presses++;
      result.command_sequences.push_back(info.sequence);
    }
  }
  return result;
}

}  // namespace

TEST(CorpusOneWayPress, AHeldButtonIsTwoCommandPresses) {
  // The capture holds OPEN (sequence 0x5E73), seven WRITE_PRIVATE beacons (0x5E74..0x5E7E, no command)
  // and the STOP sent at release (0x5E7F): two command presses.
  constexpr int EXPECTED_COMMAND_PRESSES = 2;
  const ReplayResult result = replay_presses("somfy_smoove_oneway_hold_to_dim_sx1276");

  EXPECT_EQ(result.command_presses, EXPECTED_COMMAND_PRESSES);
  ASSERT_EQ(result.command_sequences.size(), 2u);
  EXPECT_EQ(result.command_sequences[0], 0x5E73);
  EXPECT_EQ(result.command_sequences[1], 0x5E7F);
}

TEST(CorpusOneWayPress, TheRepeatsOfAShortPressAreOnePress) {
  // OPEN (0x1F44, twice) and STOP of the next press (0x1F47): the copies share a sequence, the next
  // press counts up.
  const ReplayResult result = replay_presses("unidentified_1w_remote_oneway_execute_open_and_stop_acei_61_sx1276");

  EXPECT_EQ(result.command_sequences, (std::vector<uint16_t>{0x1F44, 0x1F47}));
}
