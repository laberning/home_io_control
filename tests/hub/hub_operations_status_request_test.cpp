/// @file hub_operations_status_request_test.cpp
/// @brief Tests for request_device_status() of hub_operations.cpp: the reply it decodes, the tries and backoff of a
/// poll, the link health a failure stamps, and the polling a command arms.

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
#include "log_capture.h"

using namespace esphome::home_io_control;
using namespace hub_operations_test;

// ========================================================================================
// request_device_status tests
// ========================================================================================

TEST(HubOperations, RequestDeviceStatusSuccess) {
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

  bool ok = comp.request_device_status("ABC123");
  EXPECT_TRUE(ok) << "request_device_status should succeed for cover device";
}

TEST(HubOperations, RequestDeviceNameSuccessStoresDecodedUtf8Name) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  const uint8_t payload[] = {0x00, 'R', 0xE9, 's', 'u', 'm', 0xE9, 0x20, 0x00};
  IoFrame resp = build_name_response(comp.node_id_, payload, sizeof(payload));
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.request_device_name("ABC123")) << "request_device_name should succeed with a valid response";

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_STREQ(dev->name, "R\xC3\xA9sum\xC3\xA9") << "stored name should be cached as UTF-8";
}

TEST(HubOperations, QueueRequestDeviceNameDeduplicatesPerDevice) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  comp.queue_request_device_name("ABC123");
  comp.queue_request_device_name("ABC123");

  ASSERT_EQ(comp.op_queue_.size(), 1u) << "duplicate queued name requests should collapse";
  EXPECT_EQ(comp.op_queue_.front().type, PendingOperationType::REQUEST_NAME);
  EXPECT_EQ(comp.op_queue_.front().device_id, "ABC123");
}

TEST(HubOperations, ProcessPendingRequestNameDispatchesOperation) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  const uint8_t payload[] = {'D', 'e', 'x', 'x', 'o', 0x00};
  IoFrame resp = build_name_response(comp.node_id_, payload, sizeof(payload));
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  comp.queue_request_device_name("ABC123");
  comp.process_pending_operation_();

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_STREQ(dev->name, "Dexxo") << "queued request-name operation should flow through the shared dispatcher";
}

TEST(HubOperations, SetDevicePositionFailsOnExplicitErrorResponse) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  IoFrame resp = build_error_response(comp.node_id_, RESULT_LIMITATION_BY_WIND);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_FALSE(comp.set_device_position("ABC123", 50)) << "explicit device refusals should fail the high-level command";
}

TEST(HubOperations, SetDevicePositionErrorResponseRecordsLastResultCode) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  IoFrame resp = build_error_response(comp.node_id_, RESULT_LIMITATION_BY_WIND);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_FALSE(comp.set_device_position("ABC123", 50));

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->last_result_code, RESULT_LIMITATION_BY_WIND)
      << "the reply-to-our-own-EXECUTE path should record the result code just like the unsolicited path";
  EXPECT_NE(dev->last_result_at_ms, 0u) << "recording a result should stamp a recorded-at timestamp";
  EXPECT_NE(dev->last_seen_ms, 0u)
      << "an explicit refusal is still a reply from the device — link health must update on this path too";
}

TEST(HubOperations, SetDevicePositionSuccessClearsPriorLastResultCode) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->last_result_code = RESULT_LIMITATION_BY_WIND;
  dev->last_result_at_ms = 12345;

  IoFrame resp = build_status_response(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.set_device_position("ABC123", 50))
      << "a normal position response should still succeed the high-level command";
  EXPECT_EQ(dev->last_result_code, 0u) << "a following successful EXECUTE reply should clear a stale result reason";
  EXPECT_EQ(dev->last_result_at_ms, 0u) << "clearing the result code should also clear its timestamp";
}

TEST(HubOperations, SetDevicePositionEmptyErrorPayloadDoesNotRecordLastResultCode) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  IoFrame resp{};
  init_frame(resp, true, false, true, false);
  uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(resp, comp.node_id_);
  set_src(resp, device_node_id);
  set_cmd(resp, CMD_ERROR_RESP);  // zero-length payload — pre-existing separate rejection path
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_FALSE(comp.set_device_position("ABC123", 50));
  EXPECT_EQ(dev->last_result_code, 0u)
      << "an empty CMD_ERROR_RESP payload must keep hitting the existing rejection path unchanged";
}

TEST(HubOperations, RequestDeviceStatusExplicitErrorBacksOffTrackedPolling) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  comp.set_device_status_poll_interval("ABC123", 2000);
  comp.begin_status_poll_tracking_("ABC123", 2000);

  IoFrame resp = build_error_response(comp.node_id_, RESULT_THERMAL_PROTECTION);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_FALSE(comp.request_device_status("ABC123"))
      << "tracked status polls should treat explicit device errors as failures";

  ASSERT_NE(comp.get_device("ABC123"), nullptr);
  EXPECT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u)
      << "explicit errors during tracked polling should schedule backoff";
  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 1u)
      << "explicit non-auth errors should use the normal failure streak";
}

TEST(HubOperations, RequestDeviceStatusRejectsUnknownDevice) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  bool ok = comp.request_device_status("000000");
  EXPECT_FALSE(ok) << "request_device_status should fail for unregistered device";
}

TEST(HubOperations, RequestDeviceStatusAcceptsLockDevice) {
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

  EXPECT_TRUE(comp.request_device_status("ABC123")) << "lock devices should allow the normal status request flow";
}

TEST(HubOperations, RequestDeviceStatusOneShotFailureDoesNotArmRetries) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  bool ok = comp.request_device_status("ABC123");
  EXPECT_FALSE(ok) << "one-shot request_device_status should fail when the device stays silent";

  ASSERT_NE(comp.get_device("ABC123"), nullptr);
  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), 0u)
      << "one-shot status requests should not schedule background retries";
  EXPECT_EQ(comp.poll_policy_.get_poll_deadline("ABC123"), 0u)
      << "one-shot status requests should not start tracked polling";
  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 0u)
      << "one-shot failures should not affect tracked polling backoff";
  EXPECT_EQ(comp.poll_policy_.get_auth_poll_failures("ABC123"), 0u)
      << "one-shot failures should not affect tracked polling backoff";
}

