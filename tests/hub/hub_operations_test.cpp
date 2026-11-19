/// @file hub_operations_test.cpp
/// @brief Tests for hub_operations.cpp high-level command execution.
///
/// The heater commands are in hub_heating_operations_test.cpp.

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
#include <vector>

#include "hub_operations_fixture.h"

using namespace esphome::home_io_control;
using namespace hub_operations_test;

// ============================================================================
// HubOperations test suite
// ============================================================================
// Tests for set_device_position, set_device_tilt, request_device_status,
// set_light_position, set_switch_state, set_lock_state, and the queued operation dispatch.
//
// These tests verify capability-gating and correct command construction
// using mock radio and a testable component subclass.

namespace {
// Build a response frame from device (matching device node_id 0xABC123)
static IoFrame build_status_response(const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(f, dst);
  set_src(f, device_node_id);
  uint8_t payload[8] = {test::STATUS_BYTE_AT_REST, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));
  return f;
}

static IoFrame build_moving_status_response(const uint8_t dst[3], uint8_t remaining_s) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(f, dst);
  set_src(f, device_node_id);
  uint8_t payload[8] = {
      test::STATUS_BYTE_MOVING, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, remaining_s};  // bytes 6-7: remaining time
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));
  return f;
}

// 6-byte private response: stopped-flag + position bytes only; no hint byte.
// Matches devices that omit the delay-hint field (e.g. some V3-era actuators).
static IoFrame build_short_moving_status_response(const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(f, dst);
  set_src(f, device_node_id);
  // Bytes 0=flags(moving), 1=0, 2-3=target(0x0000=0%), 4-5=current(0x6400=50%)
  uint8_t payload[6] = {test::STATUS_BYTE_MOVING, 0x00, 0x00, 0x00, 0x64, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));
  return f;
}

// 8-byte private response in the EXECUTE-tilt ack layout: data[4] is the tilt selector and
// data[5..6] a 16-bit slat angle, where a position-bearing 0x04 carries the current position in
// data[4..5]. Byte-for-byte the ack from issue 60 — see
// tests/corpus/captures/exchange/tilt_cover_exchange_ack_tilt_block.yaml.
static IoFrame build_tilt_execute_ack_response(const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(f, dst);
  set_src(f, device_node_id);
  // Read as a position reply this decodes to target=0xC800=100% and current=0x2060=16%, neither
  // of which is where the cover is; read correctly it is selector + tilt block + absent hint.
  uint8_t payload[8] = {0x04, 0x80, 0xC8, 0x00, STATUS_TILT_SELECTOR, 0x60, 0x8D, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));
  return f;
}
static IoFrame build_name_response(const uint8_t dst[3], const uint8_t *payload, uint8_t payload_len) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(f, dst);
  set_src(f, device_node_id);
  set_cmd(f, CMD_GET_NAME_RESP, payload, payload_len);
  return f;
}

}  // namespace

// ========================================================================================
// set_device_position tests
// ========================================================================================

TEST(HubOperations, SetDevicePositionSuccess) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  // Build a valid status response and queue it
  IoFrame resp = build_status_response(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  bool ok = comp.set_device_position("ABC123", 50);
  EXPECT_TRUE(ok) << "set_device_position should succeed with valid response";
}

TEST(HubOperations, SetDevicePositionRejectsUnknownDevice) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  bool ok = comp.set_device_position("000000", 50);
  EXPECT_FALSE(ok) << "set_device_position should fail for unregistered device";
}

TEST(HubOperations, SetDevicePositionRejectedForLight) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::LIGHT);

  bool ok = comp.set_device_position("ABC123", 50);
  EXPECT_FALSE(ok) << "set_device_position should reject mid position for light device";
}

TEST(HubOperations, LightBinaryPositionExchangeFails) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::LIGHT);

  bool ok = comp.set_device_position("ABC123", 0);
  EXPECT_FALSE(ok) << "set_device_position fails because send_and_receive_ gets no response";
}

TEST(HubOperations, SetLockStateSuccess) {
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

  EXPECT_TRUE(comp.set_lock_state("ABC123", true))
      << "lock devices should support lock commands via shared execute path";
}

TEST(HubOperations, SetLockStateRejectsLightDevice) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::LIGHT);

  EXPECT_FALSE(comp.set_lock_state("ABC123", true)) << "non-lock devices should be rejected by lock entity gating";
}

TEST(HubOperations, SetLockStateRejectsTheCarControllerSubtypeOfTheLockType) {
  // The table gives lock subtype 2 (car controller) no entity class, so a `lock:` entry bound to it
  // is refused although the bare lock type would be accepted.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::LOCK, 2);

  EXPECT_FALSE(comp.set_lock_state("ABC123", true));
  EXPECT_TRUE(radio.get_tx_configs().empty()) << "a refused command puts no frame on air";
}

TEST(HubOperations, SetLockStateAcceptsTheWindowLockSubtypeOfTheLockType) {
  // Lock subtype 1 (window lock) keeps the lock class of its type, so it is driven like a lock.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::LOCK, 1);

  IoFrame resp = build_status_response(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.set_lock_state("ABC123", true));
  EXPECT_FALSE(radio.get_tx_configs().empty());
}

TEST(HubOperations, StatusRequestIsRefusedForTheCarControllerSubtypeOfTheLockType) {
  // A kind with no entity class takes no EXECUTE position and is not polled with a status request.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::LOCK, 2);

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_TRUE(radio.get_tx_configs().empty()) << "a refused status request puts no frame on air";
}

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

// Attempts the engine plans for a settling read of "ABC123" while it stays silent: the number a
// read after a STOP, a confirming read, a ladder-band poll or a one-off poll sends.
size_t planned_settling_attempts(TestableComponent &comp) {
  const IoDevice *dev = comp.get_device("ABC123");
  IoFrame request{};
  EXPECT_TRUE(create_get_status(request, comp.node_id_, dev->node_id, dev->low_power));
  const ExchangeEngine &engine = comp.exchange_engine_;
  return decisions::attempt_plan(decisions::ReadKind::SETTLING, dev->low_power, comp.tuning_,
                                 engine.tx_time_ms(request, FREQ_CH2, engine.request_preamble_for(request)))
      .max_attempts;
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

TEST(HubOperations, SilentDeviceSendsTheSilentExecutePayload) {
  // End-to-end: the YAML-declared per-cover flag has to reach the wire, not just the builder.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->travel_speed = TravelSpeed::SILENT;

  comp.set_device_position("ABC123", 100);

  ASSERT_FALSE(radio.get_sent_data().empty());
  const auto &frame = radio.get_sent_data().front();
  ASSERT_GE(frame.size(), 17u) << "an execute frame is 9 header bytes plus an 8-byte payload";
  EXPECT_EQ(frame[14], POS_FAVORITE) << "the secondary-target byte is unaffected";
  EXPECT_EQ(frame[15], 0x05) << "silent devices travel on the slow profile";
}

TEST(HubOperations, ACoverWithoutAChosenSpeedSendsTheShortFrame) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_EQ(dev->travel_speed, TravelSpeed::MOTOR_DEFAULT) << "a speed block is opt-in";

  comp.set_device_position("ABC123", 100);

  ASSERT_FALSE(radio.get_sent_data().empty());
  EXPECT_EQ(radio.get_sent_data().front().size(), 15u) << "9 header bytes plus the 6-byte main-parameter payload";
}

