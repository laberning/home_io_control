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

#include <cstdio>

namespace esphome {
namespace home_io_control {
namespace detail {

// ============================================================================
// Shared constants
// ============================================================================

inline constexpr const char *TAG = "home_io_control";         ///< Shared log tag for hub-level messages.
inline constexpr uint32_t STATUS_RETRY_AFTER_FAIL_MS = 5000;  ///< First retry after a silent status-poll failure.
inline constexpr uint32_t STATUS_RETRY_AFTER_FAIL_STEP2_MS =
    15000;  ///< Second retry after a silent status-poll failure.
inline constexpr uint32_t STATUS_RETRY_AFTER_FAIL_STEP3_MS =
    30000;  ///< Third retry after a silent status-poll failure.
inline constexpr uint32_t STATUS_RETRY_AFTER_FAIL_STEP4_MS =
    60000;  ///< Fourth retry after a silent status-poll failure.
inline constexpr uint32_t STATUS_RETRY_AFTER_FAIL_MAX_MS =
    300000;  ///< Steady-state backoff for repeated silent status-poll failures.
inline constexpr uint32_t STATUS_AUTH_RETRY_AFTER_FAIL_MS =
    30000;  ///< First retry after a challenge-seen auth-like failure.
inline constexpr uint32_t STATUS_AUTH_RETRY_AFTER_FAIL_STEP2_MS =
    120000;  ///< Second retry after a challenge-seen auth-like failure.
inline constexpr uint32_t STATUS_AUTH_RETRY_AFTER_FAIL_MAX_MS =
    300000;  ///< Steady-state backoff after repeated auth-like failures.
inline constexpr uint32_t INITIAL_STATUS_REQUEST_DELAY_MS =
    5000;  ///< Delay before the first post-boot status request from an entity.
inline constexpr uint32_t REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS =
    2000;  ///< Delay before polling after overheard remote traffic.
inline constexpr uint32_t ONEWAY_DEDUP_WINDOW_MS =
    2000;  ///< Suppress duplicate 1W log/poll for same remote+cmd within this window.
inline constexpr uint32_t MAX_TRACKED_STATUS_POLL_WINDOW_MS =
    600000;  ///< Hard stop for follow-up polling after a command or remote activity.
inline constexpr uint32_t PAIRING_DISCOVERY_RESPONSE_TIMEOUT_MS = 2000;  ///< Discovery wait window after sending 0x28.
inline constexpr uint8_t PAIRING_DISCOVERY_MAX_ATTEMPTS = 3;             ///< Retry discovery TX up to this many times.
inline constexpr uint32_t PAIRING_KEY_CHALLENGE_TIMEOUT_MS = 500;  ///< Wait window for the device's 0x3C challenge.
inline constexpr uint32_t PAIRING_KEY_CONFIRM_TIMEOUT_MS = 500;    ///< Wait for 0x33 key confirm after sending 0x32.
inline constexpr uint32_t PAIRING_KEY_CONFIRM_SLICE_MS = 150;  ///< RX slice during key confirm wait (hop each slice).
inline constexpr float BINARY_ENTITY_ON_POSITION_THRESHOLD =
    50.0F;  ///< Shared 0-100 cutoff: values below this mean binary "on".

// Binary on/off entities reuse the proven position transport encoding.
inline constexpr uint8_t BINARY_ENTITY_ON_POSITION = 0;
inline constexpr uint8_t BINARY_ENTITY_OFF_POSITION = 100;

/// @brief Clear all bounded follow-up polling state for a device.
/// @param dev Device record to reset.
inline void clear_status_poll_tracking(IoDevice &dev) {
  dev.single_follow_up_poll_pending = false;
  dev.next_update = 0;
  dev.poll_deadline = 0;
  dev.status_poll_failures = 0;
  dev.auth_poll_failures = 0;
}

/// @brief Check whether a device remains inside its bounded follow-up polling window.
/// @param dev Device record.
/// @param now Current millis() timestamp.
/// @return true when repeated polling may continue.
inline bool status_poll_tracking_active(const IoDevice &dev, uint32_t now) {
  return dev.status_poll_interval_ms != 0 && dev.poll_deadline != 0 && now <= dev.poll_deadline;
}

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
  return is_binary_entity_position(position) &&
         (device_supports_binary_control(dev.type) || device_supports_lock_control(dev.type));
}

/// @brief Can this device accept a tilt command?
/// @param dev IoDevice to check.
/// @return true only if device type is known to support tilt.
inline bool known_device_accepts_execute_tilt(const IoDevice &dev) {
  return dev.type != DeviceType::UNKNOWN && device_supports_tilt(dev.type);
}

/// @brief Compute the next background status-poll retry delay after a failed exchange.
///
/// Plain silence is treated as a soft reachability problem and ramps up gradually so sleeping
/// or temporarily busy devices are retried soon. Exchanges that reached the 0x3C challenge but
/// never completed are much more likely to represent an invalid system key or pairing mismatch,
/// so they back off more aggressively to avoid repeated 0x3D HMAC traffic.
///
/// @param consecutive_failures 1-based count of consecutive failures in the current failure class.
/// @param auth_like_failure True when the failed exchange saw a 0x3C challenge.
/// @return Delay in milliseconds before the next automatic status poll.
inline uint32_t status_poll_retry_delay_ms(uint8_t consecutive_failures, bool auth_like_failure) {
  if (auth_like_failure) {
    if (consecutive_failures <= 1)
      return STATUS_AUTH_RETRY_AFTER_FAIL_MS;
    if (consecutive_failures == 2)
      return STATUS_AUTH_RETRY_AFTER_FAIL_STEP2_MS;
    return STATUS_AUTH_RETRY_AFTER_FAIL_MAX_MS;
  }

  if (consecutive_failures <= 1)
    return STATUS_RETRY_AFTER_FAIL_MS;
  if (consecutive_failures == 2)
    return STATUS_RETRY_AFTER_FAIL_STEP2_MS;
  if (consecutive_failures == 3)
    return STATUS_RETRY_AFTER_FAIL_STEP3_MS;
  if (consecutive_failures == 4)
    return STATUS_RETRY_AFTER_FAIL_STEP4_MS;
  return STATUS_RETRY_AFTER_FAIL_MAX_MS;
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
  bytes_to_hex(buf, len, payload_hex, sizeof(payload_hex));
  if (frame != nullptr) {
    ESP_LOGD(
        "io_capture",
        "chip=%s phase=component stage=%s freq=%u ts=%u len=%u cmd=0x%02X src=%02X%02X%02X dst=%02X%02X%02X payload=%s",
        radio->chip_name(), stage, capture.freq_hz, capture.timestamp_ms, len, frame->cmd, frame->src[0], frame->src[1],
        frame->src[2], frame->dst[0], frame->dst[1], frame->dst[2], payload_hex);
    return;
  }
  ESP_LOGD("io_capture", "chip=%s phase=component stage=%s freq=%u ts=%u len=%u payload=%s", radio->chip_name(), stage,
           capture.freq_hz, capture.timestamp_ms, len, payload_hex);
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

/// @brief Log a decoded 1W remote frame at DEBUG level.
///
/// Uses decode_1w_frame() to extract structured fields, then formats a concise
/// DEBUG log line showing remote ID, target type, command intent, and priority.
/// When the remote is linked to devices, appends the linked device IDs.
/// @param frame Parsed 1W frame.
/// @param linked_devices Optional pointer to device IDs this remote is linked to.
inline void log_1w_remote_frame(const IoFrame &frame, const std::vector<std::string> *linked_devices = nullptr) {
  const OneWayFrameInfo info = decode_1w_frame(frame);
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

}  // namespace detail
}  // namespace home_io_control
}  // namespace esphome