TEST(HubOperations, RequestDeviceStatusTimeoutBackoffEscalatesDuringTrackedPolling) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  comp.set_device_status_poll_interval("ABC123", 2000);
  comp.begin_status_poll_tracking_("ABC123", 2000);

  bool ok = comp.request_device_status("ABC123");
  EXPECT_FALSE(ok) << "tracked request_device_status should fail when the device stays silent";

  ASSERT_NE(comp.get_device("ABC123"), nullptr);
  uint32_t delay1 = comp.poll_policy_.get_next_update("ABC123") - esphome::millis();
  EXPECT_GE(delay1, STATUS_RETRY_AFTER_FAIL_MS - 1000) << "first silent failure should keep the short retry delay";
  EXPECT_LE(delay1, STATUS_RETRY_AFTER_FAIL_MS + 1000) << "first silent failure should not jump to a long cooldown";
  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 1u)
      << "silent failure should increment the normal poll failure streak";
  EXPECT_EQ(comp.poll_policy_.get_auth_poll_failures("ABC123"), 0u)
      << "silent failure should not count as an auth-shaped failure";

  ok = comp.request_device_status("ABC123");
  EXPECT_FALSE(ok) << "second silent request should also fail without a queued response";

  uint32_t delay2 = comp.poll_policy_.get_next_update("ABC123") - esphome::millis();
  EXPECT_GE(delay2, STATUS_RETRY_AFTER_FAIL_STEP2_MS - 1000) << "second silent failure should escalate the retry delay";
  EXPECT_LE(delay2, STATUS_RETRY_AFTER_FAIL_STEP2_MS + 1000)
      << "second silent failure should use the configured step-2 backoff";
  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 2u)
      << "second silent failure should continue the normal failure streak";
}

// A scheduler-owned status poll (StatusPollPolicy is tracking the device) caps its exchange at
// SCHEDULED_POLL_MAX_TRIES for most ladder slots: its failure is re-armed by the backoff ladder,
// so extra blocking in-exchange tries only stall loop() while a device is unresponsive. The
// exception is a middle band of the ladder (decisions::scheduled_poll_read_kind()), where the slot
// right after a manoeuvre ends is the one chance to catch a duty-cycled receiver: it is a settling
// read, as are a one-off poll and the poll after a STOP — see the tests below this one. Every user
// command keeps the full EXCHANGE_RETRY_COUNT. All cases here fail every transmit so the send count
// is exactly the attempt count.
TEST(HubOperations, TrackedStatusPollUsesASingleExchangeTry) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 2000);

  for (int i = 0; i < EXCHANGE_RETRY_COUNT; ++i)
    radio.queue_tx_result(false);

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(radio.get_send_count(), 1)
      << "the *first* slot (no recorded failures yet, i.e. the settle poll) transmits once; the "
         "5/15/30 s backoff ladder is its retry mechanism";
}

TEST(HubOperations, TrackedStatusPollRegainsRetriesAfterTheFirstMissedSlot) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 2000);
  comp.poll_policy_.on_exchange_failed("ABC123", /*auth_like=*/false, esphome::millis());
  ASSERT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 1u);

  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    radio.queue_tx_result(false);

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(static_cast<size_t>(radio.get_send_count()), planned_settling_attempts(comp))
      << "the slot just after the first missed poll is the recovery-defining one — it must get the "
         "full retry budget back";
}

TEST(HubOperations, TrackedStatusPollDropsBackToOneTryOnceTheDeviceLooksDead) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 2000);
  for (int i = 0; i < 4; ++i)
    comp.poll_policy_.on_exchange_failed("ABC123", /*auth_like=*/false, esphome::millis());
  ASSERT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 4u);

  radio.queue_tx_result(false);

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(radio.get_send_count(), 1)
      << "four consecutive misses means unreachable, not merely duty-cycled — back to a single try";
}

TEST(HubOperations, TrackedStatusPollWithAnAuthShapedStreakStaysSingleTry) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 2000);
  comp.poll_policy_.on_exchange_failed("ABC123", /*auth_like=*/true, esphome::millis());
  ASSERT_EQ(comp.poll_policy_.get_auth_poll_failures("ABC123"), 1u);
  ASSERT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 0u);

  radio.queue_tx_result(false);

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(radio.get_send_count(), 1)
      << "a device that answered with a 0x3C challenge is already awake; retries cannot buy a "
         "wake-up, and the naive status_poll_failures-only gate would wrongly grant this the band";
}

TEST(HubOperations, GraceBandReopensAfterASuccessfulReply) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 2000);
  for (int i = 0; i < 4; ++i)
    comp.poll_policy_.on_exchange_failed("ABC123", /*auth_like=*/false, esphome::millis());
  comp.poll_policy_.clear_failure_streaks("ABC123");
  comp.poll_policy_.on_exchange_failed("ABC123", /*auth_like=*/false, esphome::millis());
  ASSERT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 1u);

  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    radio.queue_tx_result(false);

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(static_cast<size_t>(radio.get_send_count()), planned_settling_attempts(comp))
      << "a device that replied resets the streak; its next miss is worth the same grace as a fresh one";
}

TEST(HubOperations, OneShotStatusPollIsASettlingRead) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  // No begin_status_poll_tracking_(): nothing re-arms this request, so it keeps all its tries.

  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    radio.queue_tx_result(false);

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(static_cast<size_t>(radio.get_send_count()), planned_settling_attempts(comp));
}