TEST(HubOperations, AFastCoverSendsTheSpeedBlockWithTheFastProfile) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.get_device("ABC123")->travel_speed = TravelSpeed::FAST;

  comp.set_device_position("ABC123", 100);

  const auto &frame = radio.get_sent_data().front();
  ASSERT_GE(frame.size(), 17u) << "an execute frame is 9 header bytes plus an 8-byte payload";
  EXPECT_EQ(frame[14], POS_FAVORITE);
  EXPECT_EQ(frame[15], 0x06);
}

// ============================================================================
// low_power class -> CTRL1_LOW_POWER bit + start-frame preamble (end-to-end)
// ============================================================================
// The per-device low_power class, declared in YAML and carried through DeviceConfig into the
// registry, must reach BOTH the transmitted frame's CTRL1_LOW_POWER bit AND the preamble the
// exchange engine picks for the start frame. A call site reverting to a hardcoded low_power
// literal (the exact regression of issue #87) has to trip one of these assertions.

// Register "ABC123" as a ROLLER_SHUTTER with an explicit low_power class via DeviceConfig — the
// same path the codegen'd entity binding uses, exercised here without the entity mixin (that side
// is covered in platform_cover_test.cpp).
static void setup_low_power_cover(TestableComponent &comp, MockRadio &radio, bool low_power) {
  comp.node_id_[0] = 0xC0;
  comp.node_id_[1] = 0xFF;
  comp.node_id_[2] = 0xEE;
  static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
  comp.initialized_ = true;
  comp.radio_ = &radio;
  DeviceConfig cfg;
  cfg.type = DeviceType::ROLLER_SHUTTER;
  cfg.low_power = low_power;
  comp.add_device("ABC123", cfg);
  ASSERT_NE(comp.get_device("ABC123"), nullptr);
}

TEST(HubOperations, LowPowerDeviceExecuteUsesLongPreambleAndSetsLowPowerBit) {
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);

  comp.set_device_position("ABC123", 50);

  ASSERT_GE(radio.get_tx_configs().size(), 1u) << "the EXECUTE start frame should have been transmitted";
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, MOCK_WAKE_PREAMBLE)
      << "a low_power target's directed start frame must use the wake-up preamble";
  ASSERT_FALSE(radio.get_sent_data().empty());
  EXPECT_NE(radio.get_sent_data().front()[1] & CTRL1_LOW_POWER, 0)
      << "a low_power target's frame must carry CTRL1_LOW_POWER";
}

TEST(HubOperations, DefaultDeviceExecuteUsesNormalStartPreambleAndClearsLowPowerBit) {
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/false);

  comp.set_device_position("ABC123", 50);

  ASSERT_GE(radio.get_tx_configs().size(), 1u) << "the EXECUTE start frame should have been transmitted";
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "an always-alive target's directed start frame uses `start_preamble_ms`, not MOCK_WAKE_PREAMBLE";
  ASSERT_FALSE(radio.get_sent_data().empty());
  EXPECT_EQ(radio.get_sent_data().front()[1] & CTRL1_LOW_POWER, 0)
      << "an always-alive target's frame must leave CTRL1_LOW_POWER clear";
}

TEST(HubOperations, AStatusReadMarkedStateUnknownLeadsShortDespiteARecentStoppedReport) {
  // The hub carries AttemptRequest::with_state_unknown() to the exchange: a catch-up read of a cover
  // that reported "stopped" a second ago still leads with the short attempt a moving receiver hears.
  for (const bool marked : {false, true}) {
    TestableComponent comp;
    MockRadio radio;
    setup_low_power_cover(comp, radio, /*low_power=*/true);
    IoDevice *dev = comp.get_device("ABC123");
    ASSERT_NE(dev, nullptr);
    dev->last_status = esphome::millis();  // a report from just now: the read is not UNKNOWN by age

    IoFrame request{};
    create_get_status(request, comp.node_id_, dev->node_id, /*low_power=*/true);
    auto attempts = decisions::AttemptRequest::of_kind(decisions::ReadKind::SETTLING);
    if (marked)
      attempts = attempts.with_state_unknown();
    IoFrame response{};
    comp.send_and_receive_(request, response, FREQ_CH2, attempts);

    ASSERT_GE(radio.get_tx_configs().size(), 1u);
    EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, marked ? mock_preamble_bytes(comp.tuning_.start_preamble_ms)
                                                             : mock_preamble_bytes(comp.tuning_.low_power_wake_ms))
        << (marked ? "marked: the short lead first"
                   : "unmarked: a recent report keeps the read on the wake-up preamble");
  }
}

TEST(HubOperations, LowPowerDeviceStatusPollUsesLongPreambleAndSetsLowPowerBit) {
  // create_get_status is the frame the issue #87 reporter watched time out — cover it explicitly.
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);

  comp.request_device_status("ABC123");

  ASSERT_GE(radio.get_tx_configs().size(), 2u) << "the status poll start frames should have been transmitted";
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "a read to a receiver with no news of its state leads with one short attempt (ADR 0079)";
  EXPECT_EQ(radio.get_tx_configs()[1].preamble_len, MOCK_WAKE_PREAMBLE)
      << "a low_power target's status poll must use the wake-up preamble after the short lead";
  ASSERT_FALSE(radio.get_sent_data().empty());
  EXPECT_NE(radio.get_sent_data().front()[1] & CTRL1_LOW_POWER, 0)
      << "a low_power target's status poll must carry CTRL1_LOW_POWER";
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
// Low-power wake belief — evidence stamps and hub -> engine wiring
// ============================================================================
// A low_power device's start-frame preamble follows its wake belief (decisions::wake_belief()):
// the hub supplies the evidence (last_moving_evidence_ms / last_seen_ms), the engine picks the
// preamble per try. These tests run on a manual clock so ages are exact.

namespace {

void queue_frame(MockRadio &radio, const IoFrame &frame) { radio.queue_rx(test::make_rx_packet(frame)); }

/// Preamble of the transmit at `index` (0-based, in send order) — fails the test if there was none.
uint16_t preamble_of_tx(const MockRadio &radio, size_t index) {
  EXPECT_LT(index, radio.get_tx_configs().size()) << "expected a transmit #" << index;
  return index < radio.get_tx_configs().size() ? radio.get_tx_configs()[index].preamble_len : 0;
}

}  // namespace

// --- evidence: which paths stamp / clear last_moving_evidence_ms -------------------------------

TEST(HubOperations, AcceptedMoveWithoutAckStampsMovingEvidenceEvenWithoutAnOptimisticOverlay) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->optimistic_state = false;  // no overlay: the accept itself is the only signal of a move
  queue_frame(radio, build_challenge_request(dev->node_id, comp.node_id_));  // authenticated, never closed

  ASSERT_TRUE(comp.set_device_position("ABC123", 40)) << "an authenticated command is accepted";

  EXPECT_NE(dev->last_moving_evidence_ms, 0u);
}

TEST(HubOperations, FailedMoveDoesNotStampMovingEvidence) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->optimistic_state = false;

  ASSERT_FALSE(comp.set_device_position("ABC123", 40)) << "no reply at all: the command failed";

  EXPECT_EQ(dev->last_moving_evidence_ms, 0u) << "a command the device never accepted proves nothing";
}

