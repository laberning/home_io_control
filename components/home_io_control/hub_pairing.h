#pragma once

/// @file hub_pairing.h
/// @brief Internal pairing-state model for hub‑owned discovery and key‑exchange flows.
/// @ingroup hioc_hub
///
/// This header defines the state machine enum and context structures for the
/// three‑phase pairing procedure (the flow itself is implemented by PairingEngine),
/// which temporarily tracks a newly paired device in the controller's runtime
/// registry and installs the system key on the device:
///
/// Phase 1 — Discovery (broadcast 0x28 → device responds 0x29):
///   Controller broadcasts a discovery packet on the primary channel. A device in
///   pairing mode (PROG button pressed) responds with its node ID and type/subtype.
///   The controller validates the response and extracts device metadata.
///
/// Phase 2 — Authenticated Key Exchange (0x31 → 0x3C → 0x32 → 0x33):
///   The controller sends CMD_KEY_INIT (0x31) to the device. The device challenges
///   with CMD_CHALLENGE_REQ (0x3C). The controller proves knowledge of the system
///   key with CMD_CHALLENGE_RESP (0x3D) and simultaneously sends the encrypted system
///   key via CMD_KEY_TRANSFER (0x32). The device confirms with CMD_KEY_CONFIRM (0x33).
///
/// Phase 3 — Configuration (CMD_SET_CONFIG1):
///   The controller sends a configuration frame (0x6F) to enable automatic status
///   updates from the device. This phase completes even if the config frame fails;
///   the device will still operate in polled mode.
///
/// All frames use the standard authenticated exchange flow (state types in
/// hub_exchange.h, implementation in ExchangeEngine). PairingEngine serializes these
/// phases, logs the YAML metadata the user should add, and keeps the paired device
/// in the current runtime registry until reboot.

#include "proto_frame.h"
#include "radio_interface.h"
#include <cstdint>
#include <string>

namespace esphome {
namespace home_io_control {

namespace pairing {

/// @brief State machine for the three‑phase pairing flow.
enum class PairingState : uint8_t {
  IDLE,                    ///< No pairing in progress; idle state.
  TX_DISCOVER,             ///< Discovery broadcast (0x28) sent; awaiting device response.
  WAIT_DISCOVER_RESPONSE,  ///< Listening for discovery response (0x29) from a device in pairing mode.
  TX_KEY_INIT,             ///< Key‑init (0x31) sent to the discovered device.
  WAIT_KEY_CHALLENGE,      ///< Waiting for challenge (0x3C) from device as part of key transfer.
  TX_KEY_TRANSFER,         ///< Key‑transfer (0x32) sent with encrypted system key.
  WAIT_KEY_CONFIRM,        ///< Waiting for key‑confirm (0x33) from device (key receipt acknowledgement).
  REGISTER_DEVICE,         ///< Registering device in the runtime registry for the current boot.
  COMPLETE,                ///< Pairing completed successfully; device ready for use.
  FAILED,                  ///< Pairing failed (timeout, radio error, or protocol violation).
};

/// @brief Context object that lives for the duration of a single pairing attempt.
struct PairingContext {
  PairingState state{PairingState::IDLE};  ///< Current state machine state.
  IoDevice device{};                       ///< Resolved device metadata after discovery (node_id, type, subtype, etc.).
  IoFrame req{};                           ///< Outbound frame buffer (reused across all phases).
  IoFrame resp{};                          ///< Inbound frame buffer (holds key‑confirm response).
  IoFrame rx{};                            ///< Raw RX frame during waiting phases (discovery, challenge, confirm).
  IoFrame key_init{};                      ///< Key‑init frame retained for key‑transfer IV derivation.
  RadioRxPacket packet{};                  ///< Raw radio capture for the current phase.
  std::string device_id;                   ///< Hex string representation of the paired node ID.
  bool discovery_metadata_complete{false};  ///< True when discovery carried type/subtype bytes.
};

}  // namespace pairing

/// @brief Get a short, log/telemetry-friendly name for a pairing state.
/// @param state Pairing state.
/// @return Null-terminated lowercase string such as "wait_key_challenge".
const char *pairing_stage_name(pairing::PairingState state);

}  // namespace home_io_control
}  // namespace esphome