#pragma once

#include <gtest/gtest.h>

#include "hub_core.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_crypto.h"

namespace test {
using namespace esphome::home_io_control;

// Node identifiers (non-zero, not all-ones)
const uint8_t OWN_ID[3] = {0x11, 0x22, 0x33};
const uint8_t DST_ID[3] = {0x44, 0x55, 0x66};
const uint8_t FOREIGN_ID[3] = {0x77, 0x88, 0x99};

// Crypto test values (6-byte challenge, 16-byte system key)
const uint8_t TEST_CHALLENGE[6] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
const uint8_t TEST_SYSTEM_KEY[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
                                     0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10};

// Build a standard execute command frame (start frame, not end) for the given position.
inline IoFrame make_execute(uint8_t position) {
  IoFrame frame{};
  create_execute(frame, OWN_ID, DST_ID, true, position);
  return frame;
}

// Build a key-init command frame.
inline IoFrame make_key_init() {
  IoFrame frame{};
  create_key_init(frame, OWN_ID, DST_ID);
  return frame;
}

// Build a generic frame. Arguments order: src, dst, command, payload length.
inline IoFrame make_frame(const uint8_t src[3], const uint8_t dst[3], uint8_t cmd, uint8_t data_len) {
  IoFrame frame{};
  std::memcpy(frame.src, src, NODE_ID_SIZE);
  std::memcpy(frame.dst, dst, NODE_ID_SIZE);
  frame.cmd = cmd;
  frame.data_len = data_len;
  return frame;
}

/// PairingEngine subclass that promotes protected phase helpers to public.
///
/// Used by the pairing test suite as a shadow member in TestableComponent so
/// tests can script individual phases (discovery, key exchange, confirm wait)
/// without going through the discover_and_pair() orchestrator.
class TestablePairingEngine : public PairingEngine {
 public:
  using PairingEngine::PairingEngine;
  using PairingEngine::run_discovery_phase_;
  using PairingEngine::run_key_exchange_phase_;
  using PairingEngine::finalize_pairing_configuration_;
  using PairingEngine::wait_for_discovery_response_;
  using PairingEngine::wait_for_key_challenge_;
  using PairingEngine::wait_for_key_confirm_;
};

/// Hub subclass exposing protected members and methods for unit tests.
///
/// Use in place of IOHomeControlComponent in tests that need to inject a mock
/// radio, preset node-ID/key, or call internal helper methods without setup().
/// All promoted members/methods remain protected in the production hub.
class TestableHubComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::busy_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::system_key_;
  using IOHomeControlComponent::tuning_;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::poll_policy_;
  using IOHomeControlComponent::exchange_engine_;
  using IOHomeControlComponent::pairing_engine_;
  using IOHomeControlComponent::transmit_frame_;
  using IOHomeControlComponent::process_received_packet_;
  using IOHomeControlComponent::update_device_status_;
  using IOHomeControlComponent::notify_device_update_;
  using IOHomeControlComponent::begin_status_poll_tracking_;
  using IOHomeControlComponent::send_and_receive_;
  using IOHomeControlComponent::process_pending_operation_;
  using IOHomeControlComponent::authenticate_request_;
  using IOHomeControlComponent::api_rename_device_;
  using IOHomeControlComponent::register_management_actions_;
};

/// Shared mock hub base for platform entity tests.
///
/// The concrete platform test doubles override only the semantic command paths
/// they assert on. Device registry and callback fan-out stay centralized here
/// so hub interface growth does not need to be repeated in every test file.
class MockPlatformHubBase : public IOHomeControlComponent {
 public:
  ~MockPlatformHubBase() override = default;

  bool set_device_position(const std::string &, uint8_t) override { return false; }
  bool set_device_tilt(const std::string &, uint8_t) override { return false; }
  bool request_device_status(const std::string &) override { return false; }
  bool request_device_name(const std::string &) override { return false; }
  bool discover_and_pair() override { return false; }
  bool set_light_state(const std::string &, bool) override { return false; }
  bool set_switch_state(const std::string &, bool) override { return false; }
  bool set_lock_state(const std::string &, bool) override { return false; }

  void queue_set_device_position(const std::string &, uint8_t) override {}
  void queue_set_device_tilt(const std::string &, uint8_t) override {}
  void queue_request_device_status(const std::string &) override {}
  void queue_request_device_name(const std::string &) override {}
  void queue_discover_and_pair() override {}
  void queue_set_light_state(const std::string &, bool) override {}
  void queue_set_switch_state(const std::string &, bool) override {}
  void queue_set_lock_state(const std::string &, bool) override {}

  IoDevice *get_device(const std::string &device_id) override {
    auto it = devices_.find(device_id);
    return it != devices_.end() ? &it->second : nullptr;
  }

  void add_device(const std::string &device_id) override {
    if (devices_.count(device_id))
      return;
    devices_[device_id] = IoDevice{};
  }

  void add_device(const std::string &device_id, DeviceType type, uint8_t subtype, bool inverted) override {
    if (devices_.count(device_id))
      return;
    auto &device = devices_[device_id];
    device = IoDevice{};
    device.type = type;
    device.subtype = subtype;
    device.inverted = inverted;
  }

  void register_device_callback(DeviceUpdateCallback cb) override { callbacks_.push_back(std::move(cb)); }

  using IOHomeControlComponent::poll_policy_;

  void trigger_device_update(const std::string &device_id, const IoDevice &dev, bool cache_device = false) {
    if (cache_device)
      devices_[device_id] = dev;
    for (auto &cb : callbacks_)
      cb(device_id, dev);
  }

 private:
  std::map<std::string, IoDevice> devices_;
  std::vector<DeviceUpdateCallback> callbacks_;
};
}  // namespace test
