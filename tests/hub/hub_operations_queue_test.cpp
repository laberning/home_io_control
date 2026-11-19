/// @file hub_operations_queue_test.cpp
/// @brief Tests for the operation queue of hub_operations.cpp: dispatch, coalescing, and what a command's outcome does
/// (an exchange never closed, the rollback of a prediction, the re-send of an unconfirmed EXECUTE).

#include "hub_core.h"
#include "hub_internal.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_private_registers.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"
#include "reading_store.h"
#include "stubs/radio_test_common.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <utility>

#include "hub_operations_fixture.h"

using namespace esphome::home_io_control;
using namespace hub_operations_test;

// ========================================================================================
// Queued operation dispatch
// ========================================================================================

TEST(HubOperations, QueuedOperationProcessesPosition) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  IoFrame resp = build_status_response(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  comp.queue_set_device_position("ABC123", 75);
  ASSERT_EQ(comp.op_queue_.size(), 1u) << "one operation should be queued";

  comp.process_pending_operation_();
  EXPECT_TRUE(comp.op_queue_.empty()) << "queue should be empty after processing";
}

TEST(HubOperations, QueuedOperationSkipsWhenBusy) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  comp.busy_ = true;
  comp.queue_set_device_position("ABC123", 50);
  comp.process_pending_operation_();
  EXPECT_EQ(comp.op_queue_.size(), 1u) << "should not process when busy";
}

TEST(HubOperations, DuplicateDiscoverAndPairNotQueued) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  comp.queue_discover_and_pair();
  comp.queue_discover_and_pair();  // duplicate
  EXPECT_EQ(comp.op_queue_.size(), 1u) << "duplicate discover should be suppressed";
}

TEST(HubOperations, QueueDevicePositionRejectedForLight) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::LIGHT);

  comp.queue_set_device_position("ABC123", 50);
  EXPECT_TRUE(comp.op_queue_.empty()) << "non-cover device should be rejected in queue check";
}

TEST(HubOperations, QueueDeviceCommandReturnsTrueForValidCoverFalseForUnknownDevice) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  EXPECT_TRUE(comp.queue_device_command("ABC123", CoverCommand::FAVORITE))
      << "a known cover device should be validated and enqueued";
  ASSERT_EQ(comp.op_queue_.size(), 1u);
  EXPECT_EQ(comp.op_queue_.front().type, PendingOperationType::DEVICE_COMMAND);

  EXPECT_FALSE(comp.queue_device_command("999999", CoverCommand::FAVORITE))
      << "an unregistered device should be rejected, not silently enqueued";
  EXPECT_EQ(comp.op_queue_.size(), 1u) << "the rejected command should not have been enqueued";
}

TEST(HubOperations, QueueSetLockStateRejectsNonLockDevice) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  comp.queue_set_lock_state("ABC123", true);
  EXPECT_TRUE(comp.op_queue_.empty()) << "non-lock devices should be rejected in queued lock commands";
}

TEST(HubOperations, ProcessPendingLockOperationDispatchesLockCommand) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::LOCK);

  IoFrame resp = build_status_response(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  comp.queue_set_lock_state("ABC123", true);
  ASSERT_EQ(comp.op_queue_.size(), 1u);
  EXPECT_EQ(comp.op_queue_.front().type, PendingOperationType::SET_LOCK_STATE);

  comp.process_pending_operation_();
  EXPECT_TRUE(comp.op_queue_.empty()) << "queued lock operation should be consumed by the dispatcher";
}

TEST(HubOperations, QueueDeviceTiltRejectedForNonTilt) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  // ROLLER_SHUTTER does not support tilt
  comp.queue_set_device_tilt("ABC123", 50);
  EXPECT_TRUE(comp.op_queue_.empty()) << "non-tilt device should be rejected in queue check";
}

// ========================================================================================
// Queue coalescing tests — SET_POSITION + SET_TILT → SET_POSITION_AND_TILT
// ========================================================================================

