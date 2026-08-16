#pragma once

/// @file hub_decisions.h
/// @brief Pure transition helpers for hub-owned exchange and pairing frame decisions.
/// @ingroup hioc_hub
///
/// This header contains inline, testable decision logic: frame classification
/// for exchange and pairing flows, plus shared timing utilities. No state, no
/// side effects — suitable for unit testing without radio hardware.

#include "proto_constants.h"
#include "proto_frame.h"
#include "proto_timing.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace esphome {
namespace home_io_control {
namespace decisions {

/// @brief Disposition for the first response in an authenticated exchange.
enum class ExchangeFirstResponseDisposition : uint8_t {
  IGNORE_UNRELATED,  ///< Frame doesn't match endpoints or failed parse — keep waiting.
  COMPLETE_DIRECT,   ///< Matching non-challenge frame — operation complete, no auth needed.
  REQUIRE_AUTH,      ///< Matching 0x3C challenge — device demands authentication.
};

/// @brief Disposition for the final response after authentication.
enum class ExchangeFinalResponseDisposition : uint8_t {
  IGNORE_UNRELATED,  ///< Frame doesn't match endpoints — ignore.
  ACCEPT,            ///< Frame matches expected response — exchange succeeds.
};

/// @brief Disposition during pairing discovery phase.
enum class PairingDiscoveryDisposition : uint8_t {
  NO_RESPONSE,  ///< No packets received on the channel within timeout.
  INVALID,      ///< Packets seen but none were valid discovery (0x29) frames.
  ACCEPT,       ///< Valid discovery response received.
};

/// @brief Disposition during pairing key-challenge phase.
enum class PairingKeyChallengeDisposition : uint8_t {
  IGNORE,  ///< Not a valid challenge (wrong cmd, length, or sender).
  ACCEPT,  ///< Valid 0x3C challenge from target device.
};

// == Passive RX filtering ==

/// Returns true for commands that are internal to an exchange handshake and carry
/// no useful information for a passive observer (challenge request/response).
/// These frames appear in every authenticated exchange between other controllers and
/// devices on the network, but contain only ephemeral cryptographic data.
inline bool is_exchange_internal_command(uint8_t cmd) { return cmd == CMD_CHALLENGE_REQ || cmd == CMD_CHALLENGE_RESP; }

// == Utility: endpoint matching ==

/// Check if two frames have identical src/dst node IDs.
inline bool frame_matches_nodes(const IoFrame &frame, const uint8_t expected_src[NODE_ID_SIZE],
                                const uint8_t expected_dst[NODE_ID_SIZE]) {
  return std::memcmp(frame.src, expected_src, NODE_ID_SIZE) == 0 &&
         std::memcmp(frame.dst, expected_dst, NODE_ID_SIZE) == 0;
}

/// Check if candidate frame endpoints are the reverse of the request (dst==request.src, src==request.dst).
inline bool frame_matches_exchange_endpoints(const IoFrame &request, const IoFrame &candidate) {
  return frame_matches_nodes(candidate, request.dst, request.src);
}

// == Exchange first-response classification ==

/// Decide how to handle the first response packet in an authenticated exchange.
///
/// Used by wait_for_first_response_() to determine whether the exchange:
/// - completes immediately (direct response),
/// - requires authentication (challenge received), or
/// - should ignore the frame and keep waiting.
///
/// @param request  Original outbound request frame.
/// @param candidate Parsed IoFrame from the device.
/// @return Disposition indicating next step.
inline ExchangeFirstResponseDisposition classify_exchange_first_response(const IoFrame &request,
                                                                         const IoFrame &candidate) {
  if (!frame_matches_exchange_endpoints(request, candidate))
    return ExchangeFirstResponseDisposition::IGNORE_UNRELATED;
  // A matching non-0x3C frame is the entire answer for direct-response exchanges such as plain
  // status reads, so the caller must not force it through the authenticated path.
  if (candidate.cmd == CMD_CHALLENGE_REQ)
    return ExchangeFirstResponseDisposition::REQUIRE_AUTH;
  return ExchangeFirstResponseDisposition::COMPLETE_DIRECT;
}

/// Decide if a candidate frame is an acceptable final response after authentication.
///
/// Only endpoint matching is checked here; command validity is encoded in the
/// disposition mapping by the caller.
///
/// @param request  Original outbound request frame.
/// @param candidate Parsed IoFrame from the device.
/// @return ACCEPT if endpoints match; IGNORE_UNRELATED otherwise.
inline ExchangeFinalResponseDisposition classify_exchange_final_response(const IoFrame &request,
                                                                         const IoFrame &candidate) {
  return frame_matches_exchange_endpoints(request, candidate) ? ExchangeFinalResponseDisposition::ACCEPT
                                                              : ExchangeFinalResponseDisposition::IGNORE_UNRELATED;
}

/// Whether an authenticated-but-unanswered request may be sent again.
///
/// CMD_EXECUTE is the only request the hub sends that moves something, so a retry there is a
/// second side effect on a device already acting on the first copy. Every other request (status
/// polls, name reads, management actions, config writes) is idempotent and keeps its full retry
/// budget when the device authenticates but never closes the exchange.
/// @param cmd Command byte of the outbound request.
/// @return true when the remaining retries should still be spent.
[[nodiscard]] inline bool retry_after_unconfirmed_accept_is_safe(uint8_t cmd) { return cmd != CMD_EXECUTE; }

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

// == One-way (1W) remote frame handling ==

/// @brief Key fields of the last processed 1W frame, used to collapse a remote's repeat burst.
///
/// 1W remotes repeat each command 4× at ~40ms intervals for reliability, and a held button keeps
/// resending, so one logical press arrives as many identical frames. The key deliberately includes
/// the decoded intent bytes and not just the command byte: a move and a stop are *both*
/// CMD_EXECUTE and differ only in `main0`, so a command-only key silently discards a stop that
/// follows a move within the window — losing the sender event, the optimistic-target clear, and
/// the immediate poll that a stop is supposed to trigger.
struct OneWayDedupState {
  std::string src_id;      ///< Source node ID of the last processed frame; empty before the first.
  uint8_t cmd{0};          ///< Command byte.
  bool has_intent{false};  ///< Whether main0/main1 were decoded (execute / activate-mode only).
  uint8_t main0{0};        ///< First main byte — what distinguishes a move from a stop.
  uint8_t main1{0};        ///< Second main byte.
  uint32_t timestamp{0};   ///< millis() when the frame was processed.
};

/// Decide whether an incoming 1W frame repeats the previous one inside the burst window.
///
/// @param last     State recorded for the previously processed 1W frame.
/// @param incoming Candidate frame's key fields, with `timestamp` set to now.
/// @param window_ms Burst-suppression window.
/// @return true if the frame should be dropped as a repeat of `last`.
inline bool is_duplicate_1w_frame(const OneWayDedupState &last, const OneWayDedupState &incoming, uint32_t window_ms) {
  // `last.src_id` is empty until the first 1W frame is processed, so a real frame never matches it.
  if (last.src_id != incoming.src_id || last.cmd != incoming.cmd || last.has_intent != incoming.has_intent)
    return false;
  if (incoming.has_intent && (last.main0 != incoming.main0 || last.main1 != incoming.main1))
    return false;
  // Unsigned arithmetic makes this correct across the millis() wrap.
  return (incoming.timestamp - last.timestamp) < window_ms;
}

/// Decide whether to hold back a queued background poll because a 1W remote is still transmitting.
///
/// The radio is half-duplex and an authenticated exchange blocks for 1–3 s, during which no frame
/// can be received at all. A press on a linked remote schedules a status poll, so without this gate
/// the hub's own poll can start on top of the burst that triggered it and go deaf to the rest of it.
///
/// Only background polls are deferred. A user command must never wait on a remote the user may not
/// even own — 1W broadcasts carry no ownership marker, so the activity could be a neighbour's.
///
/// The hold re-arms on every 1W frame received while it is already active, so a real burst from one
/// remote (~160 ms, well under `quiet_ms`) never gets cut short mid-transmission. Left unchecked
/// that re-arming has no cap: sustained sub-`quiet_ms` 1W traffic from any source — including a
/// neighbour's, since these broadcasts carry no ownership marker — would hold background polls back
/// indefinitely. @p max_defer_ms bounds that: once that much time has passed since the burst
/// *started* (not the most recent frame), the poll is let through regardless of ongoing traffic.
/// The gate only ever delays a poll, never drops one — it stays queued and fires as soon as it is
/// no longer deferred.
///
/// @param next_op_is_background True if the queue front is a REQUEST_STATUS / REQUEST_NAME.
/// @param first_1w_activity_ms  millis() of the first frame in the current 1W burst; 0 if none seen
///                               since boot.
/// @param last_1w_activity_ms   millis() of the most recent 1W frame; 0 if none seen since boot.
/// @param now                   Current millis().
/// @param quiet_ms              How long after 1W activity to hold background polls back.
/// @param max_defer_ms          Hard cap on total defer time, measured from first_1w_activity_ms.
/// @return true if the caller should skip dispatching this loop iteration.
inline bool defer_background_poll_for_1w_activity(bool next_op_is_background, uint32_t first_1w_activity_ms,
                                                  uint32_t last_1w_activity_ms, uint32_t now, uint32_t quiet_ms,
                                                  uint32_t max_defer_ms) {
  if (!next_op_is_background || last_1w_activity_ms == 0)
    return false;
  if (now - first_1w_activity_ms >= max_defer_ms)
    return false;
  return (now - last_1w_activity_ms) < quiet_ms;
}

/// Whether a 1W frame arriving at `now` starts a new burst rather than extending the current one
/// — true if no frame has been seen yet, or the gap since the last one reached `quiet_ms` (the
/// previous burst already released any deferred poll). Callers use this to decide whether to
/// reset a burst's start-time tracking; see defer_background_poll_for_1w_activity() for why the
/// burst start (not just the latest frame) needs its own timestamp.
///
/// @param last_1w_activity_ms millis() of the most recent 1W frame before this one; 0 if none
///                             seen since boot.
/// @param now                 Current millis() (this frame's arrival time).
/// @param quiet_ms            Gap after which a previous burst is considered over.
[[nodiscard]] inline bool oneway_burst_started_fresh(uint32_t last_1w_activity_ms, uint32_t now, uint32_t quiet_ms) {
  return last_1w_activity_ms == 0 || (now - last_1w_activity_ms) >= quiet_ms;
}

// == Timing/slicing helper ==

/// Slice remaining wait time into bounded intervals to allow frequency hopping.
///
/// The wait loops (exchange and pairing) use this to avoid blocking the radio
/// for too long without hopping. Each slice is at most RESPONSE_CHANNEL_WAIT_MS.
///
/// @param remaining_ms Total time left in the wait window.
/// @return Time slice to wait in milliseconds.
inline uint32_t response_wait_slice_ms(uint32_t remaining_ms) {
  return std::min<uint32_t>(remaining_ms, RESPONSE_CHANNEL_WAIT_MS);
}

}  // namespace decisions
}  // namespace home_io_control
}  // namespace esphome