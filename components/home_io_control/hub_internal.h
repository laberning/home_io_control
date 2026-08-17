#pragma once

/// @file hub_internal.h
/// @brief Internal helpers shared by the hub implementation .cpp files.
/// @ingroup hioc_hub
///
/// This header is intentionally private to the component implementation. It keeps
/// small cross-file helpers in one place while leaving hub_core.h focused on the
/// public component shape and the member-function declarations.

#include "hub_core.h"
#include "log_frame.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace esphome {
namespace home_io_control {
namespace detail {

// ============================================================================
// Shared constants
// ============================================================================

inline constexpr const char *TAG = "home_io_control";  ///< Shared log tag for hub-level messages.
/// Suppress a repeated 1W log/poll for the same remote *and the same intent* within this window.
/// Wide on purpose: it collapses both the 4×/40ms reliability burst and a held button into one
/// logical press. A *different* intent from the same remote (a stop after a move) is not a
/// duplicate and passes through immediately — see decisions::is_duplicate_1w_frame().
inline constexpr uint32_t ONEWAY_DEDUP_WINDOW_MS = 2000;
inline constexpr float BINARY_ENTITY_ON_POSITION_THRESHOLD =
    50.0F;  ///< Shared 0-100 cutoff: values below this mean binary "on".

// ============================================================================
// Percent conversion helpers
// ============================================================================

/// @brief Convert a 0.0-1.0 HA fraction (position, tilt, or brightness) to a 0-100 IO percent.
///
/// Rounds rather than truncates: HA quantizes call values to 0-255 before they ever reach us, so
/// its "50%" is 128/255=0.50196, not exactly 0.5 — a truncating cast compounds that quantization
/// into a consistent ~1% bias, caught on real hardware in both platform_cover.cpp (position and
/// tilt) and platform_light.cpp (brightness). Callers apply their own invert/complement logic
/// (e.g. `1.0F - fraction`) before calling this; it only owns the rounding.
/// @param fraction Value in [0.0, 1.0].
/// @return Rounded 0-100 percent.
inline uint8_t round_percent(float fraction) { return static_cast<uint8_t>(std::lround(fraction * 100.0F)); }

// ============================================================================
// Capability and entity-profile helpers
// ============================================================================

/// @brief Is the given position value an on/off binary encoding?
/// @param position Position value to test.
/// @return true if position equals BINARY_ENTITY_ON_POSITION or BINARY_ENTITY_OFF_POSITION.
inline bool is_binary_entity_position(uint8_t position) {
  return position == BINARY_ENTITY_ON_POSITION || position == BINARY_ENTITY_OFF_POSITION;
}

/// @brief Does the device's type match the expected HA entity class?
/// UNKNOWN devices always match to keep imported/discovered devices working.
/// @param dev IoDevice to check.
/// @param expected Desired capability class (COVER, LIGHT, SWITCH, etc.).
/// @return true if device type matches or is UNKNOWN.
inline bool known_device_matches_entity_class(const IoDevice &dev, DeviceCapabilityClass expected) {
  return dev.type == DeviceType::UNKNOWN || device_capability_class(dev.type) == expected;
}

/// @brief Does the device support status requests?
/// UNKNOWN devices pass through.
/// @param dev IoDevice to check.
/// @return true if device type supports status requests or is UNKNOWN.
inline bool known_device_supports_status_requests(const IoDevice &dev) {
  return dev.type == DeviceType::UNKNOWN || device_supports_status_requests(dev.type);
}

/// @brief Can this device accept an execute (position) command?
/// Checks capability and, for unknown types, allows binary positions for light/switch.
/// @param dev IoDevice to check.
/// @param position Position value being sent.
/// @return true if operation is appropriate for this device type.
inline bool known_device_accepts_execute_position(const IoDevice &dev, uint8_t position) {
  if (dev.type == DeviceType::UNKNOWN)
    return true;
  if (device_supports_position_control(dev.type))
    return true;
  // Dimmable lights (platform_light.cpp's dimmable: true) send arbitrary 0-100 IO positions, not
  // just the two binary extremes — accept the full range for LIGHT here and trust the entity
  // layer to only ever send binary values for a non-dimmable light. SWITCH and LOCK have no
  // continuous concept, so they stay restricted to the binary encoding below.
  if (device_capability_class(dev.type) == DeviceCapabilityClass::LIGHT)
    return position <= BINARY_ENTITY_OFF_POSITION;
  return is_binary_entity_position(position) &&
         (device_supports_binary_control(dev.type) || device_supports_lock_control(dev.type));
}

/// @brief Can this device accept a tilt command?
/// @param dev IoDevice to check.
/// @return true only if device type is known to support tilt.
inline bool known_device_accepts_execute_tilt(const IoDevice &dev) {
  return dev.type != DeviceType::UNKNOWN && device_supports_tilt(dev.type);
}

// ============================================================================
// Logging helpers
// ============================================================================

/// @brief Log a rejected operation with capability mismatch details.
/// @param device_id Device ID string.
/// @param dev IoDevice that rejected the command.
/// @param operation Human‑readable operation name (e.g., "set position").
/// @param expected Expected capability class or profile name.
inline void log_rejected_operation(const std::string &device_id, const IoDevice &dev, const char *operation,
                                   const char *expected) {
  ESP_LOGW(TAG, "Rejecting %s for device %s: type=%s (%u) class=%s profile=%s expected=%s", operation,
           device_id.c_str(), device_type_name(dev.type), static_cast<uint8_t>(dev.type),
           device_capability_class_name(dev.type), device_operation_profile_name(dev.type), expected);
}

/// @brief Log a frame at the "io_capture" tag with structured fields.
/// Used for protocol‑level debugging (phases: component, tx, rx, parse_ok/parse_fail).
/// @param radio Radio driver instance (provides chip name and capture).
/// @param stage String label for the current phase.
/// @param buf Raw bytes being logged.
/// @param len Length of buf.
/// @param frame Optional parsed IoFrame for decoded fields (cmd, src, dst).
inline void log_component_capture(const RadioDriver *radio, const char *stage, const uint8_t *buf, uint8_t len,
                                  const IoFrame *frame = nullptr) {
  const RadioCaptureInfo &capture = radio->get_last_capture();
  char payload_hex[FRAME_LOG_HEX_BUFFER_SIZE];
  // Masks the 0x32 key-transfer payload exactly like log_frame() (log_frame.h) — this path is
  // separate from log_frame() and runs on every received frame, including a passively overheard
  // pairing exchange between two other devices, so it must carry the same redaction guarantee.
  render_frame_hex_redacted(buf, len, payload_hex, sizeof(payload_hex));
  if (frame != nullptr) {
    ESP_LOGD("io_capture",
             "chip=%s phase=component stage=%s freq=%" PRIu32 " ts=%" PRIu32
             " len=%u cmd=0x%02X src=%02X%02X%02X dst=%02X%02X%02X payload=%s",
             radio->chip_name(), stage, capture.freq_hz, capture.timestamp_ms, len, frame->cmd, frame->src[0],
             frame->src[1], frame->src[2], frame->dst[0], frame->dst[1], frame->dst[2], payload_hex);
    return;
  }
  ESP_LOGD("io_capture", "chip=%s phase=component stage=%s freq=%" PRIu32 " ts=%" PRIu32 " len=%u payload=%s",
           radio->chip_name(), stage, capture.freq_hz, capture.timestamp_ms, len, payload_hex);
}

/// @brief Log a frame‑level issue (unregistered endpoints, unsupported commands).
/// @param component Pointer to the component (for device lookup).
/// @param direction "tx" or "rx".
/// @param reason Short issue label (e.g., "unregistered_device").
/// @param frame Parsed frame.
/// @param len Serialized length.
inline void log_frame_issue(IOHomeControlComponent *component, const char *direction, const char *reason,
                            const IoFrame &frame, uint8_t len) {
  const std::string src_id = node_id_to_string(frame.src);
  const std::string dst_id = node_id_to_string(frame.dst);
  const bool src_registered = component->get_device(src_id) != nullptr;
  const bool dst_registered = component->get_device(dst_id) != nullptr;

  if (src_registered || dst_registered) {
    ESP_LOGW(TAG, "%s issue=%s cmd=%s(0x%02X) src=%s%s dst=%s%s len=%u data_len=%u", direction, reason,
             command_name(frame.cmd), frame.cmd, src_id.c_str(), src_registered ? " (registered)" : "", dst_id.c_str(),
             dst_registered ? " (registered)" : "", len, frame.data_len);
    return;
  }

  ESP_LOGD(TAG, "%s issue=%s cmd=%s(0x%02X) src=%s dst=%s len=%u data_len=%u", direction, reason,
           command_name(frame.cmd), frame.cmd, src_id.c_str(), dst_id.c_str(), len, frame.data_len);
}

// ============================================================================
// 1W remote frame decode
// ============================================================================

/// @brief Log an already-decoded 1W remote frame at DEBUG level.
///
/// Formats a concise DEBUG log line showing remote ID, target type, command intent, and
/// priority. When the remote is linked to devices, appends the linked device IDs. Takes the
/// already-decoded OneWayFrameInfo so callers that also build a HA event (see
/// build_sender_event_data()) decode the frame once, not twice.
/// @param info Already-decoded 1W frame info (see decode_1w_frame()).
/// @param linked_devices Optional pointer to device IDs this remote is linked to.
inline void log_1w_remote_frame(const OneWayFrameInfo &info, const std::vector<std::string> *linked_devices = nullptr) {
  const std::string src_id = node_id_to_string(info.src);

  // Resolve the broadcast target label: "all" for BROADCAST_ALL, otherwise the device type name.
  const char *target_label =
      (info.address_class == AddressClass::BROADCAST_ALL) ? "all" : device_type_name(info.target_type);

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

  if (info.has_intent) {
    ESP_LOGD(TAG, "rx 1W remote %s targets %s: %s(0x%02X) %s originator=%s priority=%s%s", src_id.c_str(), target_label,
             command_name(info.cmd), info.cmd, info.intent, originator_name(info.originator),
             acei_level_name(info.acei_level), suffix.c_str());
    return;
  }

  ESP_LOGD(TAG, "rx 1W remote %s targets %s: %s(0x%02X) data_len=%u%s", src_id.c_str(), target_label,
           command_name(info.cmd), info.cmd, info.data_len, suffix.c_str());
}

/// @brief Home Assistant event fired when a decoded 1W frame carries a command intent from an
/// exposed sender (a physical remote button press, or a wind/rain sensor's triggered command).
inline constexpr const char *ONEWAY_SENDER_EVENT = "esphome.home_io_control_sender_event";

/// @brief Whether a 1W sender is on the `exposed_senders` allowlist for the sender HA event.
///
/// "Sender" covers both remotes and wind/rain sensors — they use the identical 1W broadcast
/// mechanism and differ only in the `originator` byte inside the payload, not in addressing.
/// Overheard 1W traffic is always DEBUG-logged regardless of this check (see
/// log_1w_remote_frame()); this only gates whether the event reaches Home Assistant. Deliberately
/// separate from `linked_devices` — a sender can be event-enabled without controlling any
/// registered device (e.g. to trigger an HA automation with no matching cover/light/switch), or
/// vice versa.
/// @param exposed_senders Configured allowlist (`exposed_senders` YAML key, empty by default).
/// @param sender_id Node ID of the 1W sender that sent the frame.
/// @return true if the sender is in the allowlist.
inline bool is_exposed_sender(const std::vector<std::string> &exposed_senders, const std::string &sender_id) {
  return std::find(exposed_senders.begin(), exposed_senders.end(), sender_id) != exposed_senders.end();
}

/// Buffer size for format_name_and_hex(): longest command name plus "(0xXX)" and a margin.
inline constexpr size_t NAME_AND_HEX_BUFFER_SIZE = 40;

/// @brief Format a name/value pair as "name(0xXX)", e.g. "execute(0x00)".
inline std::string format_name_and_hex(const char *name, uint8_t value) {
  std::array<char, NAME_AND_HEX_BUFFER_SIZE> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%s(0x%02X)", name, value);
  return std::string(buffer.data());
}

/// Buffer size for describe_learned_device_type()'s hex fallback: "io_device_type: 0xXX" plus margin.
inline constexpr size_t LEARNED_DEVICE_TYPE_HEX_BUFFER_SIZE = 24;

/// @brief Build the YAML line to add once a device's type is learned at runtime.
///
/// Logged when `io_device_type` was left unset in YAML and an INFO2 response just resolved it
/// for the first time this boot (ADR 0018: nothing persists, so this repeats on every reboot
/// until the user copies the line in). Reuses yaml_device_type_name() so the exact syntax always
/// matches what the pairing snippet and the Python schema accept.
/// @param type The now-known device type. Must not be DeviceType::UNKNOWN.
/// @return The YAML line to add, e.g. `io_device_type: "venetian_blind"` or `io_device_type: 0x11`.
inline std::string describe_learned_device_type(DeviceType type) {
  const char *name = yaml_device_type_name(type);
  if (name != nullptr)
    return std::string("io_device_type: \"") + name + "\"";
  std::array<char, LEARNED_DEVICE_TYPE_HEX_BUFFER_SIZE> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "io_device_type: 0x%02X", static_cast<uint8_t>(type));
  return std::string(buffer.data());
}

/// @brief Build the Home Assistant event data map for a decoded 1W sender frame.
///
/// Only meaningful when `info.has_intent` is true (the caller gates emission on that); the
/// `intent` field is only populated by decode_1w_frame() in that case.
/// @param info Already-decoded 1W frame info (see decode_1w_frame()).
/// @param linked True if this sender is linked to at least one registered device.
/// @return Event data map ready for fire_homeassistant_event().
inline std::map<std::string, std::string> build_sender_event_data(const OneWayFrameInfo &info, bool linked) {
  return {
      {"remote_id", node_id_to_string(info.src)},
      {"target_class", address_class_name(info.address_class)},
      {"target_type", device_type_name(info.target_type)},
      {"cmd", format_name_and_hex(command_name(info.cmd), info.cmd)},
      {"intent", info.intent},
      {"originator", originator_name(info.originator)},
      {"acei_level", acei_level_name(info.acei_level)},
      {"linked", linked ? "true" : "false"},
  };
}

// ============================================================================
// Key-material display formatting
// ============================================================================

/// @brief Format a 16-byte key as an uppercase, unseparated hex string for display.
///
/// The one deliberate place system-key bytes are formatted for display, shared by both
/// key-recovery features so neither forks its own copy: 2W "Accept Foreign Pairing"
/// (hub_key_extraction.cpp::log_key_extraction_result_()) and 1W controller-key adoption
/// (build_oneway_adoption_report() below). See redaction.h for the masking rules this
/// intentionally does not apply to — both callers are the deliberate exception, not a loosening
/// of it.
/// @param key Pointer to AES_KEY_SIZE key bytes.
/// @return Uppercase hex string, e.g. "0102030405060708090A0B0C0D0E0F10".
inline std::string format_key_hex(const uint8_t key[AES_KEY_SIZE]) {
  std::string out;
  out.reserve(AES_KEY_SIZE * 2);
  char byte_buf[3];
  for (uint8_t i = 0; i < AES_KEY_SIZE; i++) {
    snprintf(byte_buf, sizeof(byte_buf), "%02X", key[i]);
    out += byte_buf;
  }
  return out;
}

/// @brief Log `prefix` followed by `message`, one line per log call rather than one call for the
/// whole (possibly multi-line) string.
///
/// ESPHome formats each log call into a fixed 512-byte buffer (`ESPHOME_LOGGER_TX_BUFFER_SIZE`,
/// esphome/core/defines.h) and silently truncates anything longer; a multi-line report (a YAML
/// snippet plus explanatory prose) routinely exceeds that and truncates mid-line if logged as a
/// single call — confirmed on real hardware for both call sites this function serves:
/// `scan_paired_devices()`'s report (a multi-device report cut off mid-snippet) and 1W
/// controller-key adoption's report (the recovered `system_key` line itself never made it into
/// the log at all). Splitting by line keeps every individual call's payload small regardless of
/// how long the full message is. Shared rather than duplicated a third time — a second private
/// copy is exactly how the 1W path ended up with the bug this fixes.
/// @param tag        Log tag.
/// @param is_warning True to log at WARN, false for INFO.
/// @param prefix     Prepended to the message's first line only (e.g. "Management action X: ").
/// @param message    Message to log; may contain embedded `\n` line breaks.
inline void log_multiline_result(const char *tag, bool is_warning, const std::string &prefix,
                                 const std::string &message) {
  size_t start = 0;
  bool first = true;
  while (true) {
    const size_t end = message.find('\n', start);
    const std::string line = (end == std::string::npos) ? message.substr(start) : message.substr(start, end - start);
    const std::string out = first ? prefix + line : line;
    if (is_warning) {
      ESP_LOGW(tag, "%s", out.c_str());
    } else {
      ESP_LOGI(tag, "%s", out.c_str());
    }
    first = false;
    if (end == std::string::npos || end + 1 >= message.size())
      break;
    start = end + 1;
  }
}

// ============================================================================
// 1W controller-key adoption reporting
// ============================================================================

/// @brief Human-readable name for a decoded 0x30's MAC-verification outcome.
/// @param status Outcome from decode_1w_add_controller() (see OneWayAdoptedKey::mac_status).
/// @return Short uppercase-style label used in both the summary log line and the report below.
inline const char *oneway_mac_status_name(OneWayMacStatus status) {
  switch (status) {
    case OneWayMacStatus::VERIFIED:
      return "VERIFIED";
    case OneWayMacStatus::FAILED:
      return "FAILED";
    case OneWayMacStatus::NOT_PRESENT:
    default:
      return "not present";
  }
}

/// @brief Build the full 1W controller-key-adoption report: MAC-verification status, the
/// own-address transmission rationale, and the ready-to-paste `oneway_controllers:` YAML block.
///
/// Pure — takes already-decoded values, performs no I/O — so it is directly unit-testable
/// without a live radio or a captured log line (ESP_LOG's host stub discards its arguments).
/// This is the single intentional place `adopted.system_key` is formatted for display (via
/// format_key_hex() above); the caller (hub_oneway_key_adoption.cpp) passes the returned text to
/// one ESP_LOGW(...,"%s",...) call and nowhere else.
///
/// `node_id` is deliberately never mentioned as something to fill in — a later step derives one
/// from the hub's own node ID, and the report says so rather than asking the user to invent a
/// 3-byte address. The report also explains that the hub always transmits under its own address:
/// impersonating the sender would hijack that remote's rolling sequence counter and break it.
///
/// The emitted keys must track `ONEWAY_CONTROLLER_SCHEMA` (`__init__.py`) by hand — a newly
/// required schema key needs a matching line here too. `make yaml-emitter-sync`
/// (scripts/check-yaml-emitters.py) catches drift between the two statically; it does not tell
/// you what to add here.
///
/// @param adopted Decoded controller identity from decode_1w_add_controller() (proto_codecs.h).
/// @param observed_type_known True if this sender's other 1W traffic was observed while armed
/// (see IOHomeControlComponent::record_oneway_observed_class_()); false prints a commented-out
/// fallback pointing at the DEBUG log line that would reveal it instead.
/// @param observed_type The observed target class; only meaningful when observed_type_known.
/// @return Multi-line report text, ready to pass straight to a single ESP_LOGW(...,"%s",...) call.
inline std::string build_oneway_adoption_report(const OneWayAdoptedKey &adopted, bool observed_type_known,
                                                DeviceType observed_type) {
  std::string sender_hex_lower = node_id_to_string(adopted.sender_node);
  std::transform(sender_hex_lower.begin(), sender_hex_lower.end(), sender_hex_lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  const std::string key_hex = format_key_hex(adopted.system_key);

  std::string mac_line;
  switch (adopted.mac_status) {
    case OneWayMacStatus::VERIFIED:
      mac_line = "MAC VERIFIED: this frame's MAC checked out under the recovered key -- the strongest evidence "
                 "available on the spot that it is correct.";
      break;
    case OneWayMacStatus::FAILED:
      mac_line = "MAC FAILED: this frame's MAC did NOT check out under the recovered key -- it is probably wrong. "
                 "Re-arm and repeat the key-copy gesture closer to the hub.";
      break;
    case OneWayMacStatus::NOT_PRESENT:
    default:
      mac_line = "MAC not present: this frame carried no MAC trailer to verify against -- treat this key as "
                 "unconfirmed until tested.";
      break;
  }

  // Fits "    manufacturer: 0xNN" plus its terminator with room to spare.
  constexpr size_t manufacturer_line_size = 32;
  char manufacturer_line[manufacturer_line_size];
  snprintf(manufacturer_line, sizeof(manufacturer_line), "    manufacturer: 0x%02X",
           static_cast<unsigned>(adopted.manufacturer));

  std::string type_lines;
  if (observed_type_known) {
    type_lines = "    io_device_type: " + format_device_type_for_yaml(observed_type) +
                 "      # observed from this sender's traffic; verify\n";
  } else {
    type_lines = "    # io_device_type: unknown -- no other 1W traffic was observed from this sender while armed;\n"
                 "    #   check the DEBUG \"rx 1W remote ...\" log line once you see this sender transmit again.\n";
  }

  return mac_line +
         "\nThe hub always transmits under its own node_id, never the sender's -- copying the sender's address "
         "would hijack its rolling sequence counter and break its existing remote.\n"
         "Copy the block below into your hub's YAML.\n"
         "oneway_controllers:\n"
         "  # node_id omitted -> derived from your hub node_id; see the boot log\n"
         "  - id: adopted_" +
         sender_hex_lower + "\n" + "    system_key: \"" + key_hex + "\"\n" + manufacturer_line + "\n" + type_lines +
         "    commands: [open, close, stop]";
}

/// @brief Build the ready-to-paste 2W system-key-extraction report: `node_id:`/`system_key:` as a
/// `home_io_control:` YAML block.
///
/// Pure — takes already-decoded values, performs no I/O — so it is directly unit-testable without
/// a live radio, mirroring build_oneway_adoption_report() above; the two features end up sharing
/// report *structure* as well as format_key_hex(). The caller (hub_key_extraction.cpp) logs the
/// result through log_multiline_result() and nowhere else — this is the single intentional place
/// the recovered `system_key` is formatted for display, a deliberate exception to redaction.h's
/// masking.
///
/// The emitted keys must track the hub's own `CONFIG_SCHEMA` (`__init__.py`) by hand. `make
/// yaml-emitter-sync` (scripts/check-yaml-emitters.py) catches drift between the two by
/// cross-referencing this function's emitted key names against that schema statically.
/// @param node_id Recovered hub node_id, 3 bytes.
/// @param key Recovered system key, 16 bytes.
/// @return Multi-line report text, ready to pass to log_multiline_result().
inline std::string build_key_extraction_report(const uint8_t node_id[NODE_ID_SIZE], const uint8_t key[AES_KEY_SIZE]) {
  return "SYSTEM KEY EXTRACTED -- DO NOT SHARE YOUR SYSTEM KEY\n"
         "Anyone with this key and node_id can control every device on this installation.\n"
         "This exchange has not been independently confirmed against your specific hub -- test\n"
         "this key (e.g. by controlling a device with it) before relying on it.\n"
         "Copy the block below into a new hub's YAML.\n"
         "home_io_control:\n"
         "  node_id: \"" +
         node_id_to_string(node_id) + "\"\n" + "  system_key: \"" + format_key_hex(key) + "\"";
}

// ============================================================================
// Status normalization helpers
// ============================================================================

/// @brief Normalize stopped state: some devices briefly report stopped before target/current converge.
/// @param dev Device record to update (may clear is_stopped if positions differ).
inline void normalize_stopped_state(IoDevice &dev) {
  // Some devices briefly report STATUS_STOPPED before current and target have numerically
  // converged. Keep the device in the moving state until the decoded values are effectively equal.
  if (dev.is_stopped && dev.target != UNKNOWN_POSITION && dev.position != UNKNOWN_POSITION &&
      !has_reached_target_position(dev.target, dev.position)) {
    dev.is_stopped = false;
  }
}

/// @brief Update per-device link-health stats from the radio's last capture.
///
/// Called for every frame whose `src` is a registered device, regardless of command type or
/// whether that command's own payload was well-formed — any such frame is real evidence the
/// device is reachable and at this signal strength. The two call sites cover both ways such a
/// frame arrives: update_device_status_() (inbound status path) and
/// execute_request_and_update_()'s explicit-refusal branch (a CMD_ERROR_RESP reply to our own
/// request, which returns before reaching the status path). Always stamps `last_seen_ms`; only
/// touches the RSSI fields when `radio` is non-null and its last capture is valid (real drivers
/// always populate a valid capture before a frame is handed off, but tests calling this path
/// directly without a radio, or without exercising RX through it, must not crash or fabricate
/// an RSSI).
/// @param dev Device that sent the frame.
/// @param radio Radio driver to read the last capture from; may be nullptr.
inline void update_link_health(IoDevice &dev, RadioDriver *radio) {
  dev.last_seen_ms = millis();
  if (radio == nullptr)
    return;

  const RadioCaptureInfo &capture = radio->get_last_capture();
  if (!capture.valid)
    return;

  dev.last_rssi_dbm = capture.rssi_dbm;
  if (dev.rssi_ema_scaled == RSSI_UNKNOWN_DBM) {
    // First sample: seed the EMA directly instead of blending from 0.
    dev.rssi_ema_scaled = static_cast<int16_t>(capture.rssi_dbm * RSSI_EMA_SCALE);
    return;
  }
  // Integer EMA kept in fixed point: `S += x − round(S/N)` blends each sample at weight 1/N
  // while S stays scaled by N, so sub-dBm contributions accumulate instead of truncating to
  // zero — a whole-dBm EMA would stall as soon as |sample − EMA| < N and never converge.
  dev.rssi_ema_scaled =
      static_cast<int16_t>(dev.rssi_ema_scaled + capture.rssi_dbm - rssi_scaled_to_dbm(dev.rssi_ema_scaled));
}

/// @brief Record that an outbound exchange to this device timed out (no valid response).
///
/// Called once per failed exchange from execute_request_and_update_()'s "no valid response"
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

/// @brief Log a concise status‑update line used by inbound handlers.
/// @param id Device ID.
/// @param dev Current device state.
/// @param suffix Optional suffix added after the state string (e.g., " (status update)").
inline void log_status_update(const std::string &id, const IoDevice &dev, const char *suffix = "") {
  ESP_LOGI(TAG, "Device %s: position=%s target=%s %s%s", id.c_str(), format_position(dev.position).c_str(),
           format_position(dev.target).c_str(), dev.is_stopped ? "stopped" : "moving", suffix);
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

/// @brief Clear a previously recorded CMD_ERROR_RESP result, if any.
///
/// A stale limitation reason (e.g. a rain lockout from an hour ago) is worse than none once the
/// device has since replied normally, so every successful status/command reply for a device
/// clears it. Called from the CMD_PRIVATE_RESP and CMD_STATUS_UPDATE branches of
/// update_device_status_() — not from CMD_GET_NAME_RESP/CMD_GET_INFO2_RESP, which are metadata
/// lookups unrelated to whether the device's last movement command succeeded. Like
/// record_command_result(), does not notify subscribers itself — both existing call sites clear
/// before their own notify_device_update_() call, which is what actually publishes this change.
/// @param dev Device to clear.
inline void clear_command_result(IoDevice &dev) {
  dev.last_result_code = 0;
  dev.last_result_at_ms = 0;
}

}  // namespace detail
}  // namespace home_io_control
}  // namespace esphome