TEST(HubOperations, StopAcceptedWithoutAStatusLeavesTheEvidenceAsItWas) {
  // A challenged STOP with no closing reply has no status to say whether the motor is still winding
  // down, so the evidence is left alone; the settle poll decides (a silent one spends it).
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  note_moving_evidence(*dev, esphome::millis());  // it was travelling
  const uint32_t stamp = dev->last_moving_evidence_ms;
  queue_frame(radio, build_challenge_request(dev->node_id, comp.node_id_));

  ASSERT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::STOP));

  EXPECT_EQ(dev->last_moving_evidence_ms, stamp);
}

TEST(HubOperations, StopAckThatReportsMovingKeepsTheSettlePollShortFirst) {
  // Field log (VELUX SSL, ADR 0040 amendment): the ack to an accepted STOP still reported "moving"
  // while the motor wound down, and the settle poll a second later drew nothing on three wake-up
  // tries. The ack's own flag keeps the evidence, so that poll leads with the short preamble.
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  note_moving_evidence(*dev, esphome::millis());
  queue_frame(radio, build_moving_status_response(comp.node_id_, /*remaining_s=*/0));

  ASSERT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::STOP));
  EXPECT_NE(dev->last_moving_evidence_ms, 0u) << "the motor said it is still moving";

  esphome::test_clock::advance_ms(STOP_SETTLE_POLL_CAP_MS);
  const size_t before = radio.get_tx_configs().size();
  comp.request_device_status("ABC123");

  ASSERT_EQ(radio.get_tx_configs().size() - before, static_cast<size_t>(LOW_POWER_SETTLING_ATTEMPTS));
  EXPECT_EQ(preamble_of_tx(radio, before), mock_preamble_bytes(comp.tuning_.start_preamble_ms));
  EXPECT_EQ(preamble_of_tx(radio, before + 1), MOCK_WAKE_PREAMBLE);
  EXPECT_EQ(preamble_of_tx(radio, before + 2), MOCK_WAKE_PREAMBLE);
}

TEST(HubOperations, FailedStopKeepsMovingEvidence) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  note_moving_evidence(*dev, esphome::millis());

  ASSERT_FALSE(comp.execute_device_command_("ABC123", CoverCommand::STOP)) << "no reply: the STOP failed";

  EXPECT_NE(dev->last_moving_evidence_ms, 0u) << "the receiver was never told to stop, so it may still be moving";
}

TEST(HubOperations, StopAckThatReportsStoppedLeavesNoEvidenceDespiteStalePositions) {
  // The ack to our own STOP is decoded with trust_position=false, so dev.target/position are stale
  // values from an earlier poll that have not converged, and normalize_stopped_state() reads that as
  // "still moving" (stamping evidence). The accepted STOP must still end with none, or a STOP that
  // worked would leave the receiver looking like it is travelling for LOW_POWER_MAX_TRAVEL_MS.
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->target = 100.0f;
  dev->position = 30.0f;
  note_moving_evidence(*dev, esphome::millis());
  queue_frame(radio, build_status_response(comp.node_id_));  // STATUS_STOPPED set

  ASSERT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::STOP));

  EXPECT_EQ(dev->last_moving_evidence_ms, 0u);
}

TEST(HubOperations, DecodedStatusTracksMovingEvidence) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  queue_frame(radio, build_status_response(comp.node_id_));  // stopped
  ASSERT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(dev->last_moving_evidence_ms, 0u);

  queue_frame(radio, build_moving_status_response(comp.node_id_, /*remaining_s=*/5));
  ASSERT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_NE(dev->last_moving_evidence_ms, 0u) << "a decoded 'not stopped' status is evidence of travel";

  queue_frame(radio, build_status_response(comp.node_id_));  // it arrived and stopped
  ASSERT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(dev->last_moving_evidence_ms, 0u) << "a decoded 'stopped' status spends the evidence";
}

// --- wiring: the belief the engine derives from the hub's evidence ----------------------------

TEST(HubOperations, HaMoveToARestingLowPowerDeviceLeadsWithTheWakeUpPreamble) {
  // IOHomeCover::control() applies the optimistic overlay *before* the command is dispatched. That
  // prediction must not count as moving evidence, or a resting, duty-cycling receiver would be
  // treated as awake by the very command that has to wake it.
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 40.0f));

  comp.set_device_position("ABC123", 40);

  EXPECT_EQ(preamble_of_tx(radio, 0), MOCK_WAKE_PREAMBLE);
}

TEST(HubOperations, StopToALowPowerDeviceAtRestLeadsWithTheShortPreamble) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);

  comp.execute_device_command_("ABC123", CoverCommand::STOP);

  EXPECT_EQ(preamble_of_tx(radio, 0), mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "a STOP goes to a receiver that is moving, whatever the stamps say";
}

TEST(HubOperations, SettlePollAfterAnAcceptedMoveIsASingleTryLeadingWithTheShortPreamble) {
  // The tracked settle poll gets one try (its backoff ladder is its retry), and that try follows the
  // wake belief: seconds after an accepted move the receiver is travelling, and a travelling VELUX
  // solar receiver ignores the wake-up preamble (ADR 0040 D2).
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  queue_frame(radio, build_challenge_request(dev->node_id, comp.node_id_));
  ASSERT_TRUE(comp.set_device_position("ABC123", 40));
  ASSERT_NE(dev->last_moving_evidence_ms, 0u) << "precondition: the belief would be AWAKE";
  esphome::test_clock::advance_ms(3000);
  comp.begin_status_poll_tracking_("ABC123", 0);
  const size_t before = radio.get_tx_configs().size();

  comp.request_device_status("ABC123");

  ASSERT_EQ(radio.get_tx_configs().size() - before, 1u) << "scheduler-owned poll: a single try";
  EXPECT_EQ(preamble_of_tx(radio, before), mock_preamble_bytes(comp.tuning_.start_preamble_ms));
}

TEST(HubOperations, SettlePollAfterAnAcceptedStopLeadsShortThenGetsEveryTryOnTheWakeUpPreamble) {
  // After an accepted STOP nothing moves and the user's reversal waits on this poll, so it gets the
  // full budget instead of betting on one sample (a settling read, ADR 0074). The STOP cleared the
  // moving evidence and no status report has come since, so the receiver's state is unknown: one short
  // attempt reaches it while it still winds down, then every wake-up try, which a resting receiver
  // hears, even though its reply was just heard (ADR 0040, amendment; ADR 0079).
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  queue_frame(radio, build_challenge_request(dev->node_id, comp.node_id_));
  ASSERT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::STOP));
  ASSERT_EQ(dev->last_moving_evidence_ms, 0u) << "precondition: no moving evidence before or after the STOP";
  ASSERT_NE(dev->last_seen_ms, 0u) << "precondition: the STOP's challenge was heard";
  esphome::test_clock::advance_ms(STOP_SETTLE_POLL_CAP_MS);
  const size_t before = radio.get_tx_configs().size();

  comp.request_device_status("ABC123");

  ASSERT_EQ(radio.get_tx_configs().size() - before, static_cast<size_t>(LOW_POWER_SETTLING_ATTEMPTS) + 1);
  EXPECT_EQ(preamble_of_tx(radio, before), mock_preamble_bytes(comp.tuning_.start_preamble_ms));
  EXPECT_EQ(preamble_of_tx(radio, before + 1), MOCK_WAKE_PREAMBLE);
  EXPECT_EQ(preamble_of_tx(radio, before + 2), MOCK_WAKE_PREAMBLE);
  EXPECT_EQ(preamble_of_tx(radio, before + 3), MOCK_WAKE_PREAMBLE);
}

