#pragma once

/// @file hub_decisions.h
/// @brief Pure transition helpers for hub-owned exchange and pairing frame decisions.
///
/// This header contains inline, testable decision logic: frame classification
/// for exchange and pairing flows, plus shared timing utilities. No state, no
/// side effects — suitable for unit testing without radio hardware.

#include "proto_frame.h"

#include <cstdint>
#include <cstring>

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

// == Pairing discovery & key-challenge classification ==

/// Decide if a frame is a valid discovery response (0x29) during pairing.
///
/// @param candidate Parsed IoFrame.
/// @return ACCEPT if command is CMD_DISCOVER_RESP; INVALID otherwise.
inline PairingDiscoveryDisposition classify_pairing_discovery_response(const IoFrame &candidate) {
  return candidate.cmd == CMD_DISCOVER_RESP ? PairingDiscoveryDisposition::ACCEPT
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