/// @file status_poll_policy_test.cpp
/// @brief Unit tests for StatusPollPolicy.
///
/// Covers: backoff ladder progressions, deadline expiry, interval-0 and interval-set tracking modes,
/// due-device scan, failure-streak management, and interval preservation across clears.

#include "status_poll_policy.h"

#include "proto_constants.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

using namespace esphome::home_io_control;

static constexpr uint32_t T0 = 1000000;  ///< Arbitrary base timestamp (ms) for all tests.

// ============================================================================
// Backoff ladder — plain silence failures
// ============================================================================

TEST(StatusPollPolicy, BackoffLadderSilentFailures) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 2000);
  policy.begin_tracking("DEV", 0, T0);

  // All failures at T0: the deadline is T0 + MAX_TRACKED (240 s), so the first four rungs fit.
  uint32_t b1 = policy.on_exchange_failed("DEV", false, T0);
  EXPECT_EQ(b1, STATUS_RETRY_AFTER_FAIL_MS);
  EXPECT_EQ(policy.get_status_poll_failures("DEV"), 1u);
  EXPECT_EQ(policy.get_auth_poll_failures("DEV"), 0u);

  uint32_t b2 = policy.on_exchange_failed("DEV", false, T0);
  EXPECT_EQ(b2, STATUS_RETRY_AFTER_FAIL_STEP2_MS);
  EXPECT_EQ(policy.get_status_poll_failures("DEV"), 2u);

  uint32_t b3 = policy.on_exchange_failed("DEV", false, T0);
  EXPECT_EQ(b3, STATUS_RETRY_AFTER_FAIL_STEP3_MS);

  uint32_t b4 = policy.on_exchange_failed("DEV", false, T0);
  EXPECT_EQ(b4, STATUS_RETRY_AFTER_FAIL_STEP4_MS);

  // The fifth rung (300 s) would end past the 240 s window: the window runs out instead.
  EXPECT_GT(STATUS_RETRY_AFTER_FAIL_MAX_MS, MAX_TRACKED_STATUS_POLL_WINDOW_MS);
  uint32_t b5 = policy.on_exchange_failed("DEV", false, T0);
  EXPECT_EQ(b5, 0u);
  EXPECT_FALSE(policy.is_tracking_active("DEV", T0));
}

// ============================================================================
// Backoff ladder — auth-shaped failures
// ============================================================================

TEST(StatusPollPolicy, BackoffLadderAuthShapedFailures) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 2000);
  policy.begin_tracking("DEV", 0, T0);

  uint32_t b1 = policy.on_exchange_failed("DEV", true, T0);
  EXPECT_EQ(b1, STATUS_AUTH_RETRY_AFTER_FAIL_MS);
  EXPECT_EQ(policy.get_auth_poll_failures("DEV"), 1u);
  EXPECT_EQ(policy.get_status_poll_failures("DEV"), 0u);

  uint32_t b2 = policy.on_exchange_failed("DEV", true, T0);
  EXPECT_EQ(b2, STATUS_AUTH_RETRY_AFTER_FAIL_STEP2_MS);

  uint32_t b3 = policy.on_exchange_failed("DEV", true, T0);
  EXPECT_EQ(b3, STATUS_AUTH_RETRY_AFTER_FAIL_STEP3_MS);

  // The fourth rung (300 s) would end past the 240 s window: the window runs out instead.
  EXPECT_GT(STATUS_AUTH_RETRY_AFTER_FAIL_MAX_MS, MAX_TRACKED_STATUS_POLL_WINDOW_MS);
  uint32_t b4 = policy.on_exchange_failed("DEV", true, T0);
  EXPECT_EQ(b4, 0u);
  EXPECT_FALSE(policy.is_tracking_active("DEV", T0));
}

// A single lost closing reply (the device challenged, then switched its motor off and never
// answered, issue #178) waits no longer than a silent miss; only a repeat backs off far.
TEST(StatusPollPolicy, FirstAuthShapedFailureWaitsNoLongerThanASilentOne) {
  StatusPollPolicy auth;
  StatusPollPolicy silent;
  for (StatusPollPolicy *policy : {&auth, &silent}) {
    policy->set_interval("DEV", 1000);
    policy->begin_tracking("DEV", 0, T0);
  }

  EXPECT_EQ(auth.on_exchange_failed("DEV", true, T0), silent.on_exchange_failed("DEV", false, T0));
  EXPECT_GT(auth.on_exchange_failed("DEV", true, T0), silent.on_exchange_failed("DEV", false, T0))
      << "a repeated challenge-seen miss (a wrong key) backs off further than repeated silence";
}

TEST(StatusPollPolicy, SuccessBetweenAuthShapedFailuresRestartsTheQuickRetry) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 1000);
  policy.begin_tracking("DEV", 0, T0);

  policy.on_exchange_failed("DEV", true, T0);
  policy.clear_failure_streaks("DEV");  // the 5 s retry answered
  EXPECT_EQ(policy.on_exchange_failed("DEV", true, T0), STATUS_AUTH_RETRY_AFTER_FAIL_MS)
      << "the next end-of-travel miss is again a first miss";
}

// ============================================================================
// Misses of a moving device (issue #178)
// ============================================================================

// A cover that answered a moment ago lost one frame: the next poll keeps the cadence, the failure
// streaks stay untouched (so the next poll stays a single try), and only the third miss in a row
// enters the ladder.
TEST(StatusPollPolicy, MovingDeviceSkipsItsFirstMissesAtTheConfiguredInterval) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 1000);
  policy.begin_tracking("DEV", 0, T0);

  for (uint8_t miss = 1; miss <= MOVING_POLL_MISSES_SKIPPED; miss++) {
    EXPECT_EQ(policy.on_exchange_failed("DEV", false, T0, /*device_moving=*/true), 1000u) << "miss " << +miss;
    EXPECT_EQ(policy.get_next_update("DEV"), T0 + 1000u);
    EXPECT_EQ(policy.get_moving_poll_misses("DEV"), miss);
    EXPECT_EQ(policy.get_status_poll_failures("DEV"), 0u);
    EXPECT_EQ(policy.get_auth_poll_failures("DEV"), 0u);
  }

  EXPECT_EQ(policy.on_exchange_failed("DEV", false, T0, /*device_moving=*/true), STATUS_RETRY_AFTER_FAIL_MS)
      << "a third miss in a row is no longer a lost frame: the ladder takes over";
  EXPECT_EQ(policy.get_status_poll_failures("DEV"), 1u);
}