TEST(HubOperations, OnlyTheFirstPollAfterAStopGetsTheStopBudget) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  queue_frame(radio, build_challenge_request(dev->node_id, comp.node_id_));
  ASSERT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::STOP));
  esphome::test_clock::advance_ms(STOP_SETTLE_POLL_CAP_MS);
  comp.request_device_status("ABC123");  // the post-STOP poll, all tries miss
  // Reset the streak so the ladder's own grace band cannot hand the next poll three tries either.
  comp.poll_policy_.clear_failure_streaks("ABC123");
  const size_t before = radio.get_tx_configs().size();

  comp.request_device_status("ABC123");

  EXPECT_EQ(radio.get_tx_configs().size() - before, static_cast<size_t>(SCHEDULED_POLL_MAX_TRIES));
}

TEST(HubOperations, SettlePollAfterAFailedStopDoesNotGetTheStopBudget) {
  // A STOP nobody answered has not stopped anything: the receiver may still be travelling, so the
  // normal ladder applies (its first retry slot already gets the full budget).
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  ASSERT_FALSE(comp.execute_device_command_("ABC123", CoverCommand::STOP));
  EXPECT_FALSE(comp.poll_policy_.take_stop_settle("ABC123"));
}

TEST(HubOperations, LadderSlotWithFullRetriesLeadsWithTheShortPreambleWhileBelievedAwake) {
  // The first slot after a missed settle poll gets the full retry budget, so the belief applies:
  // short / wake-up / wake-up for a receiver believed to be travelling.
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  note_moving_evidence(*dev, esphome::millis());
  comp.begin_status_poll_tracking_("ABC123", 2000);
  comp.poll_policy_.on_exchange_failed("ABC123", /*auth_like=*/false, esphome::millis());
  ASSERT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 1u);

  comp.request_device_status("ABC123");

  ASSERT_EQ(radio.get_tx_configs().size(), 3u);
  EXPECT_EQ(preamble_of_tx(radio, 0), mock_preamble_bytes(comp.tuning_.start_preamble_ms));
  EXPECT_EQ(preamble_of_tx(radio, 1), MOCK_WAKE_PREAMBLE);
  EXPECT_EQ(preamble_of_tx(radio, 2), MOCK_WAKE_PREAMBLE);
}

TEST(HubOperations, SilentPollWhileBelievedMovingSpendsTheMovingEvidence) {
  // A short movement ends within seconds, long before LOW_POWER_MAX_TRAVEL_MS: the receiver is at
  // rest and ignores the short preamble. A poll that draws nothing at all spends the evidence, so the
  // next exchange leads with the wake-up preamble instead of losing its first try again.
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  note_moving_evidence(*dev, esphome::millis());

  comp.request_device_status("ABC123");
  EXPECT_EQ(preamble_of_tx(radio, 0), mock_preamble_bytes(comp.tuning_.start_preamble_ms));
  EXPECT_EQ(dev->last_moving_evidence_ms, 0u) << "silence while believed moving spends the evidence";

  const size_t before = radio.get_tx_configs().size();
  esphome::test_clock::advance_ms(1000);
  comp.set_device_position("ABC123", 40);
  EXPECT_EQ(preamble_of_tx(radio, before), MOCK_WAKE_PREAMBLE);
}

TEST(HubOperations, SpentEvidenceStaysSpentUntilMovementIsStampedAgain) {
  // Pins a known trade-off: a travelling receiver that misses one short try looks like one at rest,
  // so its evidence is spent. Nothing re-promotes it until something stamps movement again -- a decoded
  // status that says "moving", an overheard remote, or a movement command the device accepts. The
  // reads that follow still lead with one short attempt (the state is unknown, ADR 0079) before the
  // wake-up attempts, so a receiver that is in fact still moving is reached by them. If a field report
  // shows positions freezing mid-travel, this is the behaviour to revisit.
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  note_moving_evidence(*dev, esphome::millis());

  comp.request_device_status("ABC123");  // silent: spends the evidence
  ASSERT_EQ(dev->last_moving_evidence_ms, 0u);

  for (int poll = 0; poll < 2; poll++) {
    const size_t before = radio.get_tx_configs().size();
    esphome::test_clock::advance_ms(1000);
    comp.request_device_status("ABC123");
    EXPECT_EQ(preamble_of_tx(radio, before), mock_preamble_bytes(comp.tuning_.start_preamble_ms))
        << "poll " << poll << " after the evidence was spent: the short lead";
    EXPECT_EQ(preamble_of_tx(radio, before + 1), MOCK_WAKE_PREAMBLE)
        << "poll " << poll << " after the evidence was spent: then the wake-up preamble";
    EXPECT_EQ(dev->last_moving_evidence_ms, 0u) << "silence does not bring the evidence back";
  }

  note_moving_evidence(*dev, esphome::millis());  // what a decoded moving status or accepted move does
  const size_t before = radio.get_tx_configs().size();
  comp.request_device_status("ABC123");
  EXPECT_EQ(preamble_of_tx(radio, before), mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "fresh movement evidence leads short again";
}

TEST(HubOperations, MovingEvidenceExpiresAfterMaxTravel) {
  esphome::test_clock::ManualClock clock(500000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  note_moving_evidence(*dev, esphome::millis());

  esphome::test_clock::advance_ms(LOW_POWER_MAX_TRAVEL_MS - 1000);
  comp.request_device_status("ABC123");
  EXPECT_EQ(preamble_of_tx(radio, 0), mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "still inside the travel window";

  const size_t before = radio.get_tx_configs().size();
  esphome::test_clock::advance_ms(2000);  // now past LOW_POWER_MAX_TRAVEL_MS since the move
  comp.request_device_status("ABC123");
  EXPECT_EQ(preamble_of_tx(radio, before), mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "no news since the move and the window is over: its state is unknown, so the read leads short";
  EXPECT_EQ(preamble_of_tx(radio, before + 1), MOCK_WAKE_PREAMBLE)
      << "then the wake-up preamble a resting receiver hears";
}

TEST(HubOperations, RecentReplyAloneKeepsTheWakeUpPreamble) {
  esphome::test_clock::ManualClock clock(500000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  queue_frame(radio, build_status_response(comp.node_id_));
  ASSERT_TRUE(comp.request_device_status("ABC123")) << "a real reply: stamps last_seen_ms";
  const size_t after_reply = radio.get_tx_configs().size();

  esphome::test_clock::advance_ms(100);
  comp.request_device_status("ABC123");  // heard from 100 ms ago, reported stopped
  EXPECT_EQ(preamble_of_tx(radio, after_reply), MOCK_WAKE_PREAMBLE)
      << "a resting receiver ignores the short preamble even right after it answered";
}

TEST(HubOperations, UnregisteredDestinationIsTreatedAsAsleep) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  const uint8_t stranger[NODE_ID_SIZE] = {0x12, 0x34, 0x56};
  IoFrame request{};
  ASSERT_TRUE(create_get_status(request, comp.node_id_, stranger, /*low_power=*/true));
  IoFrame response{};

  comp.exchange_engine_.send_and_receive(request, response, FREQ_CH2);

  ASSERT_EQ(radio.get_tx_configs().size(), 3u);
  for (const auto &config : radio.get_tx_configs())
    EXPECT_EQ(config.preamble_len, MOCK_WAKE_PREAMBLE) << "no record of this node, so nothing says it is awake";
}

TEST(HubOperations, AlwaysAliveDeviceKeepsTheNormalStartPreambleOnEveryTryDespiteEvidence) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/false);
  note_moving_evidence(*comp.get_device("ABC123"), esphome::millis());

  comp.request_device_status("ABC123");

  ASSERT_EQ(radio.get_tx_configs().size(), planned_settling_attempts(comp));
  for (const auto &config : radio.get_tx_configs())
    EXPECT_EQ(config.preamble_len, mock_preamble_bytes(comp.tuning_.start_preamble_ms));
}

TEST(HubOperations, WakeBeliefSwitchOffRestoresTheWakeUpPreambleForALowPowerDevice) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  comp.tuning_.low_power_wake_belief = false;
  note_moving_evidence(*comp.get_device("ABC123"), esphome::millis());

  comp.request_device_status("ABC123");

  ASSERT_EQ(radio.get_tx_configs().size(), 3u);
  for (const auto &config : radio.get_tx_configs())
    EXPECT_EQ(config.preamble_len, MOCK_WAKE_PREAMBLE);
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

// ========================================================================================
// Garage opener: main-parameter-only movement (issue #163)
// ========================================================================================

namespace {

// Wire layout: [CTRL0][CTRL1][DST 3B][SRC 3B][CMD][DATA...] — data starts at offset 9.
constexpr size_t WIRE_DATA_OFFSET = 9;

std::vector<uint8_t> first_sent_payload(const MockRadio &radio) {
  if (radio.get_sent_data().empty())
    return {};
  const auto &frame = radio.get_sent_data()[0];
  return {frame.begin() + WIRE_DATA_OFFSET, frame.end()};
}

void make_garage(TestableComponent &comp, MockRadio &radio) {
  setup_cover_component(comp, radio);
  test::set_kind(*comp.get_device("ABC123"), DeviceType::GARAGE_OPENER);
}

}  // namespace

TEST(HubOperations, GarageOpenerPositionSendsMainParameterOnly) {
  TestableComponent comp;
  MockRadio radio;
  make_garage(comp, radio);

  comp.set_device_position("ABC123", 100);  // no reply queued: only the transmitted frame matters

  ASSERT_FALSE(radio.get_sent_data().empty());
  EXPECT_EQ(radio.get_sent_data()[0][WIRE_DATA_OFFSET - 1], CMD_EXECUTE);
  EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0xC8, 0x00, 0x00, 0x00}))
      << "a garage opener gets the 6-byte form, without the functional-parameter block";
}

