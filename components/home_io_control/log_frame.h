#pragma once

/// @file log_frame.h
/// @brief Shared frame logging helpers for IO-Homecontrol.
/// @ingroup hioc_protocol

#include "proto_frame.h"

#include "esphome/core/log.h"
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

#ifdef IOHOME_FRAME_LOG
inline void log_frame(const char *prefix, const uint8_t *data, uint8_t len, uint32_t freq, uint16_t preamble = 0) {
  char hex[FRAME_LOG_HEX_BUFFER_SIZE];
  bytes_to_hex(data, len, hex, sizeof(hex));

  // Decode command name and CTRL1 flags for enhanced readability
  char flags[FRAME_LOG_FLAGS_BUFFER_SIZE] = {0};
  const char *cmd_str = "";
  if (len >= FRAME_MIN_SIZE) {
    decode_ctrl1_flags(data[1], flags, sizeof(flags));
    cmd_str = command_name(data[8]);  // Command byte is at offset 8 (after ctrl0+ctrl1+dst+src)
  }

  if (preamble > 0)
    ESP_LOGI("io_frame", "%s [%u bytes] freq=%u preamble=%u cmd=%s %s: %s", prefix, len, freq, preamble, cmd_str, flags,
             hex);
  else
    ESP_LOGI("io_frame", "%s [%u bytes] freq=%u cmd=%s %s: %s", prefix, len, freq, cmd_str, flags, hex);
}
#endif

}  // namespace home_io_control
}  // namespace esphome