TEST(StatusPollPolicy, MovingDeviceWithoutIntervalSkipsAtTheDefaultSettleDelay) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  EXPECT_EQ(policy.on_exchange_failed("DEV", false, T0, /*device_moving=*/true), DEFAULT_SETTLE_POLL_DELAY_MS);
}

TEST(StatusPollPolicy, MovingDeviceSkipsAChallengeSeenMissToo) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 1000);
  policy.begin_tracking("DEV", 0, T0);
  EXPECT_EQ(policy.on_exchange_failed("DEV", true, T0, /*device_moving=*/true), 1000u);
  EXPECT_EQ(policy.get_auth_poll_failures("DEV"), 0u);
  policy.on_exchange_failed("DEV", true, T0, true);
  EXPECT_EQ(policy.on_exchange_failed("DEV", true, T0, true), STATUS_AUTH_RETRY_AFTER_FAIL_MS)
      << "a wrong key fails every poll and still reaches the auth ladder, two polls later";
}

TEST(StatusPollPolicy, DeviceAtRestBacksOffFromTheFirstMiss) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 1000);
  policy.begin_tracking("DEV", 0, T0);
  EXPECT_EQ(policy.on_exchange_failed("DEV", false, T0, /*device_moving=*/false), STATUS_RETRY_AFTER_FAIL_MS);
  EXPECT_EQ(policy.get_moving_poll_misses("DEV"), 0u);
}

TEST(StatusPollPolicy, AnAnswerOrANewCycleResetsTheSkippedMisses) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 1000);
  policy.begin_tracking("DEV", 0, T0);
  policy.on_exchange_failed("DEV", false, T0, true);
  policy.on_exchange_failed("DEV", false, T0, true);
  policy.clear_failure_streaks("DEV");  // the next poll answered
  EXPECT_EQ(policy.get_moving_poll_misses("DEV"), 0u);
  EXPECT_EQ(policy.on_exchange_failed("DEV", false, T0, true), 1000u);

  policy.begin_tracking("DEV", 0, T0);  // a new command
  EXPECT_EQ(policy.get_moving_poll_misses("DEV"), 0u);
  policy.on_exchange_failed("DEV", false, T0, true);
  policy.clear("DEV");
  EXPECT_EQ(policy.get_moving_poll_misses("DEV"), 0u);
}

TEST(StatusPollPolicy, MovingMissNeverWaitsLongerThanTheLadderWould) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 10000);
  policy.begin_tracking("DEV", 0, T0);
  EXPECT_EQ(policy.on_exchange_failed("DEV", false, T0, /*device_moving=*/true), STATUS_RETRY_AFTER_FAIL_MS);
  EXPECT_EQ(policy.get_moving_poll_misses("DEV"), 1u) << "still a skipped miss, only sooner than the interval";
}

// A miss that arrives while a failure streak already runs (the device missed a poll at rest, then a
// status update said it moves) stays on the ladder: the streak is what grants the band's tries.
TEST(StatusPollPolicy, MovingMissDuringAFailureStreakStaysOnTheLadder) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 1000);
  policy.begin_tracking("DEV", 0, T0);
  policy.on_exchange_failed("DEV", false, T0, /*device_moving=*/false);
  EXPECT_EQ(policy.on_exchange_failed("DEV", false, T0, /*device_moving=*/true), STATUS_RETRY_AFTER_FAIL_STEP2_MS);
  EXPECT_EQ(policy.get_moving_poll_misses("DEV"), 0u);
}

TEST(StatusPollPolicy, MovingMissNeverStretchesTheTrackingWindow) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 1000);
  policy.begin_tracking("DEV", 0, T0);
  const uint32_t late = T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS - 500;
  EXPECT_EQ(policy.on_exchange_failed("DEV", false, late, /*device_moving=*/true), 0u)
      << "a skip that would land past the window falls through to the ladder, which ends tracking";
  EXPECT_EQ(policy.pop_ended_device(), std::optional<std::string>("DEV"));
}

// ============================================================================
// Failure class switching resets the opposite streak
// ============================================================================

TEST(StatusPollPolicy, AuthFailureResetssilentStreak) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 2000);
  policy.begin_tracking("DEV", 0, T0);

  policy.on_exchange_failed("DEV", false, T0);  // silent failure 1
  EXPECT_EQ(policy.get_status_poll_failures("DEV"), 1u);

  policy.on_exchange_failed("DEV", true, T0);  // switch to auth
  EXPECT_EQ(policy.get_status_poll_failures("DEV"), 0u) << "auth failure should reset silent streak";
  EXPECT_EQ(policy.get_auth_poll_failures("DEV"), 1u);
}

// ============================================================================
// Deadline expiry clears tracking
// ============================================================================

TEST(StatusPollPolicy, DeadlineExpiryClears) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 2000);
  policy.begin_tracking("DEV", 0, T0);

  // Advance time past the deadline
  uint32_t const past_deadline = T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS + 1;
  uint32_t b = policy.on_exchange_failed("DEV", false, past_deadline);
  EXPECT_EQ(b, 0u) << "failure after deadline should return 0 (tracking cleared)";
  EXPECT_EQ(policy.get_poll_deadline("DEV"), 0u) << "poll_deadline should be zeroed after expiry";
  EXPECT_EQ(policy.get_next_update("DEV"), 0u) << "next_update should be zeroed after expiry";
}

// ============================================================================
// Interval-0 tracking mode (no configured interval — device-hinted settle polling)
// ============================================================================

TEST(StatusPollPolicy, BeginTrackingIntervalZeroSetsDeadline) {
  StatusPollPolicy policy;
  // No interval configured — begin_tracking should still set a deadline.
  policy.begin_tracking("DEV", 0, T0);
  EXPECT_EQ(policy.get_poll_deadline("DEV"), T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS)
      << "begin_tracking with interval 0 should still set the tracking deadline";
  EXPECT_TRUE(policy.is_tracking_active("DEV", T0))
      << "is_tracking_active should return true within the window even for interval-0 devices";
}

TEST(StatusPollPolicy, IntervalZeroDeviceFiresWhenTrackingActive) {
  StatusPollPolicy policy;
  // begin_tracking sets the deadline; then a response handler arms the next poll via set_next_update.
  policy.begin_tracking("DEV", 0, T0);
  policy.set_next_update("DEV", T0 + 1000);

  EXPECT_FALSE(policy.pop_due_device(T0 + 999).has_value()) << "not due yet";
  auto due = policy.pop_due_device(T0 + 1001);
  EXPECT_TRUE(due.has_value());
  EXPECT_EQ(*due, "DEV");
  EXPECT_EQ(policy.get_next_update("DEV"), 0u) << "pop_due_device should consume next_update";
}