TEST(HubOperations, RollerShutterPositionAtFastSpeedCarriesTheExtendedBlock) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.get_device("ABC123")->travel_speed = TravelSpeed::FAST;

  comp.set_device_position("ABC123", 0);

  EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0x00, 0x00, 0x80, 0xD8, 0x06, 0x00}));
}

TEST(HubOperations, GarageOpenerFavoriteIsRefusedWithoutTransmitting) {
  TestableComponent comp;
  MockRadio radio;
  make_garage(comp, radio);

  EXPECT_FALSE(comp.execute_device_command_("ABC123", CoverCommand::FAVORITE));
  EXPECT_TRUE(radio.get_sent_data().empty()) << "the favourite alias is refused by the device, so it is never sent";
}

TEST(HubOperations, GarageOpenerStopIsUnchanged) {
  TestableComponent comp;
  MockRadio radio;
  make_garage(comp, radio);

  comp.execute_device_command_("ABC123", CoverCommand::STOP);

  EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0xD2, 0x00, 0x00, 0x00}));
}

TEST(HubOperations, GarageOpenerForceOpenUsesMainParameterAtForcePriority) {
  TestableComponent comp;
  MockRadio radio;
  make_garage(comp, radio);

  comp.execute_device_command_("ABC123", CoverCommand::FORCE_OPEN);

  const std::vector<uint8_t> payload = first_sent_payload(radio);
  ASSERT_EQ(payload.size(), 6u);
  EXPECT_EQ(payload[1], 0x03) << "force-open priority";
  EXPECT_EQ(payload[2], 0x00) << "fully open";
  EXPECT_EQ(payload[4], 0x00);
  EXPECT_EQ(payload[5], 0x00);
}

// ========================================================================================
// Capability bits decide the frame shape (device_feature::EXTENDED_POSITION_FRAME)
// ========================================================================================

TEST(HubOperations, AnUndeclaredCoverKeepsTheSliderTheSpeedBlockOnRequestAndTheFavourite) {
  TestableComponent comp;
  MockRadio radio;
  setup_declared_component(comp, radio, {.entity_class = DeviceCapabilityClass::COVER});
  const IoDevice *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->type, DeviceType::UNKNOWN);

  comp.set_device_position("ABC123", 50);
  EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0x64, 0x00, 0x00, 0x00}))
      << "a cover that has not learned its type still gets a slider position, in the short form";

  MockRadio fast_radio;
  TestableComponent fast_comp;
  setup_declared_component(fast_comp, fast_radio,
                           {.entity_class = DeviceCapabilityClass::COVER, .travel_speed = TravelSpeed::FAST});
  fast_comp.set_device_position("ABC123", 50);
  EXPECT_EQ(first_sent_payload(fast_radio), (std::vector<uint8_t>{0x01, 0x63, 0x64, 0x00, 0x80, 0xD8, 0x06, 0x00}))
      << "a chosen speed puts the speed block on the frame";

  MockRadio favourite_radio;
  TestableComponent favourite_comp;
  setup_declared_component(favourite_comp, favourite_radio, {.entity_class = DeviceCapabilityClass::COVER});
  favourite_comp.execute_device_command_("ABC123", CoverCommand::FAVORITE);
  EXPECT_EQ(first_sent_payload(favourite_radio), (std::vector<uint8_t>{0x01, 0x63, 0xD8, 0x00, 0x00, 0x00}));
}

TEST(HubOperations, ALightSwitchOrLockSendsZeroAndHundredInTheShortFrameWhateverTheSpeed) {
  // These kinds have no speed to choose, so even a stray FAST never puts the speed block on the frame.
  for (const DeviceCapabilityClass entity_class :
       {DeviceCapabilityClass::LIGHT, DeviceCapabilityClass::SWITCH, DeviceCapabilityClass::LOCK}) {
    for (const TravelSpeed speed : {TravelSpeed::MOTOR_DEFAULT, TravelSpeed::FAST}) {
      TestableComponent comp;
      MockRadio radio;
      setup_declared_component(comp, radio, {.entity_class = entity_class, .travel_speed = speed});

      comp.set_device_position("ABC123", 100);
      EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0xC8, 0x00, 0x00, 0x00}))
          << static_cast<unsigned>(entity_class) << " " << travel_speed_name(speed);
    }
  }
}

TEST(HubOperations, AFastCoverForceOpensWithTheSpeedBlockAndADefaultCoverWithoutIt) {
  const std::vector<std::pair<TravelSpeed, std::vector<uint8_t>>> cases = {
      {TravelSpeed::MOTOR_DEFAULT, {0x01, 0x03, 0x00, 0x00, 0x00, 0x00}},
      {TravelSpeed::FAST, {0x01, 0x03, 0x00, 0x00, 0x80, 0xD8, 0x06, 0x00}},
  };
  for (const auto &[speed, payload] : cases) {
    TestableComponent comp;
    MockRadio radio;
    setup_declared_component(comp, radio, {.type = DeviceType::ROLLER_SHUTTER, .travel_speed = speed});

    comp.execute_device_command_("ABC123", CoverCommand::FORCE_OPEN);
    EXPECT_EQ(first_sent_payload(radio), payload) << travel_speed_name(speed);
  }
}