TEST(HubOperations, PollAfterAnAcceptedStopIsASettlingRead) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 1000);
  comp.poll_policy_.mark_stop_settle("ABC123");
  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    radio.queue_tx_result(false);

  EXPECT_FALSE(comp.request_device_status("ABC123"));

  EXPECT_EQ(static_cast<size_t>(radio.get_send_count()), planned_settling_attempts(comp));
  EXPECT_GT(planned_settling_attempts(comp), static_cast<size_t>(EXCHANGE_RETRY_COUNT))
      << "a mains cover is asked more often than three times, with no gap between the attempts";
}

TEST(HubOperations, PollConfirmingAStopOffTargetIsASettlingRead) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 1000);
  comp.poll_policy_.set_confirm_stopped("ABC123", true);
  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    radio.queue_tx_result(false);

  EXPECT_FALSE(comp.request_device_status("ABC123"));

  EXPECT_EQ(static_cast<size_t>(radio.get_send_count()), planned_settling_attempts(comp));
  EXPECT_FALSE(comp.poll_policy_.take_confirm_stopped("ABC123")) << "the poll consumed the mark";
}

TEST(HubOperations, PollAfterTheBandAndWithoutAMarkStaysASingleTry) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 2000);
  for (int i = 0; i < 4; ++i)
    comp.poll_policy_.on_exchange_failed("ABC123", /*auth_like=*/false, esphome::millis());
  ASSERT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 4u);
  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    radio.queue_tx_result(false);

  EXPECT_FALSE(comp.request_device_status("ABC123"));

  EXPECT_EQ(radio.get_send_count(), 1) << "progress reads stay single tries: the ladder is their retry";
}

TEST(HubOperations, HeaterPollKeepsThreeTriesInTheBandAndOneOutsideIt) {
  // The heating-level heater shares the cover status path and keeps the count-based budget: the
  // settling plan would give it nine attempts in the band.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 0);
  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS * 2; ++i)
    radio.queue_tx_result(false);

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(radio.get_send_count(), 1) << "the first slot after a start is a single try";

  const int before = radio.get_send_count();
  ASSERT_GE(comp.poll_policy_.get_status_poll_failures("ABC123"), 1u);
  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(radio.get_send_count() - before, static_cast<int>(EXCHANGE_RETRY_COUNT))
      << "heater band stays 3: a heater is not a position-settling cover";
}

TEST(HubOperations, SettlingReadThatLosesAnAttemptSucceedsOnTheNext) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  radio.set_model_tx_airtime(true);
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 1000);
  comp.poll_policy_.mark_stop_settle("ABC123");
  // The first attempt hears nothing; the reply comes only once the second attempt is out.
  radio.queue_rx_hold_until_sent(2);
  radio.queue_rx(test::make_rx_packet(build_status_response(comp.node_id_)));

  EXPECT_TRUE(comp.request_device_status("ABC123"));

  EXPECT_EQ(radio.get_send_count(), 2) << "attempt 1 lost, attempt 2 answered, no further attempt";
}

TEST(HubOperations, ProgressReadThatLosesItsOnlyAttemptFails) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  radio.set_model_tx_airtime(true);
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 1000);  // no mark: a look at a device that is busy
  radio.queue_rx_hold_until_sent(2);
  radio.queue_rx(test::make_rx_packet(build_status_response(comp.node_id_)));

  EXPECT_FALSE(comp.request_device_status("ABC123"));

  EXPECT_EQ(radio.get_send_count(), 1);
}

TEST(HubOperations, ThreeCoversSettlingTogetherEachStayInsideTheBudget) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  radio.set_model_tx_airtime(true);
  setup_cover_component(comp, radio);
  const char *const ids[] = {"ABC123", "ABC124", "ABC125"};  // none of them answers
  for (const char *id : ids) {
    if (comp.get_device(id) == nullptr) {
      comp.add_device(id, DeviceConfig{});
      test::set_kind(*comp.get_device(id), DeviceType::ROLLER_SHUTTER);
    }
    comp.begin_status_poll_tracking_(id, 1000);
    comp.poll_policy_.mark_stop_settle(id);
  }

  for (const char *id : ids) {
    const uint32_t start = esphome::test_clock::peek_ms();
    EXPECT_FALSE(comp.request_device_status(id));
    const uint32_t blocked_ms = esphome::test_clock::peek_ms() - start;
    EXPECT_LT(blocked_ms, comp.tuning_.exchange_total_budget_ms) << "device " << id << " blocked the loop too long";
  }
  EXPECT_GE(static_cast<size_t>(radio.get_send_count()), 3u * 2u) << "every cover got several attempts";
}

TEST(HubOperations, UserCommandRunsRightAfterAFailingSettlingReadAheadOfTheNextPoll) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  radio.set_model_tx_airtime(true);
  setup_cover_component(comp, radio);
  comp.add_device("ABC124", DeviceConfig{});
  test::set_kind(*comp.get_device("ABC124"), DeviceType::ROLLER_SHUTTER);
  for (const char *id : {"ABC123", "ABC124"}) {
    comp.begin_status_poll_tracking_(id, 1000);
    comp.poll_policy_.mark_stop_settle(id);
  }
  comp.queue_request_device_status("ABC123");
  comp.process_pending_operation_();  // the settling read: nobody answers, it uses up its attempts
  const int reads = radio.get_send_count();
  ASSERT_GT(reads, 1) << "a settling read tries again within its exchange";
  const uint32_t read_end_ms = esphome::test_clock::peek_ms();

  // The user pressed a button while that read blocked; the next scheduled poll is due too.
  comp.queue_request_device_status("ABC124");
  ASSERT_TRUE(comp.queue_device_command("ABC123", CoverCommand::FAVORITE));
  comp.process_pending_operation_();

  ASSERT_GT(radio.get_send_count(), reads) << "the user's command went out in the very next operation";
  IoFrame first_after{};
  ASSERT_TRUE(parse(radio.get_sent_data()[reads].data(), static_cast<uint8_t>(radio.get_sent_data()[reads].size()),
                    first_after));
  EXPECT_EQ(first_after.cmd, CMD_EXECUTE);
  EXPECT_EQ(comp.op_queue_.size(), 1u) << "the second poll still waits behind it";
  EXPECT_LT(esphome::test_clock::peek_ms() - read_end_ms, comp.tuning_.exchange_total_budget_ms);
}