TEST(StatusPollPolicy, IntervalZeroDeviceClearedAfterDeadline) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  uint32_t const past_deadline = T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS + 1;
  policy.set_next_update("DEV", past_deadline - 1);

  auto due = policy.pop_due_device(past_deadline);
  EXPECT_FALSE(due.has_value()) << "expired tracking should be cleared and not dispatched";
  EXPECT_EQ(policy.get_poll_deadline("DEV"), 0u) << "deadline should be cleared after expiry";
}

TEST(StatusPollPolicy, IntervalZeroOnExchangeFailedAppliesBackoff) {
  StatusPollPolicy policy;
  // Prior to the fix, on_exchange_failed was a no-op without a configured interval.
  policy.begin_tracking("DEV", 0, T0);
  uint32_t const backoff = policy.on_exchange_failed("DEV", false, T0);
  EXPECT_EQ(backoff, STATUS_RETRY_AFTER_FAIL_MS) << "backoff should apply to interval-0 devices within the window";
  EXPECT_NE(policy.get_next_update("DEV"), 0u) << "on_exchange_failed should schedule a retry";
}

// ============================================================================
// Interval-tracked mode
// ============================================================================

TEST(StatusPollPolicy, IntervalModeBeginTrackingSetsDeadline) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 3000);
  policy.begin_tracking("DEV", 3000, T0);

  EXPECT_EQ(policy.get_poll_deadline("DEV"), T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS);
  EXPECT_EQ(policy.get_next_update("DEV"), T0 + 3000);
  EXPECT_TRUE(policy.is_tracking_active("DEV", T0));
}

TEST(StatusPollPolicy, TrackedModeDueScanReturnsDevice) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 1000);
  policy.begin_tracking("DEV", 500, T0);

  EXPECT_FALSE(policy.pop_due_device(T0 + 400).has_value()) << "device not due yet";
  auto due = policy.pop_due_device(T0 + 600);
  EXPECT_TRUE(due.has_value());
  EXPECT_EQ(*due, "DEV");
}

TEST(StatusPollPolicy, DueScanClearsExpiredTracking) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 1000);
  policy.begin_tracking("DEV", 500, T0);

  // Advance to after deadline
  uint32_t const past_deadline = T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS + 10000;
  policy.set_next_update("DEV", past_deadline - 1);  // would be due at past_deadline

  auto due = policy.pop_due_device(past_deadline);
  EXPECT_FALSE(due.has_value()) << "expired tracking should be cleared, not dispatched";
  EXPECT_EQ(policy.get_poll_deadline("DEV"), 0u) << "deadline should be cleared after expiry";
}

TEST(StatusPollPolicy, DueScanOnlyReturnsOneDevice) {
  StatusPollPolicy policy;
  policy.begin_tracking("A", 0, T0);
  policy.set_next_update("A", T0);
  policy.begin_tracking("B", 0, T0);
  policy.set_next_update("B", T0);

  auto due = policy.pop_due_device(T0 + 1);
  EXPECT_TRUE(due.has_value());
  // One of the two — the other remains in the map
  EXPECT_NE(policy.get_next_update("A") + policy.get_next_update("B"), 0u) << "the other device should still be due";
}

// ============================================================================
// Clear failure streaks
// ============================================================================

TEST(StatusPollPolicy, ClearFailureStreaks) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 1000);
  policy.begin_tracking("DEV", 0, T0);
  policy.on_exchange_failed("DEV", false, T0);
  policy.on_exchange_failed("DEV", false, T0);
  EXPECT_EQ(policy.get_status_poll_failures("DEV"), 2u);

  policy.clear_failure_streaks("DEV");
  EXPECT_EQ(policy.get_status_poll_failures("DEV"), 0u);
  EXPECT_EQ(policy.get_auth_poll_failures("DEV"), 0u);
}

// ============================================================================
// Clear preserves interval
// ============================================================================

TEST(StatusPollPolicy, ClearPreservesInterval) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 5000);
  policy.begin_tracking("DEV", 1000, T0);

  policy.clear("DEV");

  EXPECT_EQ(policy.get_interval("DEV"), 5000u) << "clear should not reset the configured interval";
  EXPECT_EQ(policy.get_poll_deadline("DEV"), 0u);
  EXPECT_EQ(policy.get_next_update("DEV"), 0u);
}

// ============================================================================
// is_tracking_active edge cases
// ============================================================================

TEST(StatusPollPolicy, IsTrackingActiveReturnsFalseWhenNotSet) {
  StatusPollPolicy policy;
  EXPECT_FALSE(policy.is_tracking_active("UNKNOWN", T0));
}

TEST(StatusPollPolicy, IsTrackingActiveReturnsFalseAfterDeadline) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 1000);
  policy.begin_tracking("DEV", 0, T0);
  EXPECT_FALSE(policy.is_tracking_active("DEV", T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS + 1));
}

TEST(StatusPollPolicy, OnExchangeFailedNoOpForUnknownDevice) {
  StatusPollPolicy policy;
  uint32_t result = policy.on_exchange_failed("GHOST", false, T0);
  EXPECT_EQ(result, 0u) << "on_exchange_failed for unknown device should return 0";
}

// ============================================================================
// settle_delay_ms — single source of truth for the motion-tracking cadence
// ============================================================================

TEST(SettleDelay, NoIntervalNoHintUsesDefault) {
  EXPECT_EQ(settle_delay_ms(0, 0, /*cap_for_stop=*/false), DEFAULT_SETTLE_POLL_DELAY_MS)
      << "with no configured interval and no hint, the default settle delay applies";
}

// ============================================================================
// Post-STOP settle poll mark
// ============================================================================

TEST(StatusPollPolicy, StopSettleMarkIsConsumedByTheFirstPoll) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", STOP_SETTLE_POLL_CAP_MS, T0);
  policy.mark_stop_settle("DEV");
  EXPECT_TRUE(policy.take_stop_settle("DEV"));
  EXPECT_FALSE(policy.take_stop_settle("DEV")) << "only the first poll after the STOP is a settling read";
}

TEST(StatusPollPolicy, StopSettleMarkIsDroppedByANewTrackingCycle) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", STOP_SETTLE_POLL_CAP_MS, T0);
  policy.mark_stop_settle("DEV");
  policy.begin_tracking("DEV", DEFAULT_SETTLE_POLL_DELAY_MS, T0 + 500);  // a newer move command
  EXPECT_FALSE(policy.take_stop_settle("DEV"));
}

