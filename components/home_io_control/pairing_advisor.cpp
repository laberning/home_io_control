/// @file pairing_advisor.cpp
/// @brief Read-only advisor that turns PairingTelemetry into actionable diagnostics.
/// @ingroup hioc_hub

#include "pairing_advisor.h"

#include "proto_constants.h"
#include "proto_timing.h"

#include <cstdio>
#include <cstring>

namespace esphome {
namespace home_io_control {
namespace advisor {

namespace {

/// Buffer size for the rendered advice message.
constexpr size_t ADVICE_MESSAGE_BUFFER_SIZE = 224;

bool is_rx_kind(PairingTelemetryEventKind kind) {
  return kind == PairingTelemetryEventKind::RX || kind == PairingTelemetryEventKind::RX_REJECT;
}

/// 1W pairing traffic (issue #27 case): CTRL0 1W bit set, addressed to the 1W broadcast address
/// 0x00003F, with one of the three command bytes observed in the field capture — 0x20
/// (WRITE_PRIVATE), 0x39 (1W remove), or 0x2E (alternate discovery, 1W-flagged).
bool is_oneway_pairing_frame(const PairingTelemetryEvent &event) {
  if (!event.oneway)
    return false;
  if (memcmp(event.dst_node, BROADCAST_DISCOVER_ALT, NODE_ID_SIZE) != 0)
    return false;
  return event.cmd == CMD_WRITE_PRIVATE || event.cmd == CMD_ONEWAY_REMOVE || event.cmd == CMD_DISCOVER_ALT_REQ;
}

/// Count how many RX/RX_REJECT events share `src_node`.
uint8_t count_occurrences_of_source(const PairingTelemetryEvent *events, uint8_t event_count,
                                    const uint8_t src_node[NODE_ID_SIZE]) {
  uint8_t occurrences = 0;
  for (uint8_t i = 0; i < event_count; i++) {
    if (is_rx_kind(events[i].kind) && memcmp(events[i].src_node, src_node, NODE_ID_SIZE) == 0)
      occurrences++;
  }
  return occurrences;
}

void append_one_way_pairing_advice(const PairingTelemetryEvent *events, uint8_t event_count, PairingAdvice out[],
                                   uint8_t &count) {
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
    if (!is_rx_kind(event.kind))
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

}  // namespace

uint8_t analyze_pairing_telemetry(const PairingTelemetry &telemetry, const uint8_t own_node_id[NODE_ID_SIZE],
                                  PairingAdvice out[PAIRING_ADVICE_MAX]) {
  uint8_t count = 0;
  const PairingTelemetryEvent *events = telemetry.events();
  const uint8_t event_count = telemetry.event_count();

  append_one_way_pairing_advice(events, event_count, out, count);
  append_channel_busy_advice(telemetry, events, event_count, out, count);
  append_foreign_controller_advice(own_node_id, events, event_count, out, count);

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
    case PairingAdviceCode::NONE:
    default:
      return "none";
  }
}

std::string pairing_advice_message(const PairingAdvice &advice) {
  char buf[ADVICE_MESSAGE_BUFFER_SIZE];
  switch (advice.code) {
    case PairingAdviceCode::ONE_WAY_PAIRING_TRAFFIC:
      snprintf(buf, sizeof(buf),
               "A 1W remote (src %s) is performing 1W pairing. The motor is NOT in 2W learning mode - a PROG "
               "press on a 1W remote does not enable 2W discovery. Use the Double Power Cut procedure to force "
               "2W learning mode.",
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
      return "Nothing heard on any channel during the whole discovery window - check the antenna/RF path before "
             "assuming the device isn't in pairing mode.";
    case PairingAdviceCode::NONE:
    default:
      return "";
  }
}

}  // namespace advisor
}  // namespace home_io_control
}  // namespace esphome
