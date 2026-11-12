#pragma once

/// @file management_test_fixture.h
/// @brief Shared fixture for the management-action and diagnostic-probe tests.
///
/// TestableManagementComponent exposes the hub's protected action wrappers; setup_component() gives
/// a component one registered device ("ABC123") on a MockRadio. Both test files include this header.

#include "hub_core.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

namespace management_test {

using namespace esphome::home_io_control;

class TestableManagementComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::api_force_open_device_;
  using IOHomeControlComponent::api_heating_control_;
  using IOHomeControlComponent::api_identify_device_;
  using IOHomeControlComponent::api_oneway_remove_controller_;
  using IOHomeControlComponent::api_oneway_set_position_;
  using IOHomeControlComponent::api_probe_device_;
  using IOHomeControlComponent::api_probe_sweep_;
  using IOHomeControlComponent::api_rename_device_;
  using IOHomeControlComponent::api_scan_paired_devices_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::register_management_actions_;
  using IOHomeControlComponent::registry_;
  using IOHomeControlComponent::system_key_;
  using IOHomeControlComponent::tuning_;
};

inline void setup_component(TestableManagementComponent &component, MockRadio &radio) {
  component.node_id_[0] = 0xC0;
  component.node_id_[1] = 0xFF;
  component.node_id_[2] = 0xEE;
  static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  std::memcpy(component.system_key_, key, AES_KEY_SIZE);
  component.initialized_ = true;
  component.radio_ = &radio;
  // Small window so scan_paired_devices() tests don't spend thousands of no-op host-test
  // iterations waiting out the default 2000 ms discovery window: in host tests millis()
  // advances by exactly 1 per call.
  component.tuning_.pairing_discovery_wait_ms = 20;
  component.add_device("ABC123");
}

inline RadioRxPacket frame_to_packet(const IoFrame &frame) {
  RadioRxPacket packet{};
  packet.len = serialize(frame, packet.data, sizeof(packet.data));
  packet.freq_hz = FREQ_CH2;
  return packet;
}

inline IoFrame make_error_response(const uint8_t dst[3], uint8_t result_code) {
  IoFrame frame{};
  init_frame(frame, true, false, true, false);
  const uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, dst);
  set_src(frame, device_node_id);
  uint8_t payload[1] = {result_code};
  set_cmd(frame, CMD_ERROR_RESP, payload, sizeof(payload));
  return frame;
}

inline void make_climate_device(TestableManagementComponent &component) {
  auto *dev = component.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 1);
}

}  // namespace management_test
