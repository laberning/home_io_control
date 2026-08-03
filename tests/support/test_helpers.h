#pragma once

#include <gtest/gtest.h>

#include "hub_core.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "../stubs/radio_test_common.h"

#include <cstring>

namespace test {
using namespace esphome::home_io_control;

// Node identifiers (non-zero, not all-ones)
const uint8_t OWN_ID[3] = {0x11, 0x22, 0x33};
const uint8_t DST_ID[3] = {0x44, 0x55, 0x66};
const uint8_t FOREIGN_ID[3] = {0x77, 0x88, 0x99};

// Crypto test values (6-byte challenge, 16-byte system key)
const uint8_t TEST_CHALLENGE[6] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
// "corpus key" — the one well-known key shared by the entire test suite and the
// golden-frame corpus (tests/corpus/README.md); re-keyed own-hardware captures are rewritten
// to verify under this value so no real system key is ever committed. Community-supplied
// captures are never re-keyed — they carry `key: unknown` and are never HMAC-verified against
// this value; only the maintainer's own hardware qualifies for `--rekey`.
const uint8_t TEST_SYSTEM_KEY[16] = {0xDE, 0xCA, 0xFC, 0x0F, 0xFE, 0xE0, 0xFF, 0x1C,
                                     0xEB, 0xAD, 0xBE, 0xEF, 0xF0, 0x0D, 0xBA, 0x11};

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
  using IOHomeControlComponent::key_extraction_ctx_;
  using IOHomeControlComponent::generate_key_extraction_throwaway_id_;
  using IOHomeControlComponent::update_device_status_;
  using IOHomeControlComponent::notify_device_update_;
  using IOHomeControlComponent::begin_status_poll_tracking_;
  using IOHomeControlComponent::send_and_receive_;
  using IOHomeControlComponent::process_pending_operation_;
  using IOHomeControlComponent::authenticate_request_;
  using IOHomeControlComponent::api_rename_device_;
  using IOHomeControlComponent::register_management_actions_;
};

/// Hub subclass for RX-path tests: exposes process_received_packet_/update_device_status_ plus
/// the node ID/system key/radio/queue/poll-policy state a full RX dispatch touches. Shared by
/// hub_core_test.cpp (exchange-internal filtering) and hub_status_test.cpp (status/remote-
/// activity/link-health handling) — both drive real frames through the same RX entry points.
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
inline RadioRxPacket make_rx_packet(const IoFrame &frame) {
  RadioRxPacket pkt{};
  pkt.len = serialize(frame, pkt.data, sizeof(pkt.data));
  pkt.freq_hz = FREQ_CH2;
  return pkt;
}

/// Create a minimal RX-test component with one registered device ("054E17") and a mock radio.
inline void setup_rx_test_component(RxTestableComponent &comp, MockRadio &radio) {
  comp.node_id_[0] = 0xC0;
  comp.node_id_[1] = 0xFF;
  comp.node_id_[2] = 0xEE;
  static const uint8_t key[] = {0x5B, 0x8E, 0x21, 0xF7, 0x6C, 0x0A, 0x93, 0x4D,
                                0x18, 0xF5, 0xA2, 0xE9, 0xB3, 0x17, 0xC6, 0xD0};
  std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
  comp.initialized_ = true;
  comp.radio_ = &radio;
  comp.add_device("054E17");
}

/// Pack a device type/subtype pair into the shared 2-byte metadata layout (used by GetInfo2Resp
/// and discovery-response frames).
inline void encode_device_metadata(DeviceType type, uint8_t subtype, uint8_t *payload) {
  payload[0] = static_cast<uint8_t>(static_cast<uint8_t>(type) >> DEVICE_TYPE_LOW_BITS_SHIFT);
  payload[1] = static_cast<uint8_t>(subtype | ((static_cast<uint8_t>(type) & ((1U << DEVICE_TYPE_LOW_BITS_SHIFT) - 1U))
                                               << DEVICE_TYPE_HIGH_BITS_SHIFT));
}

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
  bool set_light_position(const std::string &, uint8_t) override { return false; }
  bool set_light_state(const std::string &, bool) override { return false; }
  bool set_switch_state(const std::string &, bool) override { return false; }
  bool set_lock_state(const std::string &, bool) override { return false; }

  void queue_set_device_position(const std::string &, uint8_t) override {}
  void queue_set_device_tilt(const std::string &, uint8_t) override {}
  void queue_request_device_status(const std::string &) override {}
  void queue_request_device_name(const std::string &) override {}
  void queue_discover_and_pair() override {}
  void queue_set_light_position(const std::string &, uint8_t) override {}
  void queue_set_light_state(const std::string &, bool) override {}
  void queue_set_switch_state(const std::string &, bool) override {}
  void queue_set_lock_state(const std::string &, bool) override {}

  IoDevice *get_device(const std::string &device_id) override {
    auto it = devices_.find(device_id);
    return it != devices_.end() ? &it->second : nullptr;
  }

  void set_device_dimmable(const std::string &device_id, bool dimmable) override {
    if (auto it = devices_.find(device_id); it != devices_.end())
      it->second.dimmable = dimmable;
  }

  void add_device(const std::string &device_id) override {
    if (devices_.count(device_id))
      return;
    devices_[device_id] = IoDevice{};
  }

  void add_device(const std::string &device_id, const DeviceConfig &cfg) override {
    if (devices_.count(device_id))
      return;
    auto &device = devices_[device_id];
    device = IoDevice{};
    device.type = cfg.type;
    device.subtype = cfg.subtype;
    device.inverted = cfg.inverted;
    device.optimistic_state = cfg.optimistic_state;
  }

  bool apply_optimistic_target(const std::string &device_id, float target_io_position) override {
    auto it = devices_.find(device_id);
    if (it == devices_.end() || !it->second.optimistic_state)
      return false;
    it->second.target = target_io_position;
    it->second.is_stopped = false;
    for (auto &cb : callbacks_)
      cb(device_id, it->second);
    return true;
  }

  bool clear_optimistic_target(const std::string &device_id) override {
    auto it = devices_.find(device_id);
    if (it == devices_.end() || !it->second.optimistic_state)
      return false;
    it->second.target = UNKNOWN_POSITION;
    it->second.is_stopped = true;
    for (auto &cb : callbacks_)
      cb(device_id, it->second);
    return true;
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