TEST(HubOperations, CoalescePositionThenTilt) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::INTERIOR_VENETIAN_BLIND);  // supports tilt

  // Queue position first, then tilt for the same device
  comp.queue_set_device_position("ABC123", 50);
  comp.queue_set_device_tilt("ABC123", 75);

  // Should coalesce into a single SET_POSITION_AND_TILT
  ASSERT_EQ(comp.op_queue_.size(), 1u) << "coalesced ops should produce one entry";
  EXPECT_EQ(comp.op_queue_.front().type, PendingOperationType::SET_POSITION_AND_TILT);
  EXPECT_EQ(comp.op_queue_.front().device_id, "ABC123");
  EXPECT_EQ(comp.op_queue_.front().position, 50u);
  EXPECT_EQ(comp.op_queue_.front().tilt, 75u);
}

TEST(HubOperations, CoalesceTiltThenPosition) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::INTERIOR_VENETIAN_BLIND);

  // Queue tilt first, then position for the same device
  comp.queue_set_device_tilt("ABC123", 80);
  comp.queue_set_device_position("ABC123", 30);

  // Should coalesce into a single SET_POSITION_AND_TILT
  ASSERT_EQ(comp.op_queue_.size(), 1u) << "coalesced ops should produce one entry";
  EXPECT_EQ(comp.op_queue_.front().type, PendingOperationType::SET_POSITION_AND_TILT);
  EXPECT_EQ(comp.op_queue_.front().device_id, "ABC123");
  EXPECT_EQ(comp.op_queue_.front().position, 30u);
  EXPECT_EQ(comp.op_queue_.front().tilt, 80u);
}

TEST(HubOperations, NoCoalesceForDifferentDevices) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::INTERIOR_VENETIAN_BLIND);

  // Register a second tilt-capable device
  comp.add_device("DEF456");
  auto *dev2 = comp.get_device("DEF456");
  ASSERT_NE(dev2, nullptr);
  test::set_kind(*dev2, DeviceType::INTERIOR_VENETIAN_BLIND);

  // Queue position for one device, tilt for another — should NOT coalesce
  comp.queue_set_device_position("ABC123", 50);
  comp.queue_set_device_tilt("DEF456", 75);

  ASSERT_EQ(comp.op_queue_.size(), 2u) << "different devices should not coalesce";
  EXPECT_EQ(comp.op_queue_[0].type, PendingOperationType::SET_POSITION);
  EXPECT_EQ(comp.op_queue_[0].device_id, "ABC123");
  EXPECT_EQ(comp.op_queue_[1].type, PendingOperationType::SET_TILT);
  EXPECT_EQ(comp.op_queue_[1].device_id, "DEF456");
}

TEST(HubOperations, NoCoalescePositionWithPosition) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::INTERIOR_VENETIAN_BLIND);

  // Queue two positions — should not coalesce (no tilt involved)
  comp.queue_set_device_position("ABC123", 50);
  comp.queue_set_device_position("ABC123", 70);

  ASSERT_EQ(comp.op_queue_.size(), 2u) << "two SET_POSITION ops should not coalesce with each other";
  EXPECT_EQ(comp.op_queue_[0].type, PendingOperationType::SET_POSITION);
  EXPECT_EQ(comp.op_queue_[1].type, PendingOperationType::SET_POSITION);
}

TEST(HubOperations, NoCoalesceTiltWithTilt) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::INTERIOR_VENETIAN_BLIND);

  // Queue two tilts — should not coalesce (no position involved)
  comp.queue_set_device_tilt("ABC123", 50);
  comp.queue_set_device_tilt("ABC123", 70);

  ASSERT_EQ(comp.op_queue_.size(), 2u) << "two SET_TILT ops should not coalesce with each other";
  EXPECT_EQ(comp.op_queue_[0].type, PendingOperationType::SET_TILT);
  EXPECT_EQ(comp.op_queue_[1].type, PendingOperationType::SET_TILT);
}

