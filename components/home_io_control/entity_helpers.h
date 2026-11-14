#pragma once

/// @file entity_helpers.h
/// @brief Conversions, renderers and device bookkeeping shared by the hub, its collaborators and its Home
/// Assistant entities.
/// @ingroup hioc_hub
///
/// Carries no dependency on the hub itself, so an entity can include this header instead of the
/// hub's private helpers (make include-graph enforces that split).

#include "log_helpers.h"
#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_device_model.h"
#include "proto_device_types.h"
#include "proto_frame.h"
#include "proto_sizes.h"

#include "esphome/core/hal.h"

#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace esphome {
namespace home_io_control {
namespace detail {

// ============================================================================
// Shared constants
// ============================================================================

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

/// @brief Round an IO percent (0-100) to a whole number, for publishing to HA.
///
/// HA shows `round(position * 100)` but calls a cover closed only at exactly 0.0, so a cover resting
/// at 99.68 % closed would show "0 %" and "open" at once (issue #151). Publishing whole percent
/// keeps the displayed number and the open/closed state in agreement.
/// @param io_percent IO percent in [0.0, 100.0].
/// @return The same percent rounded to the nearest whole number.
inline float whole_percent(float io_percent) { return std::round(io_percent); }

// ============================================================================
// Last-command record rendering
// ============================================================================

/// @brief Render a Command Originator byte as "name(0xXX)", e.g. "user_remote(0x01)".
///
/// The one rendering every originator display shares (the "Last Command Source" sensor and the 0x71
/// status-update log line), so a byte with no ORIGINATOR_* case reads "unknown(0x0A)" everywhere
/// rather than being silently dropped or mislabelled.
/// @param originator Command Originator byte (ORIGINATOR_*).
/// @return "name(0xXX)".
inline std::string format_originator(uint8_t originator) {
  return format_name_and_hex(originator_name(originator), originator);
}

/// @brief Render the "Last Commanded By" sensor string.
///
/// Always leads with the raw node ID — that is the diagnostic value, and the only thing a user can
/// match against a remote they own. The qualifier is additive, never a substitute: a device naming
/// its own ID is NOT reliably "the button on the motor" (the one non-shutter this project has data
/// on, a mains gate, names its own ID with an undefined originator), so the cause belongs to the
/// separate originator sensor, not to this one's wording.
/// @param dev Device record to read.
/// @param hub_node_id This hub's own 3-byte node ID.
/// @return e.g. "3B74DC", "C0FFEE (this hub)", "2FE2D2 (this device)"; empty before the first record.
inline std::string describe_last_commander(const IoDevice &dev, const uint8_t *hub_node_id) {
  if (!dev.has_last_command)
    return {};
  std::string out = node_id_to_string(dev.last_commander);
  if (memcmp(dev.last_commander, hub_node_id, NODE_ID_SIZE) == 0) {
    out += " (this hub)";
  } else if (memcmp(dev.last_commander, dev.node_id, NODE_ID_SIZE) == 0) {
    out += " (this device)";
  }
  return out;
}

/// @brief Render the "Last Command Source" sensor string.
///
/// Rendered by format_originator(), so an undecoded byte keeps its raw hex. The decode is
/// field-validated for roller shutters (a clean 0x00/0x01 split, remote vs. motor
/// button); gates, lights and multi-channel units are not validated and are expected to surface
/// undecoded values here — which is the point of keeping the raw hex in the string.
/// @param dev Device record to read.
/// @return e.g. "user_remote(0x01)"; empty before the first record.
inline std::string describe_last_command_source(const IoDevice &dev) {
  if (!dev.has_last_command)
    return {};
  return format_originator(dev.last_command_originator);
}

// ============================================================================
// Limitation read rendering
// ============================================================================

/// Buffer size for the limitation fragments below: the longest is "selector (D8 01)" or "0xFD (unlimited)".
inline constexpr size_t LIMITATION_TEXT_BUFFER_SIZE = 32;

/// @brief Render a limitation parameter id: "MP" for the main parameter, "FP<n>" for 1-16, else hex.
inline std::string format_limitation_param(uint8_t parameter_id) {
  if (parameter_id == LIMITATION_PARAM_MP) {
    return "MP";
  }
  std::array<char, LIMITATION_TEXT_BUFFER_SIZE> buffer{};
  if (parameter_id >= LIMITATION_PARAM_FP_FIRST && parameter_id <= LIMITATION_PARAM_FP_LAST) {
    std::snprintf(buffer.data(), buffer.size(), "FP%u", parameter_id);
  } else {
    std::snprintf(buffer.data(), buffer.size(), "0x%02X", parameter_id);
  }
  return std::string(buffer.data());
}

/// @brief Render a limitation value: a percentage, or the raw selector when it is not a position.
///
/// Values above STATUS_POS_MAX are position selectors (stop, unknown, ...), never percentages, so
/// they print as `selector (D8 01)`. A percentage always keeps its raw bytes beside it, so a wrong
/// layout assumption stays visible in the log.
/// @param value_raw Big-endian raw value (LimitationStatus::value_raw).
/// @return e.g. "93% (BA 00)", "46.5% (5D 00)" or "selector (D8 01)".
inline std::string format_limitation_value(uint16_t value_raw) {
  std::array<char, LIMITATION_TEXT_BUFFER_SIZE> buffer{};
  const unsigned hi = value_raw >> BITS_PER_BYTE;
  const unsigned lo = value_raw & 0xFFU;
  if (value_raw > STATUS_POS_MAX) {
    std::snprintf(buffer.data(), buffer.size(), "selector (%02X %02X)", hi, lo);
    return std::string(buffer.data());
  }
  const double percent = value_raw * 100.0 / STATUS_POS_MAX;
  const bool whole = (value_raw * 100U) % STATUS_POS_MAX == 0;
  std::snprintf(buffer.data(), buffer.size(), whole ? "%.0f%% (%02X %02X)" : "%.1f%% (%02X %02X)", percent, hi, lo);
  return std::string(buffer.data());
}

/// @brief Render a limitation time code with its raw byte: "0x1D (900 s)", "0xFD (unlimited)", "0xFE (code 254)".
inline std::string format_limitation_time(uint8_t time_raw) {
  std::array<char, LIMITATION_TEXT_BUFFER_SIZE> buffer{};
  if (time_raw == LIMITATION_TIME_UNLIMITED) {
    std::snprintf(buffer.data(), buffer.size(), "0x%02X (unlimited)", time_raw);
  } else if (time_raw > LIMITATION_TIME_MAX_COUNTED) {
    std::snprintf(buffer.data(), buffer.size(), "0x%02X (code %u)", time_raw, time_raw);
  } else {
    std::snprintf(buffer.data(), buffer.size(), "0x%02X (%" PRIu32 " s)", time_raw, limitation_time_seconds(time_raw));
  }
  return std::string(buffer.data());
}

/// @brief Render a decoded limitation reply on one line, raw bytes beside every decoded value.
/// @return e.g. "limitation (assumed layout): param=MP value=93% (BA 00) originator=rain_sensor(0x02) time=0x1D (900
/// s)".
inline std::string format_limitation_status(const LimitationStatus &status) {
  return "limitation (assumed layout): param=" + format_limitation_param(status.parameter_id) +
         " value=" + format_limitation_value(status.value_raw) + " originator=" + format_originator(status.originator) +
         " time=" + format_limitation_time(status.time_raw);
}

/// @brief Render a CMD_LIMITATION_STATUS_RESP frame for a log line or probe report.
/// @param frame A parsed CMD_LIMITATION_STATUS_RESP frame.
/// @return format_limitation_status() of the decode, or "limitation (assumed layout): unexpected
///         length N" when the payload is not the assumed five bytes.
inline std::string describe_limitation_reply(const IoFrame &frame) {
  LimitationStatus status;
  if (!decode_limitation_status(frame, status))
    return "limitation (assumed layout): unexpected length " + std::to_string(frame.data_len);
  return format_limitation_status(status);
}

/// @brief Render a CMD_LIMITATION_STATUS_REQ frame for a log line.
/// @param frame A parsed CMD_LIMITATION_STATUS_REQ frame.
/// @return e.g. "limitation request (assumed layout): type=minimum param=MP", or "... unexpected payload"
///         when the length or selector is not one of the known shapes.
inline std::string describe_limitation_request(const IoFrame &frame) {
  LimitationType type;
  uint8_t parameter_id = 0;
  if (!decode_limitation_request(frame, type, parameter_id))
    return "limitation request (assumed layout): unexpected payload";
  return std::string("limitation request (assumed layout): type=") +
         (type == LimitationType::MINIMUM ? "minimum" : "maximum") + " param=" + format_limitation_param(parameter_id);
}

// ============================================================================
// Device bookkeeping shared by the hub and the heater controller
// ============================================================================

/// @brief Log a rejected operation with capability mismatch details.
/// @param device_id Device ID string.
/// @param dev IoDevice that rejected the command.
/// @param operation Human‑readable operation name (e.g., "set position").
/// @param expected Expected capability class or profile name.
inline void log_rejected_operation(const std::string &device_id, const IoDevice &dev, const char *operation,
                                   const char *expected) {
  ESP_LOGW(TAG, "Rejecting %s for device %s: type=%s (%u) class=%s profile=%s expected=%s", operation,
           device_id.c_str(), device_type_name(dev.type), static_cast<unsigned>(dev.type),
           device_capability_class_name(dev.type, dev.subtype), device_operation_profile_name(dev.type, dev.subtype),
           expected);
}

/// @brief Update per-device link-health stats from the radio's last capture.
///
/// Called for every frame whose `src` is a registered device, regardless of command type or
/// whether that command's own payload was well-formed — any such frame is real evidence the
/// device is reachable and at this signal strength. The call sites cover every way such a frame
/// arrives: update_device_status_() (inbound status path), exchange_and_record_()'s
/// explicit-refusal branch (a CMD_ERROR_RESP reply to our own request, which returns before
/// reaching the status path), and its failure branch when a 0x3C challenge was seen (the device
/// transmitted, even though the exchange did not complete). Always stamps `last_seen_ms`; only
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
