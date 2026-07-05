/// @file operation_queue_test.cpp
/// @brief Unit tests for OperationQueue.
///
/// Covers: FIFO ordering, position+tilt coalescing in both directions,
/// cross-device non-coalescing, status/name deduplication, discover-and-pair
/// priority front-push with poll flushing, and dedup of discover-and-pair.

#include "operation_queue.h"

#include <gtest/gtest.h>

#include <string>

using namespace esphome::home_io_control;

// ============================================================================
// FIFO ordering
// ============================================================================

TEST(OperationQueue, FifoOrder) {
  OperationQueue q;
  q.enqueue_set_position("A", 10);
  q.enqueue_set_position("B", 20);
  q.enqueue_set_position("C", 30);

  EXPECT_EQ(q.size(), 3u);
  EXPECT_EQ(q.pop()->device_id, "A");
  EXPECT_EQ(q.pop()->device_id, "B");
  EXPECT_EQ(q.pop()->device_id, "C");
  EXPECT_FALSE(q.pop().has_value());
}

TEST(OperationQueue, EmptyReturnsFalse) {
  OperationQueue q;
  EXPECT_TRUE(q.empty());
  EXPECT_FALSE(q.pop().has_value());
}

// ============================================================================
// Position + tilt coalescing
// ============================================================================

TEST(OperationQueue, CoalescePositionWithPendingTilt) {
  OperationQueue q;
  q.enqueue_set_tilt("DEV", 75);
  bool coalesced = q.enqueue_set_position("DEV", 50);

  EXPECT_TRUE(coalesced) << "position arriving after tilt should coalesce";
  ASSERT_EQ(q.size(), 1u);
  const auto &op = q.front();
  EXPECT_EQ(op.type, PendingOperationType::SET_POSITION_AND_TILT);
  EXPECT_EQ(op.position, 50u);
  EXPECT_EQ(op.tilt, 75u);
}

TEST(OperationQueue, CoalesceTiltWithPendingPosition) {
  OperationQueue q;
  q.enqueue_set_position("DEV", 50);
  bool coalesced = q.enqueue_set_tilt("DEV", 75);

  EXPECT_TRUE(coalesced) << "tilt arriving after position should coalesce";
  ASSERT_EQ(q.size(), 1u);
  const auto &op = q.front();
  EXPECT_EQ(op.type, PendingOperationType::SET_POSITION_AND_TILT);
  EXPECT_EQ(op.position, 50u);
  EXPECT_EQ(op.tilt, 75u);
}

TEST(OperationQueue, CoalesceDoesNotAffectDifferentDevice) {
  OperationQueue q;
  q.enqueue_set_position("A", 10);
  bool coalesced = q.enqueue_set_tilt("B", 20);

  EXPECT_FALSE(coalesced) << "tilt for a different device should not coalesce";
  EXPECT_EQ(q.size(), 2u);
  EXPECT_EQ(q[0].type, PendingOperationType::SET_POSITION);
  EXPECT_EQ(q[1].type, PendingOperationType::SET_TILT);
}

TEST(OperationQueue, NoCoalescePositionWithPosition) {
  OperationQueue q;
  q.enqueue_set_position("DEV", 30);
  bool coalesced = q.enqueue_set_position("DEV", 60);

  EXPECT_FALSE(coalesced) << "two SET_POSITION ops should not coalesce";
  EXPECT_EQ(q.size(), 2u);
}

TEST(OperationQueue, NoCoalesceTiltWithTilt) {
  OperationQueue q;
  q.enqueue_set_tilt("DEV", 30);
  bool coalesced = q.enqueue_set_tilt("DEV", 60);

  EXPECT_FALSE(coalesced) << "two SET_TILT ops should not coalesce";
  EXPECT_EQ(q.size(), 2u);
}

TEST(OperationQueue, CoalescePreservesOtherOpsInQueue) {
  OperationQueue q;
  q.enqueue_request_status("DEV");
  q.enqueue_set_position("DEV", 40);
  q.enqueue_set_tilt("DEV", 60);

  ASSERT_EQ(q.size(), 2u) << "status + coalesced position_and_tilt = 2 ops";
  EXPECT_EQ(q[0].type, PendingOperationType::REQUEST_STATUS);
  EXPECT_EQ(q[1].type, PendingOperationType::SET_POSITION_AND_TILT);
  EXPECT_EQ(q[1].position, 40u);
  EXPECT_EQ(q[1].tilt, 60u);
}

// ============================================================================
// SET_TILT standalone stores tilt in op.position (dispatch protocol)
// ============================================================================

TEST(OperationQueue, SetTiltStoresInPositionField) {
  OperationQueue q;
  q.enqueue_set_tilt("DEV", 88);

  ASSERT_EQ(q.size(), 1u);
  EXPECT_EQ(q.front().type, PendingOperationType::SET_TILT);
  // Standalone SET_TILT stores the tilt in op.position for the dispatcher.
  EXPECT_EQ(q.front().position, 88u);
}

