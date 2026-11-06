/// @file periodic_poll_policy_test.cpp
/// @brief Unit tests for PeriodicPollPolicy and its jitter functions.
///
/// Covers: first-poll delay and jitter bounds (including overflow-prone 24 h intervals), periodic
/// re-arm from the pop time, spreading of devices that share an interval, defer semantics, the
/// restart after a report the device gave unpolled, the one retry after an unanswered poll, the miss
/// counter that turns a reading unusable, and millis() rollover.

#include "periodic_poll_policy.h"

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

TEST(PeriodicPollPolicy, FirstDelayStaysInsideItsWindow) {
  EXPECT_EQ(periodic_poll_first_delay_ms(0), PERIODIC_POLL_INITIAL_DELAY_MS);
  EXPECT_EQ(periodic_poll_first_delay_ms(PERIODIC_POLL_INITIAL_SPREAD_MS),
            PERIODIC_POLL_INITIAL_DELAY_MS + PERIODIC_POLL_INITIAL_SPREAD_MS);
  for (uint32_t random : {1u, 12345u, MID, RANDOM_MAX}) {
    const uint32_t delay = periodic_poll_first_delay_ms(random);
    EXPECT_GE(delay, PERIODIC_POLL_INITIAL_DELAY_MS) << "random " << random;
    EXPECT_LE(delay, PERIODIC_POLL_INITIAL_DELAY_MS + PERIODIC_POLL_INITIAL_SPREAD_MS) << "random " << random;
  }
}

TEST(PeriodicPollPolicy, JitteredIntervalSpansPlusMinusTenPercent) {
  const uint32_t jitter = INTERVAL / 10;
  EXPECT_EQ(periodic_poll_jittered_interval_ms(INTERVAL, 0), INTERVAL - jitter);
  EXPECT_EQ(periodic_poll_jittered_interval_ms(INTERVAL, RANDOM_MAX), INTERVAL + jitter);
  const uint32_t centre = periodic_poll_jittered_interval_ms(INTERVAL, MID);
  EXPECT_NEAR(static_cast<double>(centre), static_cast<double>(INTERVAL), 1.0)
      << "mid-range random leaves the interval";
}

TEST(PeriodicPollPolicy, JitteredIntervalNeverLeavesItsRangeNorOverflows) {
  for (uint32_t interval : {60000u, INTERVAL, 24u * 60u * 60u * 1000u}) {
    const uint32_t jitter = interval / 10;
    for (uint32_t random : {0u, 1u, MID, RANDOM_MAX - 1, RANDOM_MAX}) {
      const uint32_t delay = periodic_poll_jittered_interval_ms(interval, random);
      EXPECT_GE(delay, interval - jitter) << "interval " << interval << " random " << random;
      EXPECT_LE(delay, interval + jitter) << "interval " << interval << " random " << random;
    }
  }
}

// ============================================================================
// Scheduling
// ============================================================================

TEST(PeriodicPollPolicy, NothingIsDueWithoutAnInterval) {
  PeriodicPollPolicy policy;
  EXPECT_FALSE(policy.pop_due_device(T0 + INTERVAL, 0).has_value());
  EXPECT_EQ(policy.get_interval("DEV"), 0u);
}

TEST(PeriodicPollPolicy, FirstPollIsDueAfterTheInitialDelay) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);

  EXPECT_EQ(policy.get_interval("DEV"), INTERVAL);
  EXPECT_FALSE(policy.pop_due_device(T0 + PERIODIC_POLL_INITIAL_DELAY_MS - 1, 0).has_value());
  const auto due = policy.pop_due_device(T0 + PERIODIC_POLL_INITIAL_DELAY_MS, 0);
  ASSERT_TRUE(due.has_value());
  EXPECT_EQ(*due, "DEV");
}

