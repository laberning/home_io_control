/// @file rain_poll_policy_test.cpp
/// @brief Unit tests for RainPollPolicy and its jitter functions.
///
/// Covers: first-poll delay and jitter bounds (including overflow-prone 24 h intervals), periodic
/// re-arm from the pop time, spreading of devices that share an interval, defer semantics, the
/// miss counter that turns a reading unusable, and millis() rollover.

#include "rain_poll_policy.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

using namespace esphome::home_io_control;

static constexpr uint32_t T0 = 1000000;               ///< Arbitrary base timestamp (ms).
static constexpr uint32_t INTERVAL = 15 * 60 * 1000;  ///< 15 minutes, the documented recommendation.
static constexpr uint32_t MID = UINT32_MAX / 2;       ///< Middle of the random range.
static constexpr uint32_t RANDOM_MAX = UINT32_MAX;    ///< Top of the random range.

// ============================================================================
// Pure jitter functions
// ============================================================================

TEST(RainPollPolicy, FirstDelayStaysInsideItsWindow) {
  EXPECT_EQ(rain_poll_first_delay_ms(0), RAIN_POLL_INITIAL_DELAY_MS);
  EXPECT_EQ(rain_poll_first_delay_ms(RAIN_POLL_INITIAL_SPREAD_MS),
            RAIN_POLL_INITIAL_DELAY_MS + RAIN_POLL_INITIAL_SPREAD_MS);
  for (uint32_t random : {1u, 12345u, MID, RANDOM_MAX}) {
    const uint32_t delay = rain_poll_first_delay_ms(random);
    EXPECT_GE(delay, RAIN_POLL_INITIAL_DELAY_MS) << "random " << random;
    EXPECT_LE(delay, RAIN_POLL_INITIAL_DELAY_MS + RAIN_POLL_INITIAL_SPREAD_MS) << "random " << random;
  }
}

TEST(RainPollPolicy, JitteredIntervalSpansPlusMinusTenPercent) {
  const uint32_t jitter = INTERVAL / 10;
  EXPECT_EQ(rain_poll_jittered_interval_ms(INTERVAL, 0), INTERVAL - jitter);
  EXPECT_EQ(rain_poll_jittered_interval_ms(INTERVAL, RANDOM_MAX), INTERVAL + jitter);
  const uint32_t centre = rain_poll_jittered_interval_ms(INTERVAL, MID);
  EXPECT_NEAR(static_cast<double>(centre), static_cast<double>(INTERVAL), 1.0)
      << "mid-range random leaves the interval";
}

TEST(RainPollPolicy, JitteredIntervalNeverLeavesItsRangeNorOverflows) {
  for (uint32_t interval : {60000u, INTERVAL, 24u * 60u * 60u * 1000u}) {
    const uint32_t jitter = interval / 10;
    for (uint32_t random : {0u, 1u, MID, RANDOM_MAX - 1, RANDOM_MAX}) {
      const uint32_t delay = rain_poll_jittered_interval_ms(interval, random);
      EXPECT_GE(delay, interval - jitter) << "interval " << interval << " random " << random;
      EXPECT_LE(delay, interval + jitter) << "interval " << interval << " random " << random;
    }
  }
}

// ============================================================================
// Scheduling
// ============================================================================

TEST(RainPollPolicy, NothingIsDueWithoutAnInterval) {
  RainPollPolicy policy;
  EXPECT_FALSE(policy.pop_due_device(T0 + INTERVAL, 0).has_value());
  EXPECT_EQ(policy.get_interval("DEV"), 0u);
}

TEST(RainPollPolicy, FirstPollIsDueAfterTheInitialDelay) {
  RainPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);

  EXPECT_EQ(policy.get_interval("DEV"), INTERVAL);
  EXPECT_FALSE(policy.pop_due_device(T0 + RAIN_POLL_INITIAL_DELAY_MS - 1, 0).has_value());
  const auto due = policy.pop_due_device(T0 + RAIN_POLL_INITIAL_DELAY_MS, 0);
  ASSERT_TRUE(due.has_value());
  EXPECT_EQ(*due, "DEV");
}

TEST(RainPollPolicy, PopRearmsFromThePopTimeBeforeTheCallerReports) {
  RainPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  const uint32_t pop_time = T0 + RAIN_POLL_INITIAL_DELAY_MS + 4000;  // loop() ran late

  ASSERT_TRUE(policy.pop_due_device(pop_time, MID).has_value());

  const uint32_t jitter = INTERVAL / 10;
  EXPECT_NEAR(static_cast<double>(policy.get_next_poll("DEV")), static_cast<double>(pop_time + INTERVAL), 1.0);
  EXPECT_FALSE(policy.pop_due_device(pop_time + INTERVAL - jitter - 1, MID).has_value());
  EXPECT_TRUE(policy.pop_due_device(pop_time + INTERVAL + jitter, MID).has_value());
}

TEST(RainPollPolicy, ZeroIntervalRemovesTheDevice) {
  RainPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  policy.set_interval("DEV", 0, T0, 0);

  EXPECT_EQ(policy.get_interval("DEV"), 0u);
  EXPECT_FALSE(policy.pop_due_device(T0 + 10 * INTERVAL, 0).has_value());
}

