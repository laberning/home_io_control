#pragma once

/// @file decisions_status.h
/// @brief Pure rules about device status: overheard commands, rain limitation, execution state, stopped reports,
/// status-update acknowledgement order, poll timer ids and the heater model check.
/// @ingroup hioc_hub
///
/// Inline, stateless helpers, testable without radio hardware or timing.

#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_device_kinds.h"
#include "proto_device_model.h"
#include "proto_frame.h"
#include "proto_timing.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace esphome {
namespace home_io_control {
namespace decisions {

// == Overheard commands ==

/// Returns true for the commands that write to a device: an execute, a mode activation or a private
/// register write. A heard write changes the device's state, so this hub's view of it is stale; a heard
/// read (any other command) changes nothing on the device. Deliberately a short list of writes rather
/// than a list of reads, so an unknown command counts as a read.
inline bool overheard_command_writes(uint8_t cmd) {
  return cmd == CMD_EXECUTE || cmd == CMD_ACTIVATE_MODE || cmd == CMD_WRITE_PRIVATE;
}

// == Passive RX handling ==

/// The order in which a 0x72 status-update acknowledgement is sent on the three channels: the channel
/// the 0x71 exchange ran on first, then the other two in their usual CH1, CH2, CH3 order. The device
/// is still listening on the channel its exchange used, so the copy that matters is not left to the
/// end of the sequence. A frequency that is not one of the three channels gives the usual order.
///
/// @param receive_freq_hz Frequency the 0x71 frame (and the challenge exchange) arrived on.
/// @return The three channel frequencies in transmit order.
inline std::array<uint32_t, 3> status_update_ack_channel_order(uint32_t receive_freq_hz) {
  std::array<uint32_t, 3> order{FREQ_CH1, FREQ_CH2, FREQ_CH3};
  if (receive_freq_hz != FREQ_CH1 && receive_freq_hz != FREQ_CH2 && receive_freq_hz != FREQ_CH3)
    return order;
  std::array<uint32_t, 3> result{receive_freq_hz, 0, 0};
  size_t next = 1;
  for (const uint32_t freq : order) {
    if (freq != receive_freq_hz)
      result[next++] = freq;
  }
  return result;
}

/// Whether a frame issue (a frame the hub did not act on) is logged at WARN rather than DEBUG. WARN only
/// when the frame is addressed to a registered device or to the hub itself: those are exchanges the hub
/// is part of, so an unhandled or rejected frame is worth a look. A frame from a registered device to a
/// broadcast address or to another controller is traffic the hub merely overheard (a heater's menu
/// beacon, a gateway polling the same device), so it stays at DEBUG however well the source is known.
///
/// @param dst_registered Frame destination is a registered device.
/// @param dst_is_us      Frame destination is the hub's own node ID.
inline bool frame_issue_is_warning(bool dst_registered, bool dst_is_us) { return dst_registered || dst_is_us; }

// == Poll timer ids ==

/// Namespace tag for `remote_poll_timer_id()` below: the node address occupies the low 24 bits,
/// and this is OR-ed in above them. Each numeric-id timer caller has its own tag (see
/// HEATING_POLL_TIMER_ID_TAG), so two timers for the same device never replace one another and an id
/// says which caller it belongs to.
constexpr uint32_t REMOTE_POLL_TIMER_ID_TAG = 0x01000000;

/// Namespace tag for `heating_poll_timer_id()`: the same node-address scheme under its own bit, so
/// a heater's regular-cadence timer and a remote-activity poll timer for the same device never replace
/// one another.
constexpr uint32_t HEATING_POLL_TIMER_ID_TAG = 0x02000000;

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

/// @brief Numeric `set_timeout()` id for a heater's regular-cadence poll timer
/// (`HeaterController::schedule_poll()`). Same keying as remote_poll_timer_id(), in
/// the HEATING_POLL_TIMER_ID_TAG namespace, so a poll that a linked remote triggers cannot cancel it.
/// @param node_id 3-byte device node address.
/// @return Numeric timer id, unique per device and distinct from remote_poll_timer_id().
inline uint32_t heating_poll_timer_id(const uint8_t node_id[NODE_ID_SIZE]) {
  return HEATING_POLL_TIMER_ID_TAG | (remote_poll_timer_id(node_id) & ~REMOTE_POLL_TIMER_ID_TAG);
}

// === Heater model check ===

/// @brief Runs of the one-off heater model check before the hub gives up on it for this boot. Each run
/// costs at most two register reads, and runs only on a poll the device answered.
constexpr uint8_t HEATER_MODEL_CHECK_MAX_RUNS = 3;

/// @brief What the model check suggests about a heater's declared `io_model:`.
enum class HeaterModelHint : uint8_t {
  NONE,                    ///< The answer fits the declaration: nothing to say.
  LOOKS_LIKE_RADIATOR,     ///< Declared a towel dryer, but the towel-dryer mode register holds no value.
  LOOKS_LIKE_TOWEL_DRYER,  ///< Declared as no towel dryer, but the towel-dryer mode register holds a value.
};

/// @brief Compare a heater's declared model with whether it answered the towel-dryer mode register
/// (0x0136) with a value.
/// @details Two Atlantic Adelis towel dryers answer that register with their mode; an AGILIA radiator
///          and a heater with a night light have not been read there yet, so the hint is worded "looks like" and never
///          changes behaviour. The caller only asks after the device answered the read (with or without a value).
/// @param declared The YAML model.
/// @param towel_mode_has_value True when the register's reply carried a value byte.
/// @return The hint to log, or NONE.
inline HeaterModelHint classify_heater_model_hint(DeviceModel declared, bool towel_mode_has_value) {
  if (declared == DeviceModel::TOWEL_DRYER && !towel_mode_has_value)
    return HeaterModelHint::LOOKS_LIKE_RADIATOR;
  if (declared != DeviceModel::TOWEL_DRYER && towel_mode_has_value)
    return HeaterModelHint::LOOKS_LIKE_TOWEL_DRYER;
  return HeaterModelHint::NONE;
}

/// First and last byte format_heater_model_name() prints as itself: printable ASCII, space to '~'.
constexpr uint8_t MODEL_NAME_PRINTABLE_FIRST = 0x20;
constexpr uint8_t MODEL_NAME_PRINTABLE_LAST = 0x7E;  ///< See MODEL_NAME_PRINTABLE_FIRST.

/// @brief Render a heater's model-name register (0x0119, ASCII) for a log line.
/// @details Stops at the first NUL, turns any other non-printable byte into '?', and trims trailing
///          spaces, so a padded or garbled reply still logs as one readable line.
/// @param bytes The value bytes of the reply.
/// @param len Their number.
/// @return The name; empty when the reply carried none.
inline std::string format_heater_model_name(const uint8_t *bytes, size_t len) {
  std::string name;
  for (size_t i = 0; i < len && bytes[i] != 0; i++) {
    const bool printable = bytes[i] >= MODEL_NAME_PRINTABLE_FIRST && bytes[i] <= MODEL_NAME_PRINTABLE_LAST;
    name.push_back(printable ? static_cast<char>(bytes[i]) : '?');
  }
  while (!name.empty() && name.back() == ' ')
    name.pop_back();
  return name;
}

// === Rain limitation ===

/// @brief Which evidence path labelled a device as limited by rain.
enum class RainLimitationRule : uint8_t {
  NONE = 0,         ///< No evidence of a rain limitation.
  RAIN_ORIGINATOR,  ///< The device names the rain sensor as the originator of its last command.
  CLAMPED_COMMAND,  ///< A position command was accepted but the stopped device ended elsewhere, while
                    ///< rain protection was recently active.
};

/// @brief What one decoded status reply said, plus what the hub had predicted.
struct RainLimitationInput {
  bool has_last_command;      ///< The device record holds a well-formed last-command record.
  uint8_t originator;         ///< Its Command Originator (ORIGINATOR_* in proto_constants.h).
  bool rain_evidence_recent;  ///< A rain originator was seen from this device within RAIN_EVIDENCE_HOLD_MS.
  float predicted_target;     ///< Hub's prediction before this reply (UNKNOWN_POSITION when none stood).
  float observed_target;      ///< dev.target after decoding this reply.
  bool stopped;               ///< The device reported itself stopped.
};

/// @brief Decide which rain-limitation evidence, if any, a status reply provides.
///
/// A rain limitation is an *inference from observations*, never a prediction (ADR 0030 keeps
/// predictions apart): windows clamp an `open` to their ventilation position without sending
/// CMD_ERROR_RESP / RESULT_LIMITATION_BY_RAIN, so the only evidence is what the device reports
/// in its ordinary status replies. Rule order matters: the originator rule is the direct
/// evidence and wins; the clamp rule only applies while earlier rain evidence is still fresh.
/// @param in What the reply said and what had been predicted.
/// @return The rule that fired, or RainLimitationRule::NONE.
[[nodiscard]] inline RainLimitationRule rain_limitation_rule(const RainLimitationInput &in) {
  if (in.has_last_command && in.originator == ORIGINATOR_RAIN_SENSOR)
    return RainLimitationRule::RAIN_ORIGINATOR;
  if (in.rain_evidence_recent && in.stopped && in.predicted_target != UNKNOWN_POSITION &&
      in.observed_target != UNKNOWN_POSITION && !has_reached_target_position(in.predicted_target, in.observed_target))
    return RainLimitationRule::CLAMPED_COMMAND;
  return RainLimitationRule::NONE;
}

/// @brief True when rain_limitation_rule() finds evidence of a rain limitation.
[[nodiscard]] inline bool is_rain_limited(const RainLimitationInput &in) {
  return rain_limitation_rule(in) != RainLimitationRule::NONE;
}

/// @brief Name of a rain-limitation rule for log lines.
[[nodiscard]] inline const char *rain_limitation_rule_name(RainLimitationRule rule) {
  switch (rule) {
    case RainLimitationRule::RAIN_ORIGINATOR:
      return "rain_originator";
    case RainLimitationRule::CLAMPED_COMMAND:
      return "clamped_command";
    case RainLimitationRule::NONE:
      break;
  }
  return "none";
}

// === Rain sensor from the minimum limitation ===

/// A window with a wired rain sensor limits its own opening while it rains. Home Assistant's VELUX
/// integration reads that limit and calls it rain from this minimum limit upward (observed values
/// 89, 91, 93 and 100 %).
static constexpr uint8_t RAIN_MINIMUM_LIMIT_PERCENT = 89;
/// Percent value of a fully limited position, the divisor of the raw position coding.
static constexpr uint16_t POSITION_FULL_PERCENT = 100;
/// Raw position units per percent (512).
static constexpr uint16_t POSITION_RAW_PER_PERCENT = STATUS_POS_MAX / POSITION_FULL_PERCENT;
static_assert(POSITION_RAW_PER_PERCENT * POSITION_FULL_PERCENT == STATUS_POS_MAX,
              "the raw rain threshold must be an exact multiple of one percent");
/// RAIN_MINIMUM_LIMIT_PERCENT in the raw position coding (0xB200).
static constexpr uint16_t RAIN_MINIMUM_LIMIT_RAW = POSITION_RAW_PER_PERCENT * RAIN_MINIMUM_LIMIT_PERCENT;

/// @brief Rain from the raw value of a minimum-limitation reply.
/// @param value_raw Big-endian raw value in position coding (LimitationStatus::value_raw).
/// @return nullopt for a value above STATUS_POS_MAX (not a percentage), true (rain) at or above
///         RAIN_MINIMUM_LIMIT_RAW, false (dry) below it.
[[nodiscard]] inline std::optional<bool> rain_from_minimum_limit(uint16_t value_raw) {
  if (value_raw > STATUS_POS_MAX)
    return std::nullopt;
  return value_raw >= RAIN_MINIMUM_LIMIT_RAW;
}

/// @brief Rain from a decoded limitation reply: the one place that knows which reply fields
/// decide. The value counts when the result code in the status byte is 0, whatever the state
/// bits (ADR 0125); originator and remaining time do not count.
/// @param status A decoded CMD_LIMITATION_STATUS_RESP payload.
/// @return nullopt for a non-zero result code (the device gave no value), else rain_from_minimum_limit().
/// \todo Experimental for a device other than VELUX: no capture shows a Somfy device under a rain or
///       wind lock, so a wet reading from one is unconfirmed (a dry one is the measured case).
[[nodiscard]] inline std::optional<bool> rain_from_limitation_reply(const LimitationStatus &status) {
  if (result_code_of(status.status_byte) != 0)
    return std::nullopt;
  return rain_from_minimum_limit(status.value_raw);
}

// === Execution state ===

/// @brief True when a device in this state is moving or about to: waiting for its turn (2), waiting for
/// power (3) or executing (4).
///
/// A queued cover is part of the move that follows, so it is followed like a running one. 0 (unable to
/// execute), 1 (error during the last move) and 5 (done) are at rest. UNDEFINED is not a state; the
/// caller falls back on the stopped bit (reads_stopped_by_status_byte()).
/// @param state State read from byte 0 of a status reply (execution_state_of()).
[[nodiscard]] inline bool is_executing(ExecutionState state) {
  return state == ExecutionState::WAITING_FOR_EXECUTING || state == ExecutionState::WAITING_FOR_POWER ||
         state == ExecutionState::EXECUTING;
}

/// @brief Does byte 0 of a status reply say the device is at rest?
///
/// The execution state decides (is_executing()). A byte whose bits [2:0] hold 6 or 7 has no state, and
/// its STATUS_STOPPED bit decides as it did before the state was read.
/// @param status_byte Byte 0 of the reply's payload.
[[nodiscard]] inline bool reads_stopped_by_status_byte(uint8_t status_byte) {
  const ExecutionState state = execution_state_of(status_byte);
  if (state == ExecutionState::UNDEFINED)
    return (status_byte & STATUS_STOPPED) != 0;
  return !is_executing(state);
}

/// @brief True when the execution state says the cover waits for its turn or for power (state 2 or 3).
///
/// A waiting cover holds its remaining time by design, so a report in a waiting state is not evidence
/// of a stuck move (ADR 0085): it neither backs the progress reads off nor counts toward the frozen
/// count that applies once the cover runs.
/// @param state The device's recorded execution state.
[[nodiscard]] inline bool is_waiting(ExecutionState state) {
  return state == ExecutionState::WAITING_FOR_EXECUTING || state == ExecutionState::WAITING_FOR_POWER;
}

/// @brief The execution state the hub records and shows for a state read from a status answer.
///
/// A device on a shared power supply may answer every command with "waiting for power" in the
/// acknowledgement, even when nothing else is moving, and report its real state at the first status
/// read. A waiting state in a command acknowledgement therefore counts as moving but is recorded as
/// EXECUTING, so it never shows as a waiting issue; a status reply or report shows its state as read
/// (ADR 0085).
/// @param state State read from byte 0 (execution_state_of()).
/// @param from_command_ack True for the acknowledgement of the hub's own command.
[[nodiscard]] inline ExecutionState shown_execution_state(ExecutionState state, bool from_command_ack) {
  return (from_command_ack && is_waiting(state)) ? ExecutionState::EXECUTING : state;
}

/// @brief True when a count in the remaining time that does not fall should back the progress reads off.
///
/// Only a frozen count while the cover claims to execute (or has no state) is a stuck one; a waiting
/// cover keeps the cadence the count gives (ADR 0085). The follow-up window still ends a cover that
/// never settles.
/// @param state The device's recorded execution state.
[[nodiscard]] inline bool backs_off_frozen_count(ExecutionState state) { return !is_waiting(state); }

// === Result code of an EXECUTE reply ===

/// @brief What the hub does about the result code in the reply to an EXECUTE.
enum class ExecuteResultCodeReport : uint8_t {
  NONE,                 ///< Code 0: the command was taken without complaint.
  SPEED_BLOCK_UNKNOWN,  ///< Code 5 after a frame with the speed block: the device has no speed parameter.
  OTHER_CODE,           ///< Any other non-zero code, logged with its name and number.
};

/// @brief Classify the result code of a reply to an EXECUTE.
///
/// The state in the same byte is unaffected: a device that answers `2C` (code 5, state 4) is running the
/// move. Code 5 after the 6-byte form names a parameter the request did not carry, so it is not
/// attributed to the speed block.
/// @param status_byte Byte 0 of the reply's payload.
/// @param request_had_speed_block True when the EXECUTE carried functional parameter 1
///        (execute_carries_speed_block()).
[[nodiscard]] inline ExecuteResultCodeReport classify_execute_result_code(uint8_t status_byte,
                                                                          bool request_had_speed_block) {
  const uint8_t code = result_code_of(status_byte);
  if (code == 0)
    return ExecuteResultCodeReport::NONE;
  if (code == error_resp_code(RESULT_PARAMETER_UNKNOWN) && request_had_speed_block)
    return ExecuteResultCodeReport::SPEED_BLOCK_UNKNOWN;
  return ExecuteResultCodeReport::OTHER_CODE;
}

// === Stopped reports ===

/// @brief What the hub makes of one status report's movement state.
enum class StoppedReport : uint8_t {
  MOVING,                 ///< The device reports itself moving.
  STOPPED,                ///< Stopped on its target (within STATUS_POS_TOLERANCE_RAW), or with no
                          ///< known target or position to compare.
  STOPPED_CONFIRMED,      ///< Stopped off its target, as in the remembered report: at rest there.
  STOPPED_UNCONFIRMABLE,  ///< Stopped off its target and no follow-up poll will run, so the
                          ///< device's own flag stands.
  SETTLING,               ///< Stopped off its target for the first time: treated as moving until
                          ///< the next report confirms it.
};

/// @brief One decoded status report, plus what the hub remembers and whether it will poll again.
struct StoppedReportInput {
  bool reported_stopped{true};       ///< The report says at rest (decisions::reads_stopped_by_status_byte()).
  float target{UNKNOWN_POSITION};    ///< Decoded target, or UNKNOWN_POSITION.
  float position{UNKNOWN_POSITION};  ///< Decoded current position, or UNKNOWN_POSITION.
  StopCandidate candidate{};         ///< The last off-target "stopped" report, if any.
  bool follow_up_possible{false};    ///< A tracked follow-up poll will run (StatusPollPolicy is tracking).
  bool has_motion{true};             ///< False for a device without motion (device_has_no_motion()): a
                                     ///< heater's target and current are a setpoint and a reading, and
                                     ///< their difference is no movement.
};

/// @brief Decide whether a "stopped" report means at rest.
///
/// A cover can come to rest off its target and report STOPPED there (corpus captures named on
/// StopCandidate). The device's flag is trusted once two reports agree on the same target
/// and the same current position; the first such report keeps the device "moving" for one more
/// poll. Comparing the target as well keeps a device safe that answers "stopped" with a newly
/// commanded target before its motor starts: the report before the command had the old target,
/// so it can never confirm. With no follow-up poll to confirm by, the device's flag stands, or the
/// device would read moving with nothing left to correct it.
///
/// Known bound: a device that answers "stopped, new target, old position" on two polls in a row
/// before its motor starts (a start delay longer than one settle poll, about 3 s) is taken as at
/// rest and polling ends. No captured device does this.
/// @param in The report and the hub's memory of the previous one.
/// @return The verdict; stopped_report_at_rest() says whether it means at rest.
[[nodiscard]] inline StoppedReport classify_stopped_report(const StoppedReportInput &in) {
  if (!in.reported_stopped)
    return StoppedReport::MOVING;
  if (!in.has_motion || in.target == UNKNOWN_POSITION || in.position == UNKNOWN_POSITION ||
      has_reached_target_position(in.target, in.position))
    return StoppedReport::STOPPED;
  if (in.candidate.armed() && has_reached_target_position(in.candidate.target, in.target) &&
      has_reached_target_position(in.candidate.position, in.position))
    return StoppedReport::STOPPED_CONFIRMED;
  if (!in.follow_up_possible)
    return StoppedReport::STOPPED_UNCONFIRMABLE;
  return StoppedReport::SETTLING;
}

/// @brief True when a verdict means the device is at rest.
[[nodiscard]] inline bool stopped_report_at_rest(StoppedReport report) {
  return report != StoppedReport::MOVING && report != StoppedReport::SETTLING;
}

/// @brief True when a verdict leaves an off-target report to remember for the next one.
///
/// Kept after a confirmation too, so a later poll that finds the device still resting at the same
/// place confirms at once instead of settling again.
[[nodiscard]] inline bool stopped_report_keeps_candidate(StoppedReport report) {
  return report == StoppedReport::STOPPED_CONFIRMED || report == StoppedReport::STOPPED_UNCONFIRMABLE ||
         report == StoppedReport::SETTLING;
}

}  // namespace decisions
}  // namespace home_io_control
}  // namespace esphome
