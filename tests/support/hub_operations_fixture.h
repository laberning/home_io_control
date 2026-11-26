#pragma once

/// @file hub_operations_fixture.h
/// @brief Test double for the hub operations tests: a component with its protected members opened, and the
/// builders that set it up with a cover and answer it with an error, a challenge or a status reply.

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
  using IOHomeControlComponent::act_on_foreign_frame_;
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

// Build a response frame from device (matching device node_id 0xABC123)
inline IoFrame build_status_response(const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(f, dst);
  set_src(f, device_node_id);
  uint8_t payload[8] = {test::STATUS_BYTE_AT_REST, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));
  return f;
}

inline IoFrame build_moving_status_response(const uint8_t dst[3], uint8_t remaining_s) {
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
inline IoFrame build_short_moving_status_response(const uint8_t dst[3]) {
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
inline IoFrame build_tilt_execute_ack_response(const uint8_t dst[3]) {
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
inline IoFrame build_name_response(const uint8_t dst[3], const uint8_t *payload, uint8_t payload_len) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(f, dst);
  set_src(f, device_node_id);
  set_cmd(f, CMD_GET_NAME_RESP, payload, payload_len);
  return f;
}

// Attempts the engine plans for a settling read of "ABC123" while it stays silent: the number a
// read after a STOP, a confirming read, a ladder-band poll or a one-off poll sends.
inline size_t planned_settling_attempts(TestableComponent &comp) {
  const IoDevice *dev = comp.get_device("ABC123");
  IoFrame request{};
  EXPECT_TRUE(create_get_status(request, comp.node_id_, dev->node_id, dev->low_power));
  const ExchangeEngine &engine = comp.exchange_engine_;
  return decisions::attempt_plan(decisions::ReadKind::SETTLING, dev->low_power, comp.tuning_, engine.band(),
                                 engine.tx_time_ms(request, FREQ_CH2, engine.request_preamble_for(request)))
      .max_attempts;
}

// Register "ABC123" as a ROLLER_SHUTTER with an explicit low_power class via DeviceConfig — the
// same path the codegen'd entity binding uses, exercised here without the entity mixin (that side
// is covered in platform_cover_test.cpp).
inline void setup_low_power_cover(TestableComponent &comp, MockRadio &radio, bool low_power) {
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

}  // namespace hub_operations_test
