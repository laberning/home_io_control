/// @file hub_operations_test.cpp
/// @brief Tests for hub_operations.cpp high-level command execution.

#include "hub_core.h"
#include "hub_internal.h"
#include "proto_frame.h"
#include "proto_commands.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <functional>

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
  using IOHomeControlComponent::tuning_;
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
// send_heating_command tests (CMD_WRITE_PRIVATE 0x20)
// ========================================================================================

static IoFrame build_write_private_ack(const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(f, dst);
  set_src(f, device_node_id);
  const uint8_t payload[] = {0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00};
  set_cmd(f, CMD_WRITE_PRIVATE_ACK, payload, sizeof(payload));
  return f;
}

TEST(HubOperations, SendHeatingCommandAcceptsClimateDeviceAndSendsExactBytes) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->type = DeviceType::HEATING_TEMPERATURE_INTERFACE;

  IoFrame resp = build_write_private_ack(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  EXPECT_TRUE(comp.send_heating_command("ABC123", HeatingFunction::SET_TEMPERATURE, 20.5f));

  ASSERT_EQ(radio.get_sent_data().size(), 1u);
  const auto &tx = radio.get_sent_data().front();
  ASSERT_GE(tx.size(), 15u);
  EXPECT_EQ(tx[8], CMD_WRITE_PRIVATE) << "command byte should be CMD_WRITE_PRIVATE (0x20)";
  // Payload begins at wire offset 9 — {0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00} for 20.5 C.
  EXPECT_EQ(tx[9], 0x0C);
  EXPECT_EQ(tx[10], 0x61);
  EXPECT_EQ(tx[11], 0x01);
  EXPECT_EQ(tx[12], 0x03);
  EXPECT_EQ(tx[13], 0xCD) << "20.5 C -> 205 -> 0xCD";
  EXPECT_EQ(tx[14], 0x00) << "trailing literal constant";
}

TEST(HubOperations, SendHeatingCommandRejectsNonClimateDevice) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);  // ROLLER_SHUTTER

  EXPECT_FALSE(comp.send_heating_command("ABC123", HeatingFunction::SET_TEMPERATURE, 20.5f))
      << "cover devices must be rejected by the climate capability predicate";
  EXPECT_TRUE(radio.get_sent_data().empty()) << "a rejected heating command must not transmit";
}

TEST(HubOperations, SendHeatingCommandRejectsOutOfRangeTemperature) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->type = DeviceType::HEATING_TEMPERATURE_INTERFACE;

  EXPECT_FALSE(comp.send_heating_command("ABC123", HeatingFunction::SET_TEMPERATURE, 28.1f));
  EXPECT_TRUE(radio.get_sent_data().empty()) << "an unencodable value must not transmit";
}

TEST(HubOperations, SendHeatingCommandFailsWithoutAnyReply) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->type = DeviceType::HEATING_TEMPERATURE_INTERFACE;

  // No reply queued -> exchange times out -> not acknowledged.
  EXPECT_FALSE(comp.send_heating_command("ABC123", HeatingFunction::POWER_ON, 0.0f));
}

// Run send_heating_command against a HEATING_TEMPERATURE_INTERFACE device with a single reply
// frame (built from the hub node id by `build_reply`) queued as the device's answer.
static bool run_heating_command_with_reply(const std::function<IoFrame(const uint8_t *)> &build_reply) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  EXPECT_NE(dev, nullptr);
  dev->type = DeviceType::HEATING_TEMPERATURE_INTERFACE;

  IoFrame resp = build_reply(comp.node_id_);
  uint8_t raw[64];
  uint8_t raw_len = serialize(resp, raw, sizeof(raw));
  RadioRxPacket pkt{};
  pkt.len = raw_len;
  memcpy(pkt.data, raw, raw_len);
  pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(pkt);

  return comp.send_heating_command("ABC123", HeatingFunction::POWER_ON, 0.0f);
}

// power_on / midnight_sync are register reads: the 0x21 ACK carries a payload the hub logs at
// DEBUG (bytes_to_hex) but does not decode. Exercise both an ACK with payload bytes and the
// empty-payload ACK to prove the log path handles either without crashing and still returns true.
TEST(HubOperations, SendHeatingCommandAcceptsReadAckWithPayload) {
  EXPECT_TRUE(run_heating_command_with_reply([](const uint8_t *dst) {
    IoFrame f{};
    init_frame(f, true, false, true, false);
    uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
    set_dst(f, dst);
    set_src(f, device_node_id);
    // Stand-in for a 0x0130 setpoint-block read answer (contents not decoded, only logged).
    const uint8_t payload[] = {0x0C, 0x60, 0x01, 0x30, 0xC8, 0x00, 0x23, 0x00, 0x28, 0x00};
    set_cmd(f, CMD_WRITE_PRIVATE_ACK, payload, sizeof(payload));
    return f;
  })) << "a CMD_WRITE_PRIVATE_ACK carrying a read payload is still an acknowledgement";
}

TEST(HubOperations, SendHeatingCommandAcceptsReadAckWithEmptyPayload) {
  EXPECT_TRUE(run_heating_command_with_reply([](const uint8_t *dst) {
    IoFrame f{};
    init_frame(f, true, false, true, false);
    uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
    set_dst(f, dst);
    set_src(f, device_node_id);
    set_cmd(f, CMD_WRITE_PRIVATE_ACK, nullptr, 0);
    return f;
  })) << "an empty-payload ACK must not trip the payload log path";
}

