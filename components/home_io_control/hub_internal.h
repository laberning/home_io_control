#pragma once

/// @file hub_internal.h
/// @brief Internal helpers shared by the hub implementation .cpp files.
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

inline constexpr const char *TAG = "home_io_control";
inline constexpr uint32_t STATUS_RETRY_AFTER_FAIL_MS = 5000;

// Binary on/off entities reuse the proven position transport encoding.
inline constexpr uint8_t BINARY_ENTITY_ON_POSITION = 0;
inline constexpr uint8_t BINARY_ENTITY_OFF_POSITION = 100;

// ============================================================================
// Capability and entity-profile helpers
// ============================================================================

inline bool is_binary_entity_position(uint8_t position) {
  return position == BINARY_ENTITY_ON_POSITION || position == BINARY_ENTITY_OFF_POSITION;
}

inline bool known_device_matches_entity_class(const IoDevice &dev, DeviceCapabilityClass expected) {
  return dev.type == DeviceType::UNKNOWN || device_capability_class(dev.type) == expected;
}

inline bool known_device_supports_status_requests(const IoDevice &dev) {
  return dev.type == DeviceType::UNKNOWN || device_supports_status_requests(dev.type);
}

inline bool known_device_accepts_execute_position(const IoDevice &dev, uint8_t position) {
  if (dev.type == DeviceType::UNKNOWN)
    return true;
  if (device_supports_position_control(dev.type))
    return true;
  return is_binary_entity_position(position) && device_supports_binary_control(dev.type);
}

inline bool known_device_accepts_execute_tilt(const IoDevice &dev) {
  return dev.type != DeviceType::UNKNOWN && device_supports_tilt(dev.type);
}

// ============================================================================
// Logging helpers
// ============================================================================

inline void log_rejected_operation(const std::string &device_id, const IoDevice &dev, const char *operation,
                                   const char *expected) {
  ESP_LOGW(TAG, "Rejecting %s for device %s: type=%s (%u) class=%s profile=%s expected=%s", operation,
           device_id.c_str(), device_type_name(dev.type), static_cast<uint8_t>(dev.type),
           device_capability_class_name(dev.type), device_operation_profile_name(dev.type), expected);
}

// ============================================================================
// Persistence key helpers
// ============================================================================

inline uint32_t saved_device_pref_hash(uint8_t index) {
  char key[16];
  snprintf(key, sizeof(key), "iohome_dev_%u", index);
  return fnv1_hash(key);
}

inline uint32_t legacy_saved_device_pref_hash(uint8_t index) { return fnv1_hash("iohome_dev") + index; }

inline void log_component_capture(const RadioDriver *radio, const char *stage, const uint8_t *buf, uint8_t len,
                                  const IoFrame *frame = nullptr) {
  const RadioCaptureInfo &capture = radio->get_last_capture();
  char payload_hex[220];
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

inline void log_frame_issue(IOHomeControlComponent *component, const char *direction, const char *reason,
                            const IoFrame &frame, uint8_t len) {
  const std::string src_id = node_id_to_string(frame.src);
  const std::string dst_id = node_id_to_string(frame.dst);
  const bool src_registered = component->get_device(src_id) != nullptr;
  const bool dst_registered = component->get_device(dst_id) != nullptr;

  if (src_registered || dst_registered) {
    ESP_LOGW(TAG, "%s issue=%s cmd=0x%02X src=%s%s dst=%s%s len=%u data_len=%u", direction, reason, frame.cmd,
             src_id.c_str(), src_registered ? " (registered)" : "", dst_id.c_str(),
             dst_registered ? " (registered)" : "", len, frame.data_len);
    return;
  }

  ESP_LOGD(TAG, "%s issue=%s cmd=0x%02X src=%s dst=%s len=%u data_len=%u", direction, reason, frame.cmd, src_id.c_str(),
           dst_id.c_str(), len, frame.data_len);
}

// ============================================================================
// Status normalization helpers
// ============================================================================

inline void normalize_stopped_state(IoDevice &dev) {
  // Some devices briefly report STATUS_STOPPED before current and target have numerically
  // converged. Keep the device in the moving state until the decoded values are effectively equal.
  if (dev.is_stopped && dev.target != UNKNOWN_POSITION && dev.position != UNKNOWN_POSITION &&
      !has_reached_target_position(dev.target, dev.position)) {
    dev.is_stopped = false;
  }
}

inline void log_status_update(const std::string &id, const IoDevice &dev, const char *suffix = "") {
  ESP_LOGI(TAG, "Device %s: position=%s target=%s %s%s", id.c_str(), format_position(dev.position).c_str(),
           format_position(dev.target).c_str(), dev.is_stopped ? "stopped" : "moving", suffix);
}

}  // namespace detail
}  // namespace home_io_control
}  // namespace esphome