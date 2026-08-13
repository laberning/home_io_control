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

// Build a standard execute command frame (start frame, not end) for the given position (0-100).
inline IoFrame make_execute(uint8_t position) {
  IoFrame frame{};
  create_execute_position(frame, OWN_ID, DST_ID, true, position);
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
  using IOHomeControlComponent::oneway_last_observed_class_;
  using IOHomeControlComponent::record_oneway_observed_class_;
  using IOHomeControlComponent::update_device_status_;
  using IOHomeControlComponent::notify_device_update_;
  using IOHomeControlComponent::begin_status_poll_tracking_;
  using IOHomeControlComponent::send_and_receive_;
  using IOHomeControlComponent::process_pending_operation_;
  using IOHomeControlComponent::authenticate_request_;
  using IOHomeControlComponent::api_rename_device_;
  using IOHomeControlComponent::register_management_actions_;
  using IOHomeControlComponent::last_1w_activity_ms_;
  using IOHomeControlComponent::first_1w_activity_ms_;
  using IOHomeControlComponent::defer_background_poll_;
#ifdef IOHOME_LR1121_FIRMWARE_UPDATE
  using IOHomeControlComponent::lr1121_firmware_updater_;
  using IOHomeControlComponent::lr1121_bootloader_version_known_;
  using IOHomeControlComponent::lr1121_bootloader_chip_type_;
  using IOHomeControlComponent::lr1121_bootloader_version_;
  using IOHomeControlComponent::lr1121_flash_verdict_known_;
  using IOHomeControlComponent::lr1121_flash_verdict_;
  using IOHomeControlComponent::lr1121_installed_fw_;
  using IOHomeControlComponent::lr1121_installed_device_type_;
  using IOHomeControlComponent::lr1121_flash_confirmation_armed_;
  using IOHomeControlComponent::describe_lr1121_flash_verdict_;
  using IOHomeControlComponent::lr1121_firmware_update_debug_lines_;
  using IOHomeControlComponent::run_lr1121_boot_time_bootloader_read_;
  using IOHomeControlComponent::cache_lr1121_flash_verdict_;
#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE
  using IOHomeControlComponent::bootloader_rewrite_allowed_;
  using IOHomeControlComponent::describe_lr1121_bootloader_refusal_;
#endif
#endif
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
  using IOHomeControlComponent::last_1w_activity_ms_;
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
/// The concrete platform test doubles override only the semantic command paths they assert on
/// (the radio-facing set_*/queue_* calls, stubbed out here). Everything else — device storage,
/// subscriber fan-out, optimistic state — is inherited from the real component and runs against
/// a real DeviceRegistry, so hub interface growth needs no repetition in any test file.
/// Device IDs must therefore be valid hex node IDs (e.g. "ABC123"): DeviceRegistry::add()
/// rejects anything else, and get_device() would then return nullptr.
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

  // add_device/get_device/set_device_dimmable/register_device_callback/apply_optimistic_* are
  // deliberately NOT overridden — the base implements them as one-line delegations to the real
  // DeviceRegistry, so inheriting them tests production code instead of a parallel copy. This
  // mock previously kept its own std::map and reimplemented that logic; it diverged silently the
  // moment the registry grew a method, because the base's virtual was never reached and every
  // platform test saw a no-op instead of the new behavior.

  using IOHomeControlComponent::poll_policy_;

  /// Publish `dev` to the entity callbacks as if it had just arrived from the radio.
  /// @param device_id   Device the update is for; must already be registered via add_device().
  /// @param dev         Device state to publish.
  /// @param cache_device True to leave `dev` in the registry afterwards; false to publish it as a
  ///                     one-off and restore the previously stored state, matching how a test
  ///                     drives a sequence of updates without disturbing the device it set up.
  void trigger_device_update(const std::string &device_id, const IoDevice &dev, bool cache_device = false) {
    IoDevice const *stored = this->registry_.get(device_id);
    // notify() publishes what the registry holds, so the update has to be stored to be seen.
    // Snapshot first when the caller wants it put back.
    bool const was_registered = stored != nullptr;
    IoDevice const previous = was_registered ? *stored : IoDevice{};
    this->registry_.put(device_id, dev);
    this->registry_.notify(device_id);
    // Restore only when there was something to restore — putting back a default-constructed
    // IoDevice for a device that was never registered would leave a phantom with a zero node ID.
    if (!cache_device && was_registered)
      this->registry_.put(device_id, previous);
  }
};
}  // namespace test
