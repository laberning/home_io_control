/// @file corpus_move_replay_test.cpp
/// @brief A whole move of a real cover, replayed on the capture's own timeline through the hub.
///
/// The Somfy Sunea io awning capture holds one close from the command to the report that it is at
/// rest. The hub drives the move itself: it sends the command, then reads the position whenever its
/// status poll policy says a read is due, and every device reply arrives its captured gap after the
/// matching send (the same release rule as the exchange replay). What the capture adds over the
/// single-exchange replays is the schedule: how many reads one move costs and when the last one,
/// the read that reports the final position, goes out.
/// @ingroup hioc_hub

#include "corpus_generated.h"
#include "hub_core.h"
#include "hub_internal.h"
#include "proto_frame.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"
#include "timed_replay.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace esphome::home_io_control;
using test::TestableHubComponent;

namespace {

constexpr const char *AWNING_MOVE_CAPTURE =
    "somfy_awning_statuspoll_close_with_remaining_time_predicted_end_read_sx1262";

/// What the hub did across one replayed move.
struct MoveRun {
  std::vector<uint32_t> read_send_ms;      ///< When each status read (the first frame of its exchange) went out.
  std::vector<uint32_t> report_done_ms;    ///< When the hub had the reply of that read (the clock after the exchange).
  std::vector<std::vector<uint8_t>> sent;  ///< Every frame the hub sent, in order.
  bool at_rest{false};
  uint16_t rest_position_pct{0};
};

/// The device node id the capture's command frame addresses, as the hex string the hub keys devices by.
std::string device_id_of(const IoFrame &execute) {
  char text[2 * NODE_ID_SIZE + 1];
  std::snprintf(text, sizeof(text), "%02X%02X%02X", execute.dst[0], execute.dst[1], execute.dst[2]);
  return text;
}

/// Plays the capture: the hub sends the move's command, then polls when the policy says so, until
/// the follow-up window ends or the capture has no more replies to give.
MoveRun replay_move(const corpus::CorpusCapture &capture) {
  const IoFrame execute = corpus_test::parse_capture_frame(capture.frames[0]);
  const std::string id = device_id_of(execute);

  esphome::test_clock::ManualClock clock;
  MockRadioSX1262 radio;
  radio.set_model_tx_airtime(true);
  const std::string error = corpus_test::queue_timed_rx(radio, capture, corpus_test::ReplayAnchor::SEND_INDEX);
  EXPECT_TRUE(error.empty()) << error;

  TestableHubComponent comp;
  comp.initialized_ = true;
  comp.radio_ = &radio;
  std::memcpy(comp.node_id_, execute.src, NODE_ID_SIZE);
  std::memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
  comp.add_device(id);
  // The type the Sunea io awning reports and docs/supported-devices.md lists.
  test::set_kind(*comp.get_device(id), DeviceType::VERTICAL_EXTERIOR_AWNING);

  MoveRun run;
  EXPECT_TRUE(comp.set_device_position(id, 0)) << "the command exchange of the capture succeeds";

  // The scheduler's job: sleep until the earliest planned read, then take it. The cap only stops a
  // policy that never runs dry from looping for ever.
  for (int step = 0; step < 20; ++step) {
    const uint32_t next = comp.poll_policy_.get_next_update(id);
    const uint32_t aim = comp.poll_policy_.get_settle_update(id);
    const uint32_t wake = aim != 0 && (next == 0 || aim < next) ? aim : next;
    if (wake == 0 || !comp.poll_policy_.is_tracking_active(id, esphome::millis()))
      break;
    // A progress read is due once the clock has passed its deadline, so wake one tick later.
    if (wake >= esphome::millis())
      esphome::test_clock::set_ms(wake + 1);
    const auto due = comp.poll_policy_.pop_due_device(esphome::millis());
    if (!due.has_value())
      continue;
    const size_t frames_before = radio.get_sent_data().size();
    if (!comp.request_device_status(*due))
      break;
    run.read_send_ms.push_back(radio.send_times_ms()[frames_before]);
    run.report_done_ms.push_back(esphome::millis());
  }

  run.sent = radio.get_sent_data();
  const IoDevice *dev = comp.get_device(id);
  run.at_rest = dev->is_stopped;
  run.rest_position_pct = dev->position;
  return run;
}

}  // namespace

