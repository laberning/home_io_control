#pragma once

/// @file hub_pairing.h
/// @brief Internal pairing-state model for hub‑owned discovery and key‑exchange flows.
///
/// This module implements the three‑phase pairing procedure that establishes a
/// new device in the controller's registry and installs the system key on the device:
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
/// All frames use the standard authenticated exchange flow defined in hub_exchange.h.
/// The pairing state machine serializes these phases and persists the device on success.

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
  PERSIST_DEVICE,          ///< Persisting device metadata and node ID to flash.
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
};

}  // namespace pairing

}  // namespace home_io_control
}  // namespace esphome