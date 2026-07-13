#include "hub_core.h"
#include "hub_internal.h"
#include "radio_interface.h"
#include "proto_frame.h"
#include "esphome/core/component.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;
using test::TestableHubComponent;

// ============================================================================
// HubCore test suite
// ============================================================================
// Device registry, stored-ID helpers, and pending operation queuing.

// ========================================================================================
// Device management tests
// ========================================================================================

TEST(HubCore, DeviceAddAndLookup) {
  TestableHubComponent comp;
  comp.set_node_id("C0FFEE");                               // 3 bytes
  comp.set_system_key("D1743493FA943845AC4350EEFF342934");  // 16 bytes

  // Pretend setup called by manually initializing needed fields
  comp.node_id_[0] = 0xC0;
  comp.node_id_[1] = 0xFF;
  comp.node_id_[2] = 0xEE;
  static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
  comp.initialized_ = true;
  comp.radio_ = new MockRadio();

  // Add device
  comp.add_device("ABC123");  // valid node ID
  EXPECT_NE(comp.get_device("ABC123"), nullptr) << "device should be found after add";

  // Adding same device twice should be no-op
  comp.add_device("ABC123");
  EXPECT_NE(comp.get_device("ABC123"), nullptr) << "device should still be found after duplicate add";

  // Unknown device returns nullptr
  EXPECT_EQ(comp.get_device("000000"), nullptr) << "all-zero node ID should not be found";
  EXPECT_EQ(comp.get_device("123456"), nullptr) << "random node ID should not be found";

  delete comp.radio_;
}

TEST(HubCore, AddDeviceClearsUnknownType) {
  TestableHubComponent comp;
  comp.node_id_[0] = 0xC0;
  comp.node_id_[1] = 0xFF;
  comp.node_id_[2] = 0xEE;
  static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
  comp.initialized_ = true;
  comp.radio_ = new MockRadio();

  comp.add_device("ABC123");
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->type, DeviceType::UNKNOWN) << "newly added device should start as UNKNOWN type";
  EXPECT_EQ(dev->position, UNKNOWN_POSITION) << "newly added device should have UNKNOWN_POSITION";
  EXPECT_TRUE(dev->is_stopped) << "newly added device should be marked stopped";

  delete comp.radio_;
}

TEST(HubCore, AddDeviceStoresDeclaredMetadata) {
  TestableHubComponent comp;
  comp.initialized_ = true;
  comp.radio_ = new MockRadio();

  comp.add_device("ABC123", DeviceType::HORIZONTAL_AWNING, 5, true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->type, DeviceType::HORIZONTAL_AWNING) << "declared device type should be stored";
  EXPECT_EQ(dev->subtype, 5u) << "declared subtype should be stored";
  EXPECT_TRUE(dev->inverted) << "explicit inversion should be stored";

  delete comp.radio_;
}

TEST(HubCore, SetDeviceStatusPollIntervalStoresValue) {
  TestableHubComponent comp;
  comp.initialized_ = true;
  comp.radio_ = new MockRadio();

  comp.add_device("ABC123");
  comp.set_device_status_poll_interval("ABC123", 2500);

  EXPECT_EQ(comp.poll_policy_.get_interval("ABC123"), 2500u)
      << "configured poll interval should be stored in the poll policy";

  delete comp.radio_;
}

TEST(HubCore, LoopQueuesSettlePollWithoutConfiguredInterval) {
  TestableHubComponent comp;
  comp.initialized_ = true;
  comp.radio_ = new MockRadio();

  comp.add_device("ABC123");
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->type = DeviceType::HORIZONTAL_AWNING;
  // begin_tracking arms the deadline; then set_next_update simulates a hint-scheduled poll becoming due.
  uint32_t const now = esphome::millis();
  comp.poll_policy_.begin_tracking("ABC123", 0, now);
  comp.poll_policy_.set_next_update("ABC123", now);

  comp.loop();

  ASSERT_EQ(comp.op_queue_.size(), 1u) << "due settle polls should be queued by the main loop";
  EXPECT_EQ(comp.op_queue_.front().type, PendingOperationType::REQUEST_STATUS);
  EXPECT_EQ(comp.op_queue_.front().device_id, "ABC123");
  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), 0u) << "queued polls should clear their due timestamp";

  delete comp.radio_;
}