TEST(HubOperations, UserCommandKeepsTheFullRetryBudgetWhilePollTrackingIsActive) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 2000);  // tracking on -- must not leak into commands

  for (int i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    radio.queue_tx_result(false);

  EXPECT_FALSE(comp.execute_device_command_("ABC123", CoverCommand::STOP));
  EXPECT_EQ(radio.get_send_count(), EXCHANGE_MAX_ATTEMPTS)
      << "the single-try rule must never extend to user commands: a stop takes the command plan";
}

TEST(HubOperations, RepeatSafeCommandToASilentMainsCoverMakesTheCommandPlansAttempts) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  EXPECT_FALSE(comp.set_device_position("ABC123", 40));

  EXPECT_EQ(radio.get_send_count(), EXCHANGE_MAX_ATTEMPTS) << "an absolute position is harmless to repeat";
}

TEST(HubOperations, FavouriteCommandToASilentCoverKeepsTodaysThreeAttempts) {
  // 'My' toggles a moving cover to a stop: a copy that arrives after the first was heard would undo
  // it, so the favourite keeps the count-based exchange.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  EXPECT_FALSE(comp.execute_device_command_("ABC123", CoverCommand::FAVORITE));

  EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT);
}

TEST(HubOperations, ExchangeTimeoutIncrementsFailureCountersOnce) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->exchange_timeout_count, 0u) << "newly added device should start with no recorded timeouts";
  EXPECT_EQ(dev->exchange_attempt_count, 0u);

  // No queued RX at all: send_and_receive_ times out with no valid response.
  EXPECT_FALSE(comp.request_device_status("ABC123")) << "a silent device should fail the high-level command";

  const uint8_t tries = comp.exchange_engine_.get_debug().tries;
  EXPECT_EQ(dev->exchange_timeout_count, 1u)
      << "one timed-out exchange should increment the failure count exactly once";
  EXPECT_EQ(dev->exchange_attempt_count, tries)
      << "the attempt counter should accumulate this exchange's attempt count";

  // A second timeout should accumulate rather than reset.
  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(dev->exchange_timeout_count, 2u) << "a second timeout should increment the counter again";
  EXPECT_EQ(dev->exchange_attempt_count, static_cast<uint16_t>(tries * 2))
      << "attempts from both timed-out exchanges should accumulate";
}

// Issue #178: a cover polled every second while it travels lost single frames, and each loss left
// a 5 s gap in its position. A poll of a device the hub believes is moving skips its first misses
// at the configured interval; the ladder starts only on the third miss in a row.
TEST(HubOperations, RequestDeviceStatusMissWhileMovingKeepsTheConfiguredInterval) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  comp.set_device_status_poll_interval("ABC123", 2000);
  comp.begin_status_poll_tracking_("ABC123", 2000);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->is_stopped = false;  // the last report said moving

  for (uint8_t miss = 1; miss <= MOVING_POLL_MISSES_SKIPPED; miss++) {
    EXPECT_FALSE(comp.request_device_status("ABC123"));
    EXPECT_NEAR(comp.poll_policy_.get_next_update("ABC123") - esphome::millis(), 2000u, 100u) << "miss " << +miss;
    EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 0u)
        << "a skipped miss is not a ladder failure, so the next poll stays a single try";
  }

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  const uint32_t delay = comp.poll_policy_.get_next_update("ABC123") - esphome::millis();
  EXPECT_GE(delay, STATUS_RETRY_AFTER_FAIL_MS - 1000) << "the third miss in a row enters the ladder";
  EXPECT_LE(delay, STATUS_RETRY_AFTER_FAIL_MS + 1000);
  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 1u);
}

// After two skipped misses the next poll is still dispatched as a single try, and an answer resets
// the skipped misses.
TEST(HubOperations, RequestDeviceStatusAfterSkippedMissesIsASingleTryAndAnAnswerResets) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.set_device_status_poll_interval("ABC123", 2000);
  comp.begin_status_poll_tracking_("ABC123", 2000);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->is_stopped = false;

  for (uint8_t miss = 1; miss <= MOVING_POLL_MISSES_SKIPPED; miss++)
    EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_FALSE(comp.request_device_status("ABC123"));  // the third poll
  EXPECT_EQ(comp.exchange_engine_.get_debug().max_tries, SCHEDULED_POLL_MAX_TRIES)
      << "skipped misses are not ladder failures, so the poll after them stays a single try";

  comp.poll_policy_.clear_failure_streaks("ABC123");
  EXPECT_EQ(comp.poll_policy_.get_moving_poll_misses("ABC123"), 0u);
}

// A cover at rest backs off from its first miss, through the hub path too.
TEST(HubOperations, RequestDeviceStatusMissAtRestTakesTheLadder) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.set_device_status_poll_interval("ABC123", 2000);
  comp.begin_status_poll_tracking_("ABC123", 2000);
  ASSERT_TRUE(comp.get_device("ABC123")->is_stopped);

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(comp.poll_policy_.get_moving_poll_misses("ABC123"), 0u);
  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 1u);
}

// A cover that reported STOPPED off its target and awaits the confirming poll is at rest: its miss
// takes the ladder, whose next slot grants the full tries.
TEST(HubOperations, RequestDeviceStatusMissWhileSettlingTakesTheLadder) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.set_device_status_poll_interval("ABC123", 2000);
  comp.begin_status_poll_tracking_("ABC123", 2000);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->is_stopped = false;
  dev->stop_candidate.target = 100.0f;
  dev->stop_candidate.position = 40.0f;

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(comp.poll_policy_.get_moving_poll_misses("ABC123"), 0u);
  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 1u);
}