TEST(HubOperations, CoalesceDoesNotAffectOtherPendingOps) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::INTERIOR_VENETIAN_BLIND);

  // Queue a status request for a different device, then position + tilt for the target.
  // The status poll for "ABC123" would be dropped, but "OTHER"'s poll should survive.
  // The tilt coalesces with the position into SET_POSITION_AND_TILT, which precedes the background poll.
  comp.queue_request_device_status("ABC123");  // This will be dropped when SET_POSITION for ABC123 arrives.
  comp.queue_set_device_position("ABC123", 40);
  comp.queue_set_device_tilt("ABC123", 60);

  // Same-device REQUEST_STATUS is dropped; the coalesced SET_POSITION_AND_TILT remains.
  ASSERT_EQ(comp.op_queue_.size(), 1u) << "same-device status request is dropped; only the coalesced op remains";
  EXPECT_EQ(comp.op_queue_[0].type, PendingOperationType::SET_POSITION_AND_TILT);
  EXPECT_EQ(comp.op_queue_[0].position, 40u);
  EXPECT_EQ(comp.op_queue_[0].tilt, 60u);
}

// ============================================================================
// A device that authenticates a command but never closes the exchange has still *taken* the
// command. Whether that counts as success depends on what was asked — see ExchangeOutcome and
// execute_request_and_update_().
// ============================================================================

TEST(HubOperations, CommandAuthenticatedWithoutFinalResponseCountsAsSuccess) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 2000);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  // Queue only the challenge — no final response, exactly like a real RS100.
  IoFrame challenge = build_challenge_request(dev->node_id, comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(challenge, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.set_device_position("ABC123", 100))
      << "the device challenged and we answered, so it has the command — that is not a failure";
  EXPECT_EQ(comp.poll_policy_.get_auth_poll_failures("ABC123"), 0u)
      << "an accepted command must not be recorded as an auth-shaped failure";
  EXPECT_EQ(dev->exchange_timeout_count, 0u) << "nor as a timeout";
  EXPECT_EQ(dev->exchange_unconfirmed_count, 1u)
      << "but it must still be counted somewhere: reported as success and recorded nowhere, a device "
         "that never closes an exchange would be invisible in the diagnostics";
}

TEST(HubOperations, StatusPollAuthenticatedWithoutFinalResponseStillFails) {
  // The mirror image, and the reason the distinction exists: a status poll's entire purpose is the
  // payload. Authenticating and then hearing nothing answers no question, so it stays a failure
  // and takes the auth-shaped backoff.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 2000);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  IoFrame challenge = build_challenge_request(dev->node_id, comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(challenge, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_FALSE(comp.request_device_status("ABC123")) << "a poll that returned no payload has not succeeded";
  EXPECT_EQ(comp.poll_policy_.get_auth_poll_failures("ABC123"), 1u) << "and it is the auth-shaped kind";
  EXPECT_EQ(dev->exchange_timeout_count, 1u) << "it counts as a failure";
  EXPECT_EQ(dev->exchange_unconfirmed_count, 1u)
      << "and as an unconfirmed one, so this is distinguishable from a device that never answered at all — "
         "the failure counter alone conflates the two";
}

// ============================================================================
// Optimistic-prediction rollback on command failure (run_execute_operation_ wraps
// try_execute_operation_ and withdraws the overlay on every false return).
// ============================================================================

// §7.12 — a silent exchange failure after an optimistic target withdraws the overlay.
TEST(HubOperations, SilentExchangeFailureRollsBackOptimisticPrediction) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 40.0f));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  EXPECT_FALSE(comp.set_device_position("ABC123", 40)) << "no response -> the command fails";
  EXPECT_TRUE(dev->optimistic.empty()) << "a failed command withdraws the prediction";
  EXPECT_TRUE(effective_is_stopped(*dev)) << "the entity falls back to the observed (stopped) state";
  EXPECT_EQ(dev->target, UNKNOWN_POSITION) << "the observed target was never touched";
}

// §7.13 — an explicit CMD_ERROR_RESP refusal withdraws the overlay too.
TEST(HubOperations, ErrorResponseRollsBackOptimisticPrediction) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 40.0f));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  IoFrame resp = build_error_response(comp.node_id_, RESULT_LIMITATION_BY_WIND);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_FALSE(comp.set_device_position("ABC123", 40));
  EXPECT_TRUE(dev->optimistic.empty()) << "a device refusal (LIMITATION_BY_WIND) also withdraws the prediction";
  EXPECT_TRUE(effective_is_stopped(*dev));
}