TEST(StatusPollPolicy, StopSettleMarkIsDroppedByClear) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", STOP_SETTLE_POLL_CAP_MS, T0);
  policy.mark_stop_settle("DEV");
  policy.clear("DEV");
  EXPECT_FALSE(policy.take_stop_settle("DEV"));
}

TEST(StatusPollPolicy, StopSettleMarkNeedsATrackedDevice) {
  StatusPollPolicy policy;
  policy.mark_stop_settle("DEV");
  EXPECT_FALSE(policy.take_stop_settle("DEV")) << "a STOP the hub is not tracking leaves no mark behind";
}

TEST(SettleDelay, ConfiguredIntervalOverridesDefault) {
  EXPECT_EQ(settle_delay_ms(30000, 0, false), 30000u) << "a configured interval replaces the default";
}

TEST(SettleDelay, HintOnlyShortens) {
  // Hint shorter than the default wins; hint longer than the default is ignored (never lengthens).
  EXPECT_EQ(settle_delay_ms(0, 1500, false), 1500u) << "a hint shorter than the default should win";
  EXPECT_EQ(settle_delay_ms(0, 9000, false), DEFAULT_SETTLE_POLL_DELAY_MS)
      << "a hint longer than the default must not lengthen the settle delay";
}

TEST(SettleDelay, HintCappedByConfiguredInterval) {
  EXPECT_EQ(settle_delay_ms(2000, 5000, false), 2000u) << "hint may not exceed the configured interval";
  EXPECT_EQ(settle_delay_ms(5000, 2000, false), 2000u) << "hint shorter than the interval wins";
}

TEST(SettleDelay, StopCapAlwaysShortens) {
  // STOP caps below the default even with no interval or hint...
  EXPECT_EQ(settle_delay_ms(0, 0, /*cap_for_stop=*/true), STOP_SETTLE_POLL_CAP_MS)
      << "STOP settles within the STOP cap for unconfigured devices";
  // ...and below a long configured interval.
  EXPECT_EQ(settle_delay_ms(30000, 0, true), STOP_SETTLE_POLL_CAP_MS)
      << "STOP caps a long configured interval down to the STOP window";
  // A hint shorter than the STOP cap still wins (nothing may lengthen the delay).
  EXPECT_LE(settle_delay_ms(0, 500, true), STOP_SETTLE_POLL_CAP_MS)
      << "a hint shorter than the STOP cap must not be lengthened by the cap";
}

TEST(SettleDelay, StopCapIsShorterThanDefault) {
  EXPECT_LT(STOP_SETTLE_POLL_CAP_MS, DEFAULT_SETTLE_POLL_DELAY_MS)
      << "STOP must settle faster than a normal move in every scenario";
}

// ============================================================================
// Tracking that ran out (pop_ended_device)
// ============================================================================

TEST(StatusPollPolicy, PopEndedDeviceReportsALadderThatNoLongerFits) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  // Late in the window: no backoff fits before the deadline any more.
  const uint32_t late = T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS - 1;
  ASSERT_EQ(policy.on_exchange_failed("DEV", false, late), 0u);

  EXPECT_EQ(policy.pop_ended_device(), std::optional<std::string>("DEV"));
  EXPECT_EQ(policy.pop_ended_device(), std::nullopt) << "consumed once";
}

TEST(StatusPollPolicy, PopEndedDeviceReportsAWindowThatExpiredWithAPollPending) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  policy.set_next_update("DEV", T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS + 1000);

  EXPECT_EQ(policy.pop_due_device(T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS + 2000), std::nullopt)
      << "the poll past the deadline is dropped";
  EXPECT_EQ(policy.pop_ended_device(), std::optional<std::string>("DEV"));
}

TEST(StatusPollPolicy, PopEndedDeviceIgnoresAnExplicitClear) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  policy.clear("DEV");

  EXPECT_EQ(policy.pop_ended_device(), std::nullopt) << "a stopped report ended it, nothing ran out";
}

TEST(StatusPollPolicy, PopEndedDeviceIgnoresAFailedPollOfAnUntrackedDevice) {
  StatusPollPolicy policy;
  policy.set_interval("DEV", 2000);
  ASSERT_EQ(policy.on_exchange_failed("DEV", false, T0), 0u);

  EXPECT_EQ(policy.pop_ended_device(), std::nullopt) << "no window was open, so none ran out";
}

TEST(StatusPollPolicy, PopEndedDeviceListsADeviceOnce) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  const uint32_t late = T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS - 1;
  ASSERT_EQ(policy.on_exchange_failed("DEV", false, late), 0u);
  policy.begin_tracking("DEV", 0, late);
  ASSERT_EQ(policy.on_exchange_failed("DEV", false, late + MAX_TRACKED_STATUS_POLL_WINDOW_MS - 1), 0u);

  EXPECT_EQ(policy.pop_ended_device(), std::optional<std::string>("DEV"));
  EXPECT_EQ(policy.pop_ended_device(), std::nullopt);
}

// ============================================================================
// Overheard foreign traffic
// ============================================================================

namespace {
constexpr ForeignFrameKind REQ = ForeignFrameKind::REQUEST_TO_DEVICE;
constexpr ForeignFrameKind CHAL = ForeignFrameKind::DEVICE_CHALLENGE_TO_OTHER;
constexpr uint8_t READ_CMD = 0x03;  // any non-write command
}  // namespace

TEST(StatusPollPolicyForeign, WriteArms) {
  StatusPollPolicy p;
  EXPECT_EQ(p.on_foreign_frame("DEV", REQ, "A", CMD_EXECUTE, T0, T0), ForeignAction::ARM);
  p.on_remote_poll_ran("DEV");
  EXPECT_EQ(p.on_foreign_frame("DEV", REQ, "A", CMD_ACTIVATE_MODE, T0 + 5000, T0), ForeignAction::ARM);
  p.on_remote_poll_ran("DEV");
  EXPECT_EQ(p.on_foreign_frame("DEV", REQ, "A", CMD_WRITE_PRIVATE, T0 + 10000, T0), ForeignAction::ARM);
}

