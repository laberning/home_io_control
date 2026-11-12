/// @file corpus_execution_state_test.cpp
/// @brief The execution-state decode applied to real main-parameter status read replies.
///
/// A VELUX SML roller shutter on a shared KUX 110 supply answered the read at rest, while queued
/// behind the other shutter and once its motor ran; covers polled by a KLF 200 gateway answered it
/// at rest.

#include "corpus_generated.h"
#include "hub_decisions.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

namespace {

/// Parse a capture frame (no CRC on the wire in these captures) and read its execution state.
ExecutionState execution_state_of(const uint8_t *bytes, size_t len) {
  IoFrame frame;
  EXPECT_TRUE(parse(bytes, static_cast<uint8_t>(len), frame));
  EXPECT_EQ(frame.cmd, CMD_PRIVATE_RESP);
  EXPECT_GE(frame.data_len, 1);
  return execution_state_from_status_read(frame.data[0]);
}

template<size_t N> ExecutionState execution_state_of(const uint8_t (&bytes)[N]) { return execution_state_of(bytes, N); }

}  // namespace

TEST(CorpusExecutionState, AShutterAtRestReadsDoneAndAQueuedOneWaitingForPower) {
  using namespace corpus;
  const ExecutionState at_rest = execution_state_of(velux_sml_probe_status_mp_fp_waiting_for_power_frame0_bytes);
  const ExecutionState queued = execution_state_of(velux_sml_probe_status_mp_fp_waiting_for_power_frame1_bytes);

  EXPECT_EQ(at_rest, ExecutionState::DONE);
  EXPECT_EQ(queued, ExecutionState::WAITING_FOR_POWER);
  EXPECT_EQ(decisions::power_wait_step(false, true, at_rest), decisions::PowerWaitStep::CONCLUDE);
  EXPECT_EQ(decisions::power_wait_step(false, true, queued), decisions::PowerWaitStep::WAIT);
}

TEST(CorpusExecutionState, AQueuedShutterReadsWaitingForPowerThenExecuting) {
  using namespace corpus;
  const ExecutionState queued =
      execution_state_of(velux_sml_probe_execution_state_waiting_then_executing_sx1262_frame0_bytes);
  const ExecutionState started =
      execution_state_of(velux_sml_probe_execution_state_waiting_then_executing_sx1262_frame1_bytes);

  EXPECT_EQ(queued, ExecutionState::WAITING_FOR_POWER);
  EXPECT_EQ(started, ExecutionState::EXECUTING);
  EXPECT_EQ(decisions::power_wait_step(true, true, queued), decisions::PowerWaitStep::WAIT);
  EXPECT_EQ(decisions::power_wait_step(true, true, started), decisions::PowerWaitStep::FOLLOW);
}

TEST(CorpusExecutionState, ACoverPolledByAGatewayAtRestReadsDone) {
  using namespace corpus;
  // Frame 1 is the first reply in the capture: flags 2D, main parameter 00 00.
  EXPECT_EQ(execution_state_of(velux_klf200_statuspoll_roundrobin_868_sx1262_frame1_bytes), ExecutionState::DONE);
}