TEST(CorpusMoveReplay, AwningMoveCostsFiveProgressReadsAndOnePredictedEndRead) {
  const corpus::CorpusCapture *capture = corpus_test::capture_by_id(AWNING_MOVE_CAPTURE);
  ASSERT_NE(capture, nullptr);
  const MoveRun run = replay_move(*capture);

  // Capture: 5 reports that still say moving (counts 15, 12, 9, 6, 3 s), then 1 that finds it at rest.
  ASSERT_EQ(run.read_send_ms.size(), 6u) << "five progress reads and the read at the predicted end";
  EXPECT_EQ(run.sent.size(), capture->frame_count / 2u) << "one frame sent per captured tx, no extra attempts";
  EXPECT_TRUE(run.at_rest);
}

TEST(CorpusMoveReplay, HubSendsTheCapturedFramesByteForByte) {
  const corpus::CorpusCapture *capture = corpus_test::capture_by_id(AWNING_MOVE_CAPTURE);
  ASSERT_NE(capture, nullptr);
  const MoveRun run = replay_move(*capture);

  // key: corpus, so the hub's command, reads and 0x3D responses must match the capture exactly:
  // the replay is the captured move, not a script that answers whatever the hub sends.
  size_t sent_index = 0;
  for (uint8_t i = 0; i < capture->frame_count; ++i) {
    const corpus::CorpusFrame &cf = capture->frames[i];
    if (!cf.tx)
      continue;
    ASSERT_LT(sent_index, run.sent.size()) << "captured tx frame " << static_cast<int>(i) << " was never sent";
    const std::vector<uint8_t> &sent = run.sent[sent_index++];
    ASSERT_EQ(sent.size(), corpus_test::wire_len(cf)) << "captured frame " << static_cast<int>(i);
    EXPECT_EQ(std::memcmp(sent.data(), cf.bytes, sent.size()), 0) << "captured frame " << static_cast<int>(i);
  }
  EXPECT_EQ(sent_index, run.sent.size());
}

TEST(CorpusMoveReplay, ProgressReadsKeepTheThreeSecondCadence) {
  const MoveRun run = replay_move(*corpus_test::capture_by_id(AWNING_MOVE_CAPTURE));
  ASSERT_EQ(run.read_send_ms.size(), 6u);

  // The manual clock makes the gaps exact (the deadline plus the one tick the loop waits past it);
  // the tolerance only keeps the test off that tick and stays far below the 1 s between a progress
  // read and the predicted-end read.
  for (size_t i = 1; i < 5; ++i) {
    const uint32_t gap = run.read_send_ms[i] - run.report_done_ms[i - 1];
    EXPECT_NEAR(gap, 3000, 50) << "progress read " << i << " follows its predecessor's report by 3 s";
  }
}

TEST(CorpusMoveReplay, FinalPositionIsReadOneSecondAfterTheLastCountRunsOut) {
  const MoveRun run = replay_move(*corpus_test::capture_by_id(AWNING_MOVE_CAPTURE));
  ASSERT_EQ(run.read_send_ms.size(), 6u);

  // The last moving report says 3 s left. The read that finds the awning at rest goes out 3 s + 1 s
  // after it: not 3 s (progress cadence, the awning still moving) and not a second progress read in
  // between (the one due at +3 s is the read the aim replaces).
  const uint32_t after_last_count = run.read_send_ms[5] - run.report_done_ms[4];
  EXPECT_NEAR(after_last_count, 3000 + 1000, 50);
}

TEST(CorpusMoveReplay, TheAwningEndsAtRestOnItsTarget) {
  const MoveRun run = replay_move(*corpus_test::capture_by_id(AWNING_MOVE_CAPTURE));
  EXPECT_TRUE(run.at_rest);
  EXPECT_EQ(run.rest_position_pct, 0u) << "the final report is raw 0x0000";
}
