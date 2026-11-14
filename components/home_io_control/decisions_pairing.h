#pragma once

/// @file decisions_pairing.h
/// @brief Pure classification of received frames during pairing: discovery, key challenge, key transfer confirm, and
/// the device behind a pairing node.
/// @ingroup hioc_hub
///
/// Inline, stateless helpers, testable without radio hardware or timing.

#include "decisions_exchange.h"
#include "proto_constants.h"
#include "proto_device_types.h"
#include "proto_frame.h"

#include <cstdint>
#include <cstring>
#include <optional>

namespace esphome {
namespace home_io_control {
namespace decisions {

/// @brief Disposition during pairing discovery phase.
enum class PairingDiscoveryDisposition : uint8_t {
  NO_RESPONSE,  ///< No packets received on the channel within timeout.
  INVALID,      ///< Packets seen but none were valid discovery (0x29) frames.
  ACCEPT,       ///< Valid discovery response received.
};

/// @brief The channel a discovery request goes out on.
///
/// The default sends every attempt on CH2, the 2W transmit channel. With @p rotate_channels the
/// attempts of one discovery command take CH2, CH1 and CH3 in turn and start over after the third, so
/// a device that hears the search on one channel only is reached within three attempts.
/// @param rotate_channels True to rotate the channels (TuningConfig::pairing_discovery_send_channels
///        `all`); false to stay on CH2.
/// @param attempt Attempt number of the command, counted from 1.
/// @return The request frequency in Hz.
[[nodiscard]] inline uint32_t discovery_request_freq(bool rotate_channels, uint8_t attempt) {
  static constexpr uint32_t ROTATION[] = {FREQ_CH2, FREQ_CH1, FREQ_CH3};
  if (!rotate_channels || attempt == 0)
    return FREQ_CH2;
  return ROTATION[(attempt - 1U) % (sizeof(ROTATION) / sizeof(ROTATION[0]))];
}

/// @brief Disposition during pairing key-challenge phase.
enum class PairingKeyChallengeDisposition : uint8_t {
  IGNORE,  ///< Not a valid challenge (wrong cmd, length, or sender).
  ACCEPT,  ///< Valid 0x3C challenge from target device.
};

/// @brief Disposition for a candidate reply to a discovery-confirm request (0x2C).
enum class PairingDiscoverConfirmDisposition : uint8_t {
  IGNORE,  ///< Not from/to the expected endpoints, or a frame the step does not act on (e.g. a
           ///< repeated 0x29) — keep waiting.
  ACK,     ///< CMD_DISCOVER_CONFIRM_ACK (0x2D) from the device — it wants to proceed.
  ERROR,   ///< CMD_ERROR_RESP from the device — an explicit, named refusal.
};

/// @brief Disposition for a candidate reply to the key-transfer (0x32) confirm wait — the
/// slow-turnaround path's expanded classification of @ref PairingKeyChallengeDisposition's device
/// challenge to also cover a direct confirm or an explicit refusal.
enum class PairingKeyConfirmDisposition : uint8_t {
  IGNORE,     ///< Not from/to the expected endpoints — keep waiting.
  CONFIRM,    ///< CMD_KEY_CONFIRM (0x33) — the device accepted the key.
  CHALLENGE,  ///< A fresh CMD_CHALLENGE_REQ (0x3C) — the device may challenge the key transfer
              ///< before confirming it; answered via ExchangeEngine::answer_challenge().
  REFUSE,     ///< Anything else (including CMD_ERROR_RESP) — an explicit or implicit refusal;
              ///< ends the whole wait, not just the current try.
};

// == Pairing discovery & key-challenge classification ==

/// Decide if a frame is a valid discovery response (0x29) during pairing.
///
/// Only the destination is checked, not the source: the discovery request goes out to a
/// shared broadcast address, so a response arriving during the same window may be a device
/// answering a *different* controller's concurrent discovery rather than ours — real hardware
/// addresses its response back to the requesting controller's own node ID, so checking that is
/// both possible and sufficient to reject it. The source can't be checked here — the device's
/// node ID is exactly what discovery exists to learn, so there is nothing yet to compare it to.
///
/// @param candidate      Parsed IoFrame.
/// @param controller_id  Node ID of this controller (expected destination).
/// @return ACCEPT if the command is CMD_DISCOVER_RESP and addressed to this controller; INVALID otherwise.
inline PairingDiscoveryDisposition classify_pairing_discovery_response(const IoFrame &candidate,
                                                                       const uint8_t controller_id[NODE_ID_SIZE]) {
  return candidate.cmd == CMD_DISCOVER_RESP && std::memcmp(candidate.dst, controller_id, NODE_ID_SIZE) == 0
             ? PairingDiscoveryDisposition::ACCEPT
             : PairingDiscoveryDisposition::INVALID;
}

/// Decide if a frame is a valid key-challenge (0x3C) during pairing key exchange.
///
/// The challenge must:
///   - be CMD_CHALLENGE_REQ,
///   - have data_len == HMAC_SIZE (6),
///   - originate from the discovered device node ID,
///   - be addressed to this controller's node ID.
///
/// @param candidate      Parsed IoFrame.
/// @param device_id      Node ID of the device being paired (expected sender).
/// @param controller_id  Node ID of this controller (expected destination).
/// @return ACCEPT if all criteria met; IGNORE otherwise.
inline PairingKeyChallengeDisposition classify_pairing_key_challenge(const IoFrame &candidate,
                                                                     const uint8_t device_id[NODE_ID_SIZE],
                                                                     const uint8_t controller_id[NODE_ID_SIZE]) {
  // Pairing reuses the normal 0x3C primitive, but here the challenge is only valid when it comes
  // from the device we just discovered and targets this controller. That keeps foreign traffic from
  // contaminating key exchange on a busy channel.
  return candidate.cmd == CMD_CHALLENGE_REQ && candidate.data_len == HMAC_SIZE &&
                 frame_matches_nodes(candidate, device_id, controller_id)
             ? PairingKeyChallengeDisposition::ACCEPT
             : PairingKeyChallengeDisposition::IGNORE;
}

/// Decide how to handle a candidate reply to a discovery-confirm request (0x2C) during pairing.
///
/// A non-matching or unrecognised frame is IGNORE, keeping the listen open rather than ending it:
/// the discover-confirm step never fails a pairing attempt (see run_discover_confirm_step_()'s
/// doc) — this classification only decides whether to keep listening or stop early.
///
/// @param candidate      Parsed IoFrame.
/// @param device_id      Node ID of the device being paired (expected sender).
/// @param controller_id  Node ID of this controller (expected destination).
/// @return ACK for a matching 0x2D, ERROR for a matching CMD_ERROR_RESP, IGNORE otherwise.
inline PairingDiscoverConfirmDisposition classify_pairing_discover_confirm_reply(
    const IoFrame &candidate, const uint8_t device_id[NODE_ID_SIZE], const uint8_t controller_id[NODE_ID_SIZE]) {
  if (!frame_matches_nodes(candidate, device_id, controller_id))
    return PairingDiscoverConfirmDisposition::IGNORE;
  if (candidate.cmd == CMD_DISCOVER_CONFIRM_ACK)
    return PairingDiscoverConfirmDisposition::ACK;
  if (candidate.cmd == CMD_ERROR_RESP)
    return PairingDiscoverConfirmDisposition::ERROR;
  // E.g. a repeated CMD_DISCOVER_RESP (0x29) from a device that missed our 0x2C and is still
  // announcing itself — not an answer to this step, but not a foreign frame either.
  return PairingDiscoverConfirmDisposition::IGNORE;
}

/// Decide how to handle a candidate reply to the key-transfer (0x32) confirm wait
/// (wait_for_key_confirm_(), slow-turnaround chips only).
///
/// @param request   The outbound CMD_KEY_TRANSFER (0x32) this reply answers.
/// @param candidate Parsed IoFrame from the device.
/// @return CONFIRM/CHALLENGE/REFUSE for a matching frame of that shape; IGNORE for a frame from
///         the wrong endpoints.
inline PairingKeyConfirmDisposition classify_pairing_key_confirm_reply(const IoFrame &request,
                                                                       const IoFrame &candidate) {
  if (!frame_matches_exchange_endpoints(request, candidate))
    return PairingKeyConfirmDisposition::IGNORE;
  if (candidate.cmd == CMD_KEY_CONFIRM)
    return PairingKeyConfirmDisposition::CONFIRM;
  if (classify_pairing_key_challenge(candidate, request.dst, request.src) == PairingKeyChallengeDisposition::ACCEPT)
    return PairingKeyConfirmDisposition::CHALLENGE;
  return PairingKeyConfirmDisposition::REFUSE;
}

/// @brief Whether discover-confirm try `try_index` (1-based) should rotate channels rather than
/// hold the request channel.
///
/// Tries 1 and 3 hold: a 0x2D is a unicast reply to a unicast 0x2C, and every other unicast
/// pairing wait in this project holds the request channel on that same expectation (see
/// listen_for_key_confirm_()'s own reasoning), and every 0x2D a Somfy Izymo dimmer sent came back on
/// the request channel. But the one real VELUX-system sample on record (from a hopping monitor, a
/// weak source) logged a 0x2D on a *different* channel than its own 0x2C, so the middle try hedges
/// by rotating instead, until a VELUX device's reply channel is measured.
/// See ADR 0039 for why this is not simply one fixed policy for every try, unlike every other
/// listen in this project (ADR 0028).
/// @param try_index 1-based try number (1..PAIRING_DISCOVER_CONFIRM_TRIES).
/// @return true if this try should use ListenPolicy::ROTATE_ALL_CHANNELS; false for
///         ListenPolicy::HOLD_REQUEST_CHANNEL.
[[nodiscard]] inline bool discover_confirm_try_rotates(uint8_t try_index) { return try_index == 2; }

// == Key-exchange step classification ==

/// @brief Disposition for a frame heard while waiting for the key a device hands over in a key pull.
enum class PairingKeyPullReplyDisposition : uint8_t {
  IGNORE,  ///< Not from the device to us: not part of this exchange, not even worth recording.
  KEY,     ///< The device's CMD_KEY_TRANSFER (0x32) with a full key: take it.
  REFUSE,  ///< CMD_ERROR_RESP from the device: an explicit refusal, ends the wait.
  REJECT,  ///< From the device to us but not a key (e.g. a short 0x32 or another command): recorded
           ///< as rejected, the wait keeps listening.
};

/// Decide how to handle a frame heard after a key pull (0x38) request.
///
/// @param frame         Parsed frame heard during the wait.
/// @param device_id     Node id of the device being paired (expected sender).
/// @param controller_id Node id of this controller (expected destination).
/// @return KEY for a CMD_KEY_TRANSFER carrying at least AES_KEY_SIZE bytes, REFUSE for CMD_ERROR_RESP,
///         REJECT for any other frame between the two nodes, IGNORE for a frame between other nodes.
inline PairingKeyPullReplyDisposition classify_key_pull_reply(const IoFrame &frame,
                                                              const uint8_t device_id[NODE_ID_SIZE],
                                                              const uint8_t controller_id[NODE_ID_SIZE]) {
  if (!frame_matches_nodes(frame, device_id, controller_id))
    return PairingKeyPullReplyDisposition::IGNORE;
  if (frame.cmd == CMD_KEY_TRANSFER && frame.data_len >= AES_KEY_SIZE)
    return PairingKeyPullReplyDisposition::KEY;
  if (frame.cmd == CMD_ERROR_RESP)
    return PairingKeyPullReplyDisposition::REFUSE;
  return PairingKeyPullReplyDisposition::REJECT;
}

/// Decide if a frame is the device's direct key confirm (0x33) to this controller, sent without a
/// challenge first. Some devices skip the 0x3C and confirm at once (see
/// classify_pairing_key_challenge() for the challenge case).
///
/// @param frame         Parsed frame heard during the wait for a key challenge.
/// @param device_id     Node id of the device being paired (expected sender).
/// @param controller_id Node id of this controller (expected destination).
inline bool is_direct_key_confirm(const IoFrame &frame, const uint8_t device_id[NODE_ID_SIZE],
                                  const uint8_t controller_id[NODE_ID_SIZE]) {
  return frame.cmd == CMD_KEY_CONFIRM && frame_matches_nodes(frame, device_id, controller_id);
}

/// @brief Verdict of the authenticated name read that checks whether a device holds our key although
/// it never confirmed the key transfer.
enum class PairingKeyVerifyDisposition : uint8_t {
  HELD,      ///< The device answered the authenticated request: it holds the key.
  REFUSED,   ///< The device refused outright (CMD_ERROR_RESP with a code): it never got as far as
             ///< checking a key, so it does not act as paired.
  NOT_HELD,  ///< No usable answer: the device does not answer authenticated requests with our key.
};

/// Decide what the reply to the key-verify name read (0x50) says about the device.
///
/// @param answered       The exchange ended with a reply frame (as opposed to silence or a failure).
/// @param saw_challenge  The device challenged the request (0x3C): it asked for the key's proof, so a
///                       reply after it was authenticated.
/// @param reply          The reply frame; read only when @p answered.
/// @return HELD for a CMD_GET_NAME_RESP after a challenge, REFUSED for a CMD_ERROR_RESP that carries a
///         code, NOT_HELD otherwise.
inline PairingKeyVerifyDisposition classify_key_verify_reply(bool answered, bool saw_challenge, const IoFrame &reply) {
  if (answered && saw_challenge && reply.cmd == CMD_GET_NAME_RESP)
    return PairingKeyVerifyDisposition::HELD;
  if (answered && reply.cmd == CMD_ERROR_RESP && reply.data_len > 0)
    return PairingKeyVerifyDisposition::REFUSED;
  return PairingKeyVerifyDisposition::NOT_HELD;
}

// === Pairing: the device behind a pairing node ===

/// @brief The (type, subtype) of a device.
struct DeviceKind {
  DeviceType type;  ///< Node type.
  uint8_t subtype;  ///< Subtype.
};

/// @brief The heater a pairing node that answered discovery this way controls at the next address.
///
/// An Atlantic radiator in its display's pairing mode answers discovery as a remote controller (node
/// type 1023), hands over its key, and is controlled at the next node address up, as the electrical
/// heater with an adjustable setpoint: three AGILIA IO radiators follow this (issue #165,
/// tests/corpus/captures/pairing/atlantic_agilia_pairing_key_pull_heater_rollcall.yaml). The heater
/// answers a roll-call only now and then, so the pairing asks that address directly first.
/// @param type Node type the pairing node answered discovery with.
/// @param manufacturer Manufacturer byte of that discovery reply (0 when the reply carried none).
/// @return The heater's kind for an Atlantic remote-controller node; nothing for any other node.
inline std::optional<DeviceKind> heater_behind_pairing_node(DeviceType type, uint8_t manufacturer) {
  if (type != DeviceType::REMOTE_CONTROLLER || manufacturer != MANUFACTURER_ATLANTIC_GROUP)
    return std::nullopt;
  return DeviceKind{DeviceType::ELECTRICAL_HEATER, SUBTYPE_HEATER_WITH_SETPOINT};
}

/// @brief The node address one above `node`, as a 24-bit number (`B6BDFB` -> `B6BDFC`).
///
/// A carry into the higher bytes (`xxxxFF`) is computed as plain arithmetic; no radiator with such an
/// address has been seen, so whether the device counts the same way is unconfirmed.
/// @param node Address to count from.
/// @param[out] next The next address; untouched when `false` is returned.
/// @return false for `FFFFFF`, which has no next address.
inline bool next_node_id(const uint8_t node[NODE_ID_SIZE], uint8_t next[NODE_ID_SIZE]) {
  uint8_t up[NODE_ID_SIZE];
  memcpy(up, node, NODE_ID_SIZE);
  // Count up from the last byte; a byte that wraps to 0 carries into the one before it.
  for (size_t i = NODE_ID_SIZE; i-- > 0;) {
    if (++up[i] != 0) {
      memcpy(next, up, NODE_ID_SIZE);
      return true;
    }
  }
  return false;  // FFFFFF wrapped to 000000.
}

}  // namespace decisions
}  // namespace home_io_control
}  // namespace esphome