TEST(StatusPollPolicyForeign, ReadArmsFirstTimeThenNotWithinResyncThenAgain) {
  StatusPollPolicy p;
  EXPECT_EQ(p.on_foreign_frame("DEV", REQ, "A", READ_CMD, T0, 0), ForeignAction::ARM) << "first read polls";
  p.on_remote_poll_ran("DEV");
  EXPECT_EQ(p.on_foreign_frame("DEV", REQ, "A", READ_CMD, T0 + FOREIGN_READ_RESYNC_MS - 1, 0), ForeignAction::IGNORE);
  EXPECT_EQ(p.on_foreign_frame("DEV", REQ, "A", READ_CMD, T0 + FOREIGN_READ_RESYNC_MS, 0), ForeignAction::ARM);
}

TEST(StatusPollPolicyForeign, ReadArmsRightAfterBoot) {
  StatusPollPolicy p;
  EXPECT_EQ(p.on_foreign_frame("DEV", REQ, "A", READ_CMD, 0, 0), ForeignAction::ARM);
}

TEST(StatusPollPolicyForeign, RecentAuthenticatedStatusSuppressesRead) {
  StatusPollPolicy p;
  EXPECT_EQ(p.on_foreign_frame("DEV", REQ, "A", READ_CMD, T0, T0 - 1000), ForeignAction::IGNORE);
  EXPECT_EQ(p.on_foreign_frame("DEV", REQ, "A", READ_CMD, T0, T0 - FOREIGN_READ_RESYNC_MS), ForeignAction::ARM);
}

TEST(StatusPollPolicyForeign, WriteOrChallengeArmedPollDoesNotStampTheReadAttempt) {
  StatusPollPolicy p;
  ASSERT_EQ(p.on_foreign_frame("DEV", REQ, "A", CMD_EXECUTE, T0, 0), ForeignAction::ARM);
  p.on_remote_poll_ran("DEV");
  EXPECT_EQ(p.on_foreign_frame("DEV", REQ, "A", READ_CMD, T0 + 5000, 0), ForeignAction::ARM) << "no read stamp yet";
}

TEST(StatusPollPolicyForeign, ChallengeAfterHeardReadWithinWindowIsIgnored) {
  StatusPollPolicy p;
  p.on_foreign_frame("DEV", REQ, "A", READ_CMD, T0, T0);  // suppressed read, still recorded
  EXPECT_EQ(p.on_foreign_frame("DEV", CHAL, "A", CMD_CHALLENGE_REQ, T0 + 3, T0), ForeignAction::IGNORE);
}

TEST(StatusPollPolicyForeign, ChallengeAfterHeardWriteArms) {
  StatusPollPolicy p;
  p.on_foreign_frame("DEV", REQ, "A", CMD_EXECUTE, T0, T0);
  p.on_remote_poll_ran("DEV");
  EXPECT_EQ(p.on_foreign_frame("DEV", CHAL, "A", CMD_CHALLENGE_REQ, T0 + 3, T0), ForeignAction::ARM);
}

TEST(StatusPollPolicyForeign, ChallengeWithNoHeardRequestArms) {
  StatusPollPolicy p;
  EXPECT_EQ(p.on_foreign_frame("DEV", CHAL, "A", CMD_CHALLENGE_REQ, T0, T0), ForeignAction::ARM);
}

TEST(StatusPollPolicyForeign, ChallengeAfterAnOlderReadArms) {
  StatusPollPolicy p;
  p.on_foreign_frame("DEV", REQ, "A", READ_CMD, T0, T0);
  EXPECT_EQ(p.on_foreign_frame("DEV", CHAL, "A", CMD_CHALLENGE_REQ, T0 + FOREIGN_CHALLENGE_AFTER_REQUEST_MS + 1, T0),
            ForeignAction::ARM);
}

TEST(StatusPollPolicyForeign, ReadFromAnotherControllerDoesNotSuppressTheChallenge) {
  StatusPollPolicy p;
  p.on_foreign_frame("DEV", REQ, "A", READ_CMD, T0, T0);
  EXPECT_EQ(p.on_foreign_frame("DEV", CHAL, "B", CMD_CHALLENGE_REQ, T0 + 3, T0), ForeignAction::ARM);
}

TEST(StatusPollPolicyForeign, ChallengeDoesNotUpdateTheRequestRecord) {
  StatusPollPolicy p;
  p.on_foreign_frame("DEV", REQ, "A", READ_CMD, T0, T0);
  p.on_foreign_frame("DEV", CHAL, "B", CMD_CHALLENGE_REQ, T0 + 3, T0);  // arms
  p.on_remote_poll_ran("DEV");
  EXPECT_EQ(p.on_foreign_frame("DEV", CHAL, "A", CMD_CHALLENGE_REQ, T0 + 50, T0), ForeignAction::IGNORE)
      << "A's read is still the last request";
}

TEST(StatusPollPolicyForeign, PendingPushesOnAnyFrame) {
  StatusPollPolicy p;
  ASSERT_EQ(p.on_foreign_frame("DEV", REQ, "A", CMD_EXECUTE, T0, T0), ForeignAction::ARM);
  EXPECT_TRUE(p.is_remote_poll_pending("DEV", T0 + 100));
  EXPECT_EQ(p.on_foreign_frame("DEV", REQ, "A", READ_CMD, T0 + 1000, T0), ForeignAction::PUSH_PENDING);
  EXPECT_EQ(p.on_foreign_frame("DEV", CHAL, "A", CMD_CHALLENGE_REQ, T0 + 2500, T0), ForeignAction::PUSH_PENDING)
      << "the first push moved the due time out to T0+3000";
  p.on_remote_poll_ran("DEV");
  EXPECT_FALSE(p.is_remote_poll_pending("DEV", T0 + 2600));
}

TEST(StatusPollPolicyForeign, PendingExpiresByItselfWhenTheRanSignalIsLost) {
  StatusPollPolicy p;
  ASSERT_EQ(p.on_foreign_frame("DEV", REQ, "A", CMD_EXECUTE, T0, T0), ForeignAction::ARM);
  const uint32_t due = T0 + REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS;
  EXPECT_TRUE(p.is_remote_poll_pending("DEV", due + REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS));
  EXPECT_FALSE(p.is_remote_poll_pending("DEV", due + REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS + 1));
  EXPECT_EQ(p.on_foreign_frame("DEV", REQ, "A", CMD_EXECUTE, due + 10000, T0), ForeignAction::ARM);
}

TEST(StatusPollPolicyForeign, SurvivesTrackingLifecycle) {
  StatusPollPolicy p;
  ASSERT_EQ(p.on_foreign_frame("DEV", REQ, "A", READ_CMD, T0, 0), ForeignAction::ARM);
  p.on_remote_poll_ran("DEV");
  p.begin_tracking("DEV", 0, T0 + 10);
  p.clear("DEV");
  EXPECT_EQ(p.on_foreign_frame("DEV", REQ, "A", READ_CMD, T0 + 20, 0), ForeignAction::IGNORE)
      << "the read stamp is not reset by tracking";
}

