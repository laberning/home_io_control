/// @file roll_call.cpp
/// @brief The roll-call sweep: keyed `0x2A` broadcasts over channels and power classes.
/// @ingroup hioc_hub

#include "roll_call.h"

#include "log_helpers.h"

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

}  // namespace home_io_control
}  // namespace esphome
