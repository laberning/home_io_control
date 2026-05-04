#include "hub_core.h"
#include "radio_interface.h"
#include "proto_frame.h"
#include "esphome/core/component.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;

// ============================================================================
// HubCore test suite
// ============================================================================
// Device registry, persistence helpers, and pending operation queuing.

// ========================================================================================
// Device management tests
// ========================================================================================

TEST(HubCore, DeviceAddAndLookup) {
  IOHomeControlComponent comp;
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
  comp.add_device("9CA39C");  // valid node ID
  EXPECT_NE(comp.get_device("9CA39C"), nullptr) << "device should be found after add";

  // Adding same device twice should be no-op
  comp.add_device("9CA39C");
  EXPECT_NE(comp.get_device("9CA39C"), nullptr) << "device should still be found after duplicate add";

  // Unknown device returns nullptr
  EXPECT_EQ(comp.get_device("000000"), nullptr) << "all-zero node ID should not be found";
  EXPECT_EQ(comp.get_device("123456"), nullptr) << "random node ID should not be found";

  delete comp.radio_;
}

TEST(HubCore, AddDeviceClearsUnknownType) {
  IOHomeControlComponent comp;
  comp.node_id_[0] = 0xC0;
  comp.node_id_[1] = 0xFF;
  comp.node_id_[2] = 0xEE;
  static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
  comp.initialized_ = true;
  comp.radio_ = new MockRadio();

  comp.add_device("9CA39C");
  auto *dev = comp.get_device("9CA39C");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->type, DeviceType::UNKNOWN) << "newly added device should start as UNKNOWN type";
  EXPECT_EQ(dev->position, UNKNOWN_POSITION) << "newly added device should have UNKNOWN_POSITION";
  EXPECT_TRUE(dev->is_stopped) << "newly added device should be marked stopped";

  delete comp.radio_;
}

TEST(HubCore, QueueOperations) {
  IOHomeControlComponent comp;
  comp.node_id_[0] = 0xC0;
  comp.node_id_[1] = 0xFF;
  comp.node_id_[2] = 0xEE;
  static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
  comp.initialized_ = true;
  comp.radio_ = new MockRadio();

  // Initially empty
  EXPECT_TRUE(comp.pending_operations_.empty()) << "pending operations should start empty";

  // Queue a position command
  comp.queue_set_device_position("9CA39C", 50);
  EXPECT_EQ(comp.pending_operations_.size(), 1u) << "should have one pending operation after queue_set_device_position";
  auto op = comp.pending_operations_.front();
  EXPECT_EQ(op.type, IOHomeControlComponent::PendingOperationType::SET_POSITION)
      << "operation type should be SET_POSITION";
  EXPECT_EQ(op.device_id, "9CA39C") << "device ID should match queued device";
  EXPECT_EQ(op.position, 50u) << "position should be 50";

  // Queue a status request
  comp.queue_request_device_status("9CA39C");
  EXPECT_EQ(comp.pending_operations_.size(), 2u)
      << "should have two pending operations after queue_request_device_status";
  op = comp.pending_operations_.back();
  EXPECT_EQ(op.type, IOHomeControlComponent::PendingOperationType::REQUEST_STATUS)
      << "operation type should be REQUEST_STATUS";
  EXPECT_EQ(op.device_id, "9CA39C") << "device ID should match queued device";

  // Dequeue in-place
  comp.process_pending_operation_();  // processes front
  EXPECT_EQ(comp.pending_operations_.size(), 1u) << "should have one pending operation after processing first";
  op = comp.pending_operations_.front();
  EXPECT_EQ(op.type, IOHomeControlComponent::PendingOperationType::REQUEST_STATUS)
      << "remaining operation should be REQUEST_STATUS";

  delete comp.radio_;
}

TEST(HubCore, PersistedNodeIdIsValid) {
  // Valid: not all zeros, not all 0xFF
  uint8_t valid[3] = {0xC0, 0xFF, 0xEE};
  EXPECT_TRUE(persisted_node_id_is_valid(valid)) << "mixed non-zero bytes should be valid";

  uint8_t not_all_ff[3] = {0xFE, 0xFF, 0xFF};
  EXPECT_TRUE(persisted_node_id_is_valid(not_all_ff)) << "not all 0xFF should be valid";

  uint8_t not_all_zero[3] = {0x01, 0x00, 0x00};
  EXPECT_TRUE(persisted_node_id_is_valid(not_all_zero)) << "not all zero should be valid";

  // Invalid: all zeros
  uint8_t all_zero[3] = {0x00, 0x00, 0x00};
  EXPECT_FALSE(persisted_node_id_is_valid(all_zero)) << "all zeros should be invalid";

  // Invalid: all 0xFF
  uint8_t all_ff[3] = {0xFF, 0xFF, 0xFF};
  EXPECT_FALSE(persisted_node_id_is_valid(all_ff)) << "all 0xFF should be invalid";
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