// The poll confirming an accepted STOP has the full tries; when all of them fail it takes the
// ladder even if the hub still believes the cover moves (optimistic overlay off).
TEST(HubOperations, RequestDeviceStatusStopSettleMissTakesTheLadder) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.set_device_status_poll_interval("ABC123", 2000);
  comp.begin_status_poll_tracking_("ABC123", 2000);
  comp.poll_policy_.mark_stop_settle("ABC123");
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->is_stopped = false;

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(comp.poll_policy_.get_moving_poll_misses("ABC123"), 0u);
  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 1u);
}

// A device without motion (a heat-level heater polled with the same status read) is never skipped.
TEST(HubOperations, RequestDeviceStatusMissOfADeviceWithoutMotionTakesTheLadder) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::EXTERIOR_HEATING);
  comp.set_device_status_poll_interval("ABC123", 2000);
  comp.begin_status_poll_tracking_("ABC123", 2000);
  dev->is_stopped = false;

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(comp.poll_policy_.get_moving_poll_misses("ABC123"), 0u);
}

// An explicit refusal is an answer, not a lost frame: it keeps the ladder while moving.
TEST(HubOperations, RequestDeviceStatusRefusalWhileMovingTakesTheLadder) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.set_device_status_poll_interval("ABC123", 2000);
  comp.begin_status_poll_tracking_("ABC123", 2000);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->is_stopped = false;

  IoFrame resp = build_error_response(comp.node_id_, RESULT_LIMITATION_BY_WIND);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(comp.poll_policy_.get_moving_poll_misses("ABC123"), 0u);
  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 1u);
}

TEST(HubOperations, RequestDeviceStatusChallengedMissWhileMovingKeepsTheConfiguredInterval) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  comp.set_device_status_poll_interval("ABC123", 2000);
  comp.begin_status_poll_tracking_("ABC123", 2000);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->is_stopped = false;

  IoFrame challenge = build_challenge_request(dev->node_id, comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(challenge, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_TRUE(comp.exchange_engine_.get_debug().saw_challenge);
  EXPECT_NEAR(comp.poll_policy_.get_next_update("ABC123") - esphome::millis(), 2000u, 100u)
      << "a lost closing reply mid-travel keeps the cadence too";
  EXPECT_EQ(comp.poll_policy_.get_auth_poll_failures("ABC123"), 0u);
}

TEST(HubOperations, RequestDeviceStatusAuthFailureTakesTheAuthBackoff) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  comp.set_device_status_poll_interval("ABC123", 2000);
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

  bool ok = comp.request_device_status("ABC123");
  EXPECT_FALSE(ok) << "missing final response after a challenge should fail the status request";

  uint32_t delay = comp.poll_policy_.get_next_update("ABC123") - esphome::millis();
  EXPECT_TRUE(comp.exchange_engine_.get_debug().saw_challenge)
      << "exchange debug should preserve that a challenge happened even if later retries time out";
  EXPECT_GE(delay, STATUS_AUTH_RETRY_AFTER_FAIL_MS - 1000)
      << "first auth-shaped failure should use the configured auth backoff";
  EXPECT_LE(delay, STATUS_AUTH_RETRY_AFTER_FAIL_MS + 1000)
      << "first auth-shaped failure should use the configured auth backoff";
  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 0u)
      << "auth-shaped failures should reset the silent-failure streak";
  EXPECT_EQ(comp.poll_policy_.get_auth_poll_failures("ABC123"), 1u)
      << "auth-shaped failures should increment their own streak";
}

TEST(HubOperations, RequestDeviceStatusUnconfirmedAcceptStillReturnsFalse) {
  // ExchangeEngine now reports SUCCESS_UNCONFIRMED for a status poll that authenticated but never
  // got a final reply (Step 1's retry fix). execute_request_and_update_()'s
  // unconfirmed_counts_as_success rule must still treat that as a failure for anything but
  // CMD_EXECUTE — a status poll exists to obtain a payload, and there is none to hand back.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

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
  // No final response queued on any try.

  EXPECT_FALSE(comp.request_device_status("ABC123"))
      << "a status poll ending SUCCESS_UNCONFIRMED must still be reported as a failure to the caller";
}

// A failed exchange in which the device sent a 0x3C challenge still stamps link health: the
// challenge is a frame from the device, so it is reachable even though the exchange did not
// complete. Without this, a device that challenges every attempt but never closes ages like a
// dead one and any reachability verdict built on last_seen_ms would be wrong.
TEST(HubOperations, SawChallengeFailureStampsLinkHealth) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_EQ(dev->last_seen_ms, 0u) << "no frame from this device yet";

  IoFrame challenge = build_challenge_request(dev->node_id, comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(challenge, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);
  // Only the challenge is queued -- no final response on any try, so the poll fails.

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  ASSERT_TRUE(comp.exchange_engine_.get_debug().saw_challenge);
  EXPECT_NE(dev->last_seen_ms, 0u)
      << "a device that answered with a 0x3C challenge is reachable, even on a failed exchange";
}

// The complement: a genuinely silent failure (no challenge, no frame of any kind) must NOT stamp
// last_seen_ms -- silence is not evidence of life.
TEST(HubOperations, SilentFailureDoesNotStampLinkHealth) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  EXPECT_FALSE(comp.request_device_status("ABC123")) << "nothing queued -> silent timeout";
  EXPECT_FALSE(comp.exchange_engine_.get_debug().saw_challenge);
  EXPECT_EQ(dev->last_seen_ms, 0u) << "a silent failure is not a frame from the device";
}