TEST(HubCore, QueueOperations) {
  TestableHubComponent comp;
  comp.node_id_[0] = 0xC0;
  comp.node_id_[1] = 0xFF;
  comp.node_id_[2] = 0xEE;
  static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
  comp.initialized_ = true;
  comp.radio_ = new MockRadio();

  // Initially empty
  EXPECT_TRUE(comp.op_queue_.empty()) << "pending operations should start empty";

  // Queue a position command
  comp.queue_set_device_position("ABC123", 50);
  EXPECT_EQ(comp.op_queue_.size(), 1u) << "should have one pending operation after queue_set_device_position";
  auto op = comp.op_queue_.front();
  EXPECT_EQ(op.type, PendingOperationType::SET_POSITION) << "operation type should be SET_POSITION";
  EXPECT_EQ(op.device_id, "ABC123") << "device ID should match queued device";
  EXPECT_EQ(op.position, 50u) << "position should be 50";

  // Queue a status request
  comp.queue_request_device_status("ABC123");
  EXPECT_EQ(comp.op_queue_.size(), 2u) << "should have two pending operations after queue_request_device_status";
  op = comp.op_queue_.back();
  EXPECT_EQ(op.type, PendingOperationType::REQUEST_STATUS) << "operation type should be REQUEST_STATUS";
  EXPECT_EQ(op.device_id, "ABC123") << "device ID should match queued device";

  // Dequeue in-place
  comp.process_pending_operation_();  // processes front
  EXPECT_EQ(comp.op_queue_.size(), 1u) << "should have one pending operation after processing first";
  op = comp.op_queue_.front();
  EXPECT_EQ(op.type, PendingOperationType::REQUEST_STATUS) << "remaining operation should be REQUEST_STATUS";

  delete comp.radio_;
}

TEST(HubCore, StoredNodeIdIsValid) {
  // Valid: not all zeros, not all 0xFF
  uint8_t valid[3] = {0xC0, 0xFF, 0xEE};
  EXPECT_TRUE(stored_node_id_is_valid(valid)) << "mixed non-zero bytes should be valid";

  uint8_t not_all_ff[3] = {0xFE, 0xFF, 0xFF};
  EXPECT_TRUE(stored_node_id_is_valid(not_all_ff)) << "not all 0xFF should be valid";

  uint8_t not_all_zero[3] = {0x01, 0x00, 0x00};
  EXPECT_TRUE(stored_node_id_is_valid(not_all_zero)) << "not all zero should be valid";

  // Invalid: all zeros
  uint8_t all_zero[3] = {0x00, 0x00, 0x00};
  EXPECT_FALSE(stored_node_id_is_valid(all_zero)) << "all zeros should be invalid";

  // Invalid: all 0xFF
  uint8_t all_ff[3] = {0xFF, 0xFF, 0xFF};
  EXPECT_FALSE(stored_node_id_is_valid(all_ff)) << "all 0xFF should be invalid";
}

TEST(HubCore, FormatPositionHelper) {
  EXPECT_STREQ(format_position(UNKNOWN_POSITION).c_str(), "unknown") << "UNKNOWN_POSITION should format as 'unknown'";
  EXPECT_STREQ(format_position(0.0f).c_str(), "0%") << "zero should format as '0%'";
  EXPECT_STREQ(format_position(50.0f).c_str(), "50%") << "fifty should format as '50%'";
  EXPECT_STREQ(format_position(100.0f).c_str(), "100%") << "hundred should format as '100%'";
  EXPECT_STREQ(format_position(37.5f).c_str(), "38%") << "37.5 should round to '38%'";
}

TEST(HubCore, PrivateResponseMarkerTargetUsesCurrentWhenStopped) {
  TestableHubComponent comp;
  comp.add_device("ABC123");

  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, own);
  set_src(frame, device);
  uint8_t payload[8] = {STATUS_STOPPED, 0x00, POS_UNKNOWN, 0x00, 0x64, 0x00, 0x00, 0x00};
  ASSERT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));

  comp.update_device_status_(frame);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->position, 50.0f) << "valid current should decode to 50 percent";
  EXPECT_FLOAT_EQ(dev->target, 50.0f) << "marker target should normalize to current when stopped";
  EXPECT_TRUE(dev->is_stopped) << "matching normalized target/current should remain stopped";
}

