/// @file radio_poll_test.cpp
/// @brief Unit tests for poll_until(), the preemption-safe radio wait helper.
/// @ingroup hioc_radio

#include "radio_poll.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"

#include <gtest/gtest.h>

using namespace esphome;
using namespace esphome::home_io_control;

TEST(RadioPoll, ReadyOnFirstSampleCostsExactlyOneClockRead) {
  // Legacy clock: each millis() call advances it by one, so the count is observable.
  uint32_t const before = millis();
  PollResult const r = poll_until([] { return true; }, 10);
  uint32_t const after = millis();
  EXPECT_TRUE(r.ready);
  EXPECT_EQ(r.elapsed_ms, 0u);
  EXPECT_EQ(after - before, 2u) << "poll_until read the clock once, plus the one read above";
}

TEST(RadioPoll, NeverReadyTimesOutAndFeedsTheWatchdog) {
  test_clock::ManualClock clock;
  uint32_t const feeds_before = App.feed_wdt_calls;
  PollResult const r = poll_until(
      [] {
        test_clock::advance_ms(1);
        return false;
      },
      10);
  EXPECT_FALSE(r.ready);
  EXPECT_GT(r.elapsed_ms, 10u);
  EXPECT_GT(App.feed_wdt_calls, feeds_before);
}

TEST(RadioPoll, ReadyOnlyOnTheDecidingSampleAfterAPreemptionCountsAsReady) {
  // The #190 shape: the first sample says busy, the task is then stalled far past the timeout, and
  // the device is ready by the time it is sampled again.
  test_clock::ManualClock clock;
  int reads = 0;
  PollResult const r = poll_until(
      [&] {
        if (reads++ == 0) {
          test_clock::advance_ms(500);
          return false;
        }
        return true;
      },
      10);
  EXPECT_TRUE(r.ready);
  EXPECT_GT(r.elapsed_ms, 10u) << "the overrun stays visible to the caller";
  EXPECT_EQ(reads, 2);
}

TEST(RadioPoll, ReportsTheElapsedTimeOfALateSuccess) {
  test_clock::ManualClock clock;
  int reads = 0;
  PollResult const r = poll_until(
      [&] {
        test_clock::advance_ms(2);
        return ++reads == 4;
      },
      100);
  EXPECT_TRUE(r.ready);
  EXPECT_LE(r.elapsed_ms, 100u);
  EXPECT_GE(r.elapsed_ms, 4u);
}