TEST(HubOperations, SetDevicePositionArmsTrackedPollingWhenConfigured) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.set_device_status_poll_interval("ABC123", 1500);

  IoFrame resp = build_status_response(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.set_device_position("ABC123", 50));

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_NE(comp.poll_policy_.get_poll_deadline("ABC123"), 0u)
      << "successful change commands should start bounded follow-up polling";
  EXPECT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u)
      << "successful change commands should schedule the first follow-up poll";
}

TEST(HubOperations, SetDevicePositionWithoutConfiguredIntervalUsesTrackedSettlePolling) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  IoFrame resp = build_moving_status_response(comp.node_id_, 5);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.set_device_position("ABC123", 50));

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_NE(comp.poll_policy_.get_poll_deadline("ABC123"), 0u)
      << "commands without a configured interval should still start bounded tracked polling";
  EXPECT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u)
      << "moving execute replies should schedule a follow-up poll at the default settle cadence";
}

TEST(HubOperations, StopCommandArmsTrackedPollingToConfirmRestingPosition) {
  // The execute ack is never trusted for position (see update_device_status_()'s trust_position
  // parameter), but is_stopped IS applied from it — and an untrusted "stopped" claim used to clear
  // tracking outright, leaving the one thing that actually writes position (a trusted CMD_PRIVATE
  // poll) never scheduled. arm_execute_confirmation_poll_() re-arms the window after every execute,
  // so a STOP must still end with a confirming poll due within STOP_SETTLE_POLL_CAP_MS.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  IoFrame resp = build_status_response(comp.node_id_);  // is_stopped=true, as a real STOP ack reports
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  uint32_t const before_ms = esphome::millis();
  EXPECT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::STOP));

  EXPECT_NE(comp.poll_policy_.get_poll_deadline("ABC123"), 0u)
      << "STOP must leave an active tracking window so the confirming poll can actually fire";
  uint32_t const next_update = comp.poll_policy_.get_next_update("ABC123");
  EXPECT_NE(next_update, 0u) << "STOP must schedule a confirming poll despite the untrusted stopped ack";
  EXPECT_LE(next_update, before_ms + STOP_SETTLE_POLL_CAP_MS + 50u)
      << "the confirming poll after STOP must be due within the STOP settle cap";
}

TEST(HubOperations, StopCommandWithMovingReplySchedulesShortSettlePoll) {
  // When a STOP response says the device is still moving (motor decelerating), the settle poll
  // should fire within STOP_SETTLE_POLL_CAP_MS — faster than the default motion-tracking cadence
  // and faster than any configured interval — so STOP confirms the resting position quickly.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  IoFrame resp = build_moving_status_response(comp.node_id_, 0xFF);  // an execute ack: bytes 6-7 ignored → default
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  uint32_t const before_ms = esphome::millis();
  EXPECT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::STOP));

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FALSE(dev->is_stopped) << "device reported moving after STOP";

  uint32_t const next_update = comp.poll_policy_.get_next_update("ABC123");
  EXPECT_NE(next_update, 0u) << "settle poll must be scheduled while device is still moving";
  EXPECT_LE(next_update, before_ms + STOP_SETTLE_POLL_CAP_MS + 50u)
      << "STOP settle poll must be capped to the STOP settle window";
  EXPECT_LT(STOP_SETTLE_POLL_CAP_MS, DEFAULT_SETTLE_POLL_DELAY_MS)
      << "STOP must settle faster than a normal move for this test to be meaningful";
}

TEST(HubOperations, MoveWithStoppedAckSchedulesConfirmingPollWithoutConfiguredInterval) {
  // The bug class is broader than STOP: set_device_position() used to re-arm only when a poll
  // interval was configured, so an interval-less device hit the same dead end on an ordinary move
  // whenever its untrusted execute ack happened to claim is_stopped=true.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);  // no poll interval configured for "ABC123"

  IoFrame resp = build_status_response(comp.node_id_);  // is_stopped=true in the untrusted execute ack
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  uint32_t const before_ms = esphome::millis();
  EXPECT_TRUE(comp.set_device_position("ABC123", 50));

  uint32_t const next_update = comp.poll_policy_.get_next_update("ABC123");
  EXPECT_NE(next_update, 0u) << "a move must schedule a confirming poll even without a configured interval";
  EXPECT_LE(next_update, before_ms + DEFAULT_SETTLE_POLL_DELAY_MS + 50u)
      << "an interval-less device must fall back to DEFAULT_SETTLE_POLL_DELAY_MS";
}

TEST(HubOperations, ConfirmingPollStopsOnceATrustedReplyReportsStopped) {
  // arm_execute_confirmation_poll_() re-arms tracking after every execute, so its termination
  // argument matters: the confirming poll itself is a real CMD_PRIVATE request, whose reply IS
  // trusted, so once it reports the device at rest polling must actually stop.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  IoFrame moving_resp = build_moving_status_response(comp.node_id_, 0xFF);
  uint8_t raw1[64];
  uint8_t raw1_len = serialize(moving_resp, raw1, sizeof(raw1));
  RadioRxPacket pkt1{};
  pkt1.len = raw1_len;
  memcpy(pkt1.data, raw1, raw1_len);
  pkt1.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt1);

  ASSERT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::STOP));
  ASSERT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u) << "setup: confirming poll must be armed";

  IoFrame stopped_resp = build_status_response(comp.node_id_);
  uint8_t raw2[64];
  uint8_t raw2_len = serialize(stopped_resp, raw2, sizeof(raw2));
  RadioRxPacket pkt2{};
  pkt2.len = raw2_len;
  memcpy(pkt2.data, raw2, raw2_len);
  pkt2.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt2);

  EXPECT_TRUE(comp.request_device_status("ABC123"));

  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), 0u)
      << "a trusted stopped reply must stop the confirming poll from repeating — one extra poll, not a loop";
  EXPECT_EQ(comp.poll_policy_.get_poll_deadline("ABC123"), 0u)
      << "a trusted stopped reply must clear the tracking window entirely";
}