// ============================================================================
// Status / name deduplication
// ============================================================================

TEST(OperationQueue, RequestStatusDeduplicates) {
  OperationQueue q;
  bool pushed1 = q.enqueue_request_status("DEV");
  bool pushed2 = q.enqueue_request_status("DEV");

  EXPECT_TRUE(pushed1);
  EXPECT_FALSE(pushed2) << "duplicate status request should be suppressed";
  EXPECT_EQ(q.size(), 1u);
}

TEST(OperationQueue, RequestStatusDifferentDevicesNotDeduped) {
  OperationQueue q;
  q.enqueue_request_status("A");
  q.enqueue_request_status("B");

  EXPECT_EQ(q.size(), 2u) << "status requests for different devices are independent";
}

TEST(OperationQueue, RequestNameDeduplicates) {
  OperationQueue q;
  bool pushed1 = q.enqueue_request_name("DEV");
  bool pushed2 = q.enqueue_request_name("DEV");

  EXPECT_TRUE(pushed1);
  EXPECT_FALSE(pushed2) << "duplicate name request should be suppressed";
  EXPECT_EQ(q.size(), 1u);
}

// ============================================================================
// Discover-and-pair priority + flushing
// ============================================================================

TEST(OperationQueue, DiscoverAndPairFlushesPollsAndPushesToFront) {
  OperationQueue q;
  q.enqueue_request_status("A");
  q.enqueue_request_name("B");
  q.enqueue_set_position("C", 50);

  bool pushed = q.enqueue_discover_and_pair();

  EXPECT_TRUE(pushed);
  ASSERT_EQ(q.size(), 2u) << "status+name flushed, position+discover remain";
  EXPECT_EQ(q.front().type, PendingOperationType::DISCOVER_AND_PAIR) << "discover should be at front";
  EXPECT_EQ(q.back().type, PendingOperationType::SET_POSITION) << "position command should be preserved";
}

TEST(OperationQueue, DiscoverAndPairDeduplicates) {
  OperationQueue q;
  bool first = q.enqueue_discover_and_pair();
  bool second = q.enqueue_discover_and_pair();

  EXPECT_TRUE(first);
  EXPECT_FALSE(second) << "duplicate discover should be suppressed";
  EXPECT_EQ(q.size(), 1u);
}

// ============================================================================
// Binary entity encoding
// ============================================================================

TEST(OperationQueue, SetLightStateOnEncoding) {
  OperationQueue q;
  q.enqueue_set_light_state("DEV", true);
  ASSERT_EQ(q.size(), 1u);
  EXPECT_EQ(q.front().type, PendingOperationType::SET_LIGHT_STATE);
  EXPECT_EQ(q.front().position, BINARY_ENTITY_ON_POSITION);
}

TEST(OperationQueue, SetLightStateOffEncoding) {
  OperationQueue q;
  q.enqueue_set_light_state("DEV", false);
  EXPECT_EQ(q.front().position, BINARY_ENTITY_OFF_POSITION);
}

TEST(OperationQueue, SetLockStateLockedEncoding) {
  OperationQueue q;
  q.enqueue_set_lock_state("DEV", true);
  ASSERT_EQ(q.size(), 1u);
  EXPECT_EQ(q.front().type, PendingOperationType::SET_LOCK_STATE);
  // locked → OFF_POSITION (100)
  EXPECT_EQ(q.front().position, BINARY_ENTITY_OFF_POSITION);
}

TEST(OperationQueue, SetLockStateUnlockedEncoding) {
  OperationQueue q;
  q.enqueue_set_lock_state("DEV", false);
  // unlocked → ON_POSITION (0)
  EXPECT_EQ(q.front().position, BINARY_ENTITY_ON_POSITION);
}

TEST(OperationQueue, SetSwitchStateOnEncoding) {
  OperationQueue q;
  q.enqueue_set_switch_state("DEV", true);
  ASSERT_EQ(q.size(), 1u);
  EXPECT_EQ(q.front().type, PendingOperationType::SET_SWITCH_STATE);
  EXPECT_EQ(q.front().position, BINARY_ENTITY_ON_POSITION);
}

// ============================================================================
// Subscript and iterator access
// ============================================================================

TEST(OperationQueue, SubscriptAccess) {
  OperationQueue q;
  q.enqueue_set_position("X", 11);
  q.enqueue_set_position("Y", 22);
  EXPECT_EQ(q[0].device_id, "X");
  EXPECT_EQ(q[1].device_id, "Y");
}

TEST(OperationQueue, IteratorAccess) {
  OperationQueue q;
  q.enqueue_set_position("P", 5);
  q.enqueue_set_position("Q", 6);

  int count = 0;
  for (const auto &op : q) {
    EXPECT_EQ(op.type, PendingOperationType::SET_POSITION);
    count++;
  }
  EXPECT_EQ(count, 2);
}