TEST(PeriodicPollPolicy, PopRearmsFromThePopTimeBeforeTheCallerReports) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  const uint32_t pop_time = T0 + PERIODIC_POLL_INITIAL_DELAY_MS + 4000;  // loop() ran late

  ASSERT_TRUE(policy.pop_due_device(pop_time, MID).has_value());

  const uint32_t jitter = INTERVAL / 10;
  EXPECT_NEAR(static_cast<double>(policy.get_next_poll("DEV")), static_cast<double>(pop_time + INTERVAL), 1.0);
  EXPECT_FALSE(policy.pop_due_device(pop_time + INTERVAL - jitter - 1, MID).has_value());
  EXPECT_TRUE(policy.pop_due_device(pop_time + INTERVAL + jitter, MID).has_value());
}

TEST(PeriodicPollPolicy, ZeroIntervalRemovesTheDevice) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  policy.set_interval("DEV", 0, T0, 0);

  EXPECT_EQ(policy.get_interval("DEV"), 0u);
  EXPECT_FALSE(policy.pop_due_device(T0 + 10 * INTERVAL, 0).has_value());
}

TEST(PeriodicPollPolicy, DevicesSharingAnIntervalComeDueAtDifferentTimes) {
  PeriodicPollPolicy policy;
  policy.set_interval("A", INTERVAL, T0, 0);
  policy.set_interval("B", INTERVAL, T0, RANDOM_MAX);
  EXPECT_NE(policy.get_next_poll("A"), policy.get_next_poll("B")) << "first polls are spread";

  // Same pop moment, different random draws: the following polls are spread as well.
  const uint32_t now = T0 + PERIODIC_POLL_INITIAL_DELAY_MS + PERIODIC_POLL_INITIAL_SPREAD_MS;
  ASSERT_TRUE(policy.pop_due_device(now, 0).has_value());
  ASSERT_TRUE(policy.pop_due_device(now, RANDOM_MAX).has_value());
  EXPECT_NE(policy.get_next_poll("A"), policy.get_next_poll("B"));
}

TEST(PeriodicPollPolicy, TwoDueDevicesPopOnePerCall) {
  PeriodicPollPolicy policy;
  policy.set_interval("A", INTERVAL, T0, 0);
  policy.set_interval("B", INTERVAL, T0, 0);
  const uint32_t now = T0 + PERIODIC_POLL_INITIAL_DELAY_MS;

  const auto first = policy.pop_due_device(now, MID);
  const auto second = policy.pop_due_device(now, MID);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(*first, *second);
  EXPECT_FALSE(policy.pop_due_device(now, MID).has_value());
}

TEST(PeriodicPollPolicy, ScheduleSurvivesTheMillisRollover) {
  PeriodicPollPolicy policy;
  const uint32_t near_wrap = UINT32_MAX - 5000;
  policy.set_interval("DEV", INTERVAL, near_wrap, 0);  // next poll lands after the rollover

  EXPECT_FALSE(policy.pop_due_device(near_wrap, 0).has_value());
  EXPECT_FALSE(policy.pop_due_device(near_wrap + PERIODIC_POLL_INITIAL_DELAY_MS - 1, 0).has_value());
  EXPECT_TRUE(policy.pop_due_device(near_wrap + PERIODIC_POLL_INITIAL_DELAY_MS, 0).has_value());
}

// ============================================================================
// Defer
// ============================================================================

TEST(PeriodicPollPolicy, DeferPullsTheNextPollEarlier) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  const uint32_t now = T0 + PERIODIC_POLL_INITIAL_DELAY_MS;
  ASSERT_TRUE(policy.pop_due_device(now, MID).has_value());  // next poll is now about one interval away

  policy.defer("DEV", PERIODIC_POLL_MOVING_RETRY_MS, now);

  EXPECT_EQ(policy.get_next_poll("DEV"), now + PERIODIC_POLL_MOVING_RETRY_MS);
}

TEST(PeriodicPollPolicy, DeferNeverPushesThePollLater) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);  // first poll is 15 s away
  const uint32_t next = policy.get_next_poll("DEV");

  policy.defer("DEV", PERIODIC_POLL_MOVING_RETRY_MS, T0);  // 30 s would be later

  EXPECT_EQ(policy.get_next_poll("DEV"), next);
}

