/// @file pairing_advisor.cpp
/// @brief Read-only advisor that turns PairingTelemetry into actionable diagnostics.
/// @ingroup hioc_hub

#include "pairing_advisor.h"

#include "hub_decisions.h"
#include "proto_constants.h"
#include "proto_timing.h"

#include <cstdio>
#include <cstring>

namespace esphome {
namespace home_io_control {
namespace advisor {

namespace {

/// Buffer size for the rendered advice message. Sized for the longest message plus its node ID,
/// and kept under ESPHome's 512-byte log line so the WARN line isn't truncated on hardware — the
/// longest message plus the "Pairing advisor: " prefix comes to ~400 bytes on the wire.
constexpr size_t ADVICE_MESSAGE_BUFFER_SIZE = 416;
/// Buffer for the " answered our key with 0xNN NAME;" fragment: the longest result name is 32
/// characters, plus 29 characters of fixed text and the terminator, with room to spare.
constexpr size_t RESULT_CODE_TEXT_BUFFER_SIZE = 72;

bool is_rx_kind(PairingTelemetryEventKind kind) {
  return kind == PairingTelemetryEventKind::RX || kind == PairingTelemetryEventKind::RX_REJECT;
}

/// A seeded pre-window sighting (PairingTelemetry::record_recent_one_way_sighting()) rather than a
/// live in-window capture — see PairingTelemetryEvent::aux's doc comment. Excluded from
/// channel_busy specifically: that advice is about *live* channel contention during this attempt,
/// and a sighting from up to PAIRING_RECENT_ONE_WAY_SIGHTING_WINDOW_MS earlier carries no evidence
/// about that. Still eligible for ONE_WAY_PAIRING_TRAFFIC, which is about the gesture itself.
bool is_seeded_sighting(const PairingTelemetryEvent &event) {
  return event.kind == PairingTelemetryEventKind::RX && event.aux == 1;
}

/// 1W pairing traffic (issue #27 case). Thin wrapper over the shared predicate (hub_decisions.h)
/// so the hub's passive RX path can classify the same frame shape without duplicating it — see
/// decisions::is_one_way_pairing_gesture()'s doc comment for the frame shape itself.
bool is_oneway_pairing_frame(const PairingTelemetryEvent &event) {
  return decisions::is_one_way_pairing_gesture(event.oneway, event.dst_node, event.cmd);
}

/// Count how many *live* RX/RX_REJECT events share `src_node` — excludes a seeded pre-window
/// sighting, which is not evidence of live channel contention (see is_seeded_sighting()).
uint8_t count_occurrences_of_source(const PairingTelemetryEvent *events, uint8_t event_count,
                                    const uint8_t src_node[NODE_ID_SIZE]) {
  uint8_t occurrences = 0;
  for (uint8_t i = 0; i < event_count; i++) {
    if (is_rx_kind(events[i].kind) && !is_seeded_sighting(events[i]) &&
        memcmp(events[i].src_node, src_node, NODE_ID_SIZE) == 0)
      occurrences++;
  }
  return occurrences;
}

void append_one_way_pairing_advice(const PairingTelemetry &telemetry, const PairingTelemetryEvent *events,
                                   uint8_t event_count, PairingAdvice out[], uint8_t &count) {
  // This advice's message tells the user no device answered their PROG gesture. On a successful
  // pairing that is simply false, and a PROG press is the normal prelude to one — so the advice
  // fired on most successes, warning about a failure that did not happen. The sibling advices are
  // not gated: channel_busy stays true on a success, and foreign_controller matters *more* there
  // (it is the case where the device that answered may not have been addressing us).
  if (telemetry.outcome() == PairingOutcome::PAIRED)
    return;
  for (uint8_t i = 0; i < event_count && count < PAIRING_ADVICE_MAX; i++) {
    const PairingTelemetryEvent &event = events[i];
    if (!is_rx_kind(event.kind) || !is_oneway_pairing_frame(event))
      continue;
    PairingAdvice &advice = out[count++];
    advice.code = PairingAdviceCode::ONE_WAY_PAIRING_TRAFFIC;
    memcpy(advice.subject_node, event.src_node, NODE_ID_SIZE);
    advice.rssi = event.rssi;
    return;
  }
}

void append_channel_busy_advice(const PairingTelemetry &telemetry, const PairingTelemetryEvent *events,
                                uint8_t event_count, PairingAdvice out[], uint8_t &count) {
  if (count >= PAIRING_ADVICE_MAX || telemetry.lbt_retries() < LBT_MAX_RETRIES)
    return;
  for (uint8_t i = 0; i < event_count; i++) {
    const PairingTelemetryEvent &event = events[i];
    if (!is_rx_kind(event.kind) || is_seeded_sighting(event))
      continue;
    if (count_occurrences_of_source(events, event_count, event.src_node) < 2)
      continue;
    PairingAdvice &advice = out[count++];
    advice.code = PairingAdviceCode::CHANNEL_BUSY_LBT_DELAYED;
    memcpy(advice.subject_node, event.src_node, NODE_ID_SIZE);
    advice.rssi = event.rssi;
    return;
  }
}

void append_foreign_controller_advice(const uint8_t own_node_id[NODE_ID_SIZE], const PairingTelemetryEvent *events,
                                      uint8_t event_count, PairingAdvice out[], uint8_t &count) {
  static const uint8_t ZERO_NODE_ID[NODE_ID_SIZE] = {0, 0, 0};
  // An all-zero own_node_id means the controller's node ID isn't provisioned yet — every
  // discovery response would look "foreign" by the memcmp below, so skip the check entirely
  // rather than emit a false advisory during first-time setup.
  if (memcmp(own_node_id, ZERO_NODE_ID, NODE_ID_SIZE) == 0)
    return;
  for (uint8_t i = 0; i < event_count && count < PAIRING_ADVICE_MAX; i++) {
    const PairingTelemetryEvent &event = events[i];
    if (!is_rx_kind(event.kind) || event.cmd != CMD_DISCOVER_RESP)
      continue;
    if (memcmp(event.dst_node, own_node_id, NODE_ID_SIZE) == 0)
      continue;
    PairingAdvice &advice = out[count++];
    advice.code = PairingAdviceCode::FOREIGN_CONTROLLER_PAIRING;
    memcpy(advice.subject_node, event.src_node, NODE_ID_SIZE);
    advice.rssi = event.rssi;
    return;
  }
}

/// Key-step advice: only for attempts that did not pair and got as far as discovering a device.
/// A pull gets its own three codes: a proven key received (how to use it), refused (the device does
/// not hand out its key), or answered with a key it did not prove. For a push, "command unknown", or a failed push to a
/// device that answered discovery as a remote controller, points at the pull direction; any other refusal is reported
/// with its code.
void append_key_step_advice(const PairingTelemetry &telemetry, PairingAdvice out[], uint8_t &count) {
  if (count >= PAIRING_ADVICE_MAX || !telemetry.has_discovered_device())
    return;
  // Every key-step advice is about the discovered device and carries its refusal code, if any.
  auto add = [&](PairingAdviceCode code) -> PairingAdvice & {
    PairingAdvice &advice = out[count++];
    advice.code = code;
    memcpy(advice.subject_node, telemetry.discovered_node_id(), NODE_ID_SIZE);
    advice.has_result_code = telemetry.has_key_refusal();
    advice.result_code = telemetry.key_refusal_code();
    return advice;
  };
  const PairingOutcome outcome = telemetry.outcome();
  if (outcome == PairingOutcome::KEY_RECEIVED) {
    add(PairingAdviceCode::PULLED_KEY_RECEIVED);
    return;
  }
  if (outcome == PairingOutcome::HUB_KEY_RETURNED) {
    add(PairingAdviceCode::HUB_KEY_NO_RESPONDER);
    return;
  }
  if (outcome != PairingOutcome::KEY_EXCHANGE_FAILED && outcome != PairingOutcome::KEY_TRANSFER_REFUSED)
    return;
  if (telemetry.key_pull_attempted()) {
    if (telemetry.has_key_refusal()) {
      add(PairingAdviceCode::KEY_PULL_NOT_OFFERED);
    } else if (telemetry.key_pull_unproven()) {
      add(PairingAdviceCode::KEY_PULL_UNPROVEN);
    }
    return;
  }
  // This attempt pushed: "command unknown", or a failed push to a device that answered discovery as a
  // remote controller, points at the pull direction.
  const bool as_controller = telemetry.discovered_node_type() == DeviceType::REMOTE_CONTROLLER;
  const bool command_unknown = telemetry.has_key_refusal() && telemetry.key_refusal_code() == RESULT_COMMAND_UNKNOWN;
  if (command_unknown || as_controller) {
    add(PairingAdviceCode::KEY_PUSH_NOT_ACCEPTED).answered_as_controller = as_controller;
  } else if (telemetry.has_key_refusal()) {
    add(PairingAdviceCode::KEY_TRANSFER_REFUSED);
  }
}

}  // namespace

uint8_t analyze_pairing_telemetry(const PairingTelemetry &telemetry, const uint8_t own_node_id[NODE_ID_SIZE],
                                  PairingAdvice out[PAIRING_ADVICE_MAX]) {
  uint8_t count = 0;
  const PairingTelemetryEvent *events = telemetry.events();
  const uint8_t event_count = telemetry.event_count();

  append_one_way_pairing_advice(telemetry, events, event_count, out, count);
  append_channel_busy_advice(telemetry, events, event_count, out, count);
  append_foreign_controller_advice(own_node_id, events, event_count, out, count);
  append_key_step_advice(telemetry, out, count);

  if (count == 0 && telemetry.heard_count() == 0 && count < PAIRING_ADVICE_MAX) {
    out[count++].code = PairingAdviceCode::RF_SILENT;
  }

  return count;
}

const char *pairing_advice_code_name(PairingAdviceCode code) {
  switch (code) {
    case PairingAdviceCode::ONE_WAY_PAIRING_TRAFFIC:
      return "1w_traffic";
    case PairingAdviceCode::CHANNEL_BUSY_LBT_DELAYED:
      return "channel_busy";
    case PairingAdviceCode::FOREIGN_CONTROLLER_PAIRING:
      return "foreign_controller";
    case PairingAdviceCode::RF_SILENT:
      return "rf_silent";
    case PairingAdviceCode::KEY_PUSH_NOT_ACCEPTED:
      return "key_push_not_accepted";
    case PairingAdviceCode::KEY_TRANSFER_REFUSED:
      return "key_refused";
    case PairingAdviceCode::KEY_PULL_NOT_OFFERED:
      return "key_pull_not_offered";
    case PairingAdviceCode::KEY_PULL_UNPROVEN:
      return "key_pull_unproven";
    case PairingAdviceCode::PULLED_KEY_RECEIVED:
      return "pulled_key_received";
    case PairingAdviceCode::HUB_KEY_NO_RESPONDER:
      return "hub_key_no_responder";
    case PairingAdviceCode::NONE:
    default:
      return "none";
  }
}

std::string pairing_advice_message(const PairingAdvice &advice) {
  char buf[ADVICE_MESSAGE_BUFFER_SIZE];
  switch (advice.code) {
    case PairingAdviceCode::ONE_WAY_PAIRING_TRAFFIC:
      // Deliberately no longer ends on "and retry", which reads as "press PROG again now": on a remote
      // already registered to the device, a PROG press is also the add/remove toggle, so a second one
      // can close the window the first opened. "Can", not "does" — this fits the field reports we have
      // rather than being proven, and one press per attempt costs nothing either way.
      snprintf(buf, sizeof(buf),
               "A 1W remote (src %s) made a PROG gesture, but no device answered. If a 2W hub already "
               "controls the device, use key extraction. Otherwise hold PROG ~2 s on a remote registered to "
               "it, release at the first jog, and keep only that device in pairing mode. Use one PROG press "
               "per attempt - a second press can close the window the first one opened. A reset alone is "
               "not a pairing gesture.",
               node_id_to_string(advice.subject_node).c_str());
      return std::string(buf);
    case PairingAdviceCode::CHANNEL_BUSY_LBT_DELAYED:
      snprintf(buf, sizeof(buf),
               "Channel busy (device %s beaconing at %d dBm); discovery TX was delayed by listen-before-talk.",
               node_id_to_string(advice.subject_node).c_str(), advice.rssi);
      return std::string(buf);
    case PairingAdviceCode::FOREIGN_CONTROLLER_PAIRING:
      snprintf(buf, sizeof(buf), "Another controller is pairing device %s right now.",
               node_id_to_string(advice.subject_node).c_str());
      return std::string(buf);
    case PairingAdviceCode::RF_SILENT:
      return "Nothing heard on any channel during the whole discovery window. If this log shows other "
             "IO-Homecontrol frames being received, the radio works and the device did not answer; otherwise "
             "check the antenna/RF path.";
    case PairingAdviceCode::KEY_PUSH_NOT_ACCEPTED: {
      char code_text[RESULT_CODE_TEXT_BUFFER_SIZE] = "";
      if (advice.has_result_code) {
        snprintf(code_text, sizeof(code_text), " answered our key with 0x%02X %s;", advice.result_code,
                 command_result_name(advice.result_code));
      }
      snprintf(buf, sizeof(buf),
               "Device %s does not take a pushed key:%s%s It may hand over its own key instead: set "
               "`pairing_key_transfer: pull` (or `auto`, the default, which pulls from a device that answers as a "
               "remote controller) under `tuning:`, put the device back into pairing mode and pair again.",
               node_id_to_string(advice.subject_node).c_str(), code_text,
               advice.answered_as_controller ? " it answered discovery as a remote controller (node type 1023)." : "");
      return std::string(buf);
    }
    case PairingAdviceCode::KEY_TRANSFER_REFUSED:
      snprintf(buf, sizeof(buf),
               "Device %s refused the key transfer with 0x%02X %s (%s). If it was paired before, run Scan Paired "
               "Devices before retrying: it may already hold a key.",
               node_id_to_string(advice.subject_node).c_str(), advice.result_code,
               command_result_name(advice.result_code), command_result_description(advice.result_code));
      return std::string(buf);
    case PairingAdviceCode::KEY_PULL_NOT_OFFERED:
      snprintf(buf, sizeof(buf),
               "Device %s does not hand out its key: it answered the key request with 0x%02X %s. Set "
               "`pairing_key_transfer: push` to force the usual pairing, put the device back into pairing mode and "
               "pair again.",
               node_id_to_string(advice.subject_node).c_str(), advice.result_code,
               command_result_name(advice.result_code));
      return std::string(buf);
    case PairingAdviceCode::KEY_PULL_UNPROVEN:
      snprintf(buf, sizeof(buf),
               "Device %s sent a key, but did not prove it holds it, so it is not printed. The 'Key pull "
               "check' line above contains no key and is safe to share.",
               node_id_to_string(advice.subject_node).c_str());
      return std::string(buf);
    case PairingAdviceCode::PULLED_KEY_RECEIVED:
      snprintf(buf, sizeof(buf),
               "Device %s handed over its key (printed above), but a roll-call under it found no device that holds "
               "it. Put the key into the `system_key:` of the entity that controls the device, flash, then run Scan "
               "Paired Devices: the log prints its config block. Each device of this kind keeps its own key, so "
               "pull each one and compare the key check values.",
               node_id_to_string(advice.subject_node).c_str());
      return std::string(buf);
    case PairingAdviceCode::HUB_KEY_NO_RESPONDER:
      snprintf(buf, sizeof(buf),
               "Device %s already holds this hub's key, but no device that holds it answered. The node that "
               "pairs is not always the one to control (an Atlantic radiator's heater sits at the next address). "
               "Press Scan Paired Devices, and again if nothing answers: it prints the YAML block of each device "
               "that holds the key.",
               node_id_to_string(advice.subject_node).c_str());
      return std::string(buf);
    case PairingAdviceCode::NONE:
    default:
      return "";
  }
}

}  // namespace advisor
}  // namespace home_io_control
}  // namespace esphome
