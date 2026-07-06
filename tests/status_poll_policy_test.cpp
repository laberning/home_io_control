/// @file status_poll_policy_test.cpp
/// @brief Unit tests for StatusPollPolicy.
///
/// Covers: backoff ladder progressions, deadline expiry, interval-0 and interval-set tracking modes,
/// due-device scan, failure-streak management, and interval preservation across clears.

#include "status_poll_policy.h"

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

  // All failures at T0 — the deadline is T0 + MAX_TRACKED (600s), so all fit.
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

  uint32_t b5 = policy.on_exchange_failed("DEV", false, T0);
  EXPECT_EQ(b5, STATUS_RETRY_AFTER_FAIL_MAX_MS);
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
  EXPECT_EQ(b3, STATUS_AUTH_RETRY_AFTER_FAIL_MAX_MS);
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