TEST(StatusPollPolicyForeign, MillisWrapAround) {
  StatusPollPolicy p;
  const uint32_t before_wrap = 0xFFFFFFFFu - 50;
  ASSERT_EQ(p.on_foreign_frame("DEV", REQ, "A", READ_CMD, before_wrap, 0), ForeignAction::ARM);
  EXPECT_TRUE(p.is_remote_poll_pending("DEV", before_wrap + 100)) << "due time wraps";
  p.on_remote_poll_ran("DEV");
  EXPECT_EQ(p.on_foreign_frame("DEV", REQ, "A", READ_CMD, before_wrap + 1000, 0), ForeignAction::IGNORE)
      << "1 s after, across the wrap";
  EXPECT_EQ(p.on_foreign_frame("DEV", CHAL, "A", CMD_CHALLENGE_REQ, before_wrap + 1000 + 50, 0), ForeignAction::IGNORE)
      << "challenge 50 ms after a read, across the wrap";
}

// ============================================================================
// A "moving" status report and the follow-up window (on_moving_report)
// ============================================================================

TEST(StatusPollPolicy, OnMovingReportFollowsAnOpenWindow) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  const uint32_t deadline = policy.get_poll_deadline("DEV");

  EXPECT_EQ(policy.on_moving_report("DEV", T0 + 60000, true), MovingReportAction::FOLLOW);
  EXPECT_EQ(policy.get_poll_deadline("DEV"), deadline) << "a later answer never extends the window";
  EXPECT_EQ(policy.on_moving_report("DEV", T0 + 60000, false), MovingReportAction::FOLLOW)
      << "an open window is followed whatever the permission to open one";
}

TEST(StatusPollPolicy, OnMovingReportOpensAWindowWhenNoneExists) {
  StatusPollPolicy policy;

  EXPECT_EQ(policy.on_moving_report("DEV", T0, true), MovingReportAction::OPEN);
  EXPECT_EQ(policy.get_poll_deadline("DEV"), T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS);
  EXPECT_EQ(policy.get_next_update("DEV"), 0u) << "the caller schedules the poll";
  EXPECT_TRUE(policy.is_tracking_active("DEV", T0));
}

TEST(StatusPollPolicy, OnMovingReportEndsAnExpiredWindowAndReportsIt) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);

  EXPECT_EQ(policy.on_moving_report("DEV", T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS + 1, true), MovingReportAction::END);
  EXPECT_EQ(policy.get_poll_deadline("DEV"), 0u) << "the window is closed";
  EXPECT_EQ(policy.pop_ended_device(), std::optional<std::string>("DEV"));
}

TEST(StatusPollPolicy, OnMovingReportAfterAnEndedWindowOpensAFreshOne) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  const uint32_t late = T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS + 1;
  ASSERT_EQ(policy.on_moving_report("DEV", late, true), MovingReportAction::END);

  EXPECT_EQ(policy.on_moving_report("DEV", late + 1000, true), MovingReportAction::OPEN);
  EXPECT_EQ(policy.get_poll_deadline("DEV"), late + 1000 + MAX_TRACKED_STATUS_POLL_WINDOW_MS);
}

TEST(StatusPollPolicy, OnMovingReportWithoutPermissionNeverOpens) {
  StatusPollPolicy policy;

  EXPECT_EQ(policy.on_moving_report("DEV", T0, false), MovingReportAction::NONE);
  EXPECT_EQ(policy.get_poll_deadline("DEV"), 0u);
  EXPECT_EQ(policy.pop_ended_device(), std::nullopt);
}

TEST(StatusPollPolicy, OnMovingReportUnknownDeviceIdOpensAndKeepsAKnownDevicesInterval) {
  StatusPollPolicy policy;
  policy.set_interval("KNOWN", 2000);

  EXPECT_EQ(policy.on_moving_report("NEW", T0, true), MovingReportAction::OPEN) << "an entry is created";
  EXPECT_EQ(policy.on_moving_report("KNOWN", T0, true), MovingReportAction::OPEN);
  EXPECT_EQ(policy.get_interval("KNOWN"), 2000u) << "opening a window keeps the configured cadence";
  EXPECT_EQ(policy.get_interval("NEW"), 0u);
}

// ============================================================================
// Confirming-read mark (a stop off target)
// ============================================================================

TEST(StatusPollPolicy, ConfirmStoppedMarkIsConsumedByTheFirstPoll) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", DEFAULT_SETTLE_POLL_DELAY_MS, T0);
  policy.set_confirm_stopped("DEV", true);
  EXPECT_TRUE(policy.take_confirm_stopped("DEV"));
  EXPECT_FALSE(policy.take_confirm_stopped("DEV")) << "only the poll right after the report confirms it";
}

TEST(StatusPollPolicy, ConfirmStoppedMarkIsDroppedByANewTrackingCycleAndByClear) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", DEFAULT_SETTLE_POLL_DELAY_MS, T0);
  policy.set_confirm_stopped("DEV", true);
  policy.begin_tracking("DEV", DEFAULT_SETTLE_POLL_DELAY_MS, T0 + 500);  // a newer move command
  EXPECT_FALSE(policy.take_confirm_stopped("DEV"));
  policy.set_confirm_stopped("DEV", true);
  policy.clear("DEV");
  EXPECT_FALSE(policy.take_confirm_stopped("DEV"));
}

TEST(StatusPollPolicy, ConfirmStoppedMarkNeedsATrackedDevice) {
  StatusPollPolicy policy;
  policy.set_confirm_stopped("DEV", true);
  EXPECT_FALSE(policy.take_confirm_stopped("DEV"));
}

TEST(StatusPollPolicy, TakeSettlingReadConsumesBothMarks) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", DEFAULT_SETTLE_POLL_DELAY_MS, T0);
  EXPECT_FALSE(policy.take_settling_read("DEV")) << "a plain tracked poll settles nothing";
  policy.mark_stop_settle("DEV");
  policy.set_confirm_stopped("DEV", true);
  EXPECT_TRUE(policy.take_settling_read("DEV"));
  EXPECT_FALSE(policy.take_stop_settle("DEV")) << "neither mark survives the poll that took them";
  EXPECT_FALSE(policy.take_confirm_stopped("DEV"));
  policy.set_confirm_stopped("DEV", true);
  EXPECT_TRUE(policy.take_settling_read("DEV")) << "either mark alone makes the poll settling";
}