TEST(PeriodicPollPolicy, DeferIgnoresAnUnscheduledDevice) {
  PeriodicPollPolicy policy;
  policy.defer("DEV", PERIODIC_POLL_MOVING_RETRY_MS, T0);
  EXPECT_FALSE(policy.pop_due_device(T0 + 10 * INTERVAL, 0).has_value());
}

// ============================================================================
// Restart after a report the device gave unpolled
// ============================================================================

TEST(PeriodicPollPolicy, RestartMovesTheNextPollAWholeIntervalOut) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);  // first poll at T0 + 15 s
  const uint32_t reported_at = T0 + 10000;

  policy.restart_interval("DEV", reported_at, 0);

  EXPECT_EQ(policy.get_next_poll("DEV"), reported_at + 810000) << "15 min less 10 %, at random 0";
  EXPECT_FALSE(policy.has_due(T0 + 15000)) << "the device reported 5 s before; no poll is needed";
}

TEST(PeriodicPollPolicy, RestartCountsFromTheReportEvenWhenThatIsEarlier) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  const uint32_t now = T0 + 15000;
  ASSERT_TRUE(policy.pop_due_device(now, RANDOM_MAX).has_value());  // next poll at now + 990 s

  policy.restart_interval("DEV", now + 1000, 0);

  EXPECT_EQ(policy.get_next_poll("DEV"), now + 1000 + 810000);
}

TEST(PeriodicPollPolicy, DevicesThatReportedTogetherComeDueAtDifferentTimes) {
  PeriodicPollPolicy policy;
  policy.set_interval("A", INTERVAL, T0, 0);
  policy.set_interval("B", INTERVAL, T0, 0);

  // A group command: both report in the same moment, the random draws differ.
  policy.restart_interval("A", T0 + 1000, 0);
  policy.restart_interval("B", T0 + 1000, RANDOM_MAX);

  EXPECT_EQ(policy.get_next_poll("A"), T0 + 1000 + 810000);
  EXPECT_EQ(policy.get_next_poll("B"), T0 + 1000 + 990000);
}

TEST(PeriodicPollPolicy, RestartDropsAnArmedRetry) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  const uint32_t now = T0 + 15000;
  ASSERT_TRUE(policy.pop_due_device(now, MID).has_value());
  policy.on_poll_unanswered("DEV", now);
  ASSERT_TRUE(policy.retry_armed("DEV"));

  policy.restart_interval("DEV", now + 1000, 0);

  EXPECT_FALSE(policy.retry_armed("DEV")) << "the device gave what the retry would have asked for";
  EXPECT_EQ(policy.get_next_poll("DEV"), now + 1000 + 810000);
}

TEST(PeriodicPollPolicy, RestartIgnoresAnUnscheduledDevice) {
  PeriodicPollPolicy policy;
  policy.restart_interval("DEV", T0, 0);

  EXPECT_EQ(policy.get_next_poll("DEV"), 0u);
  EXPECT_FALSE(policy.has_due(T0 + 10 * INTERVAL));
}

// ============================================================================
// Miss counter
// ============================================================================

TEST(PeriodicPollPolicy, ReadingBecomesUnusableOnTheThirdConsecutiveMiss) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);

  EXPECT_FALSE(policy.on_poll_failed("DEV"));
  EXPECT_FALSE(policy.on_poll_failed("DEV"));
  EXPECT_EQ(PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN, 3);
  EXPECT_TRUE(policy.on_poll_failed("DEV"));
  EXPECT_TRUE(policy.on_poll_failed("DEV")) << "stays unusable while misses continue";
  EXPECT_EQ(policy.get_failures("DEV"), 4u);
}

TEST(PeriodicPollPolicy, AnAnsweredPollResetsTheMissCounter) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  policy.on_poll_failed("DEV");
  policy.on_poll_failed("DEV");

  policy.on_poll_succeeded("DEV");

  EXPECT_EQ(policy.get_failures("DEV"), 0u);
  EXPECT_FALSE(policy.on_poll_failed("DEV"));
}

