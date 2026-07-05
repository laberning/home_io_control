#pragma once

/// @file pairing_engine.h
/// @brief Device discovery and key-exchange engine for IO-Homecontrol pairing.
/// @ingroup hioc_hub
///
/// PairingEngine encapsulates all three phases of the IO-Homecontrol pairing flow:
///
/// Phase 1 — Discovery (0x28 → 0x29):
///   Controller broadcasts a discovery packet. A device in pairing mode responds
///   with its node ID and type/subtype metadata.
///
/// Phase 2 — Authenticated Key Exchange (0x31 → 0x3C → 0x32 → 0x33):
///   The controller sends CMD_KEY_INIT (0x31). The device challenges with 0x3C.
///   The controller proves knowledge of the system key and simultaneously
///   transfers the encrypted system key (0x32). The device confirms with 0x33.
///
/// Phase 3 — Configuration (0x6F):
///   The controller sends SetConfig1 to enable automatic device status updates.
///
/// The engine is constructed once by IOHomeControlComponent. It holds double-pointer
/// indirection for the radio driver so test assignments (`comp.radio_ = &mock`) propagate
/// without calling setup(). All radio operations go through ExchangeEngine so that
/// LBT, preamble selection, and frequency hopping are centralised.
///
/// PairingEngine is non-copyable and non-movable because it stores pointer and reference
/// addresses that would dangle in a copy.

#include "proto_frame.h"
#include "hub_pairing.h"
#include "hub_decisions.h"
#include "exchange_engine.h"
#include "device_registry.h"
#include "radio_interface.h"
#include "tuning_config.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace home_io_control {

/// Owns and drives all three phases of the IO-Homecontrol device pairing flow.
///
/// Constructed once by IOHomeControlComponent; collaborators (radio, exchange engine,
/// device registry) are injected as pointers/references so the engine never outlives them.
/// @ingroup hioc_hub
class PairingEngine {
 public:
  /// Construct the engine with all required collaborators.
  ///
  /// @param radio_ptr  Double pointer into the hub's `radio_` member — survives driver replacement in tests.
  /// @param node_id    Controller 3-byte node ID buffer, owned by the hub.
  /// @param system_key 16-byte AES system key buffer, owned by the hub.
  /// @param tuning     Tuning configuration, owned by the hub.
  /// @param engine     Shared exchange engine for transmit/receive operations.
  /// @param registry   Device registry where paired devices are permanently registered.
  PairingEngine(RadioDriver **radio_ptr, const uint8_t *node_id, const uint8_t *system_key, const TuningConfig *tuning,
                ExchangeEngine &engine, DeviceRegistry &registry);

  /// Non-copyable — stores double-pointer and references into hub member addresses.
  PairingEngine(const PairingEngine &) = delete;
  PairingEngine &operator=(const PairingEngine &) = delete;

  /// Discover and pair a device currently in pairing mode (three-phase orchestrator).
  /// @return true if all three phases completed successfully; false otherwise.
  bool discover_and_pair();

  /// Extract node ID, device type, and subtype from a CMD_DISCOVER_RESP frame.
  static void parse_device_from_discovery(const IoFrame &frame, IoDevice &device, std::string &device_id);

 protected:
  // --- Phase helpers (protected; exposed to tests via TestablePairingEngine in test_helpers.h) ---

  /// Phase 1: broadcast discovery command(s) and wait for a device response (0x29).
  /// @param context Pairing context updated on success.
  /// @return ACCEPT on success; NO_RESPONSE or INVALID otherwise.
  decisions::PairingDiscoveryDisposition run_discovery_phase_(pairing::PairingContext &context);

  /// Phase 2: authenticated key exchange (0x31 → 0x3C → 0x32 → 0x33).
  /// @param context Pairing context populated by run_discovery_phase_().
  /// @return true if key exchange completes; false on any failure.
  bool run_key_exchange_phase_(pairing::PairingContext &context);

  /// Phase 3: send SetConfig1 (0x6F) to enable automatic status updates; best-effort.
  /// @param context Pairing context with device information from phases 1 and 2.
  /// @return Always true — pairing proceeds regardless of config outcome.
  bool finalize_pairing_configuration_(pairing::PairingContext &context);

  /// Wait for a discovery response (0x29) within timeout_ms with per-chip frequency hopping.
  /// @param timeout_ms     Maximum wait window.
  /// @param packet         Output: raw RadioRxPacket of the accepted discovery frame.
  /// @param response_frame Output: parsed IoFrame of the accepted discovery frame.
  /// @return ACCEPT on success; NO_RESPONSE (no traffic) or INVALID (wrong frames) otherwise.
  decisions::PairingDiscoveryDisposition wait_for_discovery_response_(uint32_t timeout_ms, RadioRxPacket &packet,
                                                                      IoFrame &response_frame);

  /// Wait for a key-challenge (0x3C) or direct key-confirm (0x33) from the target device.
  /// @param timeout_ms        Maximum wait window.
  /// @param packet            Output: raw RadioRxPacket of the accepted frame.
  /// @param challenge_frame   Output: parsed IoFrame.
  /// @param device_node_id    Expected source node ID (devices paired to).
  /// @return true if a valid challenge or confirm was received; false on timeout.
  bool wait_for_key_challenge_(uint32_t timeout_ms, RadioRxPacket &packet, IoFrame &challenge_frame,
                               const uint8_t device_node_id[NODE_ID_SIZE]);

  /// Transmit the 0x32 key transfer and wait for the 0x33 key confirm with retry.
  bool wait_for_key_confirm_(pairing::PairingContext &context);

 private:
  /// Convenience accessor returning the current radio driver (dereferences double pointer).
  [[nodiscard]] RadioDriver *radio_() const { return *radio_ptr_; }

  RadioDriver **radio_ptr_;
  const uint8_t *node_id_;
  const uint8_t *system_key_;
  const TuningConfig *tuning_;
  ExchangeEngine &engine_;
  DeviceRegistry &registry_;
};

}  // namespace home_io_control
}  // namespace esphome