// §7.14 — a profile-guard rejection (device retyped so accepts() fails) withdraws the overlay.
TEST(HubOperations, ProfileGuardRejectionRollsBackOptimisticPrediction) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 40.0f));
  test::set_kind(*dev, DeviceType::LIGHT);  // the cover-position guard now rejects a mid position

  EXPECT_FALSE(comp.set_device_position("ABC123", 50));
  EXPECT_TRUE(dev->optimistic.empty()) << "a guard rejection is still a command that will not happen";
}

// §7.15 — a SUCCESS_UNCONFIRMED CMD_EXECUTE (authenticated, no final response) KEEPS the prediction.
TEST(HubOperations, SuccessUnconfirmedExecuteRetainsOptimisticPrediction) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 40.0f));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  // Queue only the challenge — the device authenticated the command, so it has it.
  IoFrame challenge = build_challenge_request(dev->node_id, comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(challenge, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.set_device_position("ABC123", 40)) << "an authenticated command is not a failure";
  EXPECT_FALSE(dev->optimistic.empty()) << "an accepted command must keep its prediction";
  EXPECT_FLOAT_EQ(dev->optimistic.target, 40.0f);
  EXPECT_EQ(dev->optimistic.motion, OptimisticState::Motion::MOVING);
}

// A STOP that gets no answer (a device that ignores everything while it travels) must not leave the
// entity idle: the movement prediction it replaced comes back, direction and target included.
TEST(HubOperations, FailedStopRestoresTheMovementItReplaced) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 100.0f));
  ASSERT_TRUE(comp.apply_optimistic_stop("ABC123"));

  EXPECT_FALSE(comp.execute_device_command_("ABC123", CoverCommand::STOP)) << "no response -> the STOP fails";
  EXPECT_EQ(dev->optimistic.motion, OptimisticState::Motion::MOVING);
  EXPECT_FLOAT_EQ(effective_target(*dev), 100.0f);
  EXPECT_FALSE(effective_is_stopped(*dev));
}

// An accepted STOP keeps its stop prediction and can no longer be rolled back into movement.
TEST(HubOperations, AcceptedStopForgetsTheMovementItReplaced) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 100.0f));
  ASSERT_TRUE(comp.apply_optimistic_stop("ABC123"));

  // Queue only the challenge — the device authenticated the STOP, so it has it.
  IoFrame challenge = build_challenge_request(dev->node_id, comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(challenge, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::STOP));
  EXPECT_EQ(dev->optimistic.motion, OptimisticState::Motion::STOPPED);
  EXPECT_FALSE(dev->optimistic.stop_replaced_movement);
}

// §7.16 — a failed tilt command withdraws the predicted angle; effective_tilt falls back to the
// last observed one.
TEST(HubOperations, FailedTiltCommandRollsBackOptimisticTilt) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::INTERIOR_VENETIAN_BLIND);  // tilt-capable
  dev->tilt = 15.0f;                                          // a real angle from an earlier poll
  ASSERT_TRUE(comp.apply_optimistic_tilt("ABC123", 80.0f));
  ASSERT_FLOAT_EQ(effective_tilt(*dev), 80.0f);

  EXPECT_FALSE(comp.set_device_tilt("ABC123", 80)) << "no response -> the tilt command fails";
  EXPECT_TRUE(dev->optimistic.empty());
  EXPECT_FLOAT_EQ(effective_tilt(*dev), 15.0f) << "effective tilt falls back to the last observed angle";
}

// A queue-time guard rejection also withdraws the prediction. control() predicts, then calls a
// queue_*() method; a guard that rejects there never reaches run_execute_operation_(), so the
// queue method must roll back itself or the entity animates a command that will not happen.
TEST(HubOperations, QueueTimeGuardRejectionRollsBackOptimisticPrediction) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 40.0f));
  test::set_kind(*dev, DeviceType::LIGHT);  // the queued cover-position guard now rejects

  comp.queue_set_device_position("ABC123", 50);
  EXPECT_TRUE(dev->optimistic.empty()) << "a queue-time guard rejection withdraws the prediction too";

  // Same for the bool-returning STOP path (queue_device_command's inline COVER guard).
  ASSERT_TRUE(comp.apply_optimistic_stop("ABC123"));
  EXPECT_FALSE(comp.queue_device_command("ABC123", CoverCommand::STOP));
  EXPECT_TRUE(dev->optimistic.empty()) << "a rejected queued STOP withdraws the predicted stop";
}