// A CMD_PRIVATE_RESP from ABC123 answering an EXECUTE, with @p result_byte as byte 0.
static void queue_execute_answer(MockRadio &radio, const uint8_t dst[3], uint8_t result_byte) {
  IoFrame f = build_status_response(dst);
  f.data[0] = result_byte;
  uint8_t raw[64];
  const uint8_t raw_len = serialize(f, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);
}

TEST(HubOperations, ACoverThatAnswersTheSpeedBlockWithCodeFiveIsNotedOnce) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.get_device("ABC123")->travel_speed = TravelSpeed::FAST;
  ASSERT_FALSE(comp.get_device("ABC123")->speed_block_warned);

  queue_execute_answer(radio, comp.node_id_, 0x2C);  // code 5 over state 4, as a Somfy motor answers
  EXPECT_TRUE(comp.set_device_position("ABC123", 100));
  EXPECT_TRUE(comp.get_device("ABC123")->speed_block_warned) << "the first refusal is logged and remembered";
  EXPECT_EQ(comp.get_device("ABC123")->last_result_code, 0)
      << "the Last Result Code sensor keeps the meaning of a 0xFE result";

  queue_execute_answer(radio, comp.node_id_, 0x2C);
  EXPECT_TRUE(comp.set_device_position("ABC123", 0));
  EXPECT_TRUE(comp.get_device("ABC123")->speed_block_warned) << "later refusals add nothing";
}

TEST(HubOperations, AnExecuteResultCodeWarnsOncePerDeviceAndCode) {
  const IoFrame request = test::make_execute(50);  // codes 10 and 9 are never the speed block's code 5
  IoDevice first;
  IoDevice second;
  test::LogCapture log;

  detail::log_execute_result_code(first, "ABC123", request, 0x54);  // code 10
  EXPECT_EQ(log.count('W'), 1u) << "the first answer with a code warns";

  log.clear();
  detail::log_execute_result_code(first, "ABC123", request, 0x54);
  EXPECT_EQ(log.count('W'), 0u) << "the same code again does not warn";
  EXPECT_EQ(log.count('D'), 1u) << "it is still logged, at DEBUG";

  log.clear();
  detail::log_execute_result_code(first, "ABC123", request, 0x4C);  // code 9
  EXPECT_EQ(log.count('W'), 1u) << "another code on the same device warns again";

  log.clear();
  detail::log_execute_result_code(second, "DEF456", request, 0x54);
  EXPECT_EQ(log.count('W'), 1u) << "another device warns on its own";

  log.clear();
  detail::log_execute_result_code(first, "ABC123", request, 0x04);  // code 0
  EXPECT_EQ(log.lines().size(), 0u) << "code 0 logs nothing";
  EXPECT_FALSE(first.speed_block_warned);
}

TEST(HubOperations, TheShortFrameAnsweredCleanlyLogsNothing) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  queue_execute_answer(radio, comp.node_id_, 0x04);
  EXPECT_TRUE(comp.set_device_position("ABC123", 100));
  EXPECT_FALSE(comp.get_device("ABC123")->speed_block_warned);
  EXPECT_EQ(comp.get_device("ABC123")->last_result_code, 0);
}

TEST(HubOperations, CodeFiveAfterTheShortFrameDoesNotCountAsASpeedBlockRefusal) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);  // default speed: the 6-byte frame

  queue_execute_answer(radio, comp.node_id_, 0x2C);
  EXPECT_TRUE(comp.set_device_position("ABC123", 100));
  EXPECT_FALSE(comp.get_device("ABC123")->speed_block_warned);
}

TEST(HubOperations, ForceOpenSendsPositionZeroForNonInvertedDevice) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);  // ROLLER_SHUTTER, inverted=false (default)

  IoFrame resp = build_status_response(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::FORCE_OPEN));

  ASSERT_EQ(radio.get_sent_data().size(), 1u);
  const auto &tx = radio.get_sent_data().front();
  ASSERT_GE(tx.size(), 13u);
  EXPECT_EQ(tx[10], 0x03) << "ACEI should be elevated priority level 0 (protection_human)";
  EXPECT_EQ(tx[11], 0x00) << "position LSB should be 0 (fully open) for a non-inverted device";
  EXPECT_EQ(tx[12], 0x00) << "position MSB should be 0";
}

TEST(HubOperations, ForceOpenSendsPositionHundredForInvertedDevice) {
  // A horizontal-awning-style inverted device's wire-scale "fully open" is 100, not 0 — sending
  // 0 to such a device (the original bug, confirmed on real hardware) just re-targets its
  // already-resting closed position and produces no movement at all.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->invert_override = true;
  resolve_device_features(*dev);

  IoFrame resp = build_status_response(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::FORCE_OPEN));

  ASSERT_EQ(radio.get_sent_data().size(), 1u);
  const auto &tx = radio.get_sent_data().front();
  ASSERT_GE(tx.size(), 13u);
  EXPECT_EQ(tx[11], 0xC8) << "position LSB should be 200 (doubled 100%) for an inverted device";
  EXPECT_EQ(tx[12], 0x00) << "position MSB should be 0";
}

TEST(HubOperations, ExchangeFailureOnCommandSchedulesBackoffRetry) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  // No response queued — exchange will fail
  EXPECT_FALSE(comp.set_device_position("ABC123", 50));

  EXPECT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u)
      << "exchange failure (device may have received the command) should schedule a backoff retry poll";
  EXPECT_NE(comp.poll_policy_.get_poll_deadline("ABC123"), 0u) << "backoff retry requires tracking to remain active";
}

