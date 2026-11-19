#pragma once

/// @file hub_operations_fixture.h
/// @brief Test double for the hub operations tests: a component with its protected members opened, and the
/// builders that set it up with a cover and answer it with an error or a challenge.

#include "hub_core.h"
#include "hub_internal.h"
#include "proto_frame.h"
#include "proto_commands.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

namespace hub_operations_test {

using namespace esphome::home_io_control;

class TestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::begin_status_poll_tracking_;
  using IOHomeControlComponent::update_device_status_;
  using IOHomeControlComponent::execute_device_command_;
  using IOHomeControlComponent::send_and_receive_;
  using IOHomeControlComponent::process_pending_operation_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::exchange_engine_;
  using IOHomeControlComponent::heater_controller_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::system_key_;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::busy_;
  using IOHomeControlComponent::poll_policy_;
  using IOHomeControlComponent::schedule_status_poll_;
  using IOHomeControlComponent::tuning_;
};

inline IoFrame build_error_response(const uint8_t dst[3], uint8_t result) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(f, dst);
  set_src(f, device_node_id);
  uint8_t payload[1] = {result};
  set_cmd(f, CMD_ERROR_RESP, payload, sizeof(payload));
  return f;
}

inline IoFrame build_challenge_request(const uint8_t src[3], const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, false, false, false);
  set_dst(f, dst);
  set_src(f, src);
  uint8_t challenge[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  set_cmd(f, CMD_CHALLENGE_REQ, challenge, sizeof(challenge));
  return f;
}

// Setup a component with one registered cover device
/// Initialise the component and register "ABC123" from `config`, as an entity's setup() would.
inline void setup_declared_component(TestableComponent &comp, MockRadio &radio, const DeviceConfig &config) {
  comp.node_id_[0] = 0xC0;
  comp.node_id_[1] = 0xFF;
  comp.node_id_[2] = 0xEE;
  static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
  std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
  comp.initialized_ = true;
  comp.radio_ = &radio;
  comp.add_device("ABC123", config);
}

inline void setup_cover_component(TestableComponent &comp, MockRadio &radio) {
  setup_declared_component(comp, radio, DeviceConfig{});
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  test::set_kind(*dev, DeviceType::ROLLER_SHUTTER);
}

}  // namespace hub_operations_test