// ============================================================================
// Unconfirmed EXECUTE re-send — which outcomes mark a device as one that confirms, and the hub path
// ============================================================================
// The engine re-sends an unconfirmed CMD_EXECUTE only to a device that has closed an EXECUTE
// exchange with a reply before (IoDevice::confirms_execute). These pin where that is learned.

TEST(HubOperations, ExecuteAnsweredWithAReplyMarksTheDeviceAsConfirming) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_FALSE(dev->confirms_execute) << "nothing is known about a device until it has answered";
  radio.queue_rx(test::make_rx_packet(build_status_response(comp.node_id_)));

  ASSERT_TRUE(comp.set_device_position("ABC123", 50));

  EXPECT_TRUE(dev->confirms_execute);
}

TEST(HubOperations, ExecuteAnsweredWithAnErrorStillMarksTheDeviceAsConfirming) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  radio.queue_rx(test::make_rx_packet(build_error_response(comp.node_id_, RESULT_PRIORITY_LEVEL_LOCKED)));

  EXPECT_FALSE(comp.set_device_position("ABC123", 50)) << "the device refused the command";

  EXPECT_TRUE(comp.get_device("ABC123")->confirms_execute) << "a refusal still closes the exchange";
}

TEST(HubOperations, UnconfirmedExecuteDoesNotMarkTheDevice) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  radio.queue_rx(test::make_rx_packet(build_challenge_request(dev->node_id, comp.node_id_)));

  ASSERT_TRUE(comp.set_device_position("ABC123", 50)) << "an unconfirmed EXECUTE still counts as accepted";

  EXPECT_FALSE(dev->confirms_execute);
  EXPECT_EQ(dev->exchange_unconfirmed_count, 1u);
}

TEST(HubOperations, AnsweredStatusPollDoesNotMarkTheDevice) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  radio.queue_rx(test::make_rx_packet(build_status_response(comp.node_id_)));

  ASSERT_TRUE(comp.request_device_status("ABC123"));

  EXPECT_FALSE(comp.get_device("ABC123")->confirms_execute)
      << "only an EXECUTE's reply says how the device ends an EXECUTE exchange";
}

TEST(HubOperations, UnconfirmedStopIsSentTwiceOnceTheDeviceHasConfirmedACommand) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");

  // First command: answered with a reply, which is how the hub learns this device confirms.
  radio.queue_rx(test::make_rx_packet(build_status_response(comp.node_id_)));
  ASSERT_TRUE(comp.set_device_position("ABC123", 50));
  ASSERT_TRUE(dev->confirms_execute);
  const size_t sent_before = radio.get_sent_data().size();
  const int send_count_before = radio.get_send_count();

  // STOP: challenged, never closed, then the re-send is challenged and closed.
  radio.queue_rx(test::make_rx_packet(build_challenge_request(dev->node_id, comp.node_id_)));
  radio.queue_rx_hold_until_sent(send_count_before + 3);  // silent until the re-sent STOP is out
  radio.queue_rx(test::make_rx_packet(build_challenge_request(dev->node_id, comp.node_id_)));
  radio.queue_rx_hold_until_sent(send_count_before + 4);
  radio.queue_rx(test::make_rx_packet(build_status_response(comp.node_id_)));

  ASSERT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::STOP));

  int executes = 0;
  for (size_t i = sent_before; i < radio.get_sent_data().size(); ++i) {
    const auto &sent = radio.get_sent_data()[i];
    IoFrame f{};
    if (parse(sent.data(), static_cast<uint8_t>(sent.size()), f) && f.cmd == CMD_EXECUTE)
      executes++;
  }
  EXPECT_EQ(executes, 2) << "the STOP whose challenge answer went unconfirmed is sent once more";
  EXPECT_EQ(dev->exchange_unconfirmed_count, 0u) << "the re-send was confirmed, so the exchange was too";
}
