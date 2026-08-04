#include "hub_core.h"
#include "hub_internal.h"
#include "radio_interface.h"
#include "proto_frame.h"
#include "esphome/core/component.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;
using test::make_rx_packet;
using test::RxTestableComponent;
using test::setup_rx_test_component;
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
  comp.set_system_key("5B8E21F76C0A934D18F5A2E9B317C6D0");  // 16 bytes

  // Pretend setup called by manually initializing needed fields
  comp.node_id_[0] = 0xC0;
  comp.node_id_[1] = 0xFF;
  comp.node_id_[2] = 0xEE;
  static const uint8_t key[] = {0x5B, 0x8E, 0x21, 0xF7, 0x6C, 0x0A, 0x93, 0x4D,
                                0x18, 0xF5, 0xA2, 0xE9, 0xB3, 0x17, 0xC6, 0xD0};
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
  static const uint8_t key[] = {0x5B, 0x8E, 0x21, 0xF7, 0x6C, 0x0A, 0x93, 0x4D,
                                0x18, 0xF5, 0xA2, 0xE9, 0xB3, 0x17, 0xC6, 0xD0};
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

  comp.add_device("ABC123", {DeviceType::HORIZONTAL_AWNING, 5, true});
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
  static const uint8_t key[] = {0x5B, 0x8E, 0x21, 0xF7, 0x6C, 0x0A, 0x93, 0x4D,
                                0x18, 0xF5, 0xA2, 0xE9, 0xB3, 0x17, 0xC6, 0xD0};
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

// ========================================================================================
// Background-poll deferral during 1W remote activity
// ========================================================================================
// The radio is half-duplex and an exchange blocks for 1-3 s. A press on a linked remote
// schedules a status poll, so dispatching that poll while the same remote is still
// transmitting would blind the hub to the rest of the press.

TEST(HubCore, LoopDefersQueuedPollWhileRemoteIsStillTransmitting) {
  TestableHubComponent comp;
  comp.initialized_ = true;
  comp.radio_ = new MockRadio();
  comp.add_device("ABC123");

  ASSERT_TRUE(comp.op_queue_.enqueue_request_status("ABC123"));
  comp.last_1w_activity_ms_ = esphome::millis();

  EXPECT_TRUE(comp.defer_background_poll_()) << "a queued poll must yield during the remote's burst";
  comp.loop();
  EXPECT_EQ(comp.op_queue_.size(), 1u) << "the deferred poll stays queued rather than being dropped";

  delete comp.radio_;
}

TEST(HubCore, LoopDoesNotDeferQueuedPollWithoutRecentRemoteActivity) {
  TestableHubComponent comp;
  comp.initialized_ = true;
  comp.radio_ = new MockRadio();
  comp.add_device("ABC123");

  ASSERT_TRUE(comp.op_queue_.enqueue_request_status("ABC123"));
  ASSERT_EQ(comp.last_1w_activity_ms_, 0u) << "precondition: no 1W frame seen since boot";

  EXPECT_FALSE(comp.defer_background_poll_()) << "polls must dispatch normally when no remote is active";

  delete comp.radio_;
}

TEST(HubCore, LoopNeverDefersAUserCommandForRemoteActivity) {
  TestableHubComponent comp;
  comp.initialized_ = true;
  comp.radio_ = new MockRadio();
  comp.add_device("ABC123");

  comp.op_queue_.enqueue_device_command("ABC123", CoverCommand::STOP);
  comp.last_1w_activity_ms_ = esphome::millis();

  EXPECT_FALSE(comp.defer_background_poll_())
      << "1W broadcasts carry no ownership marker, so a neighbour's remote must not delay a user command";

  delete comp.radio_;
}

TEST(HubCore, RemoteActivityTimestampIsRecordedEvenForSuppressedRepeats) {
  // A duplicate frame still means the remote is transmitting, so the quiet period must extend
  // across the whole burst rather than only its first frame.
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  IoFrame f{};
  init_frame(f, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  const uint8_t broadcast_roller[NODE_ID_SIZE] = {0x00, 0x00, 0xBF};
  const uint8_t remote_id[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  set_dst(f, broadcast_roller);
  set_src(f, remote_id);
  const uint8_t payload[4] = {ORIGINATOR_USER_REMOTE, 0x41, 0xC8, 0x00};
  set_cmd(f, CMD_EXECUTE, payload, sizeof(payload));

  RadioRxPacket pkt = make_rx_packet(f);
  comp.process_received_packet_(pkt);
  const uint32_t after_first = comp.last_1w_activity_ms_;
  ASSERT_NE(after_first, 0u) << "the first frame must record activity";

  comp.last_1w_activity_ms_ = 0;
  comp.process_received_packet_(pkt);  // suppressed as a burst repeat
  EXPECT_NE(comp.last_1w_activity_ms_, 0u)
      << "activity is recorded before the dedup check, so repeats keep the radio reserved";
}