TEST(PeriodicPollPolicy, MissCounterSaturates) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  for (int i = 0; i < 300; ++i)
    policy.on_poll_failed("DEV");
  EXPECT_EQ(policy.get_failures("DEV"), std::numeric_limits<uint8_t>::max());
  EXPECT_TRUE(policy.on_poll_failed("DEV"));
}

TEST(PeriodicPollPolicy, UnscheduledDeviceCountsNoMisses) {
  PeriodicPollPolicy policy;
  EXPECT_FALSE(policy.on_poll_failed("DEV"));
  policy.on_poll_succeeded("DEV");
  EXPECT_EQ(policy.get_failures("DEV"), 0u);
}

// ============================================================================
// Retry after an unanswered poll
// ============================================================================

/// Pop the device's first poll; returns the pop time.
static uint32_t pop_first(PeriodicPollPolicy &policy) {
  const uint32_t now = T0 + PERIODIC_POLL_INITIAL_DELAY_MS;
  EXPECT_TRUE(policy.pop_due_device(now, MID).has_value());
  return now;
}

TEST(PeriodicPollPolicy, UnansweredPollIsAskedAgainSoonWithoutCountingAMiss) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  const uint32_t now = pop_first(policy);

  EXPECT_FALSE(policy.on_poll_unanswered("DEV", now));

  EXPECT_TRUE(policy.retry_armed("DEV"));
  EXPECT_EQ(policy.get_failures("DEV"), 0u);
  EXPECT_EQ(policy.get_next_poll("DEV"), now + PERIODIC_POLL_MISS_RETRY_MS);
}

TEST(PeriodicPollPolicy, UnansweredRetryCountsOneMissAndArmsNoSecondRetry) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  const uint32_t now = pop_first(policy);
  policy.on_poll_unanswered("DEV", now);
  const uint32_t retry_at = now + PERIODIC_POLL_MISS_RETRY_MS;
  ASSERT_TRUE(policy.pop_due_device(retry_at, MID).has_value());
  EXPECT_FALSE(policy.retry_armed("DEV")) << "the pop consumes the retry";

  EXPECT_FALSE(policy.on_poll_unanswered("DEV", retry_at));

  EXPECT_EQ(policy.get_failures("DEV"), 1u);
  EXPECT_FALSE(policy.retry_armed("DEV"));
  EXPECT_NEAR(static_cast<double>(policy.get_next_poll("DEV")), static_cast<double>(retry_at + INTERVAL), 1.0)
      << "back on the interval";
}

TEST(PeriodicPollPolicy, AnsweredRetryLeavesNoMiss) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  const uint32_t now = pop_first(policy);
  policy.on_poll_unanswered("DEV", now);
  ASSERT_TRUE(policy.pop_due_device(now + PERIODIC_POLL_MISS_RETRY_MS, MID).has_value());

  policy.on_poll_succeeded("DEV");

  EXPECT_EQ(policy.get_failures("DEV"), 0u);
  // The next scheduled poll gets its own retry again.
  ASSERT_TRUE(policy.pop_due_device(now + PERIODIC_POLL_MISS_RETRY_MS + INTERVAL + INTERVAL / 10, MID).has_value());
  policy.on_poll_unanswered("DEV", now + 2 * INTERVAL);
  EXPECT_TRUE(policy.retry_armed("DEV"));
}

TEST(PeriodicPollPolicy, ReadingBecomesUnusableAfterThreeUnansweredPollsAndTheirRetries) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  uint32_t now = T0 + PERIODIC_POLL_INITIAL_DELAY_MS;
  bool unusable = false;
  for (int poll = 0; poll < PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN; ++poll) {
    ASSERT_TRUE(policy.pop_due_device(now, MID).has_value()) << "scheduled poll " << poll;
    EXPECT_FALSE(policy.on_poll_unanswered("DEV", now));
    now += PERIODIC_POLL_MISS_RETRY_MS;
    ASSERT_TRUE(policy.pop_due_device(now, MID).has_value()) << "retry " << poll;
    unusable = policy.on_poll_unanswered("DEV", now);
    now += INTERVAL + INTERVAL / 10;
  }
  EXPECT_TRUE(unusable);
  EXPECT_EQ(policy.get_failures("DEV"), PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN);
}

