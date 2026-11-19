#pragma once

/// @file decisions_exchange.h
/// @brief Pure classification of received frames inside an authenticated exchange: reply dispositions, device event and
/// subscription answers, repeat-safety of requests.
/// @ingroup hioc_hub
///
/// Inline, stateless helpers, testable without radio hardware or timing.

#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_frame.h"
#include "proto_timing.h"

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

/// @brief Disposition for a frame heard while waiting for the answer to our 0x3C challenge.
enum class ChallengeAnswerDisposition : uint8_t {
  IGNORE_UNRELATED,  ///< Not the challenged device's answer to us — keep listening.
  ACCEPT,            ///< The challenged device's 0x3D — verify it.
};

// == Passive RX filtering ==

/// Returns true for commands that are internal to an exchange handshake and carry
/// no useful information for a passive observer (challenge request/response).
/// These frames appear in every authenticated exchange between other controllers and
/// devices on the network, but contain only ephemeral cryptographic data.
inline bool is_exchange_internal_command(uint8_t cmd) { return cmd == CMD_CHALLENGE_REQ || cmd == CMD_CHALLENGE_RESP; }

/// Returns true for a frame that reports a device event rather than a status: a CMD_SET_CONFIG1_RESP,
/// or a well-formed CMD_STATUS_UPDATE whose event is not EVENT_STATUS_CHANGE (a subscribed sensor
/// report, see decode_status_update_event()). Such a frame is recorded on the "Last Device Event"
/// sensor and never decoded as a position, whether it arrives on its own or inside an exchange.
inline bool is_device_event_report(const IoFrame &frame) {
  if (frame.cmd == CMD_SET_CONFIG1_RESP)
    return true;
  StatusUpdateEvent event;
  return decode_status_update_event(frame, event) && event.event_id != EVENT_STATUS_CHANGE;
}

/// Decide whether a received frame goes on the "Last Device Event" sensor. The one place that
/// decides: a device event report (is_device_event_report()) from a registered device always
/// qualifies; any other frame from one only when it reached the unhandled-command fallback with
/// `diagnostic_probes` on, so a report of an unexpected shape is not lost.
/// @param frame The received frame.
/// @param source_is_registered Whether the frame's source is a registered device.
/// @param reached_unhandled_fallback True from the fallback, after every other handler declined.
/// @param probes_enabled Whether `diagnostic_probes` is on.
inline bool should_record_device_event(const IoFrame &frame, bool source_is_registered, bool reached_unhandled_fallback,
                                       bool probes_enabled) {
  if (!source_is_registered)
    return false;
  return is_device_event_report(frame) || (reached_unhandled_fallback && probes_enabled);
}

/// Returns true when @p response is the answer to the subscription @p request: the request is a
/// CMD_SUBSCRIBE_EVENT with the subscribe action, and the response a well-formed CMD_STATUS_UPDATE
/// carrying the same event, from the device the request went to. A device answers a subscription
/// with the event's current value (tests/corpus/captures/probe/somfy_rs100_probe_subscribe_event_*),
/// so inside the exchange the hub started for that request this event report is the reply; every
/// other event report is still recorded and never taken as a reply (is_device_event_report()).
inline bool is_subscription_answer(const IoFrame &request, const IoFrame &response) {
  if (request.cmd != CMD_SUBSCRIBE_EVENT || request.data_len != SUBSCRIBE_EVENT_DATA_SIZE ||
      request.data[0] != EVENT_SUBSCRIPTION_SUBSCRIBE)
    return false;
  if (memcmp(response.src, request.dst, NODE_ID_SIZE) != 0)
    return false;
  StatusUpdateEvent event;
  if (!decode_status_update_event(response, event))
    return false;
  const auto requested = read_u16_be(request.data + 1);
  return event.event_id == requested;
}

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

/// Decide if a frame heard after our 0x3C challenge is the challenged device's answer: a 0x3D with a
/// full HMAC, sent by `challenged_node` to `own_node`. Anything else (a frame from a neighbouring
/// installation, the device's own repeated report, a 0x3D for someone else) is not the answer, and
/// the wait keeps listening. Whether the answer verifies is a separate question for the caller.
///
/// @param frame            Parsed frame heard during the challenge wait.
/// @param challenged_node  The device whose frame we challenged.
/// @param own_node         This hub's node id.
/// @return ACCEPT for the challenged device's complete 0x3D; IGNORE_UNRELATED otherwise.
inline ChallengeAnswerDisposition classify_challenge_answer(const IoFrame &frame,
                                                            const uint8_t challenged_node[NODE_ID_SIZE],
                                                            const uint8_t own_node[NODE_ID_SIZE]) {
  const bool answer = frame.cmd == CMD_CHALLENGE_RESP && frame.data_len >= HMAC_SIZE &&
                      frame_matches_nodes(frame, challenged_node, own_node);
  return answer ? ChallengeAnswerDisposition::ACCEPT : ChallengeAnswerDisposition::IGNORE_UNRELATED;
}

// == Broadcast roll-call replies ==

/// Decide if a parsed frame is a reply to a broadcast request we sent: the expected command, addressed
/// to this controller. The source is not compared, because a broadcast reaches every device in range
/// and any of them may answer; which devices answered is what the caller collects.
///
/// @param frame        Parsed frame heard during the collection window.
/// @param expected_cmd Command byte the broadcast's replies carry.
/// @param own_node     This hub's node id.
/// @return true for a reply to be handed to the caller; false for any other frame.
inline bool is_broadcast_reply_for(const IoFrame &frame, uint8_t expected_cmd, const uint8_t own_node[NODE_ID_SIZE]) {
  return frame.cmd == expected_cmd && std::memcmp(frame.dst, own_node, NODE_ID_SIZE) == 0;
}

// == Idle receive ==

/// Decide whether the hub asks ESPHome to run its loop continuously. While the radio is set up and has
/// not latched a failure the hub is a passive receiver whose idle channel hop is checked once per loop
/// pass, so a pass every 16 to 30 ms would leave the receiver on one channel for that long; a
/// continuous loop keeps the hop on its 2.7 ms timer. A radio that never came up, or has failed, has
/// nothing to scan, so the loop goes back to its normal pace.
///
/// @param radio_ready  The radio driver was constructed and initialised.
/// @param radio_failed The driver latched a failure after setup.
/// @return true to hold the continuous-loop request; false to release it.
constexpr bool wants_continuous_loop(bool radio_ready, bool radio_failed) { return radio_ready && !radio_failed; }

}  // namespace decisions
}  // namespace home_io_control
}  // namespace esphome