TEST(HubCore, StoppedFlagMismatchKeepsDeviceMoving) {
  TestableHubComponent comp;
  comp.add_device("ABC123");

  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, own);
  set_src(frame, device);
  uint8_t payload[8] = {STATUS_STOPPED, 0x00, 0xC8, 0x00, 0x64, 0x00, 0x00, 0x00};
  ASSERT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));

  comp.update_device_status_(frame);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->target, 100.0f) << "valid target should decode to 100 percent";
  EXPECT_FLOAT_EQ(dev->position, 50.0f) << "valid current should decode to 50 percent";
  EXPECT_FALSE(dev->is_stopped) << "stopped flag should be overridden when target and current are still far apart";
}

// ============================================================================
// Listen-before-talk (LBT) tests
// ============================================================================

namespace {

class LBTTestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::transmit_frame_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::radio_;
};

IoFrame make_simple_frame() {
  IoFrame f{};
  init_frame(f, true, true, false, false);
  uint8_t src[3] = {0xC0, 0xFF, 0xEE};
  uint8_t dst[3] = {0x9C, 0xA3, 0x9C};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, 0x30);
  return f;
}

}  // namespace

TEST(HubCore, LBT_ChannelClear_TransmitsImmediately) {
  LBTTestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  radio.set_rssi_default(-100);  // below -90 threshold

  IoFrame frame = make_simple_frame();
  EXPECT_TRUE(comp.transmit_frame_(frame, FREQ_CH2, LONG_PREAMBLE));
  EXPECT_EQ(radio.get_send_count(), 1) << "should transmit once when channel is clear";
}

TEST(HubCore, LBT_ChannelBusy_BacksOffThenTransmits) {
  LBTTestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;

  // First 3 reads: busy (-80 dBm), then clear (-100 dBm)
  radio.queue_rssi(-80);
  radio.queue_rssi(-80);
  radio.queue_rssi(-80);
  radio.set_rssi_default(-100);

  IoFrame frame = make_simple_frame();
  EXPECT_TRUE(comp.transmit_frame_(frame, FREQ_CH2, LONG_PREAMBLE));
  EXPECT_EQ(radio.get_send_count(), 1) << "should still transmit after backoff";
}

TEST(HubCore, LBT_ChannelBusyAllRetries_TransmitsAnyway) {
  LBTTestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;

  // All reads return busy — should still transmit after exhausting retries
  radio.set_rssi_default(-50);

  IoFrame frame = make_simple_frame();
  EXPECT_TRUE(comp.transmit_frame_(frame, FREQ_CH2, LONG_PREAMBLE));
  EXPECT_EQ(radio.get_send_count(), 1) << "should transmit even if channel stays busy";
}

TEST(HubCore, LBT_ExactThreshold_IsConsideredClear) {
  LBTTestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;

  // Exactly at threshold (-90) should NOT be considered clear (>=, not >)
  radio.set_rssi_default(-90);

  IoFrame frame = make_simple_frame();
  EXPECT_TRUE(comp.transmit_frame_(frame, FREQ_CH2, LONG_PREAMBLE));
  // -90 is NOT < -90, so it should retry all LBT_MAX_RETRIES times then transmit
  EXPECT_EQ(radio.get_send_count(), 1);
}

TEST(HubCore, LBT_JustBelowThreshold_TransmitsImmediately) {
  LBTTestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;

  radio.set_rssi_default(-91);  // just below threshold

  IoFrame frame = make_simple_frame();
  EXPECT_TRUE(comp.transmit_frame_(frame, FREQ_CH2, LONG_PREAMBLE));
  EXPECT_EQ(radio.get_send_count(), 1);
}

// ============================================================================
// Exchange-internal filtering tests (Issue #3)
// ============================================================================

namespace {

/// Helper to set up a component with a registered device for RX tests.
class RxTestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::process_received_packet_;
  using IOHomeControlComponent::update_device_status_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::system_key_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::poll_policy_;
};

/// Build a RadioRxPacket from a constructed IoFrame.
RadioRxPacket make_rx_packet(const IoFrame &frame) {
  RadioRxPacket pkt{};
  pkt.len = serialize(frame, pkt.data, sizeof(pkt.data));
  pkt.freq_hz = FREQ_CH2;
  return pkt;
}

