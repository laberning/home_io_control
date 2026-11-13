/// @file corpus_execution_state_test.cpp
/// @brief The execution-state decode applied to the status replies of the corpus.
///
/// The hub reads byte 0 of a status reply as an execution state (decisions::reads_stopped_by_status_byte()).
/// Every captured ordinary status reply carries a state that the stopped bit it replaced reads the same
/// way (4 or 5 under a result code). The main-parameter replies of a VELUX SML roller shutter that waits
/// for power on a shared supply are the one place the two disagree: state 3 has the bit set.

#include "corpus_generated.h"
#include "corpus_test_helpers.h"
#include "hub_decisions.h"
#include "proto_constants.h"
#include "proto_device_model.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace esphome::home_io_control;

namespace {

/// Captures that hold main-parameter status reads (CMD_PRIVATE function 0x01). Their replies never
/// reach the hub's status decoder, and a shutter that waits for power answers them with state 3, which
/// the stopped bit would read as at rest. A capture of an ordinary status reply whose byte 0 the two
/// readings disagree on is not listed here on purpose: it changes a decision and needs its own test.
constexpr const char *MAIN_PARAMETER_READ_CAPTURES[] = {
    "velux_sml_probe_status_mp_fp_waiting_for_power",
    "velux_sml_probe_execution_state_waiting_then_executing_sx1262",
};

bool is_main_parameter_read_capture(const char *id) {
  for (const char *listed : MAIN_PARAMETER_READ_CAPTURES) {
    if (std::strcmp(listed, id) == 0)
      return true;
  }
  return false;
}

/// Parse a capture frame and read its execution state from byte 0 of the payload.
ExecutionState execution_state_of_frame(const corpus::CorpusCapture *capture, uint8_t index) {
  EXPECT_NE(capture, nullptr);
  if (capture == nullptr || index >= capture->frame_count)
    return ExecutionState::UNDEFINED;
  const IoFrame frame = corpus_test::parse_capture_frame(capture->frames[index]);
  EXPECT_EQ(frame.cmd, CMD_PRIVATE_RESP);
  EXPECT_GE(frame.data_len, 1);
  return execution_state_of(frame.data[0]);
}

}  // namespace

TEST(CorpusExecutionState, EveryCapturedStatusReplyReadsAsItDidBeforeTheStateWasDecoded) {
  size_t replies = 0;
  for (const corpus::CorpusCapture *capture : corpus_test::all_captures()) {
    if (is_main_parameter_read_capture(capture->id))
      continue;
    for (uint8_t i = 0; i < capture->frame_count; i++) {
      const corpus::CorpusFrame &cf = capture->frames[i];
      if (cf.tx || !cf.has_cmd || (cf.cmd != CMD_PRIVATE_RESP && cf.cmd != CMD_STATUS_UPDATE))
        continue;
      const IoFrame frame = corpus_test::parse_capture_frame(cf);
      if (frame.data_len == 0)
        continue;
      replies++;
      const uint8_t status_byte = frame.data[0];
      const bool stopped_bit = (status_byte & STATUS_STOPPED) != 0;
      EXPECT_EQ(decisions::reads_stopped_by_status_byte(status_byte), stopped_bit)
          << capture->id << " frame " << static_cast<unsigned>(i) << ": byte 0 = 0x" << std::hex
          << static_cast<unsigned>(status_byte);
    }
  }
  EXPECT_GT(replies, 50u) << "the sweep found too few status replies to mean anything";
}

TEST(CorpusExecutionState, AShutterAtRestReadsDoneAndAQueuedOneWaitingForPower) {
  const corpus::CorpusCapture *capture = corpus_test::capture_by_id("velux_sml_probe_status_mp_fp_waiting_for_power");
  ASSERT_NE(capture, nullptr);
  EXPECT_EQ(execution_state_of_frame(capture, 0), ExecutionState::DONE);
  EXPECT_EQ(execution_state_of_frame(capture, 1), ExecutionState::WAITING_FOR_POWER);
  const IoFrame at_rest = corpus_test::parse_capture_frame(capture->frames[0]);
  const IoFrame queued = corpus_test::parse_capture_frame(capture->frames[1]);
  EXPECT_TRUE(decisions::reads_stopped_by_status_byte(at_rest.data[0]));
  EXPECT_FALSE(decisions::reads_stopped_by_status_byte(queued.data[0]))
      << "a waiting shutter is followed as moving although the stopped bit of 0x2B is set";
}

TEST(CorpusExecutionState, AQueuedShutterReadsWaitingForPowerThenExecuting) {
  const corpus::CorpusCapture *capture =
      corpus_test::capture_by_id("velux_sml_probe_execution_state_waiting_then_executing_sx1262");
  ASSERT_NE(capture, nullptr);
  EXPECT_EQ(execution_state_of_frame(capture, 0), ExecutionState::WAITING_FOR_POWER);
  EXPECT_EQ(execution_state_of_frame(capture, 1), ExecutionState::EXECUTING);
}
