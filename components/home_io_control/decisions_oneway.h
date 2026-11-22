#pragma once

/// @file decisions_oneway.h
/// @brief Pure rules for one-way (1W) remote frames: burst de-duplication, background-poll deferral, the
/// pairing-gesture shape and the state an assumed-state cover shows.
/// @ingroup hioc_hub
///
/// Inline, stateless helpers, testable without radio hardware or timing.

#include "proto_codecs.h"
#include "proto_constants.h"
#include "oneway_sequence_store.h"
#include "proto_frame.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

namespace esphome {
namespace home_io_control {
namespace decisions {

// == One-way (1W) remote frame handling ==

/// @brief Key fields of the last processed 1W frame, used to collapse a remote's repeat burst.
///
/// 1W remotes repeat each command 4×, a few milliseconds apart, for reliability, and a held button keeps
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
///
/// An execute frame also carries the sender's rolling sequence, and every copy of one press shares it
/// while the next press counts up. Where both frames have it and go to the same destination, the
/// sequence tells a repeat from a new press exactly, so two quick presses of one button are two
/// presses. Across destinations the intent key above still decides: a sensor that sends one trigger
/// to several device classes gives each class frame its own sequence (corpus captures
/// wind_sensor_oneway_horizontal_awning_sx1276 and wind_sensor_oneway_unnamed_target_sx1276, one
/// burst of node 540B83), and that sweep is still one press.
struct OneWayDedupState {
  std::string src_id;           ///< Source node ID of the last processed frame; empty before the first.
  uint8_t cmd{0};               ///< Command byte.
  bool has_intent{false};       ///< Whether main0/main1 were decoded (execute / activate-mode only).
  uint8_t main0{0};             ///< First main byte — what distinguishes a move from a stop.
  uint8_t main1{0};             ///< Second main byte.
  uint32_t timestamp{0};        ///< millis() when the frame was processed.
  uint8_t dst[NODE_ID_SIZE]{};  ///< Destination address; compared for frames without intent and for the sequence rule.
  bool has_sequence{false};     ///< Whether the rolling sequence was decoded (execute frames only).
  uint16_t sequence{0};         ///< Rolling sequence; the same in every copy of one press.
};

/// Decide whether an incoming 1W frame repeats the previous one inside the burst window.
///
/// When both frames carry a rolling sequence and go to the same destination, they are the same press
/// exactly when the sequences are equal (and the frame is inside the window); a different sequence is
/// a new press however soon it comes. Any other pair is compared by the key described at
/// OneWayDedupState.
///
/// @param last     State recorded for the previously processed 1W frame.
/// @param incoming Candidate frame's key fields, with `timestamp` set to now.
/// @param window_ms Burst-suppression window.
/// @return true if the frame should be dropped as a repeat of `last`.
inline bool is_duplicate_1w_frame(const OneWayDedupState &last, const OneWayDedupState &incoming, uint32_t window_ms) {
  // `last.src_id` is empty until the first 1W frame is processed, so a real frame never matches it.
  if (last.src_id != incoming.src_id || last.cmd != incoming.cmd || last.has_intent != incoming.has_intent)
    return false;
  const bool same_dst = std::memcmp(last.dst, incoming.dst, NODE_ID_SIZE) == 0;
  if (last.has_sequence && incoming.has_sequence && same_dst) {
    if (last.sequence != incoming.sequence)
      return false;
    return (incoming.timestamp - last.timestamp) < window_ms;
  }
  if (incoming.has_intent && (last.main0 != incoming.main0 || last.main1 != incoming.main1))
    return false;
  if (!incoming.has_intent && !same_dst)
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
/// remote (up to about 1.2 s with wake-up preambles, its copies well under `quiet_ms` apart) never gets
/// cut short mid-transmission. Left unchecked
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

/// Whether the hub's own one-way transmission moves the devices its identity drives (ADR 0116).
///
/// Only a burst of which at least one copy reached the radio can have been heard by a device, and only a
/// frame that carries a command (a move, a position or a stop) says what the device is to do. An enrollment
/// frame carries none; nothing is followed for it, and nothing for a burst that never left the radio.
///
/// @param transmitted  At least one copy of the burst reached the radio.
/// @param has_intent   The frame that went out decoded to a command (OneWayFrameInfo::has_intent).
[[nodiscard]] inline bool follows_own_oneway_command(bool transmitted, bool has_intent) {
  return transmitted && has_intent;
}

// == Travel estimate of a one-way device (ADR 0127) ==

/// How often a moving estimate is published, in milliseconds.
static constexpr uint32_t TRAVEL_PUBLISH_INTERVAL_MS = 1000;

/// How long a full run takes in each direction, the only thing a one-way device lets the hub know about its speed.
///
/// "Rise" is a run from level 0 to level 1 and "fall" the reverse; the owner decides what the level means (a cover's
/// openness, a lamp's brightness). The two differ because a motor runs faster one way, and a fade need not be
/// symmetric. A zero duration is treated as an instant run, never a division by zero.
struct TravelRates {
  uint32_t rise_ms{0};  ///< Time to run from level 0 to level 1.
  uint32_t fall_ms{0};  ///< Time to run from level 1 to level 0.
};

/// What the hub asks of an estimate. A kind is added here when a device obeys a new intent (an instant jump).
struct TravelIntent {
  /// The kinds of intent.
  enum class Kind : uint8_t {
    STOP,   ///< Freeze where the estimate is.
    GO_TO,  ///< Travel to `level` at the run rate and stop there.
  };
  Kind kind{Kind::STOP};  ///< What is asked.
  float level{0.0F};      ///< The level to travel to (GO_TO only), 0..1.

  /// A stop.
  [[nodiscard]] static constexpr TravelIntent stop() { return {Kind::STOP, 0.0F}; }
  /// Travel to @p target, clamped to 0..1; 1.0 and 0.0 are the two ends.
  [[nodiscard]] static constexpr TravelIntent go_to(float target) {
    return {Kind::GO_TO, std::clamp(target, 0.0F, 1.0F)};
  }
};

/// The direction an estimate is running in at some moment. Not named RISING/FALLING: the Arduino core defines those
/// as macros.
enum class TravelDirection : uint8_t {
  IDLE,  ///< Not moving: stopped, or arrived.
  UP,    ///< Level going up.
  DOWN,  ///< Level going down.
};

/// The hub's belief about a one-way device: a level it was at, and where it set out to go from there.
///
/// Plain data with no clock inside, so it is a function of (estimate, intent, time): the owner keeps one and feeds it
/// the times of the commands and presses it learns of. The level moves linearly at the TravelRates rate and is never
/// published by the device, so it is an estimate that every error keeps until the next run to an end.
/// The owner chooses where it starts (a cover starts fully open, ADR 0127); the default is idle at level 0.
struct TravelEstimate {
  float level{0.0F};       ///< Level 0..1 at `updated_ms`.
  float target{0.0F};      ///< Where the run set out for; equals `level` when idle.
  uint32_t updated_ms{0};  ///< millis() at which `level` held.
};

/// The level at @p now: the estimate's level moved towards its target at the rate of that direction, and no further
/// than the target.
[[nodiscard]] inline float travel_level_at(const TravelEstimate &estimate, const TravelRates &rates, uint32_t now) {
  // Unsigned subtraction keeps the elapsed time right across the millis() wrap.
  const uint32_t elapsed = now - estimate.updated_ms;
  if (estimate.target > estimate.level) {
    if (rates.rise_ms == 0)
      return estimate.target;
    const float level = estimate.level + (static_cast<float>(elapsed) / static_cast<float>(rates.rise_ms));
    return level > estimate.target ? estimate.target : level;
  }
  if (estimate.target < estimate.level) {
    if (rates.fall_ms == 0)
      return estimate.target;
    const float level = estimate.level - (static_cast<float>(elapsed) / static_cast<float>(rates.fall_ms));
    return level < estimate.target ? estimate.target : level;
  }
  return estimate.level;
}

/// The direction the estimate runs in at @p now: IDLE once it has arrived.
[[nodiscard]] inline TravelDirection travel_direction_at(const TravelEstimate &estimate, const TravelRates &rates,
                                                         uint32_t now) {
  const float level = travel_level_at(estimate, rates, now);
  if (level < estimate.target)
    return TravelDirection::UP;
  if (level > estimate.target)
    return TravelDirection::DOWN;
  return TravelDirection::IDLE;
}

/// The estimate as it stands at @p now: its level moved to now, and idle when the run has arrived.
///
/// A run that lasts a whole duration ends at its end stop whatever the level was before, because the level stops at
/// the target. The owner settles an arrived run, so that the elapsed time never has to span more than one run.
[[nodiscard]] inline TravelEstimate travel_settled(const TravelEstimate &estimate, const TravelRates &rates,
                                                   uint32_t now) {
  const float level = travel_level_at(estimate, rates, now);
  if (level == estimate.target)
    return {level, level, now};
  return {level, estimate.target, now};
}

/// The estimate after the hub learns of an intent at @p now.
[[nodiscard]] inline TravelEstimate travel_after(const TravelEstimate &estimate, const TravelRates &rates,
                                                 const TravelIntent &intent, uint32_t now) {
  const float level = travel_level_at(estimate, rates, now);
  if (intent.kind == TravelIntent::Kind::GO_TO)
    return {level, intent.level, now};
  return {level, level, now};
}

/// Whether a running estimate is due to be published again: its direction changed or it ended since the last publish,
/// or a second has passed while it moves. Idle and unchanged is never due.
/// @param moving_now       The estimate's direction now.
/// @param published        The direction at the last publish.
/// @param last_publish_ms  millis() of the last publish.
/// @param now              Current millis().
/// @param interval_ms      Publish period while moving.
[[nodiscard]] inline bool travel_publish_due(TravelDirection moving_now, TravelDirection published,
                                             uint32_t last_publish_ms, uint32_t now, uint32_t interval_ms) {
  if (moving_now != published)
    return true;
  return moving_now != TravelDirection::IDLE && (now - last_publish_ms) >= interval_ms;
}

// == Assumed-state cover of a one-way controller (ADR 0117) ==

/// Percent scale of a one-way position (0 = open, 100 = closed) and its midpoint: a position above it is nearer the
/// closed end.
static constexpr float ONEWAY_COVER_FULL_PERCENT = 100.0F;
static constexpr float ONEWAY_COVER_MIDPOINT_PERCENT = 50.0F;
/// The same midpoint as a level on the cover scale (1.0 open): a level from it up is nearer the open end, as a
/// position at the midpoint opens in oneway_cover_intent_from_frame().
static constexpr float ONEWAY_COVER_NEARER_END_LEVEL =
    1.0F - (ONEWAY_COVER_MIDPOINT_PERCENT / ONEWAY_COVER_FULL_PERCENT);

/// What a command or a press asks of a cover, as the user means it (after the identity's inversion).
enum class OneWayCoverIntent : uint8_t {
  OPEN,   ///< Go fully open.
  CLOSE,  ///< Go fully closed.
  STOP,   ///< Stop where it is.
};

/// What an assumed-state cover shows. A one-way cover has no read, so this is what the hub believes from the
/// commands and presses it knows of, not what the device did.
enum class OneWayCoverState : uint8_t {
  OPEN,     ///< Last heard to open.
  CLOSED,   ///< Last heard to close.
  STOPPED,  ///< Last heard to stop: somewhere between the ends, the position unknown.
};

/// The state an assumed-state cover shows after a command or a press.
///
/// There is no travel time, so a move shows its end state at once and no "moving" phase exists; the rule needs
/// no earlier state, because every intent names its own result.
/// @param intent What the command or the press asked for.
[[nodiscard]] inline OneWayCoverState oneway_cover_state_after(OneWayCoverIntent intent) {
  switch (intent) {
    case OneWayCoverIntent::OPEN:
      return OneWayCoverState::OPEN;
    case OneWayCoverIntent::CLOSE:
      return OneWayCoverState::CLOSED;
    case OneWayCoverIntent::STOP:
    default:
      return OneWayCoverState::STOPPED;
  }
}

/// What a decoded one-way frame asks of a cover, or nothing when it says nothing a cover can show.
///
/// A stop is a stop. A position is treated as its nearer end. A named position (favourite, ventilation, ...), an
/// unknown value, the wire value that 50 % shares with the force-open alias (oneway_intent_to_target() resolves none)
/// and a frame without a command change nothing.
///
/// The main byte is a wire value. An inverted identity's kind has wire 0 = closed, so its position is mirrored
/// here; a stop needs no mirror.
/// @param info    The frame, from decode_1w_frame(): the hub's own transmission or a remote's press.
/// @param inverted Whether the identity's open/close are mirrored on the wire (effective_inverted()).
/// @return The intent, or an empty optional when the frame changes nothing.
[[nodiscard]] inline std::optional<OneWayCoverIntent> oneway_cover_intent_from_frame(const OneWayFrameInfo &info,
                                                                                     bool inverted) {
  if (!info.has_intent)
    return std::nullopt;
  if (info.main0 == POS_STOP)
    return OneWayCoverIntent::STOP;
  const std::optional<float> wire_percent = oneway_intent_to_target(info.main0, info.main1);
  if (!wire_percent.has_value())
    return std::nullopt;
  const float user_percent = inverted ? ONEWAY_COVER_FULL_PERCENT - *wire_percent : *wire_percent;
  return user_percent > ONEWAY_COVER_MIDPOINT_PERCENT ? OneWayCoverIntent::CLOSE : OneWayCoverIntent::OPEN;
}

/// What a decoded one-way frame asks of a cover that knows its travel times: a stop, or a position to travel to.
///
/// Same inputs and same "says nothing" cases as oneway_cover_intent_from_frame(), but a position keeps its percentage
/// instead of snapping to the nearer end. The wire mirror of an inverted identity is undone first, so the level is
/// the user's: 1.0 fully open, 0.0 fully closed (the ESPHome cover scale).
/// @param info     The frame, from decode_1w_frame(): the hub's own transmission or a remote's press.
/// @param inverted Whether the identity's open/close are mirrored on the wire (effective_inverted()).
/// @return The intent, or an empty optional when the frame changes nothing.
[[nodiscard]] inline std::optional<TravelIntent> oneway_cover_travel_intent_from_frame(const OneWayFrameInfo &info,
                                                                                       bool inverted) {
  if (!info.has_intent)
    return std::nullopt;
  if (info.main0 == POS_STOP)
    return TravelIntent::stop();
  const std::optional<float> wire_percent = oneway_intent_to_target(info.main0, info.main1);
  if (!wire_percent.has_value())
    return std::nullopt;
  const float user_percent = inverted ? ONEWAY_COVER_FULL_PERCENT - *wire_percent : *wire_percent;
  return TravelIntent::go_to(1.0F - (user_percent / ONEWAY_COVER_FULL_PERCENT));
}

// == Setting a controller identity's next sequence ==

/// @brief Outcome of asking to set an identity's next rolling sequence (see decide_sequence_set()).
struct SequenceSetDecision {
  bool accepted{false};              ///< Whether the new value may be written.
  uint32_t commands_before_wrap{0};  ///< Commands the identity can send from the requested value before it wraps.
  bool kept_at_boot{true};           ///< Whether the next boot resumes from the value, not from `initial_sequence`.
};

/// Decide whether an identity's next sequence may be set to `requested`.
///
/// A receiver obeys a command whose sequence is above the one it remembers and ignores one that is
/// not, and it never accepts one across the wrap from 65535 to 0. So a value above the current
/// one is always safe to set, and any value at or below it is refused unless the caller asked for
/// a lower one explicitly: a device that is in step would then ignore every command until the
/// counter passes its old number again.
///
/// The boot rule (resolve_boot_sequence(), ADR 0114) still applies to the written value: when the
/// identity's `initial_sequence` is above it by less than half the range, the next boot jumps
/// forward to `initial_sequence` before any command is sent, so the caller warns about it.
///
/// @param current Sequence the identity would send next.
/// @param requested Sequence the caller wants it to send next.
/// @param allow_lower Whether a value that is not above `current` is acceptable.
/// @param initial_sequence The identity's configured `initial_sequence`.
/// @return `accepted`, how many commands fit from `requested` up to and including 65535 (a wrapped
///         identity is lost on a receiver that refuses the wrap, so the caller reports it), and
///         whether a reboot keeps `requested`.
[[nodiscard]] inline SequenceSetDecision decide_sequence_set(uint16_t current, uint16_t requested, bool allow_lower,
                                                             uint16_t initial_sequence) {
  SequenceSetDecision decision;
  decision.accepted = requested > current || allow_lower;
  decision.commands_before_wrap = static_cast<uint32_t>(ONEWAY_SEQUENCE_MAX) - requested + 1;
  decision.kept_at_boot = resolve_boot_sequence(requested, initial_sequence) == requested;
  return decision;
}

}  // namespace decisions
}  // namespace home_io_control
}  // namespace esphome
