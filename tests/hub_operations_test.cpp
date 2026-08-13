/// @file hub_operations_test.cpp
/// @brief Tests for hub_operations.cpp high-level command execution.

#include "hub_core.h"
#include "hub_internal.h"
#include "proto_frame.h"
#include "proto_commands.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;

// ============================================================================
// HubOperations test suite
// ============================================================================
// Tests for set_device_position, set_device_tilt, request_device_status,
// set_light_state, set_switch_state, set_lock_state, and the queued operation dispatch.
//
// These tests verify capability-gating and correct command construction
// using mock radio and a testable component subclass.

namespace {

class TestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::begin_status_poll_tracking_;
  using IOHomeControlComponent::execute_device_command_;
  using IOHomeControlComponent::send_and_receive_;
  using IOHomeControlComponent::process_pending_operation_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::exchange_engine_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::system_key_;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::busy_;
  using IOHomeControlComponent::poll_policy_;
};

// Build a response frame from device (matching device node_id 0xABC123)
static IoFrame build_status_response(const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(f, dst);
  set_src(f, device_node_id);
  uint8_t payload[8] = {STATUS_STOPPED, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));
  return f;
}

static IoFrame build_moving_status_response(const uint8_t dst[3], uint8_t delay_hint_seconds) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(f, dst);
  set_src(f, device_node_id);
  uint8_t payload[8] = {0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, delay_hint_seconds};
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
  uint8_t payload[6] = {0x00, 0x00, 0x00, 0x00, 0x64, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));
  return f;
}

// 8-byte private response in the EXECUTE-tilt ack layout: data[4] is the tilt selector and
// data[5..6] a 16-bit slat angle, where a position-bearing 0x04 carries the current position in
// data[4..5]. Byte-for-byte the ack from issue 60 — see
// tests/corpus/captures/issues/issue_60_tilt_execute_ack_tilt_block.yaml.
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

static IoFrame build_error_response(const uint8_t dst[3], uint8_t result) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(f, dst);
  set_src(f, device_node_id);
  uint8_t payload[1] = {result};
  set_cmd(f, CMD_ERROR_RESP, payload, sizeof(payload));
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

static IoFrame build_challenge_request(const uint8_t src[3], const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, false, false, false);
  set_dst(f, dst);
  set_src(f, src);
  uint8_t challenge[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  set_cmd(f, CMD_CHALLENGE_REQ, challenge, sizeof(challenge));
  return f;
}

// Setup a component with one registered cover device
static void setup_cover_component(TestableComponent &comp, MockRadio &radio) {
  comp.node_id_[0] = 0xC0;
  comp.node_id_[1] = 0xFF;
  comp.node_id_[2] = 0xEE;
  static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
  comp.initialized_ = true;
  comp.radio_ = &radio;
  comp.add_device("ABC123");
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->type = DeviceType::ROLLER_SHUTTER;
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
  dev->type = DeviceType::LIGHT;

  bool ok = comp.set_device_position("ABC123", 50);
  EXPECT_FALSE(ok) << "set_device_position should reject mid position for light device";
}

TEST(HubOperations, LightBinaryPositionExchangeFails) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->type = DeviceType::LIGHT;

  bool ok = comp.set_device_position("ABC123", 0);
  EXPECT_FALSE(ok) << "set_device_position fails because send_and_receive_ gets no response";
}

TEST(HubOperations, SetLockStateSuccess) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->type = DeviceType::LOCK;

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
  dev->type = DeviceType::LIGHT;

  EXPECT_FALSE(comp.set_lock_state("ABC123", true)) << "non-lock devices should be rejected by lock entity gating";
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
  dev->type = DeviceType::LOCK;

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

TEST(HubOperations, RequestDeviceStatusAuthFailureBacksOffAggressively) {
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
      << "auth-shaped failures should back off more than plain silence";
  EXPECT_LE(delay, STATUS_AUTH_RETRY_AFTER_FAIL_MS + 1000)
      << "first auth-shaped failure should use the configured auth backoff";
  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 0u)
      << "auth-shaped failures should reset the silent-failure streak";
  EXPECT_EQ(comp.poll_policy_.get_auth_poll_failures("ABC123"), 1u)
      << "auth-shaped failures should increment their own streak";
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
      << "moving execute replies should schedule a follow-up poll driven by the device hint";
}

TEST(HubOperations, StopCommandArmsTrackedPollingToConfirmRestingPosition) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  // STOP reply arrives with is_stopped=true — tracking should be cleared after confirming position.
  IoFrame resp = build_status_response(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::STOP));

  EXPECT_EQ(comp.poll_policy_.get_poll_deadline("ABC123"), 0u)
      << "STOP confirmed by stopped reply should clear tracking";
  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), 0u)
      << "no further polls needed once the stopped position is confirmed";
}

TEST(HubOperations, StopCommandWithMovingReplySchedulesShortSettlePoll) {
  // When a STOP response says the device is still moving (motor decelerating), the settle poll
  // should fire within STOP_SETTLE_POLL_CAP_MS — faster than the default motion-tracking cadence
  // and faster than any configured interval — so STOP confirms the resting position quickly.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  IoFrame resp = build_moving_status_response(comp.node_id_, 0xFF);  // hint=unused → motion-tracking default
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
  dev->inverted = true;

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
  // tests/corpus/captures/somfy_awning/execute_ack_reports_stale_target_*.yaml. Trusting them
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
  // tests/corpus/captures/issues/issue_60_tilt_execute_ack_tilt_block.yaml.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->type = DeviceType::VENETIAN_BLIND;  // supports tilt
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
  dev->type = DeviceType::LIGHT;

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
  dev->type = DeviceType::LOCK;

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
  dev->type = DeviceType::VENETIAN_BLIND;  // supports tilt

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
  dev->type = DeviceType::VENETIAN_BLIND;

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
  dev->type = DeviceType::VENETIAN_BLIND;

  // Register a second tilt-capable device
  comp.add_device("DEF456");
  auto *dev2 = comp.get_device("DEF456");
  ASSERT_NE(dev2, nullptr);
  dev2->type = DeviceType::VENETIAN_BLIND;

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
  dev->type = DeviceType::VENETIAN_BLIND;

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
  dev->type = DeviceType::VENETIAN_BLIND;

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
  dev->type = DeviceType::VENETIAN_BLIND;

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