TEST(HubOperations, ASubtype58CoverHasNoTiltBecauseItTakesTheMainParameterOnly) {
  // VELUX KLF 200 API specification, Appendix 2: subtype 58 is main parameter only, so no slat angle.
  for (const DeviceType type : {DeviceType::INTERIOR_VENETIAN_BLIND, DeviceType::VERTICAL_INTERIOR_BLIND,
                                DeviceType::EXTERIOR_VENETIAN_BLIND, DeviceType::LOUVRE_BLIND}) {
    TestableComponent comp;
    MockRadio radio;
    setup_declared_component(comp, radio, {.type = type, .subtype = SUBTYPE_MAIN_PARAMETER_ONLY});

    EXPECT_FALSE(comp.set_device_tilt("ABC123", 50)) << static_cast<unsigned>(type);
    EXPECT_TRUE(radio.get_sent_data().empty());
  }
}

TEST(HubOperations, ASilentMoveToAnUnsilentableKindUsesTheShortFrame) {
  TestableComponent comp;
  MockRadio radio;
  setup_declared_component(comp, radio, {.type = DeviceType::GARAGE_OPENER, .travel_speed = TravelSpeed::SILENT});

  comp.set_device_position("ABC123", 0);
  EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0x00, 0x00, 0x00, 0x00}))
      << "the silent profile is a functional parameter, which a main-parameter-only device does not take";
}

TEST(HubOperations, InteriorVenetianAndDualShutterPositionsUseTheShortFrameEvenWhenSilent) {
  // VELUX KLF 200 API specification, Appendix 2: functional parameter 1 is the slat orientation on type 1 and
  // the upper curtain on type 13, not a travel speed, so neither takes the 8-byte form or `silent:`.
  for (const DeviceType type : {DeviceType::INTERIOR_VENETIAN_BLIND, DeviceType::DUAL_ROLLER_SHUTTER}) {
    TestableComponent comp;
    MockRadio radio;
    setup_declared_component(comp, radio, {.type = type, .travel_speed = TravelSpeed::SILENT});

    comp.set_device_position("ABC123", 50);
    EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0x64, 0x00, 0x00, 0x00}))
        << static_cast<unsigned>(type);
  }
}

TEST(HubOperations, InteriorVenetianAndDualShutterForceOpenUsesTheShortFrameAtForcePriority) {
  for (const DeviceType type : {DeviceType::INTERIOR_VENETIAN_BLIND, DeviceType::DUAL_ROLLER_SHUTTER}) {
    TestableComponent comp;
    MockRadio radio;
    setup_declared_component(comp, radio, {.type = type});

    comp.execute_device_command_("ABC123", CoverCommand::FORCE_OPEN);
    EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x03, 0x00, 0x00, 0x00, 0x00}))
        << static_cast<unsigned>(type);
  }
}

TEST(HubOperations, ARollerShutterAtFastSpeedStillSendsTheEightByteFrame) {
  TestableComponent comp;
  MockRadio radio;
  setup_declared_component(comp, radio, {.type = DeviceType::ROLLER_SHUTTER, .travel_speed = TravelSpeed::FAST});

  comp.set_device_position("ABC123", 50);
  EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0x64, 0x00, 0x80, 0xD8, 0x06, 0x00}));
}

TEST(HubOperations, EachTravelSpeedSendsItsOwnPositionFrame) {
  // A VELUX KLR 200 pad moves a window with the 6-byte frame; the window then runs at its own speed
  // (44.5 s full travel), against 26.5 s on profile 0x06 and about 65 s on 0x05.
  const std::vector<std::pair<TravelSpeed, std::vector<uint8_t>>> cases = {
      {TravelSpeed::MOTOR_DEFAULT, {0x01, 0x63, 0x64, 0x00, 0x00, 0x00}},
      {TravelSpeed::SILENT, {0x01, 0x63, 0x64, 0x00, 0x80, 0xD8, 0x05, 0x00}},
      {TravelSpeed::FAST, {0x01, 0x63, 0x64, 0x00, 0x80, 0xD8, 0x06, 0x00}},
  };
  for (const auto &[speed, payload] : cases) {
    TestableComponent comp;
    MockRadio radio;
    setup_declared_component(comp, radio, {.type = DeviceType::WINDOW_OPENER, .travel_speed = speed});

    comp.set_device_position("ABC123", 50);
    EXPECT_EQ(first_sent_payload(radio), payload) << travel_speed_name(speed);
  }
}

TEST(HubOperations, ASpeedSetAtRuntimeShapesTheNextMove) {
  TestableComponent comp;
  MockRadio radio;
  setup_declared_component(comp, radio, {.type = DeviceType::WINDOW_OPENER});

  comp.set_device_travel_speed("ABC123", TravelSpeed::MOTOR_DEFAULT);
  comp.set_device_position("ABC123", 100);
  EXPECT_EQ(first_sent_payload(radio), (std::vector<uint8_t>{0x01, 0x63, 0xC8, 0x00, 0x00, 0x00}))
      << "the select changes what the next move sends";
}

TEST(HubOperations, OnlyTheSilentSpeedExtendsTheFavoritePosition) {
  // The favorite position goes out in the 6-byte form unless silent; a Somfy hub sends exactly that pair.
  const std::vector<std::pair<TravelSpeed, std::vector<uint8_t>>> cases = {
      {TravelSpeed::MOTOR_DEFAULT, {0x01, 0x63, 0xD8, 0x00, 0x00, 0x00}},
      {TravelSpeed::SILENT, {0x01, 0x63, 0xD8, 0x00, 0x80, 0xD8, 0x05, 0x00}},
      {TravelSpeed::FAST, {0x01, 0x63, 0xD8, 0x00, 0x00, 0x00}},
  };
  for (const auto &[speed, payload] : cases) {
    TestableComponent comp;
    MockRadio radio;
    setup_declared_component(comp, radio, {.type = DeviceType::WINDOW_OPENER, .travel_speed = speed});

    comp.execute_device_command_("ABC123", CoverCommand::FAVORITE);
    EXPECT_EQ(first_sent_payload(radio), payload) << travel_speed_name(speed);
  }
}

TEST(HubOperations, AnInteriorVenetianBlindKeepsTheTiltFrameOnFunctionalParameterThree) {
  TestableComponent comp;
  MockRadio radio;
  setup_declared_component(comp, radio, {.type = DeviceType::INTERIOR_VENETIAN_BLIND});

  comp.set_device_tilt("ABC123", 25);  // no device answers; the sent frame is what is asserted
  const std::vector<uint8_t> payload = first_sent_payload(radio);
  ASSERT_EQ(payload.size(), 8u);
  EXPECT_EQ(payload[2], POS_UNKNOWN) << "tilt leaves the main position alone";
  EXPECT_EQ(payload[4], STATUS_TILT_SELECTOR);
  EXPECT_EQ(payload[5], 0x96) << "25 % open is 75 % closed, 0x9600";
}

TEST(HubOperations, AVerticalInteriorBlindHasNoTilt) {
  // VELUX KLF 200 API specification, Appendix 2: type 10 lists no slat parameter.
  TestableComponent comp;
  MockRadio radio;
  setup_declared_component(comp, radio, {.type = DeviceType::VERTICAL_INTERIOR_BLIND});

  EXPECT_FALSE(comp.set_device_tilt("ABC123", 50));
  EXPECT_TRUE(radio.get_sent_data().empty());
}

