/// @file
/// @brief The lockstep adapter: the simulator's time follows the manual clock and nothing else.
/// @ingroup hioc_radio

#include <gtest/gtest.h>

#include "fullstack_rig.h"

using esphome::test_clock::ManualClock;

TEST(SimBridge, TheSimulatorsTimeFollowsTheManualClockAndNothingElse) {
  ManualClock clock;
  SimBridge bridge(fullstack::sim_path());
  bridge.pump();
  EXPECT_EQ(bridge.sim_now_us(), 0u);
  esphome::test_clock::advance_us(12345);
  EXPECT_EQ(bridge.sim_now_us(), 0u) << "the simulator stands still until it is polled";
  bridge.pump();
  EXPECT_EQ(bridge.sim_now_us(), 12345u);
}

TEST(SimBridge, AnEmptyPollOfTheStreamAdvancesBothClocksByOneTick) {
  ManualClock clock;
  SimBridge bridge(fullstack::sim_path());
  uint8_t drain[512];  // the simulator announces itself at boot
  while (bridge.stream().read(drain, sizeof(drain)) > 0) {
  }
  const uint64_t before = esphome::test_clock::state().now_us;
  const uint64_t sim_before = bridge.sim_now_us();
  EXPECT_EQ(bridge.stream().available(), 0u);
  EXPECT_EQ(esphome::test_clock::state().now_us - before, FakeByteStream::TICK_US);
  EXPECT_EQ(bridge.sim_now_us() - sim_before, FakeByteStream::TICK_US);
}

TEST(SimBridge, ResettingTheSimulatorMakesItAnnounceItselfOnTheLink) {
  ManualClock clock;
  SimBridge bridge(fullstack::sim_path());
  bridge.reset_line().digital_write(false);
  bridge.reset_line().digital_write(true);
  EXPECT_GT(bridge.stream().queued(), 0u) << "the simulator's ready message";
}

TEST(SimBridge, AFrameInThePastIsRefusedByTheSimulator) {
  ManualClock clock;
  SimBridge bridge(fullstack::sim_path());
  esphome::test_clock::advance_us(1000);
  bridge.pump();
  EXPECT_THROW(bridge.inject_air(2450000000u, 10, {0x01, 0x69, 0x6F, 0x00}), std::runtime_error);
}

TEST(SimBridge, ASimulatorBinaryThatIsMissingIsNamedInTheError) {
  ManualClock clock;
  try {
    SimBridge bridge("/nonexistent/bridle-sim");
    FAIL() << "a missing binary must throw";
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("missing or not executable: /nonexistent/bridle-sim"), std::string::npos)
        << error.what();
    EXPECT_NE(std::string(error.what()).find("BRIDLE_SIM"), std::string::npos);
  }
}

TEST(SimBridge, ASimulatorThatHasGoneIsAnErrorTheTestSeesNotASignalThatEndsTheRun) {
  ManualClock clock;
  SimBridge bridge(fullstack::sim_path());
  bridge.terminate_simulator();
  esphome::test_clock::advance_us(100);
  EXPECT_THROW(bridge.pump(), std::runtime_error);
  const uint8_t byte = 0;
  EXPECT_THROW(bridge.stream().write(&byte, 1), std::runtime_error) << "a write to the dead simulator is no SIGPIPE";
}
