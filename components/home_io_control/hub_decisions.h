#pragma once

/// @file hub_decisions.h
/// @brief Pure transition helpers for hub-owned exchange and pairing frame decisions.
/// @ingroup hioc_hub
///
/// This header contains inline, testable decision logic: frame classification
/// for exchange and pairing flows, plus shared timing utilities. No state, no
/// side effects — suitable for unit testing without radio hardware.

#include "proto_constants.h"
#include "proto_device_model.h"
#include "proto_frame.h"
#include "proto_timing.h"

#include <algorithm>
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

// == One-way (1W) remote frame handling ==

/// @brief Key fields of the last processed 1W frame, used to collapse a remote's repeat burst.
///
/// 1W remotes repeat each command 4× at ~40ms intervals for reliability, and a held button keeps
/// resending, so one logical press arrives as many identical frames. The key deliberately includes
/// the decoded intent bytes and not just the command byte: a move and a stop are *both*
/// CMD_EXECUTE and differ only in `main0`, so a command-only key silently discards a stop that
/// follows a move within the window — losing the sender event, the optimistic-target clear, and
/// the immediate poll that a stop is supposed to trigger.
///
/// For a frame with no decoded intent the destination is part of the key as well. A VELUX KLI's
/// Gear press sends its `0x2E` to several device classes a few hundred ms apart, and the classes it
/// names are the ones a new controller has to enroll on; keying on src+cmd alone would log only the
/// first of them.
/// Intent-bearing frames keep ignoring the destination: they fire sender events and optimistic
/// state, where one press must stay one press.
struct OneWayDedupState {
  std::string src_id;           ///< Source node ID of the last processed frame; empty before the first.
  uint8_t cmd{0};               ///< Command byte.
  bool has_intent{false};       ///< Whether main0/main1 were decoded (execute / activate-mode only).
  uint8_t main0{0};             ///< First main byte — what distinguishes a move from a stop.
  uint8_t main1{0};             ///< Second main byte.
  uint32_t timestamp{0};        ///< millis() when the frame was processed.
  uint8_t dst[NODE_ID_SIZE]{};  ///< Destination address; compared only when `has_intent` is false.
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
  if (!incoming.has_intent && std::memcmp(last.dst, incoming.dst, NODE_ID_SIZE) != 0)
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

/// @brief Transmit-attempt budget for a scheduler-owned status poll, by backoff-ladder position.
///
/// See SCHEDULED_POLL_MAX_TRIES and SCHEDULED_POLL_RETRY_GRACE_FIRST_FAILURE (proto_timing.h) for
/// why the full budget belongs to a middle band of the ladder rather than to its start or its tail.
///
/// The two counters are mutually exclusive by construction — StatusPollPolicy::on_exchange_failed()
/// zeroes one while incrementing the other — so an auth-shaped streak reads status_poll_failures
/// as 0 and would otherwise fall into the band's own "fresh window" case. It is rejected first,
/// deliberately, so the predicate stays correct even if that exclusivity is ever relaxed.
///
/// The settle poll after an accepted STOP (@p settles_a_stop) gets the full budget whatever the
/// counters say: see STOP_SETTLE_POLL_TRIES (proto_timing.h).
///
/// @param status_poll_failures Consecutive silent failures already recorded for this device.
/// @param auth_poll_failures   Consecutive challenge-seen failures already recorded.
/// @param settles_a_stop       True for the first poll after an accepted STOP
///                             (StatusPollPolicy::take_stop_settle()).
/// @return EXCHANGE_RETRY_COUNT inside the band or after a STOP, SCHEDULED_POLL_MAX_TRIES everywhere else.
inline uint8_t scheduled_poll_max_tries(uint8_t status_poll_failures, uint8_t auth_poll_failures,
                                        bool settles_a_stop = false) {
  if (settles_a_stop)
    return STOP_SETTLE_POLL_TRIES;
  if (auth_poll_failures != 0)
    return SCHEDULED_POLL_MAX_TRIES;
  if (status_poll_failures < SCHEDULED_POLL_RETRY_GRACE_FIRST_FAILURE ||
      status_poll_failures > SCHEDULED_POLL_RETRY_GRACE_LAST_FAILURE)
    return SCHEDULED_POLL_MAX_TRIES;
  return EXCHANGE_RETRY_COUNT;
}

// == Low-power wake belief ==

/// @brief How likely a low-power receiver is to be awake right now, judged from what this hub has
/// seen of it. Orders the tries of a directed exchange: an awake receiver hears the short start
/// preamble and ignores the long wake-up one, a sleeping one needs the long one.
enum class WakeBelief : uint8_t {
  ASLEEP,       ///< No recent sign of life — lead with the wake-up preamble.
  MAYBE_AWAKE,  ///< Recently heard from — try the short preamble first, keep the wake-up as fallback.
  AWAKE,        ///< Moving, or about to be stopped — the short preamble is the one it hears.
};

static_assert(LOW_POWER_AWAKE_HOLD_MS <= LOW_POWER_MAX_TRAVEL_MS,
              "wake_belief() relies on moving evidence outliving the maybe-awake hold");

/// @brief What the hub knows about the device an exchange is addressed to, as the exchange engine
/// sees it. The engine has no device registry of its own; the hub hands this over through
/// ExchangeEngine::set_target_evidence_provider(), and every per-target decision the engine makes
/// (today: wake_belief()) reads from it. Timestamps are `millis()` values, 0 = never.
struct TargetEvidence {
  uint32_t last_moving_evidence_ms;  ///< Last sign the receiver is travelling (see note_moving_evidence(),
                                     ///< clear_moving_evidence()).
  uint32_t last_seen_ms;             ///< Last frame received from the receiver, any command.
};

/// Build the exchange engine's view of a device record.
/// @param dev Device record to read.
[[nodiscard]] inline TargetEvidence target_evidence(const IoDevice &dev) {
  return {dev.last_moving_evidence_ms, dev.last_seen_ms};
}

/// True for a CMD_EXECUTE whose main byte is POS_STOP. A STOP is only ever sent to a receiver that
/// is (believed) moving, so it is treated as awake whatever the stamps say.
/// @param request Outbound request frame.
[[nodiscard]] inline bool is_stop_request(const IoFrame &request) {
  return request.cmd == CMD_EXECUTE && request.data_len > EXECUTE_MAIN_BYTE_OFFSET &&
         request.data[EXECUTE_MAIN_BYTE_OFFSET] == POS_STOP;
}

/// Judge how awake a low-power receiver is. `AWAKE` for a STOP request or while moving evidence is
/// younger than LOW_POWER_MAX_TRAVEL_MS; otherwise `MAYBE_AWAKE` while the newest of moving evidence
/// and last frame heard is younger than LOW_POWER_AWAKE_HOLD_MS; otherwise `ASLEEP`. A zero stamp
/// means never and is never recent. Ages use unsigned subtraction, so `millis()` wrap-around is safe.
/// @param evidence Stamps for the target device.
/// @param now      Current millis().
/// @param is_stop  True when the request being sent is a STOP (see is_stop_request()).
[[nodiscard]] inline WakeBelief wake_belief(const TargetEvidence &evidence, uint32_t now, bool is_stop) {
  const auto recent = [now](uint32_t stamp, uint32_t window_ms) { return stamp != 0 && (now - stamp) < window_ms; };
  if (is_stop || recent(evidence.last_moving_evidence_ms, LOW_POWER_MAX_TRAVEL_MS))
    return WakeBelief::AWAKE;
  // Moving evidence needs no check here: any that is younger than the hold window already returned
  // AWAKE above (the hold is the shorter window), and older evidence is not recent by either measure.
  if (recent(evidence.last_seen_ms, LOW_POWER_AWAKE_HOLD_MS))
    return WakeBelief::MAYBE_AWAKE;
  return WakeBelief::ASLEEP;
}

/// Start-preamble length for one try of a directed exchange to a low-power receiver. The plans
/// (short = `short_preamble`, LONG = LONG_PREAMBLE): AWAKE short/LONG/short, MAYBE_AWAKE
/// short/LONG/LONG, ASLEEP LONG on every try — so an exchange allowed more than one try still tries
/// the wake-up preamble at least once, and a wrong belief costs one try, not the exchange. A
/// single-try exchange (most scheduler-owned status polls) sends only try 1's preamble; its backoff
/// ladder, whose three-try slots include the wake-up preamble, covers a wrong belief there.
/// @param belief         See wake_belief().
/// @param try_index      1-based try number, clamped to [1, EXCHANGE_RETRY_COUNT].
/// @param short_preamble Preamble for a receiver known to be awake (`normal_start_preamble`).
[[nodiscard]] inline uint16_t low_power_try_preamble(WakeBelief belief, uint8_t try_index, uint16_t short_preamble) {
  const uint8_t try_1based = std::max<uint8_t>(1, std::min<uint8_t>(try_index, EXCHANGE_RETRY_COUNT));
  switch (belief) {
    case WakeBelief::AWAKE:
      return try_1based == 2 ? LONG_PREAMBLE : short_preamble;
    case WakeBelief::MAYBE_AWAKE:
      return try_1based == 1 ? short_preamble : LONG_PREAMBLE;
    case WakeBelief::ASLEEP:
    default:
      return LONG_PREAMBLE;
  }
}

/// Lowercase name of a belief for log lines.
[[nodiscard]] inline const char *wake_belief_name(WakeBelief belief) {
  switch (belief) {
    case WakeBelief::AWAKE:
      return "awake";
    case WakeBelief::MAYBE_AWAKE:
      return "maybe_awake";
    case WakeBelief::ASLEEP:
    default:
      return "asleep";
  }
}

/// True if a frame's shape matches a 1W remote's pairing gesture (issue #27/#65): CTRL0 1W bit
/// set, addressed to the 1W broadcast address (0x00003F), with one of the three command bytes
/// observed in the field capture — 0x20 (WRITE_PRIVATE), 0x39 (1W remove), or 0x2E (alternate
/// discovery, 1W-flagged). Shared between PairingAdvisor (classifying recorded telemetry events,
/// pairing_advisor.cpp) and the hub's normal passive RX path (hub_status.cpp), which remembers a
/// recent sighting so a PROG press completed just before "Discover & Pair" is pressed isn't
/// invisible to the advisor purely because of when the discovery telemetry window happened to
/// open — see PairingTelemetry::record_recent_one_way_sighting().
///
/// @param oneway CTRL0 1W-protocol bit.
/// @param dst    Frame destination node ID.
/// @param cmd    Frame command byte.
inline bool is_one_way_pairing_gesture(bool oneway, const uint8_t dst[NODE_ID_SIZE], uint8_t cmd) {
  if (!oneway)
    return false;
  if (std::memcmp(dst, BROADCAST_DISCOVER_ALT, NODE_ID_SIZE) != 0)
    return false;
  return cmd == CMD_WRITE_PRIVATE || cmd == CMD_ONEWAY_REMOVE || cmd == CMD_DISCOVER_ALT_REQ;
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

/// Namespace tag for `remote_poll_timer_id()` below: the node address occupies the low 24 bits,
/// and this is OR-ed in above them. Nothing else uses `Component::set_timeout`'s numeric-id
/// overload today, so the tag has no collision to avoid yet — it exists so the id space stays
/// self-describing (which caller a given id belongs to) the day a second numeric-id timer is added.
constexpr uint32_t REMOTE_POLL_TIMER_ID_TAG = 0x01000000;

/// @brief Numeric `set_timeout()` id for a device's remote-activity poll timer (hub_status.cpp's
/// `schedule_status_poll_()`).
///
/// Keyed by the device's node address rather than a name, because `Component::set_timeout(const
/// char *name, ...)` stores the caller's *pointer*, not a copy of the string (its header documents
/// this: static lifetime required, use the numeric-id overload for a dynamically-built name) — see
/// `schedule_status_poll_()`'s own comment for why a per-device name can't satisfy that here. A
/// node address is unique per device, so this id is collision-free by construction.
///
/// @param node_id 3-byte device node address.
/// @return Numeric timer id, unique per device.
inline uint32_t remote_poll_timer_id(const uint8_t node_id[NODE_ID_SIZE]) {
  return REMOTE_POLL_TIMER_ID_TAG | (static_cast<uint32_t>(node_id[0]) << (2 * BITS_PER_BYTE)) |
         (static_cast<uint32_t>(node_id[1]) << BITS_PER_BYTE) | static_cast<uint32_t>(node_id[2]);
}

}  // namespace decisions
}  // namespace home_io_control
}  // namespace esphome