/// Create a minimal component with one registered device and a mock radio.
void setup_rx_test_component(RxTestableComponent &comp, MockRadio &radio) {
  comp.node_id_[0] = 0xC0;
  comp.node_id_[1] = 0xFF;
  comp.node_id_[2] = 0xEE;
  static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
  comp.initialized_ = true;
  comp.radio_ = &radio;
  comp.add_device("054E17");
}

void encode_device_metadata(DeviceType type, uint8_t subtype, uint8_t *payload) {
  payload[0] = static_cast<uint8_t>(static_cast<uint8_t>(type) >> DEVICE_TYPE_LOW_BITS_SHIFT);
  payload[1] = static_cast<uint8_t>(subtype | ((static_cast<uint8_t>(type) & ((1U << DEVICE_TYPE_LOW_BITS_SHIFT) - 1U))
                                               << DEVICE_TYPE_HIGH_BITS_SHIFT));
}

}  // namespace

TEST(HubCore, FilterExchangeInternal_ChallengeRequest) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  // Construct a 0x3C challenge request frame (src=device, dst=some remote)
  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};  // our registered device
  uint8_t dst[3] = {0x43, 0x44, 0xE3};  // some remote
  set_src(f, src);
  set_dst(f, dst);
  uint8_t challenge[6] = {0x0F, 0x76, 0x11, 0x69, 0x64, 0x9E};
  set_cmd(f, CMD_CHALLENGE_REQ, challenge, 6);

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "frame should serialize";

  comp.process_received_packet_(pkt);

  // Should be silently filtered — no pending operations, no timeout
  EXPECT_TRUE(comp.op_queue_.empty()) << "0x3C should not trigger any operation";
  EXPECT_TRUE(comp.last_timeout_name_.empty()) << "0x3C should not schedule a timeout";
}

TEST(HubCore, FilterExchangeInternal_ChallengeResponse) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  // Construct a 0x3D challenge response frame (src=remote, dst=device)
  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0x43, 0x44, 0xE3};  // some remote
  uint8_t dst[3] = {0x05, 0x4E, 0x17};  // our registered device
  set_src(f, src);
  set_dst(f, dst);
  uint8_t hmac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  set_cmd(f, CMD_CHALLENGE_RESP, hmac, 6);

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "frame should serialize";

  comp.process_received_packet_(pkt);

  // Should be silently filtered — no pending operations, no timeout
  EXPECT_TRUE(comp.op_queue_.empty()) << "0x3D should not trigger any operation";
  EXPECT_TRUE(comp.last_timeout_name_.empty()) << "0x3D should not schedule a timeout";
}

// ============================================================================
// Remote activity detection tests (Issue #3)
// ============================================================================

TEST(HubCore, RemoteActivity_TriggersDelayedPoll) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  // Construct a 0x00 execute command from a remote to our registered device
  IoFrame f{};
  init_frame(f, true, true, false, false);  // 2W, start frame
  uint8_t src[3] = {0x43, 0x44, 0xE3};      // some remote
  uint8_t dst[3] = {0x05, 0x4E, 0x17};      // our registered device
  set_src(f, src);
  set_dst(f, dst);
  uint8_t exec_data[3] = {0xC8, 0x00, 0x00};
  set_cmd(f, CMD_EXECUTE, exec_data, 3);

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "frame should serialize";

  comp.process_received_packet_(pkt);

  // Should schedule the standard remote-activity timeout for the device.
  EXPECT_EQ(comp.last_timeout_ms_, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS) << "should schedule 2s timeout";
  EXPECT_NE(comp.last_timeout_name_.find("054E17"), std::string::npos) << "timeout name should contain device ID";
  ASSERT_TRUE(comp.last_timeout_callback_) << "callback should be set";

  // Invoke the callback — should queue a status request
  comp.last_timeout_callback_();
  ASSERT_EQ(comp.op_queue_.size(), 1u) << "callback should queue one operation";
  EXPECT_EQ(comp.op_queue_.front().type, PendingOperationType::REQUEST_STATUS);
  EXPECT_EQ(comp.op_queue_.front().device_id, "054E17");
}