// ============================================================================
// Predicted-end read (on_trusted_report, pop_due_device)
// ============================================================================

TEST(StatusPollPredictedEnd, ALongRemainingTimeAimsAtTheEndPlusAMargin) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);

  policy.on_trusted_report("DEV", 12, T0);

  EXPECT_EQ(policy.get_settle_update("DEV"), T0 + 12000u + PREDICTED_END_MARGIN_MS);
}

TEST(StatusPollPredictedEnd, AShortOrZeroCountAimsAtItsEndToo) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);

  for (const uint16_t remaining : {uint16_t{2}, uint16_t{1}, uint16_t{0}}) {
    policy.on_trusted_report("DEV", remaining, T0);
    EXPECT_EQ(policy.get_settle_update("DEV"), T0 + remaining * 1000u + PREDICTED_END_MARGIN_MS)
        << "the end read of a count of " << remaining << " s is a settling read";
  }
}

TEST(StatusPollPredictedEnd, AShortCountReAimsNearerAndPopsAsASettlingRead) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  policy.on_trusted_report("DEV", 12, T0);
  policy.on_trusted_report("DEV", 2, T0 + 10500);
  EXPECT_EQ(policy.get_settle_update("DEV"), T0 + 13500u);

  EXPECT_FALSE(policy.pop_due_device(T0 + 13500).has_value());
  ASSERT_EQ(policy.pop_due_device(T0 + 13501).value_or(""), "DEV");
  EXPECT_TRUE(policy.take_settling_read("DEV"));
}

TEST(StatusPollPredictedEnd, AStaleCountDropsTheAimAndAFallingOneRestoresIt) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  policy.on_trusted_report("DEV", 12, T0);
  for (uint8_t i = 1; i < PREDICTED_END_STALE_REPORTS; ++i) {
    policy.on_trusted_report("DEV", 12, T0 + i * 3000u);
    EXPECT_NE(policy.get_settle_update("DEV"), 0u) << "stale report " << static_cast<int>(i) << " of the limit";
  }

  policy.on_trusted_report("DEV", 12, T0 + 20000);
  EXPECT_EQ(policy.get_settle_update("DEV"), 0u)
      << "the count has not fallen for " << PREDICTED_END_STALE_REPORTS << " reports: progress reads only";
  policy.on_trusted_report("DEV", 12, T0 + 23000);
  EXPECT_EQ(policy.get_settle_update("DEV"), 0u) << "it stays given up while the count stays frozen";
  policy.on_trusted_report("DEV", 11, T0 + 26000);
  EXPECT_EQ(policy.get_settle_update("DEV"), T0 + 26000u + 12000u) << "a falling count aims again";
}

TEST(StatusPollPredictedEnd, ARisingCountCountsAsStaleToo) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  uint16_t remaining = 10;
  for (uint8_t i = 0; i <= PREDICTED_END_STALE_REPORTS; ++i)
    policy.on_trusted_report("DEV", remaining++, T0 + i * 3000u);
  EXPECT_EQ(policy.get_settle_update("DEV"), 0u);
}

TEST(StatusPollPredictedEnd, NoCountDropsTheAimAndAnUntrackedDeviceHasNone) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  policy.on_trusted_report("DEV", 12, T0);

  policy.on_trusted_report("DEV", std::nullopt, T0 + 1000);
  EXPECT_EQ(policy.get_settle_update("DEV"), 0u);

  policy.on_trusted_report("OTHER", 12, T0);
  EXPECT_EQ(policy.get_settle_update("OTHER"), 0u) << "only a tracked device is aimed at";
}

TEST(StatusPollPredictedEnd, ANewTrackingCycleAndClearForgetTheAim) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  policy.on_trusted_report("DEV", 12, T0);
  policy.begin_tracking("DEV", 3000, T0 + 500);  // a newer command
  EXPECT_EQ(policy.get_settle_update("DEV"), 0u);

  policy.on_trusted_report("DEV", 12, T0);
  policy.clear("DEV");
  EXPECT_EQ(policy.get_settle_update("DEV"), 0u);
  policy.begin_tracking("DEV", 0, T0);
  policy.on_trusted_report("DEV", 12, T0);
  policy.on_trusted_report("DEV", 12, T0 + 1);
  policy.begin_tracking("DEV", 0, T0 + 2);
  policy.on_trusted_report("DEV", 12, T0 + 3);
  EXPECT_NE(policy.get_settle_update("DEV"), 0u) << "the stale count of the earlier move does not carry over";
}

TEST(StatusPollPredictedEnd, ADueAimWinsAndMarksTheReadAsSettling) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  policy.on_trusted_report("DEV", 12, T0);
  const uint32_t aim = policy.get_settle_update("DEV");

  EXPECT_EQ(policy.pop_due_device(aim - 1), std::nullopt) << "not due yet";
  const auto due = policy.pop_due_device(aim + 1);
  ASSERT_TRUE(due.has_value());
  EXPECT_EQ(policy.get_settle_update("DEV"), 0u) << "the aim is consumed";
  EXPECT_TRUE(policy.take_settling_read("DEV"));
  EXPECT_FALSE(policy.take_settling_read("DEV")) << "the mark belongs to that one poll";
}

TEST(StatusPollPredictedEnd, ADueAimDropsAProgressReadDueAtTheSameTime) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  policy.on_trusted_report("DEV", 12, T0);
  policy.set_next_update("DEV", T0 + 12500);
  const uint32_t aim = policy.get_settle_update("DEV");

  ASSERT_TRUE(policy.pop_due_device(aim + 1).has_value());
  EXPECT_EQ(policy.get_next_update("DEV"), 0u) << "one read, not a progress read and a settling read";
  EXPECT_EQ(policy.pop_due_device(aim + 2), std::nullopt);
}

TEST(StatusPollPredictedEnd, AProgressReadJustBeforeTheAimIsDroppedAndTheAimStays) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  policy.on_trusted_report("DEV", 12, T0);
  const uint32_t aim = policy.get_settle_update("DEV");
  policy.set_next_update("DEV", aim - PREDICTED_END_DROPS_PROGRESS_WITHIN_MS);

  EXPECT_EQ(policy.pop_due_device(aim - PREDICTED_END_DROPS_PROGRESS_WITHIN_MS + 1), std::nullopt);
  EXPECT_EQ(policy.get_next_update("DEV"), 0u) << "the progress read is dropped";
  EXPECT_EQ(policy.get_settle_update("DEV"), aim) << "the predicted-end read stays planned";
  EXPECT_FALSE(policy.take_settling_read("DEV")) << "the dropped read marked nothing";
  EXPECT_TRUE(policy.pop_due_device(aim + 1).has_value());
}

