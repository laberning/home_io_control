/// @file roll_call.cpp
/// @brief The roll-call sweep: keyed `0x2A` broadcasts over channels and power classes.
/// @ingroup hioc_hub

#include "roll_call.h"

#include "log_helpers.h"
#include "proto_constants.h"
#include "proto_device_model.h"

#include <cstring>
#include <string>

namespace esphome {
namespace home_io_control {

ScanAddResult add_scan_responder(ScanResponder *responders, uint8_t &count, uint8_t capacity, const IoFrame &frame,
                                 int16_t rssi_dbm, uint8_t key_slot, const ScanResponder *&entry) {
  for (uint8_t i = 0; i < count; i++) {
    if (memcmp(responders[i].src, frame.src, NODE_ID_SIZE) == 0) {
      entry = &responders[i];
      return ScanAddResult::DUPLICATE;
    }
  }
  if (count >= capacity) {
    entry = nullptr;
    return ScanAddResult::FULL;
  }

  // decode_discovery_response() also produces the hex device-ID string, which is deliberately
  // discarded here and rebuilt from `src` when the report is formatted. Keeping it would mean a
  // std::string per entry — more memory than the entire ScanResponder — to save re-deriving six
  // characters that fit in a small-string buffer. Do not "optimise" this by adding a string member.
  IoDevice device{};
  std::string unused_device_id;
  const DiscoveryResponseInfo info = decode_discovery_response(frame, device, unused_device_id);

  ScanResponder &new_entry = responders[count++];
  memcpy(new_entry.src, frame.src, NODE_ID_SIZE);
  new_entry.type = device.type;
  new_entry.subtype = device.subtype;
  new_entry.inverted = device.inverted;
  new_entry.rssi_dbm = rssi_dbm;
  new_entry.manufacturer = info.manufacturer;
  new_entry.flags = info.flags;
  new_entry.has_extended = info.has_extended;
  new_entry.metadata_complete = info.metadata_complete;
  new_entry.key_slot = key_slot;
  entry = &new_entry;
  return ScanAddResult::ADDED;
}

uint8_t count_enabled_passes(ScanPowerClasses selection) {
  uint8_t count = 0;
  for (const RollCallPass &pass : ROLL_CALL_PASSES) {
    if (scan_power_classes_include(selection, pass.power_save_mode))
      count++;
  }
  return count;
}

namespace {

/// @brief Per-attempt facts a roll-call reply's DEBUG line needs, kept in one small struct so the
/// reply lambda's capture list stays a couple of pointers rather than growing one field at a time.
struct RollCallAttemptContext {
  const RollCallPass *pass;
  uint8_t attempt;      ///< Global attempt number (1-based, across all passes).
  uint32_t tx_freq_hz;  ///< Channel this attempt transmitted the request on.
  uint8_t key_slot;     ///< Key slot of the sweep this attempt belongs to.
};

/// @brief Record one accepted roll-call reply into the scan state, and log it.
///
/// Reads back `add_scan_responder()`'s out-parameter rather than decoding the frame a second time
/// to learn the responder's self-reported power-save class. RSSI in the log line is this reply's
/// own `info.rssi_dbm`, not the value stored on first sighting, so a later, stronger or weaker
/// reception of an already-known responder is still visible. Duplicates are logged too — a repeat
/// still answers "which pass heard this device".
void record_roll_call_reply(ScanState &state, const RollCallAttemptContext &ctx, const IoFrame &frame,
                            const ExchangeEngine::BroadcastReplyInfo &info) {
  const ScanResponder *entry = nullptr;
  if (add_scan_responder(state.responders, state.count, SCAN_MAX_REPLIES, frame, info.rssi_dbm, ctx.key_slot, entry) ==
      ScanAddResult::FULL) {
    state.truncated = true;
  }

  const char *power_save = (entry != nullptr && entry->has_extended)
                               ? power_save_mode_name(discovery_power_save_mode(entry->flags))
                               : "unknown";
  ESP_LOGD(detail::TAG,
           "Roll-call reply src=%s pass=%s attempt=%u tx=%" PRIu32 " rx=%" PRIu32 " +%" PRIu32
           "ms rssi=%d power_save=%s",
           node_id_to_string(frame.src).c_str(), power_save_mode_name(ctx.pass->power_save_mode), ctx.attempt,
           ctx.tx_freq_hz, info.rx_freq_hz, info.after_tx_ms, info.rssi_dbm, power_save);
}

/// @brief Transmit one roll-call attempt and collect its replies into `state`.
///
/// Builds a fresh request every call (create_discovery_request() draws a new random nonce each
/// time) rather than replaying one frame across attempts, and logs one attempt-summary DEBUG line
/// after collection closes. The pass determines the frame shape (CTRL1, and therefore preamble via
/// request_preamble_for()'s rule) and the listen policy; this function does not choose either.
RollCallAttemptResult run_roll_call_attempt(ExchangeEngine &engine, const uint8_t *node_id, const uint8_t *key,
                                            uint8_t key_slot, const TuningConfig &tuning, const RollCallPass &pass,
                                            uint32_t tx_freq_hz, uint8_t attempt, uint8_t total_attempts,
                                            ScanState &state) {
  IoFrame request;
  const bool low_power = pass.power_save_mode == POWER_SAVE_LOW_POWER;
  if (!create_discovery_request(request, node_id, CMD_DISCOVER_SPE_REQ, BROADCAST_DISCOVER, low_power, pass.ack_capable,
                                /*payload_enabled=*/false, /*payload=*/0, key)) {
    return RollCallAttemptResult::BUILD_FAILED;
  }

  const uint8_t before = state.count;
  RollCallAttemptContext ctx{&pass, attempt, tx_freq_hz, key_slot};
  const uint8_t heard = engine.collect_broadcast_responses(
      request, tx_freq_hz, CMD_DISCOVER_SPE_RESP, tuning.pairing_discovery_wait_ms,
      [&state, &ctx](const IoFrame &frame, const ExchangeEngine::BroadcastReplyInfo &info) {
        record_roll_call_reply(state, ctx, frame, info);
      },
      pass.listen_policy);

  // Diagnostic only (not part of the user-facing report). Reporting heard and new separately is
  // what makes it useful: "heard 3, 0 new" means devices are answering every attempt (so the
  // extra channels are redundant here). ctrl1 is read back from the built frame rather than
  // re-derived from the pass, so this line can never disagree with what was actually transmitted.
  const uint8_t new_count = state.count - before;
  ESP_LOGD(detail::TAG,
           "Roll-call attempt %u/%u (pass=%s, ctrl1=0x%02X, tx %" PRIu32 " Hz, %u ms window): %u repl%s heard, %u new",
           attempt, total_attempts, power_save_mode_name(pass.power_save_mode), request.ctrl1, tx_freq_hz,
           tuning.pairing_discovery_wait_ms, heard, heard == 1 ? "y" : "ies", new_count);
  return RollCallAttemptResult::OK;
}

}  // namespace

RollCallAttemptResult run_roll_call_sweep(ExchangeEngine &engine, const uint8_t *node_id, const uint8_t *key,
                                          uint8_t key_slot, const TuningConfig &tuning, ScanState &state,
                                          uint8_t &attempt, uint8_t total_attempts) {
  const ScanPowerClasses selection = tuning.scan_power_classes;
  for (const RollCallPass &pass : ROLL_CALL_PASSES) {
    if (!scan_power_classes_include(selection, pass.power_save_mode))
      continue;
    for (const uint32_t tx_freq_hz : SCAN_CHANNELS) {
      attempt++;
      if (run_roll_call_attempt(engine, node_id, key, key_slot, tuning, pass, tx_freq_hz, attempt, total_attempts,
                                state) == RollCallAttemptResult::BUILD_FAILED) {
        return RollCallAttemptResult::BUILD_FAILED;
      }
    }
  }
  return RollCallAttemptResult::OK;
}

namespace {

/// The `hint:` lines for a responder the registry knows: a power-save class that disagrees with its
/// YAML `low_power`, and an answer under another key than its YAML uses.
///
/// Advice only: this never touches the registry or the device's runtime low_power property (an
/// explicit YAML override stays authoritative). A responder without extended metadata has nothing to
/// compare, so it gets no power-save hint either way. A power_save value outside the two known classes
/// (2 or 3) is not evidence of either class, so it gets none either: only an exact match against one
/// of the two known classes triggers advice.
std::string known_responder_hints(const ScanResponder &responder, const IoDevice &registered,
                                  const RollCallReplyKeys &keys, uint8_t power_save) {
  std::string hints;
  if (responder.has_extended) {
    if (power_save == POWER_SAVE_LOW_POWER && !registered.low_power) {
      hints += "    hint: reports power_save=" + std::string(power_save_mode_name(power_save)) +
               " but its YAML has no low_power: true; add it or directed commands may miss it while it "
               "sleeps\n";
    } else if (power_save == POWER_SAVE_ALWAYS_ALIVE && registered.low_power) {
      hints += "    hint: reports power_save=" + std::string(power_save_mode_name(power_save)) +
               " but its YAML sets low_power: true; remove it, the " + std::to_string(LONG_PREAMBLE) +
               "-byte wake-up preamble can stop an always-alive receiver answering\n";
    }
  }
  // The exchanges with this device use its YAML key (or the hub's). If it answered a roll-call built
  // with another key, every exchange will fail.
  const uint8_t *yaml_key = registered.has_system_key ? registered.system_key : keys.hub;
  if (memcmp(yaml_key, keys.answered, AES_KEY_SIZE) != 0) {
    hints += "    hint: answered under the key with check value " + detail::format_key_check_value(keys.answered) +
             " but its YAML uses the key with check value " + detail::format_key_check_value(yaml_key) +
             "; fix its system_key:\n";
  }
  return hints;
}

/// The paste-ready part of an unknown responder's entry, or the "not supported yet" line.
/// @param key The per-device key it answered under, or nullptr for the hub key.
std::string unknown_responder_block(const ScanResponder &responder, const std::string &device_id, const uint8_t *key,
                                    RollCallKeyDisplay display, bool low_power) {
  const bool with_key = key != nullptr && display != RollCallKeyDisplay::HIDDEN;
  const std::string snippet =
      build_device_yaml_snippet(responder.type, responder.subtype, device_id, responder.metadata_complete,
                                responder.inverted, low_power, with_key ? detail::format_key_hex(key) : std::string());
  if (snippet.empty()) {
    if (key != nullptr && display == RollCallKeyDisplay::PLAIN) {
      return "    not supported yet. Its system_key, for when support lands: \"" + detail::format_key_hex(key) + "\"\n";
    }
    return "    not supported yet\n";
  }
  if (key == nullptr || display == RollCallKeyDisplay::PLAIN)
    return "    Paste this into your YAML to register it:\n" + snippet;
  if (display == RollCallKeyDisplay::HIDDEN) {
    return "    Paste this into your YAML to register it, and add this device's system_key: (the key is printed in "
           "the device log, not here).\n" +
           snippet;
  }
  // The snippet carries the device's own key: the one place this report prints key material, so it sits
  // inside the same kind of banner the key-extraction and key-pull reports use.
  return "    DEVICE KEY BELOW -- DO NOT SHARE IT. Keep it in secrets.yaml and write `system_key: !secret ...`.\n"
         "    Paste this into your YAML to register it:\n" +
         snippet + "    END OF DEVICE KEY BLOCK\n";
}

}  // namespace

std::string format_roll_call_responder(const ScanResponder &responder, const std::string &device_id,
                                       const IoDevice *registered, const RollCallReplyKeys &keys,
                                       RollCallKeyDisplay display) {
  const uint8_t *key = keys.per_device ? keys.answered : nullptr;
  std::string line = "  " + device_id + ": " + format_device_kind(responder.type, responder.subtype) + " (" +
                     format_device_ids(responder.type, responder.subtype) + ")";
  if (key != nullptr)
    line += " key=" + detail::format_key_check_value(key);
  line += " rssi=" + std::to_string(responder.rssi_dbm) + "dBm";
  uint8_t power_save = 0;
  if (responder.has_extended) {
    uint8_t const att = discovery_att_class(responder.flags);
    power_save = discovery_power_save_mode(responder.flags);
    line += std::string(" manufacturer=") + manufacturer_name(responder.manufacturer) +
            " turnaround=" + att_class_name(att) + " power_save=" + power_save_mode_name(power_save);
  }
  line += registered != nullptr ? " [known]\n" : " [unknown]\n";

  if (registered != nullptr)
    return line + known_responder_hints(responder, *registered, keys, power_save);
  const bool low_power = responder.has_extended && power_save == POWER_SAVE_LOW_POWER;
  return line + unknown_responder_block(responder, device_id, key, display, low_power);
}

std::string build_hub_key_pull_report(const PulledKeyReportInfo &info, const ScanState &state,
                                      DeviceRegistry &registry) {
  if (state.count == 0)
    return "";
  std::string report = "Device " + info.pairing_device_id + " (" + format_device_kind(info.type, info.subtype) + ", " +
                       format_device_ids(info.type, info.subtype) +
                       ") answered the key pull with this hub's own key. A roll-call under it found " +
                       std::to_string(state.count) + " device" + (state.count == 1 ? "" : "s") + " that hold it:\n";
  const RollCallReplyKeys keys{info.hub_key, info.hub_key, false};
  for (uint8_t i = 0; i < state.count; i++) {
    const std::string device_id = node_id_to_string(state.responders[i].src);
    report += format_roll_call_responder(state.responders[i], device_id, registry.get(device_id), keys,
                                         RollCallKeyDisplay::HIDDEN);
  }
  if (state.truncated)
    report += "NOTE: more devices answered than the list holds; it is truncated.\n";
  return report;
}

std::string build_pulled_key_report(const PulledKeyReportInfo &info, const ScanState &state, DeviceRegistry &registry) {
  const std::string key_hex = detail::format_key_hex(info.key);
  std::string report = "DEVICE KEY RECEIVED -- DO NOT SHARE IT\n";
  report += "Device " + info.pairing_device_id + " (" + format_device_kind(info.type, info.subtype) + ", " +
            format_device_ids(info.type, info.subtype) +
            ") holds a different system key than this hub (key check value " +
            detail::format_key_check_value(info.key) + ") and proved it holds it.\n";
  report += "Anyone with this key can control every device that holds it. Keep it in secrets.yaml and write\n"
            "`system_key: !secret ...` in the entity of each device.\n";
  if (state.count == 0) {
    report += "A roll-call under that key found no device. Put this key into the system_key: of the entity that\n"
              "controls the device:\n"
              "    system_key: \"" +
              key_hex + "\"\n";
  } else {
    report += "A roll-call under that key found " + std::to_string(state.count) + " device" +
              (state.count == 1 ? "" : "s") + " that hold it. Paste a block per device:\n";
    const RollCallReplyKeys keys{info.key, info.hub_key, true};
    for (uint8_t i = 0; i < state.count; i++) {
      const std::string device_id = node_id_to_string(state.responders[i].src);
      report += format_roll_call_responder(state.responders[i], device_id, registry.get(device_id), keys,
                                           RollCallKeyDisplay::PLAIN);
    }
    if (state.truncated)
      report += "NOTE: more devices answered than the list holds; it is truncated.\n";
  }
  report += "END OF DEVICE KEY REPORT";
  return report;
}

}  // namespace home_io_control
}  // namespace esphome
