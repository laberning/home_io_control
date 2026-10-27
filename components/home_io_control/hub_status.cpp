#include "hub_internal.h"
#include "proto_heating_profiles.h"

#include "hub_decisions.h"
#include "proto_commands.h"

#include <cinttypes>
#include <optional>

/// @file hub_status.cpp
/// @brief Inbound status handling and passive receive-side state updates.
/// @ingroup hioc_hub
///
/// This file owns the receive-side state path for the hub:
/// - decode status-bearing frames into normalized device state,
/// - decide when passive traffic should arm one-shot or tracked follow-up polls,
/// - ACK authenticated device-initiated status updates.
///
/// @todo Validate the unsolicited CMD_STATUS_UPDATE path on hardware that actually emits
///       device-initiated updates after pairing, including inbound authentication,
///       three-channel ACK broadcast, and Home Assistant state publication without polling.
///
/// The goal of the split is to keep hub_core.cpp focused on lifecycle,
/// device registry, and scheduling while leaving the protocol-specific receive
/// interpretation in one place.

namespace esphome {
namespace home_io_control {

namespace {

/// Minimum payload length for 0x04 position-bearing replies: bytes 0-5 (stopped flag, target,
/// current) are mandatory; bytes 6-7 (remaining time) are optional and checked separately.
constexpr uint8_t PRIVATE_RESPONSE_MIN_DATA_LEN = 6;
constexpr uint8_t STATUS_UPDATE_MIN_DATA_LEN = 11;       ///< Minimum payload length for 0x71 device-initiated updates.
constexpr uint8_t GET_NAME_RESPONSE_MIN_DATA_LEN = 1;    ///< Minimum payload length for 0x51 name-bearing replies.
constexpr uint8_t GET_INFO2_RESPONSE_MIN_DATA_LEN = 12;  ///< Minimum payload length for 0x57 type/subtype metadata.
constexpr uint8_t ERROR_RESPONSE_MIN_DATA_LEN = 1;       ///< Minimum payload length for 0xFE result-bearing replies.
constexpr uint8_t EXTENDED_TILT_RESPONSE_MIN_DATA_LEN =
    15;  ///< Minimum payload length for tilt-capable extended status replies.
constexpr uint8_t STATUS_STOPPED_FLAGS_OFFSET = 0;  ///< Byte containing STATUS_STOPPED.
constexpr uint8_t STATUS_BATTERY_OFFSET = 1;  ///< Byte holding STATUS_BATTERY_MASK, the same in 0x04 and 0x71 payloads.
constexpr uint8_t PRIVATE_RESPONSE_REMAINING_TIME_OFFSET =
    6;  ///< MSB of the 16-bit remaining travel time (seconds) in a status reply (0x04 to a 0x03 poll).
// A 0x04 payload does not describe its own layout — which fields these offsets name depends on
// the request that drew the reply, and only the caller knows that. A reply to a tilt EXECUTE puts
// the tilt selector 0x20 at offset 4 and a 16-bit slat angle at 5..6, straddling the bytes a
// reply to a status poll uses for the current position (4..5). Do not try to recover the
// difference by sniffing the payload: 0x20 is also a perfectly legal current-position MSB (raw
// 0x2000-0x20FF is 16.0%-16.5%), and a tilt ack is 8 bytes like a status reply carrying the remaining time,
// so neither a `data[4] == STATUS_TILT_SELECTOR` test nor a length test can separate them — the
// first would blank real readings from any cover resting near 16%. The request-derived
// `trust_position` parameter on update_device_status_() is the discriminator, and the only
// correct one. See tests/corpus/captures/exchange/tilt_cover_exchange_ack_tilt_block*.yaml.
constexpr uint16_t MAIN_PARAMETER_LOW_BYTE_MASK = 0xFF;  ///< Low byte of a 16-bit main parameter.
constexpr uint8_t PRIVATE_RESPONSE_TARGET_OFFSET = 2;    ///< Target-position MSB offset in 0x04 replies.
constexpr uint8_t PRIVATE_RESPONSE_CURRENT_OFFSET = 4;   ///< Current-position MSB offset in 0x04 replies.
constexpr uint8_t STATUS_UPDATE_TARGET_OFFSET = 5;       ///< Target-position MSB offset in 0x71 updates.
constexpr uint8_t STATUS_UPDATE_CURRENT_OFFSET = 7;      ///< Current-position MSB offset in 0x71 updates.
constexpr uint8_t GET_INFO2_TYPE_OFFSET = 10;            ///< Packed device type byte in 0x57 replies.
constexpr uint8_t GET_INFO2_TYPE_SUBTYPE_OFFSET = 11;    ///< Packed type low bits plus subtype byte in 0x57 replies.
constexpr uint8_t EXTENDED_TILT_SELECTOR_OFFSET = 12;    ///< Selector byte announcing extended tilt payload.
constexpr uint8_t EXTENDED_TILT_MSB_OFFSET = 13;         ///< Tilt-position MSB within extended replies.
constexpr uint8_t EXTENDED_TILT_LSB_OFFSET = 14;         ///< Tilt-position LSB within extended replies.
constexpr uint8_t PRIVATE_RESPONSE_REMAINING_TIME_MIN_DATA_LEN =
    8;  ///< A status reply shorter than this (a 6-byte VELUX reply) carries no remaining time.
constexpr uint16_t PRIVATE_RESPONSE_REMAINING_TIME_NONE = 0xFFFF;    ///< All bits set: no remaining time provided.
constexpr uint32_t PRIVATE_RESPONSE_REMAINING_TIME_SCALE_MS = 1000;  ///< The remaining time counts in seconds.
constexpr uint32_t PRIVATE_RESPONSE_REMAINING_TIME_BIAS_MS =
    1000;  ///< Observed covers come to rest up to about a second after the count runs out (issue #178).

/// @brief Turn a decoded report's STOPPED flag into the hub's movement state.
///
/// Runs on every trusted report (a reply to a status poll, a device-originated status update).
/// `dev.is_stopped` holds the report's own flag on entry and the verdict on return; see
/// decisions::classify_stopped_report() for the rule. A cover that comes to rest off its target
/// reads moving for one more poll, then stopped, instead of moving for good.
/// @param id Device ID, for the log line.
/// @param dev Device record, with this report's target and position already decoded.
/// @param follow_up_possible A tracked follow-up poll will run.
void settle_stopped_report(const std::string &id, IoDevice &dev, bool follow_up_possible) {
  decisions::StoppedReportInput in;
  in.reported_stopped = dev.is_stopped;
  in.target = dev.target;
  in.position = dev.position;
  in.candidate = dev.stop_candidate;
  in.follow_up_possible = follow_up_possible;
  in.has_motion = !device_has_no_motion(dev.type, dev.subtype);
  const decisions::StoppedReport verdict = decisions::classify_stopped_report(in);
  if (verdict == decisions::StoppedReport::SETTLING) {
    ESP_LOGD(detail::TAG, "Device %s: reports stopped at %.2f%%, target %.2f%%: confirming with one more poll",
             id.c_str(), static_cast<double>(dev.position), static_cast<double>(dev.target));
  } else if (verdict == decisions::StoppedReport::STOPPED_CONFIRMED && follow_up_possible) {
    // Only inside a tracked poll chain, where it ends the settle: an untracked poll of a device
    // resting at the same place confirms on every report and would log each time.
    ESP_LOGD(detail::TAG, "Device %s: stopped at %.2f%% again, target %.2f%%: at rest off target", id.c_str(),
             static_cast<double>(dev.position), static_cast<double>(dev.target));
  } else if (verdict == decisions::StoppedReport::STOPPED_UNCONFIRMABLE) {
    ESP_LOGD(detail::TAG,
             "Device %s: reports stopped at %.2f%%, target %.2f%%, no poll to confirm by: taken as at rest", id.c_str(),
             static_cast<double>(dev.position), static_cast<double>(dev.target));
  }
  if (decisions::stopped_report_keeps_candidate(verdict)) {
    dev.stop_candidate.target = dev.target;
    dev.stop_candidate.position = dev.position;
  } else {
    dev.stop_candidate.clear();
  }
  dev.is_stopped = decisions::stopped_report_at_rest(verdict);
}

/// @brief Decode the shared target/current position fields used by private response and status‑update frames.
/// Different frame types use different byte offsets, but the normalization policy is identical once offsets known.
/// @param dev Device record to update.
/// @param frame IoFrame containing a status‑bearing command.
/// @param target_offset Byte offset of target MSB within frame.data.
/// @param current_offset Byte offset of current MSB within frame.data.
/// @param allow_tilt_from_extended_response If true and frame is extended, decode tilt from the extended tilt bytes.
/// @param id Device ID, for the settle log line.
/// @param follow_up_possible A tracked follow-up poll will run; see settle_stopped_report().
void decode_status_fields(IoDevice &dev, const IoFrame &frame, uint8_t target_offset, uint8_t current_offset,
                          bool allow_tilt_from_extended_response, const std::string &id, bool follow_up_possible) {
  uint16_t const tgt = (frame.data[target_offset] << 8) | frame.data[target_offset + 1];
  uint16_t const cur = (frame.data[current_offset] << 8) | frame.data[current_offset + 1];
  decode_position_report(tgt, cur, dev.is_stopped, dev.target, dev.position);
  settle_stopped_report(id, dev, follow_up_possible);
  // A decoded position is the observation this prediction existed to stand in for.
  dev.optimistic.clear_position();

  if (allow_tilt_from_extended_response && has_feature(dev.features, device_feature::TILT) &&
      frame.data_len >= EXTENDED_TILT_RESPONSE_MIN_DATA_LEN &&
      frame.data[EXTENDED_TILT_SELECTOR_OFFSET] == STATUS_TILT_SELECTOR) {
    uint16_t const tilt_raw = (frame.data[EXTENDED_TILT_MSB_OFFSET] << 8) | frame.data[EXTENDED_TILT_LSB_OFFSET];
    dev.tilt = decode_tilt_report(tilt_raw);
    dev.optimistic.clear_tilt();
  }
}

/// @brief Keep the wake belief's moving evidence in step with a status the device just reported.
/// @param dev Device the status came from.
/// @param moving True when the status says the device is travelling.
/// @param now_ms When the status was received.
void track_motion_evidence(IoDevice &dev, bool moving, uint32_t now_ms) {
  if (moving) {
    note_moving_evidence(dev, now_ms);
  } else {
    clear_moving_evidence(dev);
  }
}

/// @brief Decode the remaining travel time of a status reply.
///
/// A 0x04 answering a status poll carries the device's remaining travel time in seconds at bytes
/// 6-7 (big-endian), after status, functional-parameter indicator, target and current position:
/// the order of the "main info" status answer in the VELUX KLF 200 API.
/// The 0x04 answering a command (EXECUTE) has a request-dependent layout: there those bytes hold
/// something else (`06 00` from a Somfy Izymo, a tilt block's `8D 00`), so they are read only when
/// the reply's position is trusted, i.e. on a status-poll reply.
/// @param frame The private response frame.
/// @param trust_position True for a status-poll reply (see apply_private_response_status()).
/// @return The remaining time in seconds, or nullopt when the reply carries none.
std::optional<uint16_t> decode_remaining_time_s(const IoFrame &frame, bool trust_position) {
  if (!trust_position || frame.data_len < PRIVATE_RESPONSE_REMAINING_TIME_MIN_DATA_LEN)
    return std::nullopt;
  const uint16_t remaining = (frame.data[PRIVATE_RESPONSE_REMAINING_TIME_OFFSET] << 8) |
                             frame.data[PRIVATE_RESPONSE_REMAINING_TIME_OFFSET + 1];
  if (remaining == PRIVATE_RESPONSE_REMAINING_TIME_NONE)
    return std::nullopt;
  return remaining;
}

/// @brief Compute the delay before the next status poll for a private‑response device.
/// @param dev Device record.
/// @param remaining_s Usable remaining travel time in seconds (nullopt when there is none).
/// @param policy Policy used to look up the configured poll interval.
/// @param id Device ID for policy lookup.
/// @return Delay in milliseconds, or 0 if the device is stopped.
uint32_t compute_private_response_delay_ms(const IoDevice &dev, std::optional<uint16_t> remaining_s,
                                           const StatusPollPolicy &policy, const std::string &id) {
  if (effective_is_stopped(dev))
    return 0;
  // The remaining time plus a second is when the device should be at rest; settle_delay_ms()
  // reconciles it with the configured interval and the default (it can only shorten them).
  const uint32_t remaining_delay_ms =
      remaining_s.has_value()
          ? (*remaining_s * PRIVATE_RESPONSE_REMAINING_TIME_SCALE_MS) + PRIVATE_RESPONSE_REMAINING_TIME_BIAS_MS
          : 0;
  // A private response is the shared reply to both polls (0x03) and commands (0x00); it carries no
  // marker for STOP, so the STOP cap is applied by the command path, not here.
  return settle_delay_ms(policy.get_interval(id), remaining_delay_ms, /*cap_for_stop=*/false);
}

/// @brief Compute the delay before the next status poll for a device‑originated status update.
/// @param dev Device record.
/// @param policy Policy used to look up the configured poll interval.
/// @param id Device ID for policy lookup.
/// @return Delay in milliseconds for tracked polling; 0 if stopped.
uint32_t compute_status_update_delay_ms(const IoDevice &dev, const StatusPollPolicy &policy, const std::string &id) {
  if (effective_is_stopped(dev))
    return 0;
  // Device-originated updates carry no remaining time and are never STOP replies.
  return settle_delay_ms(policy.get_interval(id), /*remaining_delay_ms=*/0, /*cap_for_stop=*/false);
}

/// @brief Does a status reply's flags byte say the device is at rest?
///
/// A heater has no motion, so a heating-level heater and an exterior heater are always at rest
/// whatever their flags byte says (device_has_no_motion()): the movement settle loop (follow-up
/// polls while "moving") must never run for them, and what that byte holds on a heater is
/// unconfirmed. Every other device reports it in STATUS_STOPPED.
/// @param dev Device the reply came from.
/// @param flags The reply's stopped-flags byte.
/// @return true when the device is at rest.
bool reports_stopped(const IoDevice &dev, uint8_t flags) {
  return device_has_no_motion(dev.type, dev.subtype) || (flags & STATUS_STOPPED) != 0;
}

/// @brief Apply a private-response frame to the device record.
/// @param id Device ID for policy lookup.
/// @param dev Device record to update.
/// @param frame Private-response frame.
/// @param policy Poll policy for scheduling follow-up polls.
/// @param trust_position False to skip decoding target/current from `frame` — the immediate
/// reply to our own just-sent CMD_EXECUTE has been observed (real hardware, see
/// tests/corpus/captures/exchange/somfy_awning_exchange_ack_reports_stale_target_*.yaml) echoing
/// pre-command target/current values rather than the freshly-commanded target. `is_stopped` is
/// still applied either way; the optimistic target already set by the caller (or the follow-up
/// status poll a few seconds later) remains the source of truth for target/current in that case.
void apply_private_response_status(const std::string &id, IoDevice &dev, const IoFrame &frame, StatusPollPolicy &policy,
                                   bool trust_position = true) {
  const bool reported_stopped = reports_stopped(dev, frame.data[STATUS_STOPPED_FLAGS_OFFSET]);
  dev.is_stopped = reported_stopped;
  dev.last_status = millis();
  if (trust_position) {
    decode_status_fields(dev, frame, PRIVATE_RESPONSE_TARGET_OFFSET, PRIVATE_RESPONSE_CURRENT_OFFSET, true, id,
                         policy.is_tracking_active(id, dev.last_status));
  } else if (!detail::stop_candidate_matches_observation(dev)) {
    // An ack's own position bytes are not decoded, so it is held against the last trusted report.
    // When that report is a stop the hub already accepted off target, the gap is not movement. The
    // candidate stays: the first poll after the command compares targets, so a report from before
    // a command to a new target can never confirm one after it.
    detail::normalize_stopped_state(dev);
  }
  // The wake belief follows the device's own flag, not the hub's verdict. On the execute-ack path
  // (trust_position == false) dev.target/position are stale, so the normalized is_stopped can read
  // "moving" for a device that just reported stopped. It matters for the ack to a STOP: a VELUX SSL
  // still reports "moving" while it winds down, and in that state it ignores the wake-up preamble,
  // so the settle poll after the STOP must lead short. A move's ack is overridden anyway:
  // run_execute_operation_() stamps the evidence once the move is accepted, after this runs. On a
  // trusted report a settling device has said "stopped": at rest a VELUX SSL hears only the wake-up
  // preamble, so the confirming poll must lead with it (issue #151).
  track_motion_evidence(dev, !reported_stopped, dev.last_status);

  // A zero remaining time means "about to stop" only in a report that says moving, from a device
  // that has sent a count before: a counting cover reports 0 in its last second of travel (issue
  // #178), while a device that has never sent a count may simply not provide one, and would
  // otherwise be polled every second. A "stopped" report keeps the default delay, which is what a
  // STOPPED off target needs before its confirming poll (decisions::classify_stopped_report()).
  std::optional<uint16_t> remaining_s = decode_remaining_time_s(frame, trust_position);
  if (remaining_s.has_value() && *remaining_s != 0)
    dev.reports_remaining_time = true;
  if (remaining_s.has_value() && *remaining_s == 0 && (reported_stopped || !dev.reports_remaining_time))
    remaining_s.reset();

  if (effective_is_stopped(dev) || !policy.is_tracking_active(id, dev.last_status)) {
    policy.clear(id);
    return;
  }

  uint32_t const delay_ms = compute_private_response_delay_ms(dev, remaining_s, policy, id);
  ESP_LOGD(detail::TAG,
           "Device %s: next status poll in %" PRIu32 " ms (remaining time=%s, configured interval=%" PRIu32 " ms)",
           id.c_str(), delay_ms, remaining_s.has_value() ? std::to_string(*remaining_s).append("s").c_str() : "none",
           policy.get_interval(id));
  uint32_t const new_deadline = dev.last_status + delay_ms;
  uint32_t const existing_deadline = policy.get_next_update(id);
  // Don't push the deadline forward — only move it earlier. This prevents repeated command
  // responses (e.g. multiple rapid STOP presses) from compounding the wait time.
  policy.set_next_update(
      id, (existing_deadline != 0 && existing_deadline < new_deadline) ? existing_deadline : new_deadline);
}

/// @brief Apply a device-originated status-update frame to the device record.
/// @param id Device ID for policy lookup.
/// @param dev Device record to update.
/// @param frame Status-update frame.
/// @param policy Poll policy for scheduling follow-up polls.
void apply_unsolicited_status_update(const std::string &id, IoDevice &dev, const IoFrame &frame,
                                     StatusPollPolicy &policy) {
  const bool reported_stopped = reports_stopped(dev, frame.data[STATUS_STOPPED_FLAGS_OFFSET]);
  dev.is_stopped = reported_stopped;
  dev.last_status = millis();
  decode_status_fields(dev, frame, STATUS_UPDATE_TARGET_OFFSET, STATUS_UPDATE_CURRENT_OFFSET, false, id,
                       policy.is_tracking_active(id, dev.last_status));
  // The device's own flag, as on the poll path (apply_private_response_status()).
  track_motion_evidence(dev, !reported_stopped, dev.last_status);

  if (effective_is_stopped(dev) || !policy.is_tracking_active(id, dev.last_status)) {
    policy.clear(id);
    return;
  }

  policy.set_next_update(id, dev.last_status + compute_status_update_delay_ms(dev, policy, id));
}

/// @brief Record what a heater commanded with an EXECUTE reports as its current main parameter.
///
/// A heating-level heater reports a level; a temperature heater reports an alias (kept in
/// HeatingReadings::level) or a plain temperature (HeatingReadings::setpoint_c, decoded over the
/// device's encoding range, which also clears a stale alias).
/// @param id Device ID for the log line.
/// @param dev Device record to update; other devices are left alone.
/// @param frame A reply to a status poll (the layout of a reply to an EXECUTE follows the request).
void apply_heating_main_parameter(const std::string &id, IoDevice &dev, const IoFrame &frame) {
  const HeatingProfile *profile = find_heating_profile(dev.type, dev.subtype);
  if (profile == nullptr || profile->path == HeatingControlPath::PRIVATE_REGISTERS)
    return;
  const auto current = static_cast<uint16_t>((frame.data[PRIVATE_RESPONSE_CURRENT_OFFSET] << BITS_PER_BYTE) |
                                             frame.data[PRIVATE_RESPONSE_CURRENT_OFFSET + 1]);
  dev.heating.main_parameter = current;
  if ((current >> BITS_PER_BYTE) == profile->mp_prefix) {
    const auto wire = static_cast<int16_t>(current & MAIN_PARAMETER_LOW_BYTE_MASK);
    if (dev.heating.level != wire)
      ESP_LOGI(detail::TAG, "Device %s: heating value 0x%02X", id.c_str(), static_cast<unsigned>(wire));
    dev.heating.level = wire;
    return;
  }
  float celsius = 0.0F;
  if (profile->path == HeatingControlPath::TEMPERATURE_EXECUTE &&
      decode_temperature_main_parameter(current, dev.temperature_range_min_c, dev.temperature_range_max_c, celsius)) {
    dev.heating.setpoint_c = celsius;
    dev.heating.level = -1;
    return;
  }
  ESP_LOGD(detail::TAG, "Device %s: status main parameter 0x%04X is not a heating value", id.c_str(), current);
}

/// @brief Record the coarse battery level a status payload carries, logging a change.
/// @param id Device ID for the log line.
/// @param dev Device record to update.
/// @param frame A status poll reply or a device-originated status update (both put it at data[1]).
void apply_battery_state(const std::string &id, IoDevice &dev, const IoFrame &frame) {
  const BatteryState reported = decode_battery_state(frame.data[STATUS_BATTERY_OFFSET]);
  if (reported == dev.battery)
    return;
  // A battery level changes rarely, so a change is worth an INFO line; mains devices stay at
  // NOT_REPORTED and never log.
  ESP_LOGI(detail::TAG, "Device %s: battery %s -> %s", id.c_str(), battery_state_name(dev.battery),
           battery_state_name(reported));
  dev.battery = reported;
}

/// @brief Apply INFO2 metadata to the device record when YAML has not already declared it.
/// @param dev Device record to update.
/// @param frame INFO2 response frame.
void apply_info2_response(IoDevice &dev, const IoFrame &frame) {
  if (dev.type != DeviceType::UNKNOWN)
    return;

  dev.type = decode_packed_device_type(frame.data[GET_INFO2_TYPE_OFFSET], frame.data[GET_INFO2_TYPE_SUBTYPE_OFFSET]);
  dev.subtype = decode_packed_device_subtype(frame.data[GET_INFO2_TYPE_SUBTYPE_OFFSET]);
  resolve_device_features(dev);
}

/// @brief Apply a name response frame to the device record.
/// @param dev Device record to update.
/// @param frame Name response frame.
void apply_name_response(IoDevice &dev, const IoFrame &frame) {
  std::string const name = decode_device_name_payload(frame.data, frame.data_len);
  memset(dev.name, 0, sizeof(dev.name));
  if (!name.empty())
    memcpy(dev.name, name.c_str(), name.length());
}

}  // namespace

void IOHomeControlComponent::begin_status_poll_tracking_(const std::string &device_id, uint32_t initial_delay_ms) {
  if (this->get_device(device_id) == nullptr)
    return;
  this->poll_policy_.begin_tracking(device_id, initial_delay_ms, millis());
}

void IOHomeControlComponent::settle_ended_status_tracking_() {
  while (const auto ended = this->poll_policy_.pop_ended_device()) {
    IoDevice *dev = this->registry_.get(*ended);
    if (dev == nullptr)
      continue;
    if (detail::is_settling(*dev)) {
      // The device's own last word was "stopped"; nothing is left to confirm it.
      ESP_LOGD(detail::TAG, "Device %s: no poll left to confirm its stop at %.2f%%, target %.2f%%: taken as at rest",
               ended->c_str(), static_cast<double>(dev->position), static_cast<double>(dev->target));
    } else if (!effective_is_stopped(*dev) && detail::known_device_reports_motion_through_status(*dev)) {
      // The device was last seen (or commanded) moving and no report has followed; with no poll left
      // the cover would show movement for good. Position and target stay as last reported.
      if (dev->position == UNKNOWN_POSITION) {
        ESP_LOGI(detail::TAG, "Device %s: no status report since its move; shown at rest at an unknown position",
                 ended->c_str());
      } else {
        ESP_LOGI(detail::TAG, "Device %s: no status report since its move; shown at rest at %.0f%%", ended->c_str(),
                 static_cast<double>(dev->position));
      }
      dev->optimistic.clear_position();
      // A resting device needs the wake-up preamble again, so the next request must not assume the
      // device is still awake from the movement.
      clear_moving_evidence(*dev);
    } else {
      continue;
    }
    dev->is_stopped = true;
    detail::log_status_update(*ended, *dev);
    this->notify_device_update_(*ended);
  }
}

void IOHomeControlComponent::schedule_status_poll_(const std::string &device_id, uint32_t delay_ms) {
  // Keyed by the device's node address (decisions::remote_poll_timer_id()), not by a per-device
  // name string: Component::set_timeout(const char*, ...) stores the caller's pointer rather than
  // copying it, so a name built from `device_id` here would dangle the moment this function
  // returned. The numeric id is still per-device, so repeated remote traffic resets the pending
  // poll instead of stacking multiple delayed callbacks for the same actuator.
  uint8_t node_id[NODE_ID_SIZE];
  if (!hex_to_bytes(device_id, node_id, NODE_ID_SIZE))
    return;  // malformed device_id: every caller passes one already validated by the registry
  this->set_timeout(decisions::remote_poll_timer_id(node_id), delay_ms, [this, device_id]() {
    this->poll_policy_.on_remote_poll_ran(device_id);
    this->queue_request_device_status(device_id);
  });
}

void IOHomeControlComponent::schedule_heating_poll_(const std::string &device_id, uint32_t delay_ms) {
  uint8_t node_id[NODE_ID_SIZE];
  if (!hex_to_bytes(device_id, node_id, NODE_ID_SIZE))
    return;  // malformed device_id: the caller passes one already validated by the registry
  this->set_timeout(decisions::heating_poll_timer_id(node_id), delay_ms,
                    [this, device_id]() { this->queue_request_device_status(device_id); });
}

void IOHomeControlComponent::schedule_device_polls_(const std::vector<std::string> &device_ids, uint32_t delay_ms) {
  for (const auto &device_id : device_ids) {
    this->begin_status_poll_tracking_(device_id, 0);
    this->schedule_status_poll_(device_id, delay_ms);
  }
}

void IOHomeControlComponent::act_on_foreign_frame_(const std::string &device_id, ForeignFrameKind kind,
                                                   const std::string &controller, uint8_t cmd) {
  const IoDevice *dev = this->get_device(device_id);
  if (dev == nullptr)
    return;
  const uint32_t now = millis();
  const char *what = kind == ForeignFrameKind::DEVICE_CHALLENGE_TO_OTHER ? "challenge" : command_name(cmd);
  switch (this->poll_policy_.on_foreign_frame(device_id, kind, controller, cmd, now, dev->last_status)) {
    case ForeignAction::ARM:
      ESP_LOGD(detail::TAG, "rx remote_activity device=%s controller=%s %s(0x%02X): scheduling status poll",
               device_id.c_str(), controller.c_str(), what, cmd);
      this->begin_status_poll_tracking_(device_id, 0);
      this->schedule_status_poll_(device_id, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS);
      break;
    case ForeignAction::PUSH_PENDING:
      ESP_LOGD(detail::TAG, "rx remote_activity device=%s controller=%s %s(0x%02X): poll pending, pushed out",
               device_id.c_str(), controller.c_str(), what, cmd);
      this->schedule_status_poll_(device_id, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS);
      break;
    case ForeignAction::IGNORE:
      ESP_LOGD(detail::TAG, "rx remote_activity device=%s controller=%s %s(0x%02X): %s, no poll", device_id.c_str(),
               controller.c_str(), what, cmd,
               kind == ForeignFrameKind::DEVICE_CHALLENGE_TO_OTHER
                   ? "answers a heard read"
                   : "a read, and the device was read in the last 10 min");
      break;
  }
}

void IOHomeControlComponent::note_foreign_challenge_(const IoFrame &frame) {
  // A registered device challenging another controller: a write whose request frame was missed
  // looks exactly like this, so the policy decides whether it warrants a poll.
  if (frame.cmd != CMD_CHALLENGE_REQ || memcmp(frame.dst, this->node_id_, NODE_ID_SIZE) == 0)
    return;
  const std::string challenger = node_id_to_string(frame.src);
  if (this->get_device(challenger) == nullptr)
    return;
  this->act_on_foreign_frame_(challenger, ForeignFrameKind::DEVICE_CHALLENGE_TO_OTHER, node_id_to_string(frame.dst),
                              frame.cmd);
}

bool IOHomeControlComponent::act_on_overheard_request_(const IoFrame &frame) {
  // Another controller's frame to a registered device (e.g. a gateway commanding a shutter we also
  // control). Our own frames are never foreign.
  const std::string dst_id = node_id_to_string(frame.dst);
  if (this->get_device(dst_id) != nullptr && memcmp(frame.src, this->node_id_, NODE_ID_SIZE) != 0) {
    this->act_on_foreign_frame_(dst_id, ForeignFrameKind::REQUEST_TO_DEVICE, node_id_to_string(frame.src), frame.cmd);
    return true;
  }

  // A linked 2W controller whose commands target a device at an address we don't have registered.
  // 1W remotes are handled before this, via the CTRL0_PROTOCOL_1W check.
  const std::string src_id = node_id_to_string(frame.src);
  const std::vector<std::string> *linked = this->registry_.linked_devices(src_id);
  if (linked == nullptr)
    return false;
  // Copied: the helper may call back into code that touches the registry.
  const std::vector<std::string> devices = *linked;
  for (const auto &device_id : devices)
    this->act_on_foreign_frame_(device_id, ForeignFrameKind::REQUEST_TO_DEVICE, src_id, frame.cmd);
  return true;
}

std::vector<std::string> IOHomeControlComponent::resolve_1w_target_devices_(const OneWayFrameInfo &info,
                                                                            const std::string &src_id) const {
  std::vector<std::string> devices;
  if (const std::vector<std::string> *id_linked = this->registry_.linked_devices(src_id)) {
    devices = *id_linked;
  }
  if (info.address_class == AddressClass::BROADCAST_TYPE && info.target_type != DeviceType::UNKNOWN) {
    if (const std::vector<std::string> *class_linked = this->registry_.linked_devices_for_class(info.target_type)) {
      for (const auto &device_id : *class_linked) {
        if (std::find(devices.begin(), devices.end(), device_id) == devices.end())
          devices.push_back(device_id);
      }
    }
  }
  return devices;
}

bool IOHomeControlComponent::apply_optimistic_linked_state_(const OneWayFrameInfo &info,
                                                            const std::vector<std::string> &device_ids) {
  if (!info.has_intent)
    return false;

  const bool is_stop = info.main0 == POS_STOP;
  const std::optional<float> target = is_stop ? std::nullopt : oneway_intent_to_target(info.main0, info.main1);

  for (const auto &device_id : device_ids) {
    IoDevice *dev = this->registry_.get(device_id);
    if (dev != nullptr && info.target_type != DeviceType::UNKNOWN && dev->type != DeviceType::UNKNOWN &&
        dev->type != info.target_type) {
      continue;  // Type mismatch: still polled by schedule_device_polls_(), just not moved optimistically.
    }
    // Evidence first: the overlay calls below notify entity callbacks, and `dev` is not touched after
    // them.
    if (is_stop) {
      if (dev != nullptr)
        clear_moving_evidence(*dev);
      this->registry_.apply_optimistic_stop(device_id);
    } else if (target.has_value()) {
      // Unlike our own commands, this movement was really started by a remote the device heard, so
      // it is evidence of travel even where the optimistic overlay is disabled.
      if (dev != nullptr)
        note_moving_evidence(*dev, millis());
      this->registry_.apply_optimistic_target(device_id, *target);
    }
  }
  return is_stop;
}

void IOHomeControlComponent::maybe_fire_sender_event_(const OneWayFrameInfo &info, bool linked,
                                                      const std::string &src_id) {
  if (!info.has_intent)
    return;
  // All overheard 1W traffic is DEBUG-logged regardless (see log_1w_remote_frame()); the HA event
  // additionally requires the sender to be on the `exposed_senders` allowlist, since 1W broadcasts
  // carry no ownership marker and this radio may overhear a neighbor's remote (or sensor) as
  // easily as the user's own. DEBUG-log the reason it did or didn't fire so a live log capture is
  // enough to diagnose a misconfigured allowlist vs. a disconnected API.
  if (!this->is_connected()) {
    ESP_LOGD(detail::TAG, "1W sender %s has intent but the API is not connected, skipping %s", src_id.c_str(),
             detail::ONEWAY_SENDER_EVENT);
    return;
  }
  if (!detail::is_exposed_sender(this->exposed_senders_, src_id)) {
    ESP_LOGD(detail::TAG, "1W sender %s has intent but is not in exposed_senders, skipping %s", src_id.c_str(),
             detail::ONEWAY_SENDER_EVENT);
    return;
  }
  ESP_LOGD(detail::TAG, "Firing %s for sender %s", detail::ONEWAY_SENDER_EVENT, src_id.c_str());
  this->fire_homeassistant_event(detail::ONEWAY_SENDER_EVENT, detail::build_sender_event_data(info, linked));
}

void IOHomeControlComponent::update_device_status_(const IoFrame &frame, bool trust_position) {
  const std::string id = node_id_to_string(frame.src);
  IoDevice *device_ptr = this->registry_.get(id);
  if (device_ptr == nullptr) {
    detail::log_frame_issue(this, "rx", "unregistered_device", frame, frame_length(frame));
    return;
  }
  IoDevice &dev = *device_ptr;
  detail::update_link_health(dev, this->radio_);

  if (frame.cmd == CMD_PRIVATE_RESP) {
    if (frame.data_len < PRIVATE_RESPONSE_MIN_DATA_LEN) {
      detail::log_frame_issue(this, "rx", "unsupported_payload", frame, frame_length(frame));
      return;
    }

    // CMD_PRIVATE_RESP (0x04) serves as the reply to both status polls (0x03) and execute
    // commands (0x00). The position fields are shared across both response types, but the
    // immediate reply to our own execute command is not necessarily trustworthy for them (see
    // apply_private_response_status()'s trust_position parameter).
    // Decoding a position clears the optimistic prediction, so capture it first: a clamped
    // command is the disagreement between that prediction and what the device then reports.
    const float predicted_target = dev.optimistic.target;
    apply_private_response_status(id, dev, frame, this->poll_policy_, trust_position);
    // The device names, in its own status payload, the controller that last commanded it. Skipped
    // on an execute ack (trust_position == false): that reply's payload layout is request-derived
    // rather than self-describing (see the offset comment at the top of this file), and our own
    // ack is not a report of the *last* command anyway — the settle poll a few seconds later is.
    if (trust_position) {
      const LastCommandRecord record = decode_last_command_record(frame, PRIVATE_RESPONSE_LAST_COMMAND_OFFSET);
      detail::apply_last_command_record(dev, record);
      detail::update_rain_limitation(id, dev, predicted_target, record.valid, millis());
      // Same reason as the record above: only a reply to a status poll is a status report.
      apply_battery_state(id, dev, frame);
      apply_heating_main_parameter(id, dev, frame);
    }
    detail::clear_command_result(dev);
    detail::log_status_update(id, dev);
    this->notify_device_update_(id);
    return;
  }

  if (frame.cmd == CMD_STATUS_UPDATE) {
    if (frame.data_len < STATUS_UPDATE_MIN_DATA_LEN) {
      detail::log_frame_issue(this, "rx", "unsupported_payload", frame, frame_length(frame));
      return;
    }

    // Status-update frames come from the device itself rather than from a direct controller poll.
    // They use different offsets for the target/current fields and do not carry reliable tilt data.
    const float predicted_target = dev.optimistic.target;
    apply_unsolicited_status_update(id, dev, frame, this->poll_policy_);
    const LastCommandRecord record = decode_last_command_record(frame, STATUS_UPDATE_LAST_COMMAND_OFFSET);
    detail::apply_last_command_record(dev, record);
    detail::update_rain_limitation(id, dev, predicted_target, record.valid, millis());
    apply_battery_state(id, dev, frame);
    detail::clear_command_result(dev);

    // What caused the device to move (wind sensor, timer, a remote). Empty when the payload is
    // too short to carry the byte — a 11-14 byte 0x71 is still applied for its position fields,
    // it just has no originator to report. This reads the same data[14] byte the last-command
    // record above just decoded, but through a separate, older, unguarded accessor kept for its
    // own pinned test (StatusUpdateOriginatorIsAtOffset14AndDecodePathUndisturbed) — it can render
    // a byte here that the "Last Command Source" sensor leaves empty, on the one payload shape
    // that differs between them: an all-zero commander (which apply_last_command_record() above
    // treats as "no record", see decode_last_command_record()'s doc comment) paired with a
    // populated originator byte. No capture has shown that combination in practice.
    const std::string originator = detail::describe_status_update_originator(frame);
    if (!originator.empty())
      ESP_LOGD(detail::TAG, "Device %s: status update originator=%s", id.c_str(), originator.c_str());

    detail::log_status_update(id, dev, " (status update)");
    this->notify_device_update_(id);
    return;
  }

  if (frame.cmd == CMD_GET_NAME_RESP) {
    if (frame.data_len < GET_NAME_RESPONSE_MIN_DATA_LEN) {
      detail::log_frame_issue(this, "rx", "unsupported_payload", frame, frame_length(frame));
      return;
    }

    apply_name_response(dev, frame);
    ESP_LOGI(detail::TAG, "Device %s: name=%s", id.c_str(), dev.name[0] == '\0' ? "" : dev.name);
    this->notify_device_update_(id);
    return;
  }

  if (frame.cmd == CMD_GET_INFO2_RESP) {
    if (frame.data_len < GET_INFO2_RESPONSE_MIN_DATA_LEN) {
      detail::log_frame_issue(this, "rx", "unsupported_payload", frame, frame_length(frame));
      return;
    }

    // INFO2 is metadata, not movement state. Only learn type from radio if still UNKNOWN;
    // YAML-declared type takes priority.
    const bool type_was_unknown = dev.type == DeviceType::UNKNOWN;
    apply_info2_response(dev, frame);
    ESP_LOGI(detail::TAG, "Device %s: type=%s (%u) class=%s profile=%s subtype=%u", id.c_str(),
             device_type_name(dev.type), static_cast<unsigned>(dev.type), device_capability_class_name(dev.type),
             device_operation_profile_name(dev.type, dev.subtype), dev.subtype);
    if (type_was_unknown && dev.type != DeviceType::UNKNOWN) {
      ESP_LOGI(detail::TAG,
               "Device %s: type learned at runtime, not declared in YAML — add `%s` to skip "
               "re-learning it on every future boot",
               id.c_str(), detail::describe_learned_device_type(dev.type).c_str());
    }
    return;
  }

  if (frame.cmd == CMD_ERROR_RESP) {
    if (frame.data_len < ERROR_RESPONSE_MIN_DATA_LEN) {
      detail::log_frame_issue(this, "rx", "unsupported_payload", frame, frame_length(frame));
      return;
    }

    detail::record_command_result(dev, id, frame.data[0]);
    this->notify_device_update_(id);
    return;
  }
}

void IOHomeControlComponent::record_1w_activity_(uint32_t now) {
  // A gap of a full quiet period or more since the last frame means the previous burst already
  // released any deferred poll, so this frame starts a new burst window rather than extending the
  // old one (which would make ONEWAY_POLL_DEFER_CAP_MS fire on the very next frame).
  if (decisions::oneway_burst_started_fresh(this->last_1w_activity_ms_, now, ONEWAY_QUIET_PERIOD_MS))
    this->first_1w_activity_ms_ = now;
  this->last_1w_activity_ms_ = now;
}

void IOHomeControlComponent::record_oneway_pairing_gesture_(const IoFrame &frame, uint32_t now) {
  if (!decisions::is_one_way_pairing_gesture((frame.ctrl0 & CTRL0_PROTOCOL_1W) != 0, frame.dst, frame.cmd))
    return;
  memcpy(this->recent_oneway_pairing_sighting_.src, frame.src, NODE_ID_SIZE);
  memcpy(this->recent_oneway_pairing_sighting_.dst, frame.dst, NODE_ID_SIZE);
  this->recent_oneway_pairing_sighting_.cmd = frame.cmd;
  this->recent_oneway_pairing_sighting_.rssi = this->radio_->get_last_capture().rssi_dbm;
  this->recent_oneway_pairing_sighting_.seen_ms = now;
}

/// @brief Log an overheard limitation request/reply decoded at DEBUG; any other frame is ignored.
static void log_overheard_limitation_read(const IoFrame &frame) {
  if (frame.cmd == CMD_LIMITATION_STATUS_REQ) {
    ESP_LOGD(detail::TAG, "rx %s", detail::describe_limitation_request(frame).c_str());
  } else if (frame.cmd == CMD_LIMITATION_STATUS_RESP) {
    ESP_LOGD(detail::TAG, "rx %s", detail::describe_limitation_reply(frame).c_str());
  }
}

void IOHomeControlComponent::log_heard_controller_(const IoFrame &frame) {
  const bool is_request = (frame.ctrl0 & CTRL0_START) != 0 && (frame.ctrl0 & CTRL0_END) == 0;
  if (!is_request || memcmp(frame.src, this->node_id_, NODE_ID_SIZE) == 0)
    return;
  const std::string src_id = node_id_to_string(frame.src);
  if (this->registry_.get(src_id) != nullptr)
    return;  // one of our own devices, not a controller
  if (!this->heard_sources_.should_log(frame.src, millis()))
    return;
  ESP_LOGI(detail::TAG, "Heard a request from %s: %s(0x%02X) to %s", src_id.c_str(), command_name(frame.cmd), frame.cmd,
           node_id_to_string(frame.dst).c_str());
}

void IOHomeControlComponent::handle_overheard_status_(const IoFrame &frame, uint8_t len) {
  const std::string src_id = node_id_to_string(frame.src);
  IoDevice *dev = this->get_device(src_id);
  if (dev == nullptr) {
    // Neither ours nor addressed to us: a neighbour's traffic, debug only.
    detail::log_frame_issue(this, "rx", "unauthenticated_status_ignored", frame, len);
    return;
  }
  detail::update_link_health(*dev, this->radio_);
  const std::string dst_id = node_id_to_string(frame.dst);
  // A reply addressed to us reaches this point only outside an exchange (a late reply after the
  // exchange gave up), so it has its own wording.
  const bool to_us = memcmp(frame.dst, this->node_id_, NODE_ID_SIZE) == 0;
  if (!to_us && this->overheard_status_sources_.should_log(frame.src, millis())) {
    ESP_LOGI(
        detail::TAG,
        "Device %s answered another controller (%s): %s(0x%02X) not applied, only this hub's own reads set its state",
        src_id.c_str(), dst_id.c_str(), command_name(frame.cmd), frame.cmd);
    return;
  }
  ESP_LOGD(detail::TAG, "rx %s cmd=%s(0x%02X) src=%s dst=%s len=%u data_len=%u not applied",
           to_us ? "unsolicited_status" : "overheard_status", command_name(frame.cmd), frame.cmd, src_id.c_str(),
           dst_id.c_str(), len, frame.data_len);
}

void IOHomeControlComponent::process_received_packet_(const RadioRxPacket &packet) {
  IoFrame frame;
  if (!parse(packet.data, packet.len, frame)) {
    detail::log_component_capture(this->radio_, "parse_fail", packet.data, packet.len);
    return;
  }

  detail::log_component_capture(this->radio_, "parse_ok", packet.data, packet.len, &frame);

  // === Key-extraction responder ("Accept Foreign Pairing") ===
  // Runs before the exchange-internal drop below: a hub-issued 0x3C challenging our own 0x37 is
  // addressed to us and is ours to answer (key_extraction_responder.cpp). Self-gated on armed + our
  // throwaway ID, so the disarmed path (and every command other than 0x28/0x2C/0x31/0x32/0x36/0x3C)
  // is bit-for-bit unchanged by this ordering — try_handle_frame() returns false immediately
  // whenever it doesn't apply, and this reorder can only affect frames where BOTH this call and
  // the is_exchange_internal_command() check below would otherwise fire, i.e. only 0x3C/0x3D (that
  // predicate's entire domain, hub_decisions.h).
  if (this->key_extraction_.try_handle_frame(frame))
    return;

  // Exchange-internal frames (0x3C challenge request, 0x3D challenge response) belonging to
  // *another* controller's authenticated exchange carry no extractable status data for a passive
  // observer — skip silently. They remain visible in io_capture (stage=parse_ok).
  // A registered device's challenge to another controller may still warrant a poll of our own
  // (note_foreign_challenge_()); its content is never applied.
  if (decisions::is_exchange_internal_command(frame.cmd)) {
    this->note_foreign_challenge_(frame);
    return;
  }

  // === 1W remote frame decode ===
  // 1W remotes broadcast commands to a typed device-class address (e.g., "all awnings").
  // Decode the frame content for diagnostic logging, then fall through to linked_remotes
  // handling which may schedule a status poll for devices this remote controls.
  if ((frame.ctrl0 & CTRL0_PROTOCOL_1W) != 0) {
    const std::string src_id = node_id_to_string(frame.src);
    const uint32_t now = millis();

    // Any 1W frame means a remote is transmitting right now, duplicate or not — record it before
    // the dedup check so loop() keeps background polls off the radio for the rest of the burst.
    this->record_1w_activity_(now);
    // Remember a pairing-gesture sighting the same way, before dedup, so a repeated gesture frame
    // still refreshes the timestamp (issue #27/#65) — see record_oneway_pairing_gesture_()'s doc
    // comment for why the discovery telemetry window alone isn't enough to catch this.
    this->record_oneway_pairing_gesture_(frame, now);

    // Decode once and reuse for the dedup key, logging, and (when it carries a command intent) the
    // sender HA event, so a physical remote press (or sensor trigger) can drive automations directly.
    // The decode must happen *before* the dedup check: a move and a stop share the CMD_EXECUTE
    // command byte and are told apart only by the decoded intent, which is part of the key.
    const OneWayFrameInfo info = decode_1w_frame(frame);

    // Opt-in, receive-only 1W key adoption (oneway_key_adoption.cpp). Both calls sit after
    // the parse and before the dedup check so an add-controller broadcast is seen even when its
    // repeats would collapse into one logical press. Both self-gate on armed, so the disarmed
    // path is unchanged; they observe the frame rather than consuming it, and execution always
    // continues into the normal logging path below. Order matters only in that the class
    // observation must be recorded before an adoption can consume it.
    this->oneway_key_adoption_.record_observed_class(info);
    this->oneway_key_adoption_.try_adopt(frame);

    // 1W remotes repeat each command 4× at 40ms intervals across channels, and a held button keeps
    // resending. Collapse that into one logical press per remote+command+intent (plus destination for
    // intent-less frames, so each class of a multi-class sweep is kept).
    decisions::OneWayDedupState incoming{src_id, frame.cmd, info.has_intent, info.main0, info.main1, now};
    memcpy(incoming.dst, frame.dst, NODE_ID_SIZE);
    if (decisions::is_duplicate_1w_frame(this->last_1w_logged_, incoming, detail::ONEWAY_DEDUP_WINDOW_MS))
      return;
    this->last_1w_logged_ = incoming;

    const std::vector<std::string> *linked = this->registry_.linked_devices(src_id);
    detail::log_1w_remote_frame(info, linked);
    this->maybe_fire_sender_event_(info, linked != nullptr && !linked->empty(), src_id);
    // Id-linked devices plus, for a typed broadcast, class-linked devices — deduplicated so a
    // device linked both ways is only touched once per press.
    const std::vector<std::string> target_devices = this->resolve_1w_target_devices_(info, src_id);
    const bool is_stop = this->apply_optimistic_linked_state_(info, target_devices);
    this->schedule_device_polls_(target_devices, is_stop ? 0 : REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS);
    return;
  }

  // A request overheard on air: say who sent it, so a user can press a button on a remote and read
  // its node ID for the pull_key action. Each source at most once per HeardSourceLog::INTERVAL_MS,
  // so a neighbour's controller polling its devices does not fill the log.
  this->log_heard_controller_(frame);

  // Limitation reads between other parties (a KLF 200 polling a window) are only rendered, so a
  // passive monitor's log shows what they asked and answered. Nothing is applied -- the sender's
  // key is unproven (ADR 0022) -- and the frame continues into the normal handling below.
  log_overheard_limitation_read(frame);

  if (frame.cmd == CMD_STATUS_UPDATE && memcmp(frame.dst, this->node_id_, NODE_ID_SIZE) == 0) {
    if (this->authenticate_request_(frame, packet.freq_hz)) {
      IoFrame resp;
      if (!create_status_update_resp(resp, this->node_id_, frame.src)) {
        detail::log_frame_issue(this, "rx", "ack_build_failed", frame, packet.len);
        return;
      }
      // Device-originated updates may arrive while the sender and receiver are not aligned on the
      // same hop channel anymore. Broadcasting the ACK across all three IO-homecontrol channels
      // matched the behavior of real controllers and made updates reliable in practice.
      const uint16_t ack_preamble = this->radio_->response_preamble();
      this->transmit_frame_(resp, FREQ_CH1, ack_preamble);
      this->transmit_frame_(resp, FREQ_CH2, ack_preamble);
      this->transmit_frame_(resp, FREQ_CH3, ack_preamble);
      this->update_device_status_(frame);
    } else {
      detail::log_frame_issue(this, "rx", "auth_failed", frame, packet.len);
    }
    return;
  }

  if (frame.cmd == CMD_PRIVATE_RESP || frame.cmd == CMD_STATUS_UPDATE) {
    // Passive receive mode can still observe replies/status from other exchanges (another
    // controller sharing a device, or an attacker who knows the device's node ID -- node IDs
    // travel in the clear, see README.md's "Reporting Unsupported Devices"). Nothing here proves
    // the frame's source currently holds the system key, so its content is never applied -- see
    // ADR 0022. The frame only stamps the device's link health (handle_overheard_status_()); whether
    // the request that drew it warrants a poll of our own was decided when that request was heard
    // (act_on_overheard_request_()).
    this->handle_overheard_status_(frame, packet.len);
    return;
  }

  // A frame of another controller to one of our registered devices, or from a linked 2W
  // controller: act_on_overheard_request_() decides whether it warrants a poll of our own.
  if (this->act_on_overheard_request_(frame))
    return;

  detail::log_frame_issue(this, "rx", "unhandled_cmd", frame, packet.len);

  // If the command is not in our known set AND the frame was addressed to our hub, it may be a
  // protocol extension we should support — ask the user to report it. Frames merely overheard
  // between other devices (not addressed to us) are still logged above at debug level, but do
  // not warrant a warning: we are not a party to that exchange, so there is nothing to add.
  const bool addressed_to_us = memcmp(frame.dst, this->node_id_, NODE_ID_SIZE) == 0;
  if (addressed_to_us && std::strcmp(command_name(frame.cmd), "UNKNOWN_CMD") == 0) {
    const std::string src_id = node_id_to_string(frame.src);
    ESP_LOGW(detail::TAG,
             "Received unknown command 0x%02X from %s. "
             "If you see this repeatedly, please file a GitHub issue with this command ID, "
             "your device model, and the log context so protocol support can be extended.",
             frame.cmd, src_id.c_str());
  }
}

}  // namespace home_io_control
}  // namespace esphome