TEST(StatusPollPredictedEnd, AProgressReadWellBeforeTheAimIsServedAsUsual) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  policy.on_trusted_report("DEV", 12, T0);
  policy.set_next_update("DEV", T0 + 3000);

  ASSERT_TRUE(policy.pop_due_device(T0 + 3001).has_value());
  EXPECT_FALSE(policy.take_settling_read("DEV")) << "a progress read stays a progress read";
  EXPECT_NE(policy.get_settle_update("DEV"), 0u);
}

TEST(StatusPollPredictedEnd, ATrustedReportWithdrawsAPoppedReadThatHasNotRunYet) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  policy.on_trusted_report("DEV", 12, T0);
  ASSERT_TRUE(policy.pop_due_device(policy.get_settle_update("DEV") + 1).has_value());

  policy.on_trusted_report("DEV", 20, T0 + 13000);  // moved again before the queued read ran
  EXPECT_FALSE(policy.take_settling_read("DEV"));
}

TEST(StatusPollPredictedEnd, AnAimBeyondTheWindowIsNotPlanned) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);

  policy.on_trusted_report("DEV", 0xFFF0, T0);  // far longer than the 240 s window
  EXPECT_EQ(policy.get_settle_update("DEV"), 0u) << "the window ends first: progress reads and its end handling stay";
  policy.on_trusted_report("DEV", 230, T0);
  EXPECT_NE(policy.get_settle_update("DEV"), 0u) << "an aim inside the window is kept";
}

TEST(StatusPollPredictedEnd, TheAimSurvivesAMillisWrap) {
  StatusPollPolicy policy;
  const uint32_t now = 0xFFFFFFFFu - 5000u;
  policy.begin_tracking("DEV", 0, now);
  policy.on_trusted_report("DEV", 12, now);
  const uint32_t aim = policy.get_settle_update("DEV");
  ASSERT_LT(aim, now) << "the aim wrapped past zero";

  EXPECT_EQ(policy.pop_due_device(now + 3000u), std::nullopt) << "not due before the wrapped aim";
  EXPECT_TRUE(policy.pop_due_device(aim + 1u).has_value());
}

// ============================================================================
// Frozen-count back-off and the follow-up window (ADR 0079)
// ============================================================================

TEST(StatusPollFrozenCount, NoBackOffWhileTheCountFalls) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  for (uint16_t remaining = 40; remaining > 30; remaining--) {
    policy.on_trusted_report("DEV", remaining, T0);
    EXPECT_EQ(policy.frozen_count_delay_ms("DEV"), 0u) << remaining;
  }
}

TEST(StatusPollFrozenCount, StepsGrowFromTheThirdReportThatDidNotFallAndStopAtTheLast) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  policy.on_trusted_report("DEV", 30, T0);  // nothing to compare with
  policy.on_trusted_report("DEV", 30, T0);
  policy.on_trusted_report("DEV", 30, T0);
  EXPECT_EQ(policy.frozen_count_delay_ms("DEV"), 0u) << "two reports that did not fall";
  policy.on_trusted_report("DEV", 30, T0);
  EXPECT_EQ(policy.frozen_count_delay_ms("DEV"), FROZEN_COUNT_BACKOFF_FIRST_MS);
  policy.on_trusted_report("DEV", 31, T0);  // a rising count is no fall either
  EXPECT_EQ(policy.frozen_count_delay_ms("DEV"), FROZEN_COUNT_BACKOFF_SECOND_MS);
  policy.on_trusted_report("DEV", 31, T0);
  EXPECT_EQ(policy.frozen_count_delay_ms("DEV"), FROZEN_COUNT_BACKOFF_MAX_MS);
  policy.on_trusted_report("DEV", 31, T0);
  EXPECT_EQ(policy.frozen_count_delay_ms("DEV"), FROZEN_COUNT_BACKOFF_MAX_MS);
}

TEST(StatusPollFrozenCount, AFallingCountOrNoCountResetsTheSteps) {
  StatusPollPolicy policy;
  policy.begin_tracking("DEV", 0, T0);
  for (int i = 0; i < 5; i++)
    policy.on_trusted_report("DEV", 30, T0);
  ASSERT_GT(policy.frozen_count_delay_ms("DEV"), 0u);
  policy.on_trusted_report("DEV", 29, T0);
  EXPECT_EQ(policy.frozen_count_delay_ms("DEV"), 0u);

  for (int i = 0; i < 5; i++)
    policy.on_trusted_report("DEV", 29, T0);
  ASSERT_GT(policy.frozen_count_delay_ms("DEV"), 0u);
  policy.on_trusted_report("DEV", std::nullopt, T0);
  EXPECT_EQ(policy.frozen_count_delay_ms("DEV"), 0u);
}

TEST(StatusPollFrozenCount, AnUnknownDeviceHasNone) {
  StatusPollPolicy policy;
  EXPECT_EQ(policy.frozen_count_delay_ms("NOPE"), 0u);
}

TEST(StatusPollFrozenCount, TheStepsAreLongerThanTheShortCadenceAndNeverShrink) {
  static_assert(FROZEN_COUNT_BACKOFF_FIRST_MS > DEFAULT_SETTLE_POLL_DELAY_MS);
  static_assert(FROZEN_COUNT_BACKOFF_FIRST_MS < FROZEN_COUNT_BACKOFF_SECOND_MS);
  static_assert(FROZEN_COUNT_BACKOFF_SECOND_MS < FROZEN_COUNT_BACKOFF_MAX_MS);
  static_assert(MAX_TRACKED_STATUS_POLL_WINDOW_MS == 240000);
}

TEST(StatusPollFrozenCount, AMovingReportThatOpensTheWindowFixesItsDeadlineThere) {
  StatusPollPolicy policy;
  ASSERT_EQ(policy.on_moving_report("DEV", T0, true), MovingReportAction::OPEN);
  EXPECT_EQ(policy.get_poll_deadline("DEV"), T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS);
  EXPECT_EQ(policy.on_moving_report("DEV", T0 + 200000, true), MovingReportAction::FOLLOW);
  EXPECT_EQ(policy.get_poll_deadline("DEV"), T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS) << "a later report never moves it";
  EXPECT_EQ(policy.on_moving_report("DEV", T0 + MAX_TRACKED_STATUS_POLL_WINDOW_MS + 1, true), MovingReportAction::END);
}