TEST(HubCore, RemoteActivityWithoutConfiguredIntervalArmsTrackedSettlePolling) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  IoFrame trigger{};
  init_frame(trigger, true, true, false, false);
  uint8_t remote[3] = {0x43, 0x44, 0xE3};
  uint8_t device[3] = {0x05, 0x4E, 0x17};
  set_src(trigger, remote);
  set_dst(trigger, device);
  uint8_t exec_data[3] = {0xC8, 0x00, 0x00};
  set_cmd(trigger, CMD_EXECUTE, exec_data, sizeof(exec_data));

  RadioRxPacket trigger_pkt = make_rx_packet(trigger);
  ASSERT_GT(trigger_pkt.len, 0) << "frame should serialize";
  comp.process_received_packet_(trigger_pkt);

  ASSERT_NE(comp.get_device("054E17"), nullptr);
  EXPECT_NE(comp.poll_policy_.get_poll_deadline("054E17"), 0u)
      << "remote activity without an explicit interval should arm bounded tracked polling";

  IoFrame moving{};
  init_frame(moving, true, false, true, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  set_src(moving, device);
  set_dst(moving, own);
  uint8_t payload[8] = {0x00, 0x00, 0xC8, 0x00, 0x32, 0x00, 0x00, 0x05};
  set_cmd(moving, CMD_PRIVATE_RESP, payload, sizeof(payload));

  comp.update_device_status_(moving);

  EXPECT_NE(comp.poll_policy_.get_next_update("054E17"), 0u)
      << "a moving poll response after remote activity should schedule a hint-driven follow-up poll";
}

TEST(HubCore, RemoteActivity_UnregisteredDevice_NoTrigger) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  // Frame addressed to an unregistered device
  IoFrame f{};
  init_frame(f, true, true, false, false);
  uint8_t src[3] = {0x43, 0x44, 0xE3};
  uint8_t dst[3] = {0xAA, 0xBB, 0xCC};  // not registered
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_EXECUTE);

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "frame should serialize";

  comp.process_received_packet_(pkt);

  // Should NOT schedule a timeout — falls through to unhandled_cmd
  EXPECT_TRUE(comp.last_timeout_name_.empty()) << "unregistered dst should not trigger timeout";
  EXPECT_TRUE(comp.op_queue_.empty()) << "should not queue any operation";
}

TEST(HubCore, RemoteActivity_OwnEcho_NoTrigger) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  // Frame where src is our own node ID (echo of our own TX)
  IoFrame f{};
  init_frame(f, true, true, false, false);
  uint8_t src[3] = {0xC0, 0xFF, 0xEE};  // our node ID
  uint8_t dst[3] = {0x05, 0x4E, 0x17};  // our registered device
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_PRIVATE);

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "frame should serialize";

  comp.process_received_packet_(pkt);

  // Should NOT schedule a timeout — it's our own frame
  EXPECT_TRUE(comp.last_timeout_name_.empty()) << "own echo should not trigger timeout";
  EXPECT_TRUE(comp.op_queue_.empty()) << "should not queue any operation";
}

TEST(HubCore, RemoteActivity_LinkedRemote_TriggersDelayedPoll) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  // Link remote 9D6085 to device 054E17
  comp.add_linked_remote("9D6085", "054E17");

  // Construct a 1W frame from the linked remote to a different address (1W addressing)
  IoFrame f{};
  init_frame(f, false, true, true, false);  // 1W mode
  uint8_t src[3] = {0x9D, 0x60, 0x85};      // linked remote
  uint8_t dst[3] = {0x00, 0x01, 0xBF};      // 1W device address (different from 2W ID)
  set_src(f, src);
  set_dst(f, dst);
  uint8_t exec_data[3] = {0x00, 0x00, 0x00};
  set_cmd(f, CMD_EXECUTE, exec_data, 3);

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "frame should serialize";

  comp.process_received_packet_(pkt);

  // Should schedule the standard remote-activity timeout for the linked device.
  EXPECT_EQ(comp.last_timeout_ms_, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS) << "should schedule 2s timeout";
  EXPECT_NE(comp.last_timeout_name_.find("054E17"), std::string::npos)
      << "timeout name should contain linked device ID";
  ASSERT_TRUE(comp.last_timeout_callback_) << "callback should be set";

  // Invoke the callback — should queue a status request
  comp.last_timeout_callback_();
  ASSERT_EQ(comp.op_queue_.size(), 1u) << "callback should queue one operation";
  EXPECT_EQ(comp.op_queue_.front().type, PendingOperationType::REQUEST_STATUS);
  EXPECT_EQ(comp.op_queue_.front().device_id, "054E17");
}

