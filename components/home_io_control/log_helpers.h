#pragma once

/// @file log_helpers.h
/// @brief Hub-layer log tag and log/format helpers shared by the hub and its collaborators.
/// @ingroup hioc_hub
///
/// Carries no dependency on the hub itself, so a collaborator that only needs to log can include
/// this header instead of the hub's private helpers (make include-graph enforces that split).

#include "log_frame.h"
#include "proto_frame.h"
#include "proto_sizes.h"
#include "radio_interface.h"

#include "esphome/core/log.h"

#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

namespace esphome {
namespace home_io_control {
namespace detail {

// ============================================================================
// Shared constants
// ============================================================================

inline constexpr const char *TAG = "home_io_control";  ///< Shared log tag for hub-level messages.

// ============================================================================
// Formatting helpers
// ============================================================================

/// Buffer size for format_name_and_hex(): longest command name plus "(0xXX)" and a margin.
inline constexpr size_t NAME_AND_HEX_BUFFER_SIZE = 40;

/// @brief Format a name/value pair as "name(0xXX)", e.g. "execute(0x00)".
inline std::string format_name_and_hex(const char *name, uint8_t value) {
  std::array<char, NAME_AND_HEX_BUFFER_SIZE> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%s(0x%02X)", name, value);
  return std::string(buffer.data());
}

// ============================================================================
// Key-material display formatting
// ============================================================================

/// @brief Format a 16-byte key as an uppercase, unseparated hex string for display.
///
/// The one deliberate place system-key bytes are formatted for display, shared by both
/// key-recovery features so neither forks its own copy: 2W "Accept Foreign Pairing"
/// (build_key_extraction_report(), key_extraction_responder.cpp) and 1W controller-key adoption
/// (build_oneway_adoption_report(), oneway_key_adoption.cpp). See redaction.h for the masking rules this
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

// ============================================================================
// Logging helpers
// ============================================================================

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

}  // namespace detail
}  // namespace home_io_control
}  // namespace esphome