TEST(HubOperations, SendHeatingCommandFailsOnNonAckReply) {
  // The acknowledged check is `response.cmd == CMD_WRITE_PRIVATE_ACK`; a reply with any other cmd
  // must fail the command.
  EXPECT_FALSE(run_heating_command_with_reply([](const uint8_t *dst) {
    return build_error_response(dst, RESULT_LIMITATION_BY_WIND);
  })) << "a CMD_ERROR_RESP reply must not count as an acknowledgement";
  EXPECT_FALSE(run_heating_command_with_reply([](const uint8_t *dst) { return build_status_response(dst); }))
      << "a CMD_PRIVATE_RESP reply must not count as an acknowledgement";
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

// A scheduler-owned status poll (StatusPollPolicy is tracking the device) caps its exchange at
// SCHEDULED_POLL_MAX_TRIES: its failure is re-armed by the backoff ladder, so extra blocking
// in-exchange tries only stall loop() while a device is unresponsive. A one-off poll, and every
// user command, keep the full EXCHANGE_RETRY_COUNT. All three cases fail every transmit so the
// send count is exactly the try count.
TEST(HubOperations, TrackedStatusPollUsesASingleExchangeTry) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 2000);

  for (int i = 0; i < EXCHANGE_RETRY_COUNT; ++i)
    radio.queue_tx_result(false);

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(radio.get_send_count(), 1)
      << "a scheduler-owned poll transmits once; the 5/15/30 s backoff ladder is its retry mechanism";
}

TEST(HubOperations, OneShotStatusPollKeepsTheFullRetryBudget) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  // No begin_status_poll_tracking_(): nothing re-arms this request, so it keeps all its tries.

  for (int i = 0; i < EXCHANGE_RETRY_COUNT; ++i)
    radio.queue_tx_result(false);

  EXPECT_FALSE(comp.request_device_status("ABC123"));
  EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT);
}

TEST(HubOperations, UserCommandKeepsTheFullRetryBudgetWhilePollTrackingIsActive) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 2000);  // tracking on -- must not leak into commands

  for (int i = 0; i < EXCHANGE_RETRY_COUNT; ++i)
    radio.queue_tx_result(false);

  EXPECT_FALSE(comp.execute_device_command_("ABC123", CoverCommand::STOP));
  EXPECT_EQ(radio.get_send_count(), EXCHANGE_RETRY_COUNT) << "the single-try rule must never extend to user commands";
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
}

TEST(HubOperations, StatusPollAuthenticatedWithoutFinalResponseStillFails) {
  // The mirror image, and the reason the distinction exists: a status poll's entire purpose is the
  // payload. Authenticating and then hearing nothing answers no question, so it stays a failure
  // and keeps the aggressive auth-shaped backoff.
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
}

TEST(HubOperations, SilentDeviceSendsTheSilentExecutePayload) {
  // End-to-end: the YAML-declared per-cover flag has to reach the wire, not just the builder.
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->silent = true;

  comp.set_device_position("ABC123", 100);

  ASSERT_FALSE(radio.get_sent_data().empty());
  const auto &frame = radio.get_sent_data().front();
  ASSERT_GE(frame.size(), 17u) << "an execute frame is 9 header bytes plus an 8-byte payload";
  EXPECT_EQ(frame[14], POS_FAVORITE) << "the secondary-target byte is unaffected";
  EXPECT_EQ(frame[15], 0x05) << "silent devices travel on the slow profile";
}

TEST(HubOperations, NonSilentDeviceKeepsTheExistingExecutePayload) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_FALSE(dev->silent) << "silent is opt-in";

  comp.set_device_position("ABC123", 100);

  ASSERT_FALSE(radio.get_sent_data().empty());
  const auto &frame = radio.get_sent_data().front();
  ASSERT_GE(frame.size(), 17u);
  EXPECT_EQ(frame[14], POS_FAVORITE) << "the default payload is unchanged by this feature";
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
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, LONG_PREAMBLE)
      << "a low_power target's directed start frame must use the 1024-byte wake-up preamble";
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
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, comp.tuning_.normal_start_preamble)
      << "an always-alive target's directed start frame uses normal_start_preamble, not LONG_PREAMBLE";
  ASSERT_FALSE(radio.get_sent_data().empty());
  EXPECT_EQ(radio.get_sent_data().front()[1] & CTRL1_LOW_POWER, 0)
      << "an always-alive target's frame must leave CTRL1_LOW_POWER clear";
}

TEST(HubOperations, LowPowerDeviceStatusPollUsesLongPreambleAndSetsLowPowerBit) {
  // create_get_status is the frame the issue #87 reporter watched time out — cover it explicitly.
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);

  comp.request_device_status("ABC123");

  ASSERT_GE(radio.get_tx_configs().size(), 1u) << "the status poll start frame should have been transmitted";
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, LONG_PREAMBLE)
      << "a low_power target's status poll must use the wake-up preamble";
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
  dev->type = DeviceType::LIGHT;  // the cover-position guard now rejects a mid position

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

// §7.16 — a failed tilt command withdraws the predicted angle; effective_tilt falls back to the
// last observed one.
TEST(HubOperations, FailedTiltCommandRollsBackOptimisticTilt) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->type = DeviceType::VENETIAN_BLIND;  // tilt-capable
  dev->tilt = 15.0f;                       // a real angle from an earlier poll
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
  dev->type = DeviceType::LIGHT;  // the queued cover-position guard now rejects

  comp.queue_set_device_position("ABC123", 50);
  EXPECT_TRUE(dev->optimistic.empty()) << "a queue-time guard rejection withdraws the prediction too";

  // Same for the bool-returning STOP path (queue_device_command's inline COVER guard).
  ASSERT_TRUE(comp.apply_optimistic_stop("ABC123"));
  EXPECT_FALSE(comp.queue_device_command("ABC123", CoverCommand::STOP));
  EXPECT_TRUE(dev->optimistic.empty()) << "a rejected queued STOP withdraws the predicted stop";
}