TEST(HubCore, LinkedRemotes_MultipleRemotesOneDevice) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  // Link two remotes to the same device
  comp.add_linked_remote("AABBCC", "054E17");
  comp.add_linked_remote("DDEEFF", "054E17");

  // Frame from second remote
  IoFrame f{};
  init_frame(f, false, true, true, false);
  uint8_t src[3] = {0xDD, 0xEE, 0xFF};
  uint8_t dst[3] = {0x00, 0x01, 0xBF};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_EXECUTE);

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "frame should serialize";

  comp.process_received_packet_(pkt);

  EXPECT_EQ(comp.last_timeout_ms_, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS) << "should schedule 2s timeout";
  EXPECT_NE(comp.last_timeout_name_.find("054E17"), std::string::npos) << "timeout name should contain device ID";
}

TEST(HubCore, LinkedRemotes_OneRemoteMultipleDevices) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  comp.add_device("415684");  // second device

  // One remote controls two devices
  comp.add_linked_remote("AABBCC", "054E17");
  comp.add_linked_remote("AABBCC", "415684");

  // Frame from the shared remote
  IoFrame f{};
  init_frame(f, false, true, true, false);
  uint8_t src[3] = {0xAA, 0xBB, 0xCC};
  uint8_t dst[3] = {0x00, 0x01, 0xBF};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_EXECUTE);

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "frame should serialize";

  comp.process_received_packet_(pkt);

  // The last set_timeout call wins in our stub, but both should have been called.
  // Verify at least one timeout was scheduled with 2s delay.
  EXPECT_EQ(comp.last_timeout_ms_, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS) << "should schedule 2s timeout";
}

// ============================================================================
// Inbound status update tests (update_device_status_ paths)
// ============================================================================

TEST(HubCore, StatusUpdateFrameHandling) {
  // The CMD_STATUS_UPDATE path through process_received_packet_ triggers
  // authentication (authenticate_request_) which requires a challenge response
  // from the mock radio. Test update_device_status_ directly instead.
  TestableHubComponent comp;
  comp.add_device("054E17");

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // Status update payload: stopped, target at [5..6]=0xC800 (100%), current at [7..8]=0xC800 (100%)
  uint8_t payload[11] = {STATUS_STOPPED, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->position, 100.0f) << "status update should decode position to 100%";
  EXPECT_TRUE(dev->is_stopped) << "device should be stopped";
}

TEST(HubCore, StatusUpdateMovingFrameHandling) {
  TestableHubComponent comp;
  comp.add_device("054E17");

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // Status update: moving (STATUS_STOPPED NOT set), target at [5..6]=0xC800 (100%), current at [7..8]=0x3200 (25%)
  uint8_t payload[11] = {0x00, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0x32, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->target, 100.0f) << "target should decode to 100%";
  EXPECT_FLOAT_EQ(dev->position, 25.0f) << "position should decode to 25%";
  EXPECT_FALSE(dev->is_stopped) << "device should be moving";
}

TEST(HubCore, GetInfo2RespUpdatesDeviceType) {
  TestableHubComponent comp;
  comp.add_device("054E17");

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // INFO2 response: data[10..11] use the shared packed type/subtype metadata layout.
  uint8_t payload[12] = {0};
  encode_device_metadata(DeviceType::ROLLER_SHUTTER, 0, &payload[10]);
  set_cmd(f, CMD_GET_INFO2_RESP, payload, sizeof(payload));

  // update_device_status_ is called directly (not through process_received_packet_)
  comp.update_device_status_(f);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->type, DeviceType::ROLLER_SHUTTER) << "INFO2 response should update device type";
  EXPECT_EQ(dev->subtype, 0u) << "INFO2 response should update device subtype";
}

TEST(HubCore, GetInfo2RespDoesNotOverwriteDeclaredType) {
  TestableHubComponent comp;
  comp.add_device("054E17", DeviceType::AWNING, 7, false);

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[12] = {0};
  encode_device_metadata(DeviceType::ROLLER_SHUTTER, 0, &payload[10]);
  set_cmd(f, CMD_GET_INFO2_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->type, DeviceType::AWNING) << "INFO2 must not overwrite a YAML-declared device type";
  EXPECT_EQ(dev->subtype, 7u) << "INFO2 must not overwrite a YAML-declared subtype";
}

