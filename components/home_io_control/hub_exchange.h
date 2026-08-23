#pragma once

/// @file hub_exchange.h
/// @brief Internal exchange-state model for hub-owned authenticated non‑pairing flows.
/// @ingroup hioc_hub
///
/// This module defines the progress-stage enums and context structures used for
/// outbound authenticated exchanges (controller → device) and inbound authentication
/// (device → controller). These are the building blocks that power commands like
/// set_position, request_status, and handling unsolicited status‑update frames.
///
/// Note on the enums: ExchangeEngine's blocking helpers drive control flow through
/// the decisions:: classifiers; the state enums below are written at each step but
/// only read back by the exchange debug snapshot, so log lines can name the stage
/// an exchange reached before failing.
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
///
/// This header also defines the shared listen primitive's types (ListenPolicy,
/// ReplyDisposition, ListenOutcome, ReplyHandler, ListenSpec) — the single channel-policy-aware
/// wait loop both ExchangeEngine's outbound waits and PairingEngine's waits are built on. See
/// ExchangeEngine::listen() (exchange_engine.h) for the loop itself.

#include "proto_frame.h"
#include "radio_interface.h"
#include <cstdint>
#include <functional>
#include <string>

namespace esphome {
namespace home_io_control {

namespace exchange {

/// @brief Progress stages of an outbound authenticated exchange (non‑pairing).
///
/// Recorded for the exchange debug snapshot; never branched on. See the file
/// header note for why this is a stage marker rather than a driving state machine.
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

/// @brief Progress stages of inbound authentication (device‑initiated commands).
///
/// Recorded for the exchange debug snapshot; never branched on. See the file
/// header note for why this is a stage marker rather than a driving state machine.
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

/// @brief Which channels a listen covers.
///
/// A property of what is being waited for, not of the radio: a unicast exchange is a conversation
/// pinned to the channel the request went out on, so its reply always lands there too and there is
/// nothing to hop for. A broadcast has no single recipient, though — every device that answers is
/// continuing its own independent channel-hopping rather than joining a pinned conversation, so the
/// channel it happens to be on when it replies is effectively decoupled from whichever channel
/// carried the request. In practice a broadcast reply essentially never lands back on the
/// requesting channel, so dwelling there wastes part of the listen window. Shared by
/// @ref ExchangeEngine::listen() and, prospectively, any other caller that waits for a radio reply
/// — hence living beside the primitive rather than folded into one loop's local logic.
enum class ListenPolicy : uint8_t {
  HOLD_REQUEST_CHANNEL,     ///< Never retunes, never slices. Unicast replies.
  ROTATE_ALL_CHANNELS,      ///< CH1->CH2->CH3->CH1. Broadcast whose reply channel is unknown.
  ROTATE_SKIPPING_REQUEST,  ///< The two channels that are not the request channel. Roll-call.
};

/// @brief What the caller wants done with the frame a listen just received.
enum class ReplyDisposition : uint8_t {
  ACCEPT,  ///< This is the frame the caller was waiting for — stop listening, return ACCEPTED.
  IGNORE,  ///< Unparsable / not ours / wrong exchange — keep listening.
  ABORT,   ///< An explicit refusal (e.g. CMD_ERROR_RESP) — stop listening, return ABORTED.
};

/// @brief How one call to @ref ExchangeEngine::listen() ended.
enum class ListenOutcome : uint8_t {
  ACCEPTED,   ///< The handler returned ReplyDisposition::ACCEPT for some received frame.
  ABORTED,    ///< The handler returned ReplyDisposition::ABORT for some received frame.
  TIMED_OUT,  ///< `spec.window_ms` elapsed with no ACCEPT/ABORT.
};

/// @brief Invoked for every packet the radio delivers during a listen, before the listen decides
/// whether to keep waiting.
///
/// @param parsed Points at the caller's own output frame, already filled by `parse()`; nullptr
///   when `parse()` rejected the packet (the exchange waits log that case, the pairing waits do
///   not).
/// @param packet The raw packet, for length/frequency logging.
/// @return What the listen should do next — see @ref ReplyDisposition.
///
/// Keep captures to a few pointers: small callables avoid `std::function`'s heap fallback on the
/// implementations this project builds against.
using ReplyHandler = std::function<ReplyDisposition(const IoFrame *parsed, const RadioRxPacket &packet)>;

/// @brief How one listen window is to be spent — everything @ref ExchangeEngine::listen() needs;
/// everything else is the handler's business.
struct ListenSpec {
  uint32_t window_ms{0};                                    ///< Total time budget for this listen, in milliseconds.
  ListenPolicy policy{ListenPolicy::HOLD_REQUEST_CHANNEL};  ///< Which channels this listen covers.
  /// Channel the request went out on (Hz). Required by ListenPolicy::ROTATE_SKIPPING_REQUEST,
  /// where it names the channel to leave before the first listen; ignored by the other policies.
  uint32_t request_freq{0};
  /// Per-channel dwell for the ROTATE_* policies, in milliseconds; ignored by
  /// ListenPolicy::HOLD_REQUEST_CHANNEL. **0 (the default) means "ask the driver"** —
  /// `listen()` falls back to `radio->hop_dwell_ms(tuning)`, the chip's own answer to "how long
  /// must this radio sit on a channel before it can hear anything at all". Every ROTATE_* call
  /// site today leaves this at 0: discovery and the broadcast roll-call have no measured reason to
  /// dwell differently from each other. A non-zero value is reserved for the rare case where one
  /// listen *does* have a measured reason to dwell differently from the others on every chip — a
  /// difference that is per-chip and per-loop, not just per-chip, is the only thing that justifies
  /// it; that measurement doesn't exist for any call site today. Not a tuning knob either way: a
  /// constant at the call site, never user-configurable.
  uint32_t dwell_ms{0};
  /// Hop after a frame the handler ignored. Only discovery sets this true (the default): a
  /// broadcast discovery window is full of unrelated traffic and the reply channel is unknown, so
  /// an ignored frame there is no reason to keep spending the window on this channel. The
  /// roll-call is the one rotating listen that sets this false — a reception is positive evidence
  /// the other way: it proves responders are on this channel, and replies arrive spread across the
  /// whole window, so staying put costs nothing.
  bool hop_after_ignored_frame{true};
  /// Extend the listen instead of hopping while the chip reports a preamble or sync word, so an
  /// arriving frame is not cut off mid-reception. Set by both rotating listens (pairing discovery
  /// and the broadcast roll-call): every rotating dwell is short relative to one frame's air time,
  /// so without this guard a reply would routinely get cut off mid-retune.
  bool linger_on_preamble{false};
  /// Length of the preamble/sync extension, in milliseconds. Sized to one frame's air time rather
  /// than to a hop slice, so it is the same on every chip regardless of how long that chip dwells
  /// per channel.
  uint32_t linger_dwell_ms{0};
  /// Called on every hop, for callers that count hops in their own telemetry (pairing does; the
  /// exchange loops do not). Empty by default, in which case no callback fires.
  std::function<void()> on_hop;
};

}  // namespace home_io_control
}  // namespace esphome