// ========================================================================================
// The predicted-end read: a moving cover that counts down is read once more, with the settling
// plan, a second after its count runs out.
// ========================================================================================

namespace {

/// A trusted 0x04 from ABC123 that says moving, with @p remaining_s left.
IoFrame moving_report(const TestableComponent &comp, uint16_t remaining_s) {
  IoFrame f = build_moving_status_response(comp.node_id_, 0);
  f.data[6] = static_cast<uint8_t>(remaining_s >> 8);
  f.data[7] = static_cast<uint8_t>(remaining_s & 0xFF);
  return f;
}

/// A trusted 0x04 from ABC123 that says stopped off its target (target 100 %, current 50 %), with
/// @p remaining_s at bytes 6-7.
IoFrame stopped_off_target_report(const TestableComponent &comp, uint16_t remaining_s) {
  IoFrame f = build_moving_status_response(comp.node_id_, 0);
  f.data[0] = test::STATUS_BYTE_AT_REST;
  f.data[2] = 0xC8;
  f.data[3] = 0x00;
  f.data[6] = static_cast<uint8_t>(remaining_s >> 8);
  f.data[7] = static_cast<uint8_t>(remaining_s & 0xFF);
  return f;
}

/// What the scheduler would do at the clock's current time: pop a due poll and run it against a
/// silent radio. Returns the number of transmissions it made (0 = nothing was due).
int run_due_poll_against_silence(TestableComponent &comp, MockRadio &radio) {
  const auto due = comp.poll_policy_.pop_due_device(esphome::millis());
  if (!due.has_value())
    return 0;
  const int before = radio.get_send_count();
  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    radio.queue_tx_result(false);
  EXPECT_FALSE(comp.request_device_status(*due));
  return radio.get_send_count() - before;
}

}  // namespace

TEST(HubOperations, MovingCoverIsReadOnceAtItsPredictedEndWithTheSettlingPlan) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 0);

  comp.update_device_status_(moving_report(comp, 12));
  const uint32_t t0 = esphome::millis();
  EXPECT_EQ(comp.poll_policy_.get_settle_update("ABC123"), t0 + 13000u) << "12 s left plus a second";

  // A progress read, answered: the count fell, so the aim moves with it (to the same instant, give or take).
  esphome::test_clock::advance_ms(3001);
  ASSERT_TRUE(comp.poll_policy_.pop_due_device(esphome::millis()).has_value()) << "the progress read is due";
  comp.update_device_status_(moving_report(comp, 9));
  EXPECT_EQ(comp.poll_policy_.get_settle_update("ABC123"), t0 + 3001u + 10000u) << "re-aimed from the falling count";

  // Between the two the progress reads stay single tries.
  esphome::test_clock::advance_ms(3001);
  EXPECT_EQ(run_due_poll_against_silence(comp, radio), 1) << "a progress read is one attempt";

  // The aim is reached: one read with the settling plan.
  // The silent progress read left a progress read due as well; the due aim wins over it.
  esphome::test_clock::set_ms(t0 + 13002u);
  ASSERT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u);
  ASSERT_LT(comp.poll_policy_.get_next_update("ABC123"), esphome::millis()) << "a progress read is due too";
  EXPECT_EQ(static_cast<size_t>(run_due_poll_against_silence(comp, radio)), planned_settling_attempts(comp))
      << "the read at the predicted end is a settling read";
  EXPECT_GT(planned_settling_attempts(comp), 1u);
}

namespace {

/// Preambles of the transmissions @p radio made after its first @p already_sent.
std::vector<uint16_t> preambles_since(const MockRadio &radio, size_t already_sent) {
  std::vector<uint16_t> preambles;
  const auto &configs = radio.get_tx_configs();
  for (size_t i = already_sent; i < configs.size(); ++i)
    preambles.push_back(configs[i].preamble_len);
  return preambles;
}

}  // namespace

// \todo Untested on hardware: needs a low-power cover that reports a remaining time (ADR 0077).
TEST(HubOperations, MissedShortTryMidMoveKeepsTheShortPreambleUntilTheCountRunsOut) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(moving_report(comp, 12));
  const uint32_t t0 = esphome::millis();
  const uint16_t short_preamble = mock_preamble_bytes(comp.tuning_.start_preamble_ms);
  const IoDevice *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  // A progress read, lost: by its own count the cover is still moving, so the next one is short again.
  esphome::test_clock::advance_ms(3001);
  size_t sent = radio.get_tx_configs().size();
  EXPECT_EQ(run_due_poll_against_silence(comp, radio), 1);
  EXPECT_EQ(preambles_since(radio, sent), (std::vector<uint16_t>{short_preamble}));
  EXPECT_NE(dev->last_moving_evidence_ms, 0u) << "one missed frame is not proof that it has come to rest";

  esphome::test_clock::advance_ms(3001);
  sent = radio.get_tx_configs().size();
  EXPECT_EQ(run_due_poll_against_silence(comp, radio), 1);
  EXPECT_EQ(preambles_since(radio, sent), (std::vector<uint16_t>{short_preamble}));

  // The count has run out and the cover is silent: now it has most likely come to rest.
  esphome::test_clock::set_ms(t0 + 12000u + PREDICTED_END_MARGIN_MS + 1);
  sent = radio.get_tx_configs().size();
  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    radio.queue_tx_result(false);
  EXPECT_FALSE(comp.request_device_status("ABC123"));
  ASSERT_FALSE(preambles_since(radio, sent).empty());
  EXPECT_EQ(preambles_since(radio, sent).front(), short_preamble) << "the belief still leads short...";
  EXPECT_EQ(dev->last_moving_evidence_ms, 0u) << "...and a silent try after the count spends the evidence";
}

TEST(HubOperations, PredictedEndReadToALowPowerCoverLeadsWithTheWakeUpPreamble) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(moving_report(comp, 12));
  const uint32_t t0 = esphome::millis();
  const IoDevice *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_NE(dev->last_moving_evidence_ms, 0u) << "the belief would lead short: moving evidence is fresh";

  esphome::test_clock::set_ms(t0 + 13002u);
  const size_t sent = radio.get_tx_configs().size();
  const int attempts = run_due_poll_against_silence(comp, radio);

  EXPECT_GT(attempts, 1) << "a settling read";
  const std::vector<uint16_t> preambles = preambles_since(radio, sent);
  ASSERT_EQ(preambles.size(), static_cast<size_t>(attempts));
  for (const uint16_t preamble : preambles)
    EXPECT_EQ(preamble, mock_preamble_bytes(comp.tuning_.low_power_wake_ms)) << "at its predicted end the cover rests";
}

