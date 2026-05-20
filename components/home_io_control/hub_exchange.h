#pragma once

/// @file hub_exchange.h
/// @brief Internal exchange-state model for hub-owned authenticated non‑pairing flows.
/// @ingroup hioc_hub
///
/// This module defines the state machines and context structures used for
/// outbound authenticated exchanges (controller → device) and inbound authentication
/// (device → controller). These are the building blocks that power commands like
/// set_position, request_status, and handling unsolicited status‑update frames.
///
/// Exchange lifecycle (outbound):
///   1. Controller sends a command with START flag (e.g., CMD_EXECUTE, CMD_PRIVATE).
///   2. Device may challenge with CMD_CHALLENGE_REQ (0x3C) if it requires auth.
///   3. Controller computes HMAC and responds with CMD_CHALLENGE_RESP (0x3D).
///   4. Device finally sends the response frame (e.g., CMD_PRIVATE_RESP with position).
///
/// Inbound authentication (device-initiated):
///   Device sends a command that requires verification (e.g., CMD_STATUS_UPDATE).
///   Controller challenges with 0x3C, device proves knowledge of system key with 0x3D,
///   controller acknowledges with CMD_STATUS_UPDATE_RESP (0x72).
///
/// Both paths rely on the HMAC construction defined in proto_crypto.h which uses
/// AES-128-ECB to encrypt an IV derived from the original frame bytes and a
/// 6-byte random challenge.

#include "proto_frame.h"
#include "radio_interface.h"
#include <cstdint>
#include <string>

namespace esphome {
namespace home_io_control {

namespace exchange {

/// @brief State machine for an outbound authenticated exchange (non‑pairing).
enum class OutboundExchangeState : uint8_t {
  IDLE,                 ///< No active exchange; idle state.
  TX_REQUEST,           ///< Request frame transmitted; awaiting first response from device.
  WAIT_FIRST_RESPONSE,  ///< Listening for first response. This may be a challenge (0x3C) or the final response.
  BUILD_AUTH_RESPONSE,  ///< Building the 0x3D challenge response after receiving 0x3C.
  TX_AUTH_RESPONSE,     ///< Auth response (0x3D) transmitted; awaiting device's final reply.
  WAIT_FINAL_RESPONSE,  ///< Listening for the authenticated final response (e.g., status frame).
  SUCCESS,              ///< Exchange completed successfully; device acknowledged.
  FAILED,               ///< Exchange failed (timeout, retries exhausted, or radio error).
};

/// @brief State machine for inbound authentication (device‑initiated commands).
enum class InboundAuthState : uint8_t {
  IDLE,                     ///< No inbound authentication in progress.
  TX_CHALLENGE,             ///< Challenge (0x3C) sent to device; awaiting 0x3D response.
  WAIT_CHALLENGE_RESPONSE,  ///< Timer running; waiting for device's HMAC proof (0x3D).
  VERIFIED,                 ///< Device successfully authenticated; command is trusted.
  FAILED,                   ///< Authentication failed (timeout or HMAC mismatch).
};

/// @brief Context carried across one outbound authenticated exchange.
struct OutboundExchangeContext {
  OutboundExchangeState state{OutboundExchangeState::IDLE};  ///< Current state machine state.
  uint8_t try_index{0};           ///< Current retry attempt (1‑based within EXCHANGE_RETRY_COUNT).
  bool saw_challenge{false};      ///< True if a 0x3C challenge was received during this exchange.
  uint32_t exchange_start_ms{0};  ///< Timestamp when the exchange attempt began (millis).
  uint32_t wait_ms{0};            ///< Current timeout window for the active wait (ms).
  uint32_t first_response_ms{0};  ///< Timestamp when the first valid response arrived (for RTT/timing).
  IoFrame rx{};                   ///< Most recent candidate frame received during the exchange.
};

/// @brief Context for a single inbound authentication (device‑initiated command).
struct InboundAuthContext {
  InboundAuthState state{InboundAuthState::IDLE};  ///< Current authentication state.
  IoFrame challenge{};  ///< The 0x3C challenge frame we sent (needed to verify 0x3D response).
};

}  // namespace exchange

}  // namespace home_io_control
}  // namespace esphome