TEST(RainPollPolicy, DevicesSharingAnIntervalComeDueAtDifferentTimes) {
  RainPollPolicy policy;
  policy.set_interval("A", INTERVAL, T0, 0);
  policy.set_interval("B", INTERVAL, T0, RANDOM_MAX);
  EXPECT_NE(policy.get_next_poll("A"), policy.get_next_poll("B")) << "first polls are spread";

  // Same pop moment, different random draws: the following polls are spread as well.
  const uint32_t now = T0 + RAIN_POLL_INITIAL_DELAY_MS + RAIN_POLL_INITIAL_SPREAD_MS;
  ASSERT_TRUE(policy.pop_due_device(now, 0).has_value());
  ASSERT_TRUE(policy.pop_due_device(now, RANDOM_MAX).has_value());
  EXPECT_NE(policy.get_next_poll("A"), policy.get_next_poll("B"));
}

TEST(RainPollPolicy, TwoDueDevicesPopOnePerCall) {
  RainPollPolicy policy;
  policy.set_interval("A", INTERVAL, T0, 0);
  policy.set_interval("B", INTERVAL, T0, 0);
  const uint32_t now = T0 + RAIN_POLL_INITIAL_DELAY_MS;

  const auto first = policy.pop_due_device(now, MID);
  const auto second = policy.pop_due_device(now, MID);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(*first, *second);
  EXPECT_FALSE(policy.pop_due_device(now, MID).has_value());
}

TEST(RainPollPolicy, ScheduleSurvivesTheMillisRollover) {
  RainPollPolicy policy;
  const uint32_t near_wrap = UINT32_MAX - 5000;
  policy.set_interval("DEV", INTERVAL, near_wrap, 0);  // next poll lands after the rollover

  EXPECT_FALSE(policy.pop_due_device(near_wrap, 0).has_value());
  EXPECT_FALSE(policy.pop_due_device(near_wrap + RAIN_POLL_INITIAL_DELAY_MS - 1, 0).has_value());
  EXPECT_TRUE(policy.pop_due_device(near_wrap + RAIN_POLL_INITIAL_DELAY_MS, 0).has_value());
}

// ============================================================================
// Defer
// ============================================================================

TEST(RainPollPolicy, DeferPullsTheNextPollEarlier) {
  RainPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  const uint32_t now = T0 + RAIN_POLL_INITIAL_DELAY_MS;
  ASSERT_TRUE(policy.pop_due_device(now, MID).has_value());  // next poll is now about one interval away

  policy.defer("DEV", RAIN_POLL_MOVING_RETRY_MS, now);

  EXPECT_EQ(policy.get_next_poll("DEV"), now + RAIN_POLL_MOVING_RETRY_MS);
}

TEST(RainPollPolicy, DeferNeverPushesThePollLater) {
  RainPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);  // first poll is 15 s away
  const uint32_t next = policy.get_next_poll("DEV");

  policy.defer("DEV", RAIN_POLL_MOVING_RETRY_MS, T0);  // 30 s would be later

  EXPECT_EQ(policy.get_next_poll("DEV"), next);
}

TEST(RainPollPolicy, DeferIgnoresAnUnscheduledDevice) {
  RainPollPolicy policy;
  policy.defer("DEV", RAIN_POLL_MOVING_RETRY_MS, T0);
  EXPECT_FALSE(policy.pop_due_device(T0 + 10 * INTERVAL, 0).has_value());
}

// ============================================================================
// Miss counter
// ============================================================================

TEST(RainPollPolicy, ReadingBecomesUnusableOnTheThirdConsecutiveMiss) {
  RainPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);

  EXPECT_FALSE(policy.on_poll_failed("DEV"));
  EXPECT_FALSE(policy.on_poll_failed("DEV"));
  EXPECT_EQ(RAIN_POLL_FAILURES_BEFORE_UNKNOWN, 3);
  EXPECT_TRUE(policy.on_poll_failed("DEV"));
  EXPECT_TRUE(policy.on_poll_failed("DEV")) << "stays unusable while misses continue";
  EXPECT_EQ(policy.get_failures("DEV"), 4u);
}

TEST(RainPollPolicy, AnAnsweredPollResetsTheMissCounter) {
  RainPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  policy.on_poll_failed("DEV");
  policy.on_poll_failed("DEV");

  policy.on_poll_succeeded("DEV");

  EXPECT_EQ(policy.get_failures("DEV"), 0u);
  EXPECT_FALSE(policy.on_poll_failed("DEV"));
}

TEST(RainPollPolicy, MissCounterSaturates) {
  RainPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  for (int i = 0; i < 300; ++i)
    policy.on_poll_failed("DEV");
  EXPECT_EQ(policy.get_failures("DEV"), std::numeric_limits<uint8_t>::max());
  EXPECT_TRUE(policy.on_poll_failed("DEV"));
}

TEST(RainPollPolicy, UnscheduledDeviceCountsNoMisses) {
  RainPollPolicy policy;
  EXPECT_FALSE(policy.on_poll_failed("DEV"));
  policy.on_poll_succeeded("DEV");
  EXPECT_EQ(policy.get_failures("DEV"), 0u);
}