TEST(HubOperations, ShortPrivateResponseSixBytesIsAcceptedAndSchedulesSettlePoll) {
  // Reproduces the case where a device responds with data_len=6 (no hint byte).
  // Before the fix, PRIVATE_RESPONSE_MIN_DATA_LEN=8 caused these replies to be silently
  // discarded as unsupported_payload, leaving HA stuck on the pre-command position.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  IoFrame resp = build_short_moving_status_response(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  uint32_t const before_ms = esphome::millis();
  EXPECT_TRUE(comp.set_device_position("ABC123", 50));

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FALSE(dev->is_stopped) << "6-byte moving response should mark device as moving";
  EXPECT_NE(comp.poll_policy_.get_poll_deadline("ABC123"), 0u)
      << "6-byte response without hint should still arm bounded settle polling";
  uint32_t const next_update = comp.poll_policy_.get_next_update("ABC123");
  EXPECT_NE(next_update, 0u) << "settle poll should be scheduled using the default fallback delay (no hint present)";
  EXPECT_GE(next_update, before_ms + DEFAULT_SETTLE_POLL_DELAY_MS)
      << "hint-less moving reply should settle at the default motion-tracking delay";
  EXPECT_LE(next_update, before_ms + DEFAULT_SETTLE_POLL_DELAY_MS + 50u)
      << "hint-less moving reply should settle at the default motion-tracking delay";
}

TEST(HubOperations, ExecuteReplyDoesNotOverwriteTargetOrPositionWithStaleValues) {
  // Real hardware showed the immediate reply to a just-sent CMD_EXECUTE can carry stale
  // pre-command target/current bytes rather than the freshly-commanded target — see
  // tests/corpus/captures/exchange/somfy_awning_exchange_ack_reports_stale_target_*.yaml. Trusting them
  // clobbers a correct optimistic UI state with a wrong one for a few seconds until the next
  // status poll self-corrects. update_device_status_()'s trust_position parameter fixes this:
  // an EXECUTE's own reply updates is_stopped but leaves target/position alone.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->target = 77.0F;
  dev->position = 88.0F;

  // Decodes to target=0%, position=50%, moving — neither matches the sentinel values above nor
  // the position=50 being commanded below, so any change would prove the reply was (wrongly) trusted.
  IoFrame resp = build_short_moving_status_response(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.set_device_position("ABC123", 50));

  EXPECT_FLOAT_EQ(dev->target, 77.0F) << "EXECUTE's own reply must not overwrite target with stale data";
  EXPECT_FLOAT_EQ(dev->position, 88.0F) << "EXECUTE's own reply must not overwrite position with stale data";
  EXPECT_FALSE(dev->is_stopped) << "is_stopped is still applied from the reply";
}

TEST(HubOperations, TiltExecuteReplyTiltBlockIsNotDecodedAsPosition) {
  // Issue 60: an EXECUTE-tilt ack lays out its payload differently from a position status
  // reply — data[4] is the tilt selector 0x20 and data[5..6] carry a 16-bit tilt value, where a
  // position-bearing 0x04 has the current position in data[4..5]. Decoded generically, data[4..5]
  // reads 0x2060 = 16%, so every tilt command appeared to snap the cover to the same position
  // (84% in Home Assistant, the inverted form) regardless of where it actually was.
  // The bytes below are the ones off the reporter's wire; see
  // tests/corpus/captures/exchange/tilt_cover_exchange_ack_tilt_block.yaml.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::INTERIOR_VENETIAN_BLIND);  // supports tilt
  dev->target = 77.0F;
  dev->position = 88.0F;
  // The angle the entity layer already applied optimistically for this very command
  // (DeviceRegistry::apply_optimistic_tilt()); the ack must not overwrite it.
  dev->tilt = 83.0F;

  IoFrame resp = build_tilt_execute_ack_response(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.set_device_tilt("ABC123", 83));

  EXPECT_FLOAT_EQ(dev->position, 88.0F)
      << "the tilt selector byte must not be decoded as the current-position MSB (would read 16%)";
  EXPECT_FLOAT_EQ(dev->target, 77.0F) << "a tilt ack carries no main-position target either (would read 100%)";
  EXPECT_FLOAT_EQ(dev->tilt, 83.0F) << "the ack's tilt block (0x608D ~= 52%) is in-flight or pre-command state of "
                                       "unknown meaning, so it must not replace the optimistic commanded angle";
  EXPECT_FALSE(dev->is_stopped) << "is_stopped is still applied from the reply";
}

TEST(HubOperations, StatusPollReplyStillTrustsTargetAndPosition) {
  // Contrast with the test above: a reply to our own status poll (request_device_status(), not
  // set_device_position()) must still be trusted for target/position as before — only the
  // EXECUTE command's own reply is suspect.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->target = 77.0F;
  dev->position = 88.0F;

  IoFrame resp = build_short_moving_status_response(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.request_device_status("ABC123"));

  EXPECT_FLOAT_EQ(dev->target, 0.0F) << "a status-poll reply must still be trusted for target";
  EXPECT_FLOAT_EQ(dev->position, 50.0F) << "a status-poll reply must still be trusted for position";
}

TEST(HubOperations, QueueRequestDeviceStatusDeduplicatesPerDevice) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  comp.queue_request_device_status("ABC123");
  comp.queue_request_device_status("ABC123");

  ASSERT_EQ(comp.op_queue_.size(), 1u) << "duplicate queued status polls for the same device should collapse";
  EXPECT_EQ(comp.op_queue_.front().type, PendingOperationType::REQUEST_STATUS);
  EXPECT_EQ(comp.op_queue_.front().device_id, "ABC123");
}

TEST(HubOperations, HotWaterDeclaredAsSwitchQueuesNoStatusRequestAndCountsNoFailure) {
  // The switch declared with io_device_type: 0x33 schedules an initial status request; the guard
  // drops it before anything is queued or sent, so no exchange failure is recorded.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->entity_class = DeviceCapabilityClass::SWITCH;
  test::set_kind(*dev, DeviceType::DOMESTIC_HOT_WATER);

  comp.queue_request_device_status("ABC123");

  EXPECT_TRUE(comp.op_queue_.empty());
  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_TRUE(radio.get_sent_data().empty()) << "nothing is transmitted, so no exchange can fail";
}
