#pragma once

/// @file hub_internal.h
/// @brief Helpers shared only by the hub's own implementation files (`hub_*.cpp`).
/// @ingroup hioc_hub
///
/// Collaborators and entities include log_helpers.h / entity_helpers.h instead; `make
/// include-graph` enforces this. Keeping these helpers here leaves hub_core.h focused on the
/// public component shape and the member-function declarations.

#include "hub_core.h"
#include "entity_helpers.h"
#include "decisions_status.h"
#include "device_yaml_snippet.h"
#include "log_frame.h"
#include "log_helpers.h"
#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_device_kinds.h"
#include "proto_device_types.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace esphome {
namespace home_io_control {
namespace detail {

// ============================================================================
// Shared constants
// ============================================================================

// ============================================================================
// Capability and entity-profile helpers
// ============================================================================

/// @brief Is the given position value an on/off binary encoding?
/// @param position Position value to test.
/// @return true if position equals BINARY_ENTITY_ON_POSITION or BINARY_ENTITY_OFF_POSITION.
inline bool is_binary_entity_position(uint8_t position) {
  return position == BINARY_ENTITY_ON_POSITION || position == BINARY_ENTITY_OFF_POSITION;
}

/// @brief Does the device's (type, subtype) row match the expected HA entity class?
/// UNKNOWN devices always match to keep imported/discovered devices working.
/// @param dev IoDevice to check.
/// @param expected Desired capability class (COVER, LIGHT, SWITCH, etc.).
/// @return true if the device's row class matches or the type is UNKNOWN.
inline bool known_device_matches_entity_class(const IoDevice &dev, DeviceCapabilityClass expected) {
  return dev.type == DeviceType::UNKNOWN || device_capability_class(dev.type, dev.subtype) == expected;
}

/// @brief Does the device kind answer a status read at all?
/// @details False for a kind with device_feature::NO_STATUS_READ, which keeps its values in data objects: the hub
///          never arms, queues or sends a status poll for it (the request after boot, a refresh, the poll after
///          another controller's traffic). The one place that rule lives.
/// @param dev IoDevice to check.
inline bool device_reads_status(const IoDevice &dev) {
  return !has_feature(dev.features, device_feature::NO_STATUS_READ);
}

/// @brief Does the device support status requests?
/// UNKNOWN devices pass through.
/// @param dev IoDevice to check.
/// @return true if the device's (type, subtype) row is polled (status request or heater poll), or
///         the type is UNKNOWN.
inline bool known_device_supports_status_requests(const IoDevice &dev) {
  // A heater is polled over its register protocol rather than with the 0x03 status request, but it
  // is polled all the same, and so is a water heater (HeaterController::poll()).
  return device_reads_status(dev) &&
         (dev.type == DeviceType::UNKNOWN || device_takes_execute_position(dev.type, dev.subtype) ||
          device_supports_climate_control(dev.type, dev.subtype) ||
          has_feature(dev.features, device_feature::WATER_HEATER));
}

/// @brief Does the device report its movement through status replies?
/// Covers, lights, switches and locks do: their 0x04 reply says whether they are travelling. A
/// heater never moves (device_has_no_motion()), whether it takes a heat level in the position field
/// or is driven over its register protocol. A device not yet typed passes.
/// @param dev IoDevice to check.
/// @return true if the device takes execute positions and can move, or is UNKNOWN.
inline bool known_device_reports_motion_through_status(const IoDevice &dev) {
  return dev.type == DeviceType::UNKNOWN ||
         (device_takes_execute_position(dev.type, dev.subtype) && !device_has_no_motion(dev.type, dev.subtype));
}

/// @brief Can this device accept an execute (position) command?
/// The device's entity class must have an EXECUTE position path (cover, light, switch, lock, heater
/// level; a device not yet typed passes), and the main parameter the position maps to must be one
/// its features accept: open, close and stop always, a percent only with LEVELS
/// (device_accepts_main_parameter()). A light's LEVELS come from its `dimmable:` key, a switch or
/// lock never has them, so they stay binary.
/// @param dev IoDevice to check.
/// @param position Position value being sent (percent, or POS_STOP).
/// @return true if operation is appropriate for this device.
inline bool known_device_accepts_execute_position(const IoDevice &dev, uint8_t position) {
  if (dev.type != DeviceType::UNKNOWN && !device_takes_execute_position(dev.type, dev.subtype))
    return false;
  return device_accepts_main_parameter(dev.features, position_main_parameter(position));
}

/// @brief Can this device accept a tilt command?
/// @param dev IoDevice to check.
/// @return true only if the device has device_feature::TILT.
inline bool known_device_accepts_execute_tilt(const IoDevice &dev) {
  return has_feature(dev.features, device_feature::TILT);
}

// ============================================================================
// Logging helpers
// ============================================================================

/// @brief Log a frame‑level issue (unregistered endpoints, unsupported commands).
///
/// Logs on FRAME_TAG so the lines can be filtered on their own. The level follows
/// decisions::frame_issue_is_warning(): WARN when the frame is addressed to a registered device or to
/// this hub, DEBUG for overheard traffic. Reasons that are real problems (auth_failed, ack_build_failed,
/// unsupported_payload) only arise behind a dst == own node ID check or on replies to our own requests,
/// so they stay at WARN without an override.
/// @param component Pointer to the component (for device lookup).
/// @param own_node_id The hub's 3-byte node ID.
/// @param direction "tx" or "rx".
/// @param reason Short issue label (e.g., "unregistered_device").
/// @param frame Parsed frame.
/// @param len Serialized length.
inline void log_frame_issue(IOHomeControlComponent *component, const uint8_t *own_node_id, const char *direction,
                            const char *reason, const IoFrame &frame, uint8_t len) {
  const std::string src_id = node_id_to_string(frame.src);
  const std::string dst_id = node_id_to_string(frame.dst);
  const bool src_registered = component->get_device(src_id) != nullptr;
  const bool dst_registered = component->get_device(dst_id) != nullptr;
  const bool dst_is_us = std::memcmp(frame.dst, own_node_id, NODE_ID_SIZE) == 0;

  if (decisions::frame_issue_is_warning(dst_registered, dst_is_us)) {
    ESP_LOGW(FRAME_TAG, "%s issue=%s cmd=%s(0x%02X) src=%s%s dst=%s%s len=%u data_len=%u", direction, reason,
             command_name(frame.cmd), frame.cmd, src_id.c_str(), src_registered ? " (registered)" : "", dst_id.c_str(),
             dst_registered ? " (registered)" : "", len, frame.data_len);
    return;
  }

  ESP_LOGD(FRAME_TAG, "%s issue=%s cmd=%s(0x%02X) src=%s%s dst=%s len=%u data_len=%u", direction, reason,
           command_name(frame.cmd), frame.cmd, src_id.c_str(), src_registered ? " (registered)" : "", dst_id.c_str(),
           len, frame.data_len);
}

// ============================================================================
// 1W remote frame decode
// ============================================================================

/// @brief Log an already-decoded 1W remote frame at DEBUG level.
///
/// Formats a concise DEBUG log line showing remote ID, destination class, command intent, and
/// priority. When the remote is linked to devices, appends the linked device IDs. Takes the
/// already-decoded OneWayFrameInfo so callers that also build a HA event (see
/// build_sender_event_data()) decode the frame once, not twice.
/// @param info Already-decoded 1W frame info (see decode_1w_frame()).
/// @param linked_devices Optional pointer to device IDs this remote is linked to.
inline void log_1w_remote_frame(const OneWayFrameInfo &info, const std::vector<std::string> *linked_devices = nullptr) {
  const std::string src_id = node_id_to_string(info.src);

  // Resolve the broadcast target label: "all" for BROADCAST_ALL, otherwise the device type name.
  // Shared with the TX path (OneWayTransmitter::send_burst()) via oneway_target_label() so the two
  // cannot render the same address differently.
  //
  // Rendered as `dst-class=`, never "targets X": this is the class encoded in the frame's
  // destination, not a statement about the device the remote drives. A Somfy remote sends every
  // press twice, once to the device-class broadcast and once to the all-devices broadcast,
  // whatever the channel actually controls — so an awning channel logs a light-class frame. Worded
  // as "targets light" it read as a claim about the device, and cost an issue reporter two rounds
  // chasing a channel mix-up that never happened.
  const char *target_label = oneway_target_label(info);

  // Build optional suffix showing linked devices.
  std::string suffix;
  if (linked_devices != nullptr && !linked_devices->empty()) {
    suffix = " (linked →";
    for (const auto &dev_id : *linked_devices) {
      suffix += ' ';
      suffix += dev_id;
    }
    suffix += ')';
  }

  if (info.has_intent && info.mode_switch != OneWayModeSwitch::NONE) {
    // An A/M wall switch's flip: the STOP is how the switch reports it, so name the reported state.
    ESP_LOGD(TAG, "rx 1W remote %s dst-class=%s: %s(0x%02X) %s originator=%s priority=%s am_state=%s(0x%02X)%s",
             src_id.c_str(), target_label, command_name(info.cmd), info.cmd, info.intent,
             originator_name(info.originator), acei_level_name(info.acei_level),
             oneway_mode_switch_name(info.mode_switch), info.mode_switch_raw, suffix.c_str());
    return;
  }

  if (info.has_intent) {
    ESP_LOGD(TAG, "rx 1W remote %s dst-class=%s: %s(0x%02X) %s originator=%s priority=%s%s", src_id.c_str(),
             target_label, command_name(info.cmd), info.cmd, info.intent, originator_name(info.originator),
             acei_level_name(info.acei_level), suffix.c_str());
    return;
  }

  ESP_LOGD(TAG, "rx 1W remote %s dst-class=%s: %s(0x%02X) data_len=%u%s", src_id.c_str(), target_label,
           command_name(info.cmd), info.cmd, info.data_len, suffix.c_str());
}

/// @brief Home Assistant event fired when a decoded 1W frame carries a command intent from an
/// exposed sender (a physical remote button press, or a wind/rain sensor's triggered command).
inline constexpr const char *ONEWAY_SENDER_EVENT = "esphome.home_io_control_sender_event";

/// @brief Build the YAML line to add once a device's type is learned at runtime.
///
/// Logged when `io_device_type` was left unset in YAML and an INFO2 response just resolved it
/// for the first time this boot (ADR 0018: nothing persists, so this repeats on every reboot
/// until the user copies the line in). Reuses format_device_type_for_yaml() so the exact syntax always
/// matches what the pairing snippet and the Python schema accept.
/// @param type The now-known device type. Must not be DeviceType::UNKNOWN.
/// @return The YAML line to add, e.g. `io_device_type: "interior_venetian_blind"` or `io_device_type: 0x11`.
inline std::string describe_learned_device_type(DeviceType type) {
  return "io_device_type: " + format_device_type_for_yaml(type);
}

/// @brief Build the Home Assistant event data map for a decoded 1W sender frame.
///
/// Only meaningful when `info.has_intent` is true (the caller gates emission on that); the
/// `intent` field is only populated by decode_1w_frame() in that case.
///
/// An A/M wall switch's flip adds `am_state` (`auto`, `manual` or `unknown`); every other frame
/// leaves the key out, so an automation can tell a flip from an ordinary STOP press by its presence.
/// @param info Already-decoded 1W frame info (see decode_1w_frame()).
/// @param linked True if this sender is linked to at least one registered device.
/// @return Event data map ready for fire_homeassistant_event().
inline std::map<std::string, std::string> build_sender_event_data(const OneWayFrameInfo &info, bool linked) {
  std::map<std::string, std::string> data{
      {"remote_id", node_id_to_string(info.src)},
      {"target_class", address_class_name(info.address_class)},
      {"target_type", device_type_name(info.target_type)},
      {"cmd", format_name_and_hex(command_name(info.cmd), info.cmd)},
      {"intent", info.intent},
      {"originator", originator_name(info.originator)},
      {"acei_level", acei_level_name(info.acei_level)},
      {"linked", linked ? "true" : "false"},
  };
  if (info.mode_switch != OneWayModeSwitch::NONE)
    data.emplace("am_state", oneway_mode_switch_name(info.mode_switch));
  return data;
}

// ============================================================================
// Status normalization helpers
// ============================================================================

/// @brief Normalize the stopped flag of an execute ack against the last observed target and position.
///
/// Used only on the execute-ack path (`trust_position = false`), whose own position bytes are not
/// decoded: a "stopped" ack is read as moving while the last *trusted* target and position still
/// disagree. Trusted reports go through decisions::classify_stopped_report() instead, which
/// accepts a cover resting off its target once a second report agrees.
///
/// Deliberately reads and writes observed fields only — never effective_*(). On the execute-ack
/// path `dev.target` does not hold the commanded value, so this function does not use it to flip
/// `is_stopped` back to false — effective_is_stopped() owns that.
/// @param dev Device record to update (may clear is_stopped if positions differ).
inline void normalize_stopped_state(IoDevice &dev) {
  if (dev.is_stopped && dev.target != UNKNOWN_POSITION && dev.position != UNKNOWN_POSITION &&
      !has_reached_target_position(dev.target, dev.position)) {
    dev.is_stopped = false;
  }
}

/// @brief Record that an outbound exchange to this device timed out (no valid response).
///
/// Called once per failed exchange from exchange_and_record_()'s "no valid response"
/// branch — the single place every device-directed exchange already reads
/// ExchangeEngine::DebugInfo. Both counters saturate at UINT16_MAX instead of wrapping,
/// matching PairingTelemetry's counters.
/// @param dev Device the failed exchange was addressed to.
/// @param tries Number of attempts the failed exchange made (`DebugInfo::tries`, 1-based).
inline void record_exchange_timeout(IoDevice &dev, uint8_t tries) {
  if (dev.exchange_timeout_count < UINT16_MAX)
    dev.exchange_timeout_count++;
  dev.exchange_attempt_count =
      static_cast<uint16_t>(std::min<uint32_t>(static_cast<uint32_t>(dev.exchange_attempt_count) + tries, UINT16_MAX));
}

/// @brief Record what an outbound exchange's ending says about the device, whichever way the caller
/// then classifies the outcome.
///
/// - `ExchangeOutcome::SUCCESS_UNCONFIRMED` (the device answered the request with a challenge, we
///   answered that, and the closing reply never arrived) counts in exchange_unconfirmed_count. A
///   CMD_EXECUTE treats that outcome as success and records no failure, so this counter is the only
///   place it is visible to the diagnostics at all. Saturates at UINT16_MAX instead of wrapping,
///   like the counters above.
/// - A CMD_EXECUTE that got a reply (`ExchangeOutcome::SUCCESS_WITH_RESPONSE`, a status or an error
///   alike, since either one closes the exchange) marks the device as one that confirms, which is
///   what lets a later unconfirmed EXECUTE to it be sent again.
/// @param dev         Device the exchange was addressed to.
/// @param request_cmd Command byte of the request.
/// @param outcome     How the exchange ended.
inline void record_exchange_outcome(IoDevice &dev, uint8_t request_cmd, ExchangeOutcome outcome) {
  if (outcome == ExchangeOutcome::SUCCESS_UNCONFIRMED && dev.exchange_unconfirmed_count < UINT16_MAX)
    dev.exchange_unconfirmed_count++;
  if (outcome == ExchangeOutcome::SUCCESS_WITH_RESPONSE && request_cmd == CMD_EXECUTE)
    dev.confirms_execute = true;
}

/// @brief Store a decoded record on the device, if it is valid.
///
/// A short or unpopulated payload leaves whatever was last learned in place rather than clearing
/// it: the record is inherently last-writer-wins and only refreshes when something commands the
/// device, so a stale value is the honest answer and a blanked one is not.
inline void apply_last_command_record(IoDevice &dev, const LastCommandRecord &record) {
  if (!record.valid)
    return;
  memcpy(dev.last_commander, record.commander, NODE_ID_SIZE);
  dev.last_command_originator = record.originator;
  dev.has_last_command = true;
}

/// @brief Refresh the derived rain-limitation flag from a status reply the device just sent.
///
/// Call after the reply's position and last-command record are applied. Stamps the rain-evidence
/// memory when the device names the rain sensor, drops it once a stopped device sits where the
/// hub had predicted (the clamp is over), then derives `limited_by_rain` through
/// decisions::rain_limitation_rule(). Logs every change at DEBUG with its inputs: a clamped
/// command's last-command record has not been observed in the field, so these lines are how field
/// reports settle which evidence path a real window takes.
/// @param id Device ID, for the log line.
/// @param dev Device the reply came from.
/// @param predicted_target `dev.optimistic.target` as it stood before the reply was decoded
///        (decoding clears it), or UNKNOWN_POSITION.
/// @param record_in_reply True when *this* reply carried a well-formed last-command record. The
///        record on the device is last-writer-wins and persists across replies, so only a reply
///        that carried it may refresh the rain-evidence memory; a shorter reply must not keep
///        stale evidence alive.
/// @param now_ms When the reply was received.
inline void update_rain_limitation(const std::string &id, IoDevice &dev, float predicted_target, bool record_in_reply,
                                   uint32_t now_ms) {
  const bool evidence_recent =
      dev.last_rain_evidence_ms != 0 && (now_ms - dev.last_rain_evidence_ms) <= RAIN_EVIDENCE_HOLD_MS;
  const bool names_rain = dev.has_last_command && dev.last_command_originator == ORIGINATOR_RAIN_SENSOR;
  if (names_rain && record_in_reply) {
    dev.last_rain_evidence_ms = now_ms;
  } else if (dev.is_stopped && has_reached_target_position(predicted_target, dev.target)) {
    dev.last_rain_evidence_ms = 0;
  }

  const decisions::RainLimitationRule rule =
      decisions::rain_limitation_rule({dev.has_last_command, dev.last_command_originator, evidence_recent,
                                       predicted_target, dev.target, dev.is_stopped});
  const bool limited = rule != decisions::RainLimitationRule::NONE;
  if (limited != dev.limited_by_rain) {
    ESP_LOGD(TAG, "Device %s: rain limitation %s (rule=%s originator=0x%02X predicted=%.0f%% observed=%.0f%% %s)",
             id.c_str(), limited ? "set" : "cleared", decisions::rain_limitation_rule_name(rule),
             dev.last_command_originator, static_cast<double>(predicted_target), static_cast<double>(dev.target),
             dev.is_stopped ? "stopped" : "moving");
  }
  dev.limited_by_rain = limited;
}

/// @brief Describe a 0x71 status update's Command Originator as "name(0xXX)".
///
/// Pure rather than inlined into the log line it feeds, so the offset is testable: ESP_LOG* is a
/// no-op stub in host tests, which makes the rendered line itself unobservable.
/// @param frame A CMD_STATUS_UPDATE frame.
/// @return "name(0xXX)", e.g. "user_remote(0x01)", or an empty string when the payload is too
///         short to carry the byte. Shorter frames still carry usable position data — the branch
///         gate is STATUS_UPDATE_MIN_DATA_LEN (11) — they just have no originator to report.
inline std::string describe_status_update_originator(const IoFrame &frame) {
  if (frame.data_len <= STATUS_UPDATE_ORIGINATOR_OFFSET)
    return {};
  return format_originator(frame.data[STATUS_UPDATE_ORIGINATOR_OFFSET]);
}

/// @brief Describe the hub's live optimistic predictions where they disagree with the observation.
///
/// Pure rather than inlined into log_status_update() so it is testable (ESP_LOG* is a no-op stub in
/// host tests). log_status_update() runs from the inbound frame handlers and reports what the device
/// said; a hub-side prediction must never be substituted into those observed fields, so it is
/// appended as a clearly-labelled annotation instead. Only terms that actually differ are rendered —
/// a prediction that merely confirms the observation adds nothing.
/// @param dev Device record to read.
/// @return e.g. " [predicted: target=100% stopped]", or an empty string when no prediction stands
///         or every prediction agrees with what the device reported.
inline std::string describe_prediction(const IoDevice &dev) {
  const bool target_differs = effective_target(dev) != dev.target;
  const bool motion_differs = effective_is_stopped(dev) != dev.is_stopped;
  if (!target_differs && !motion_differs)
    return {};
  std::string out = " [predicted:";
  if (target_differs)
    out += " target=" + format_position(effective_target(dev));
  if (motion_differs)
    out += effective_is_stopped(dev) ? " stopped" : " moving";
  out += "]";
  return out;
}

/// @brief Describe the device's battery level for the status line.
///
/// Pure for the same reason as describe_prediction(). Rendered whenever the device reports a level,
/// and also as "not_reported" when the YAML asked for battery entities (`battery: true`), so a
/// device that reports none says so on every status line instead of only once.
/// @param dev Device record to read.
/// @return e.g. " battery=full", or an empty string for a device without battery entities that
///         reports no level.
inline std::string describe_battery(const IoDevice &dev) {
  const BatteryState battery = battery_state_of(dev);
  if (battery == BatteryState::NOT_REPORTED && !reading_asked(dev, DeviceReading::BATTERY_STATE))
    return {};
  return std::string(" battery=") + battery_state_name(battery);
}

/// @brief True while a device that reported stopped off its target waits for the confirming poll.
///
/// The hub handles it as moving until decisions::classify_stopped_report() confirms the stop.
/// @param dev Device record to read.
inline bool is_settling(const IoDevice &dev) { return !dev.is_stopped && dev.stop_candidate.armed(); }

/// @brief True when the remembered off-target stop is the device's last trusted report.
///
/// Then the gap between `dev.target` and `dev.position` is a rest the hub has seen reported, not a
/// movement, and normalize_stopped_state() must not read it as one.
/// @param dev Device record to read.
inline bool stop_candidate_matches_observation(const IoDevice &dev) {
  return dev.stop_candidate.armed() && has_reached_target_position(dev.stop_candidate.target, dev.target) &&
         has_reached_target_position(dev.stop_candidate.position, dev.position);
}

/// @brief Name the device's observed movement state for the status line.
///
/// "settling" is a device that reported stopped off its target and waits for the confirming poll
/// (decisions::classify_stopped_report()); it is still handled as moving.
/// @param dev Device record to read.
/// @return "stopped", "settling" or "moving".
inline const char *describe_motion(const IoDevice &dev) {
  if (dev.is_stopped)
    return "stopped";
  return is_settling(dev) ? "settling" : "moving";
}

/// @brief Log a concise status‑update line used by inbound handlers.
///
/// The position/target/motion fields report what the device observed — never a hub prediction. A
/// diverging live prediction is appended by describe_prediction(), clearly labelled, not merged in.
/// @param id Device ID.
/// @param dev Current device state.
/// @param suffix Optional suffix added after the state string (e.g., " (status update)").
inline void log_status_update(const std::string &id, const IoDevice &dev, const char *suffix = "") {
  ESP_LOGI(TAG, "Device %s: position=%s target=%s %s%s%s%s", id.c_str(), format_position(dev.position).c_str(),
           format_position(dev.target).c_str(), describe_motion(dev), describe_battery(dev).c_str(),
           describe_prediction(dev).c_str(), suffix);
}

/// @brief Log a decoded CMD_ERROR_RESP result with optional request-command context.
/// @param id Device ID.
/// @param result Result byte from CMD_ERROR_RESP data[0].
/// @param request_cmd Original outbound request command when known.
/// @param include_request_cmd True to include request_cmd in the log line.
inline void log_command_result(const std::string &id, uint8_t result, uint8_t request_cmd = 0,
                               bool include_request_cmd = false) {
  const char *kind = is_limitation_result(result) ? "limitation" : "error";
  if (include_request_cmd) {
    ESP_LOGW(TAG, "Device %s: %s (0x%02X) returned %s result=0x%02X %s (%s)", id.c_str(), command_name(request_cmd),
             request_cmd, kind, result, command_result_name(result), command_result_description(result));
    return;
  }

  ESP_LOGW(TAG, "Device %s: explicit %s result=0x%02X %s (%s)", id.c_str(), kind, result, command_result_name(result),
           command_result_description(result));
}

/// @brief Store a decoded CMD_ERROR_RESP result on the device and log it.
///
/// Single place both CMD_ERROR_RESP call sites (the unsolicited status path and the reply to
/// our own EXECUTE) route through, so the store-and-log policy cannot drift between them. Does
/// not notify subscribers itself — callers already call notify_device_update_() once per
/// handled frame; call it after this.
/// @param dev Device that returned the result.
/// @param id Device ID (for the log line).
/// @param result Result byte from CMD_ERROR_RESP data[0].
/// @param request_cmd Original outbound request command when known.
/// @param include_request_cmd True to include request_cmd context in the log line.
inline void record_command_result(IoDevice &dev, const std::string &id, uint8_t result, uint8_t request_cmd = 0,
                                  bool include_request_cmd = false) {
  dev.last_result_code = result;
  dev.last_result_at_ms = millis();
  log_command_result(id, result, request_cmd, include_request_cmd);
}

/// @brief Log the result code in the reply to an EXECUTE, when it is not 0.
///
/// A device that does not take the speed block says so with code 5 on every move, so that case warns
/// once per device and boot (IoDevice::speed_block_warned); every other non-zero code warns the first time
/// the device answers with it and logs at DEBUG afterwards (IoDevice::reported_result_codes). The Last
/// Result Code and Active Issue sensors keep the meaning of a 0xFE result.
/// @param dev Device that replied (its warned flag or its reported codes are set).
/// @param id Device ID (for the log line).
/// @param request The EXECUTE the reply answers.
/// @param status_byte Byte 0 of the reply's payload.
inline void log_execute_result_code(IoDevice &dev, const std::string &id, const IoFrame &request, uint8_t status_byte) {
  const uint8_t code = result_code_of(status_byte);
  switch (decisions::classify_execute_result_code(status_byte, execute_carries_speed_block(request),
                                                  dev.reported_result_codes)) {
    case decisions::ExecuteResultCodeReport::NONE:
      return;
    case decisions::ExecuteResultCodeReport::SPEED_BLOCK_UNKNOWN:
      if (!dev.speed_block_warned) {
        dev.speed_block_warned = true;
        ESP_LOGW(
            TAG,
            "Device %s: does not take a speed setting (result code %u %s); remove travel_speed or set it to default",
            id.c_str(), code, result_code_name(code));
      }
      return;
    case decisions::ExecuteResultCodeReport::OTHER_CODE:
      dev.reported_result_codes |= decisions::result_code_bit(code);
      ESP_LOGW(TAG, "Device %s: EXECUTE answered with result code %u %s (reply byte 0x%02X)", id.c_str(), code,
               result_code_name(code), status_byte);
      return;
    case decisions::ExecuteResultCodeReport::OTHER_CODE_REPEATED:
      ESP_LOGD(TAG, "Device %s: EXECUTE answered with result code %u %s (reply byte 0x%02X)", id.c_str(), code,
               result_code_name(code), status_byte);
      return;
  }
}

}  // namespace detail
}  // namespace home_io_control
}  // namespace esphome
