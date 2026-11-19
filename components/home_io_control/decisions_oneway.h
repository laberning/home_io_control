#pragma once

/// @file decisions_oneway.h
/// @brief Pure rules for one-way (1W) remote frames: burst de-duplication, background-poll deferral and the
/// pairing-gesture shape.
/// @ingroup hioc_hub
///
/// Inline, stateless helpers, testable without radio hardware or timing.

#include "proto_constants.h"
#include "proto_frame.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace esphome {
namespace home_io_control {
namespace decisions {

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

/// Whether a frame an exchange wait ignored is kept for the one-way remote handling: only a 1W
/// frame, the handling's input. A two-way frame from a device outside the exchange stays ignored.
/// @param frame A parsed frame heard during an exchange wait.
[[nodiscard]] inline bool keep_overheard_frame_for_oneway_handling(const IoFrame &frame) {
  return (frame.ctrl0 & CTRL0_PROTOCOL_1W) != 0;
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
/// @param next_op_is_background True if the queue front is a background poll (OperationQueue::is_background_op()).
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

}  // namespace decisions
}  // namespace home_io_control
}  // namespace esphome
