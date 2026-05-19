#pragma once

/// @file log_frame.h
/// @brief Shared frame logging helpers for IO-Homecontrol.

#include "esphome/core/log.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace esphome {
namespace home_io_control {

inline constexpr size_t FRAME_LOG_HEX_BUFFER_SIZE = 220;  ///< Fits a full 32-byte frame rendered as spaced hex text.

inline void bytes_to_hex(const uint8_t *data, uint8_t len, char *out, size_t out_size) {
  size_t pos = 0;
  if (out_size == 0)
    return;
  out[0] = '\0';
  for (uint8_t i = 0; i < len && pos + 4 < out_size; i++)
    pos += snprintf(out + pos, out_size - pos, "%02X ", data[i]);
}

#ifdef IOHOME_FRAME_LOG
inline void log_frame(const char *prefix, const uint8_t *data, uint8_t len, uint32_t freq, uint16_t preamble = 0) {
  char hex[FRAME_LOG_HEX_BUFFER_SIZE];
  bytes_to_hex(data, len, hex, sizeof(hex));
  if (preamble > 0)
    ESP_LOGI("io_frame", "%s [%u bytes] freq=%u preamble=%u: %s", prefix, len, freq, preamble, hex);
  else
    ESP_LOGI("io_frame", "%s [%u bytes] freq=%u: %s", prefix, len, freq, hex);
}
#endif

}  // namespace home_io_control
}  // namespace esphome