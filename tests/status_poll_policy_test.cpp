/// @file status_poll_policy_test.cpp
/// @brief Unit tests for StatusPollPolicy.
///
/// Covers: backoff ladder progressions, deadline expiry, one-shot vs interval mode,
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
// One-shot mode (no configured interval)
// ============================================================================

TEST(StatusPollPolicy, OneShotPendingFlagSetAndConsumed) {
  StatusPollPolicy policy;

  policy.set_one_shot_pending("DEV", true);
  EXPECT_TRUE(policy.is_one_shot_pending("DEV"));

  policy.clear_one_shot_pending("DEV");
  EXPECT_FALSE(policy.is_one_shot_pending("DEV"));
}

TEST(StatusPollPolicy, BeginTrackingNoOpWithoutInterval) {
  StatusPollPolicy policy;
  // No interval set — begin_tracking should be a no-op
  policy.begin_tracking("DEV", 500, T0);
  EXPECT_EQ(policy.get_poll_deadline("DEV"), 0u) << "begin_tracking without interval should not set deadline";
  EXPECT_EQ(policy.get_next_update("DEV"), 0u);
}

TEST(StatusPollPolicy, OneShotDueScanFiresWithoutInterval) {
  StatusPollPolicy policy;
  // Set up a one-shot poll (no interval, just a direct next_update)
  policy.set_next_update("DEV", T0);

  auto due = policy.pop_due_device(T0 + 1);
  EXPECT_TRUE(due.has_value());
  EXPECT_EQ(*due, "DEV");
  EXPECT_EQ(policy.get_next_update("DEV"), 0u) << "pop_due_device should clear next_update";
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
  policy.set_next_update("A", T0);
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
  EXPECT_FALSE(policy.is_one_shot_pending("DEV"));
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