TEST(HubOperations, ReadAfterASilentPredictedEndReadLeadsShortThenWakes) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(moving_report(comp, 12));
  const uint32_t t0 = esphome::millis();
  const IoDevice *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  const uint16_t short_preamble = mock_preamble_bytes(comp.tuning_.start_preamble_ms);
  const uint16_t wake_preamble = mock_preamble_bytes(comp.tuning_.low_power_wake_ms);

  esphome::test_clock::set_ms(t0 + 13002u);
  ASSERT_GT(run_due_poll_against_silence(comp, radio), 1);
  EXPECT_NE(dev->last_moving_evidence_ms, 0u)
      << "the wake-only read took the belief out of the exchange: it spends nothing";

  // Still moving past its count, or at rest: the next read covers both, and its silence decides.
  const uint32_t next = comp.poll_policy_.get_next_update("ABC123");
  ASSERT_NE(next, 0u) << "a missed read is retried";
  esphome::test_clock::set_ms(next + 1);
  const size_t sent = radio.get_tx_configs().size();
  const int attempts = run_due_poll_against_silence(comp, radio);
  ASSERT_GT(attempts, 1) << "the read after a missed one is a settling read";
  const std::vector<uint16_t> preambles = preambles_since(radio, sent);
  EXPECT_EQ(preambles.front(), short_preamble) << "a cover still travelling past its count hears the first try";
  for (size_t i = 1; i < preambles.size(); ++i)
    EXPECT_EQ(preambles[i], wake_preamble) << "a resting cover hears the later ones";
  EXPECT_EQ(dev->last_moving_evidence_ms, 0u) << "silent after its count: the evidence is spent";
}

TEST(HubOperations, PredictedEndReadToAMainsCoverKeepsTheNormalStartPreamble) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/false);
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(moving_report(comp, 12));
  const uint32_t t0 = esphome::millis();

  esphome::test_clock::set_ms(t0 + 13002u);
  const size_t sent = radio.get_tx_configs().size();
  ASSERT_GT(run_due_poll_against_silence(comp, radio), 0);
  for (const uint16_t preamble : preambles_since(radio, sent))
    EXPECT_EQ(preamble, mock_preamble_bytes(comp.tuning_.start_preamble_ms));
}

TEST(HubOperations, StaleRemainingTimeFallsBackToSingleTryProgressReads) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 0);

  for (int i = 0; i <= PREDICTED_END_STALE_REPORTS; ++i) {
    comp.update_device_status_(moving_report(comp, 12));  // the count never falls
    esphome::test_clock::advance_ms(3001);
  }

  EXPECT_EQ(comp.poll_policy_.get_settle_update("ABC123"), 0u) << "a frozen count is not a prediction";
  comp.poll_policy_.set_next_update("ABC123", esphome::millis() - 1);
  EXPECT_EQ(run_due_poll_against_silence(comp, radio), 1);

  comp.update_device_status_(moving_report(comp, 11));
  EXPECT_NE(comp.poll_policy_.get_settle_update("ABC123"), 0u) << "a count that falls again is aimed at again";
}

TEST(HubOperations, EndReadOfAShortOrZeroCountRetriesALostReplyAndProgressReadsStaySingleTries) {
  for (const uint16_t remaining : {uint16_t{2}, uint16_t{1}, uint16_t{0}}) {
    SCOPED_TRACE(remaining);
    esphome::test_clock::ManualClock clock(50000);
    TestableComponent comp;
    MockRadio radio;
    setup_cover_component(comp, radio);
    comp.begin_status_poll_tracking_("ABC123", 0);

    // The first count teaches the hub that the cover counts down (a count of 0 is only believed after that);
    // the progress read it leaves due is one attempt.
    comp.update_device_status_(moving_report(comp, 12));
    comp.poll_policy_.set_next_update("ABC123", esphome::millis() - 1);
    EXPECT_EQ(run_due_poll_against_silence(comp, radio), 1) << "a progress read is one attempt";

    comp.update_device_status_(moving_report(comp, remaining));
    const uint32_t t0 = esphome::millis();
    EXPECT_EQ(comp.poll_policy_.get_settle_update("ABC123"), t0 + remaining * 1000u + 1000u);

    // The end of the count: one read with the settling plan, repeated while the reply is lost.
    esphome::test_clock::advance_ms(remaining * 1000u + 1001u);
    EXPECT_EQ(static_cast<size_t>(run_due_poll_against_silence(comp, radio)), planned_settling_attempts(comp))
        << "a lost reply at the end of the count is retried";
    EXPECT_GT(planned_settling_attempts(comp), 1u);
  }
}

TEST(HubOperations, ZeroFromACoverThatNeverCountedPlansNoEndRead) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 0);

  comp.update_device_status_(moving_report(comp, 0));
  EXPECT_EQ(comp.poll_policy_.get_settle_update("ABC123"), 0u) << "a 0 from a cover that never counted is no count";
  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), esphome::millis() + DEFAULT_SETTLE_POLL_DELAY_MS);
  esphome::test_clock::advance_ms(DEFAULT_SETTLE_POLL_DELAY_MS + 1);
  EXPECT_EQ(run_due_poll_against_silence(comp, radio), 1) << "the usual single-try progress read";
}

TEST(HubOperations, AWithdrawnAimLeavesTheProgressReadOfTheReportThatWithdrewIt) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 0);

  // A count of 1 aims at +2 s; the progress read due then is dropped in favour of the aim.
  comp.update_device_status_(moving_report(comp, 12));
  comp.update_device_status_(moving_report(comp, 1));
  const uint32_t t0 = esphome::millis();
  comp.poll_policy_.set_next_update("ABC123", t0 + 1500u);
  esphome::test_clock::set_ms(t0 + 1501u);
  EXPECT_EQ(run_due_poll_against_silence(comp, radio), 0) << "the progress read right before the aim is dropped";
  ASSERT_EQ(comp.poll_policy_.get_next_update("ABC123"), 0u);

  // A report without a count withdraws the aim before it falls due; that report plans the next read.
  comp.update_device_status_(moving_report(comp, 0xFFFF));  // all bits set: no count
  EXPECT_EQ(comp.poll_policy_.get_settle_update("ABC123"), 0u);
  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), esphome::millis() + DEFAULT_SETTLE_POLL_DELAY_MS)
      << "the cover is still read";
  esphome::test_clock::advance_ms(DEFAULT_SETTLE_POLL_DELAY_MS + 1);
  EXPECT_EQ(run_due_poll_against_silence(comp, radio), 1) << "as a single-try progress read";
}

TEST(HubOperations, StoppedOffTargetConfirmingReadKeepsTheDefaultDelayAndWinsOverAnyCount) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(moving_report(comp, 12));  // teaches the hub that the cover counts down
  ASSERT_NE(comp.poll_policy_.get_settle_update("ABC123"), 0u);

  // Stopped off target with a zero count (and, in the next round, a short one): the confirming read
  // still comes after the default delay and is the only read planned.
  for (const uint16_t remaining : {uint16_t{0}, uint16_t{1}}) {
    comp.poll_policy_.set_next_update("ABC123", 0);
    comp.update_device_status_(stopped_off_target_report(comp, remaining));
    EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), esphome::millis() + DEFAULT_SETTLE_POLL_DELAY_MS)
        << "a stopped report is not scheduled from its remaining time, remaining=" << remaining;
    EXPECT_EQ(comp.poll_policy_.get_settle_update("ABC123"), 0u) << "no predicted-end read next to the confirming one";
    comp.update_device_status_(moving_report(comp, 12));  // moves again: rearm for the next round
  }

  comp.poll_policy_.set_next_update("ABC123", 0);
  comp.update_device_status_(stopped_off_target_report(comp, 0));
  esphome::test_clock::advance_ms(DEFAULT_SETTLE_POLL_DELAY_MS + 1);
  EXPECT_EQ(static_cast<size_t>(run_due_poll_against_silence(comp, radio)), planned_settling_attempts(comp))
      << "the confirming read is a settling read";
}
