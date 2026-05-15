/// @file hub_operations_test.cpp
/// @brief Tests for hub_operations.cpp high-level command execution.

#include "hub_core.h"
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
// set_light_state, set_switch_state, and the queued operation dispatch.
//
// These tests verify capability-gating and correct command construction
// using mock radio and a testable component subclass.

namespace {

class TestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::send_and_receive_;
  using IOHomeControlComponent::process_pending_operation_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::system_key_;
  using IOHomeControlComponent::devices_;
  using IOHomeControlComponent::pending_operations_;
};

// Build a response frame from device (matching device node_id 0x9CA39C)
static IoFrame build_status_response(const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t device_node_id[3] = {0x9C, 0xA3, 0x9C};
  set_dst(f, dst);
  set_src(f, device_node_id);
  uint8_t payload[8] = {STATUS_STOPPED, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));
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
  comp.add_device("9CA39C");
  auto *dev = comp.get_device("9CA39C");
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

  bool ok = comp.set_device_position("9CA39C", 50);
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
  auto *dev = comp.get_device("9CA39C");
  ASSERT_NE(dev, nullptr);
  dev->type = DeviceType::LIGHT;

  bool ok = comp.set_device_position("9CA39C", 50);
  EXPECT_FALSE(ok) << "set_device_position should reject mid position for light device";
}

TEST(HubOperations, LightBinaryPositionExchangeFails) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("9CA39C");
  ASSERT_NE(dev, nullptr);
  dev->type = DeviceType::LIGHT;

  bool ok = comp.set_device_position("9CA39C", 0);
  EXPECT_FALSE(ok) << "set_device_position fails because send_and_receive_ gets no response";
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

  bool ok = comp.request_device_status("9CA39C");
  EXPECT_TRUE(ok) << "request_device_status should succeed for cover device";
}

TEST(HubOperations, RequestDeviceStatusRejectsUnknownDevice) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  bool ok = comp.request_device_status("000000");
  EXPECT_FALSE(ok) << "request_device_status should fail for unregistered device";
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

  comp.queue_set_device_position("9CA39C", 75);
  ASSERT_EQ(comp.pending_operations_.size(), 1u) << "one operation should be queued";

  comp.process_pending_operation_();
  EXPECT_TRUE(comp.pending_operations_.empty()) << "queue should be empty after processing";
}

TEST(HubOperations, QueuedOperationSkipsWhenBusy) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  comp.busy_ = true;
  comp.queue_set_device_position("9CA39C", 50);
  comp.process_pending_operation_();
  EXPECT_EQ(comp.pending_operations_.size(), 1u) << "should not process when busy";
}

TEST(HubOperations, DuplicateDiscoverAndPairNotQueued) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);

  comp.queue_discover_and_pair();
  comp.queue_discover_and_pair();  // duplicate
  EXPECT_EQ(comp.pending_operations_.size(), 1u) << "duplicate discover should be suppressed";
}

TEST(HubOperations, QueueDevicePositionRejectedForLight) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  auto *dev = comp.get_device("9CA39C");
  ASSERT_NE(dev, nullptr);
  dev->type = DeviceType::LIGHT;

  comp.queue_set_device_position("9CA39C", 50);
  EXPECT_TRUE(comp.pending_operations_.empty()) << "non-cover device should be rejected in queue check";
}

TEST(HubOperations, QueueDeviceTiltRejectedForNonTilt) {
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  // ROLLER_SHUTTER does not support tilt
  comp.queue_set_device_tilt("9CA39C", 50);
  EXPECT_TRUE(comp.pending_operations_.empty()) << "non-tilt device should be rejected in queue check";
}