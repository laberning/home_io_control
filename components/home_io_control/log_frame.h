#pragma once

/// @file log_frame.h
/// @brief Shared frame logging helpers for IO-Homecontrol.
/// @ingroup hioc_protocol

#include "proto_frame.h"
#include "redaction.h"

#include "esphome/core/log.h"
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace esphome {
namespace home_io_control {

inline constexpr size_t FRAME_LOG_HEX_BUFFER_SIZE = 220;   ///< Fits a full 32-byte frame rendered as spaced hex text.
inline constexpr size_t FRAME_LOG_FLAGS_BUFFER_SIZE = 32;  ///< Buffer for decoded CTRL1 flag annotations.

inline void bytes_to_hex(const uint8_t *data, uint8_t len, char *out, size_t out_size) {
  size_t pos = 0;
  if (out_size == 0)
    return;
  out[0] = '\0';
  for (uint8_t i = 0; i < len && pos + 4 < out_size; i++)
    pos += snprintf(out + pos, out_size - pos, "%02X ", data[i]);
}

/// @brief Render decoded CTRL1 flags into a short annotation string.
/// @param ctrl1 The CTRL1 byte from a frame.
/// @param out Buffer to write into (at least FRAME_LOG_FLAGS_BUFFER_SIZE bytes).
/// @param out_size Size of the output buffer.
inline void decode_ctrl1_flags(uint8_t ctrl1, char *out, size_t out_size) {
  size_t pos = 0;
  if (out_size == 0)
    return;
  out[0] = '\0';
  if ((ctrl1 & CTRL1_ACK) != 0)
    pos += snprintf(out + pos, out_size - pos, "[ACK]");
  if ((ctrl1 & CTRL1_LOW_POWER) != 0)
    pos += snprintf(out + pos, out_size - pos, "[LPM]");
  if ((ctrl1 & CTRL1_PRIORITY) != 0)
    pos += snprintf(out + pos, out_size - pos, "[PRIO]");
  if ((ctrl1 & CTRL1_ROUTED) != 0)
    pos += snprintf(out + pos, out_size - pos, "[R]");
  if ((ctrl1 & CTRL1_BEACON) != 0)
    snprintf(out + pos, out_size - pos, "[B]");
}

/// @brief Render a frame's bytes as spaced hex text, masking the payload when the command
/// carries key material (see command_carries_key_material()).
///
/// Kept outside the IOHOME_FRAME_LOG guard so the redaction behavior is unit-testable
/// independent of the firmware-only logging build flag.
/// @param data Raw serialized frame bytes (header + payload, no CRC).
/// @param len Total length of data.
/// @param out Buffer to write the rendered text into.
/// @param out_size Size of out.
inline void render_frame_hex_redacted(const uint8_t *data, uint8_t len, char *out, size_t out_size) {
  if (out_size == 0)
    return;
  if (len >= FRAME_MIN_SIZE && command_carries_key_material(data[FRAME_CMD_OFFSET])) {
    // Sized for FRAME_MIN_SIZE bytes of "XX " hex (not the general FRAME_LOG_HEX_BUFFER_SIZE) so
    // GCC's -Wformat-truncation can prove the snprintf() below never truncates into `out`.
    // +4 rather than +1: bytes_to_hex()'s loop guard (`pos + 4 < out_size`) needs headroom beyond
    // the last byte's own 3 chars, not just space for the content + null terminator.
    char header_hex[FRAME_MIN_SIZE * 3 + 4];
    bytes_to_hex(data, FRAME_MIN_SIZE, header_hex, sizeof(header_hex));
    snprintf(out, out_size, "%s[%u bytes masked]", header_hex, static_cast<unsigned>(len - FRAME_MIN_SIZE));
  } else {
    bytes_to_hex(data, len, out, out_size);
  }
}

#ifdef IOHOME_FRAME_LOG
inline void log_frame(const char *prefix, const uint8_t *data, uint8_t len, uint32_t freq, uint16_t preamble = 0) {
  char hex[FRAME_LOG_HEX_BUFFER_SIZE];
  render_frame_hex_redacted(data, len, hex, sizeof(hex));

  // Decode command name and CTRL1 flags for enhanced readability
  char flags[FRAME_LOG_FLAGS_BUFFER_SIZE] = {0};
  const char *cmd_str = "";
  if (len >= FRAME_MIN_SIZE) {
    decode_ctrl1_flags(data[1], flags, sizeof(flags));
    cmd_str = command_name(data[FRAME_CMD_OFFSET]);
  }

  if (preamble > 0)
    ESP_LOGI("io_frame", "%s [%u bytes] freq=%" PRIu32 " preamble=%u cmd=%s %s: %s", prefix, len, freq, preamble,
             cmd_str, flags, hex);
  else
    ESP_LOGI("io_frame", "%s [%u bytes] freq=%" PRIu32 " cmd=%s %s: %s", prefix, len, freq, cmd_str, flags, hex);
}
#endif

}  // namespace home_io_control
}  // namespace esphome