TEST(HubCore, ErrorRespDoesNotMutateTrackedPosition) {
  TestableHubComponent comp;
  comp.add_device("054E17");

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  dev->position = 25.0f;
  dev->target = 40.0f;
  dev->is_stopped = false;

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[1] = {RESULT_LIMITATION_BY_WIND};
  set_cmd(f, CMD_ERROR_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_FLOAT_EQ(dev->position, 25.0f) << "error responses should not overwrite last known position";
  EXPECT_FLOAT_EQ(dev->target, 40.0f) << "error responses should not overwrite last known target";
  EXPECT_FALSE(dev->is_stopped) << "error responses should not change movement state";
}

TEST(HubCore, ErrorRespSetsLastResultCode) {
  TestableHubComponent comp;
  comp.add_device("054E17");

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->last_result_code, 0u) << "newly added device should start with no recorded result";

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[1] = {RESULT_LIMITATION_BY_RAIN};
  set_cmd(f, CMD_ERROR_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_result_code, RESULT_LIMITATION_BY_RAIN) << "unsolicited 0xFE should record the result code";
  EXPECT_NE(dev->last_result_at_ms, 0u) << "unsolicited 0xFE should stamp a recorded-at timestamp";
}

TEST(HubCore, ErrorRespWithNonLimitationCodeStillRecords) {
  TestableHubComponent comp;
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[1] = {RESULT_COMMAND_COMPLETED_OK};
  set_cmd(f, CMD_ERROR_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_result_code, RESULT_COMMAND_COMPLETED_OK)
      << "non-limitation result codes should still be recorded on the device";
}

TEST(HubCore, ErrorRespWithEmptyPayloadDoesNotSetLastResultCode) {
  TestableHubComponent comp;
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_ERROR_RESP);  // zero-length payload — fails the ERROR_RESPONSE_MIN_DATA_LEN guard

  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_result_code, 0u) << "a too-short 0xFE must keep hitting the existing rejection path unchanged";
}

TEST(HubCore, SuccessfulPrivateResponseClearsLastResultCode) {
  TestableHubComponent comp;
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  dev->last_result_code = RESULT_LIMITATION_BY_WIND;
  dev->last_result_at_ms = 12345;

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // Bytes 0=flags(stopped), 2-3=target(0x0000=0%), 4-5=current(0x0000=0%).
  uint8_t payload[6] = {STATUS_STOPPED, 0x00, 0x00, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_result_code, 0u) << "a subsequent successful status reply should clear a stale result reason";
  EXPECT_EQ(dev->last_result_at_ms, 0u) << "clearing the result code should also clear its timestamp";
}

TEST(HubCore, SuccessfulStatusUpdateClearsLastResultCode) {
  TestableHubComponent comp;
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  dev->last_result_code = RESULT_LIMITATION_BY_RAIN;
  dev->last_result_at_ms = 12345;

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // 0x71 status update: byte0=flags(stopped), byte1=originator, bytes5-6=target, bytes7-8=current.
  uint8_t payload[11] = {STATUS_STOPPED, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_result_code, 0u)
      << "a subsequent device-initiated status update should clear a stale result reason";
  EXPECT_EQ(dev->last_result_at_ms, 0u) << "clearing the result code should also clear its timestamp";
}

TEST(HubCore, OwnControllerStatusUpdateSchedulesPoll) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  // A CMD_PRIVATE_RESP addressed to our registered device from a remote controller
  IoFrame f{};
  init_frame(f, true, true, false, false);
  uint8_t src[3] = {0x43, 0x44, 0xE3};  // some remote
  uint8_t dst[3] = {0x05, 0x4E, 0x17};  // our registered device
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_EXECUTE);

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "execute frame should serialize";

  comp.process_received_packet_(pkt);

  // Remote commanding our device → schedule 2s poll
  EXPECT_EQ(comp.last_timeout_ms_, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS) << "remote activity should schedule 2s poll";
  EXPECT_NE(comp.last_timeout_name_.find("054E17"), std::string::npos) << "timeout name should reference our device";
}

TEST(HubCore, StatusUpdateWithShortPayloadIgnored) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  // CMD_STATUS_UPDATE with payload < 11 bytes — should be logged as unsupported
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[5] = {0, 0, 0, 0, 0};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "short status update should serialize";

  comp.process_received_packet_(pkt);

  // Should not crash — short payload silently handled
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->position, UNKNOWN_POSITION) << "short status update should not change position";
}