TEST(PeriodicPollPolicy, UnansweredPollKeepsAnUnusableReadingUnusable) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  for (int i = 0; i < PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN; ++i)
    policy.on_poll_failed("DEV");
  const uint32_t now = pop_first(policy);

  EXPECT_TRUE(policy.on_poll_unanswered("DEV", now)) << "arming a retry does not bring the reading back";
  EXPECT_TRUE(policy.retry_armed("DEV"));
}

TEST(PeriodicPollPolicy, DroppedRetryLeavesTheNextPollItsOwnRetry) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  const uint32_t now = pop_first(policy);
  policy.on_poll_unanswered("DEV", now);
  // The retry is popped but never run (pairing flushed it), so no outcome is reported.
  ASSERT_TRUE(policy.pop_due_device(now + PERIODIC_POLL_MISS_RETRY_MS, MID).has_value());
  const uint32_t next = now + PERIODIC_POLL_MISS_RETRY_MS + INTERVAL + INTERVAL / 10;
  ASSERT_TRUE(policy.pop_due_device(next, MID).has_value());

  EXPECT_FALSE(policy.on_poll_unanswered("DEV", next));

  EXPECT_TRUE(policy.retry_armed("DEV")) << "a scheduled poll, not a retry";
  EXPECT_EQ(policy.get_failures("DEV"), 0u);
}

TEST(PeriodicPollPolicy, NewIntervalClearsAnArmedRetry) {
  PeriodicPollPolicy policy;
  policy.set_interval("DEV", INTERVAL, T0, 0);
  policy.on_poll_unanswered("DEV", pop_first(policy));

  policy.set_interval("DEV", INTERVAL, T0, 0);

  EXPECT_FALSE(policy.retry_armed("DEV"));
}

TEST(PeriodicPollPolicy, UnscheduledDeviceArmsNoRetry) {
  PeriodicPollPolicy policy;
  EXPECT_FALSE(policy.on_poll_unanswered("DEV", T0));
  EXPECT_FALSE(policy.retry_armed("DEV"));
}

// ============================================================================
// Due check and error-reply warning
// ============================================================================

TEST(PeriodicPollPolicy, HasDueMatchesPopWithoutConsumingTheSchedule) {
  PeriodicPollPolicy policy;
  EXPECT_FALSE(policy.has_due(T0)) << "an empty schedule has nothing due";
  policy.set_interval("DEV", INTERVAL, T0, 0);
  const uint32_t due_at = T0 + PERIODIC_POLL_INITIAL_DELAY_MS;

  EXPECT_FALSE(policy.has_due(due_at - 1));
  EXPECT_TRUE(policy.has_due(due_at));
  EXPECT_TRUE(policy.has_due(due_at)) << "asking does not re-arm the poll";
  ASSERT_TRUE(policy.pop_due_device(due_at, MID).has_value());
  EXPECT_FALSE(policy.has_due(due_at));
}

TEST(PeriodicPollPolicy, ErrorReplyWarningIsGivenOncePerDevice) {
  PeriodicPollPolicy policy;
  policy.set_interval("A", INTERVAL, T0, 0);
  policy.set_interval("B", INTERVAL, T0, 0);

  EXPECT_TRUE(policy.first_error_reply("A"));
  EXPECT_FALSE(policy.first_error_reply("A"));
  EXPECT_TRUE(policy.first_error_reply("B")) << "each device gets its own warning";
  EXPECT_FALSE(policy.first_error_reply("UNKNOWN")) << "an unscheduled device is never warned about";
}
