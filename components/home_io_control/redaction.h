#pragma once

/// @file redaction.h
/// @brief Key-material redaction helpers shared by every surface that can emit frame bytes
/// or debug text (frame logging today; telemetry and diagnostic reports later).
/// @ingroup hioc_protocol

#include "proto_constants.h"
#include "proto_sizes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace home_io_control {

/// @brief Whether a frame command's payload carries key material that must never be logged
/// or reported verbatim.
///
/// CMD_KEY_TRANSFER (0x32) carries the system key encrypted with the pairing transfer key —
/// still key material, so callers rendering frame bytes must mask this payload rather than
/// print it.
/// @param cmd Frame command byte.
/// @return true if the payload of a frame with this command must be masked.
inline bool command_carries_key_material(uint8_t cmd) { return cmd == CMD_KEY_TRANSFER; }

/// @brief Whether a buffer contains the system key as a contiguous run of bytes.
///
/// A last-line-of-defense scan for accidental key leaks in ad hoc debug or report text,
/// independent of which command produced the buffer.
/// @param buf Buffer to scan.
/// @param len Length of buf in bytes.
/// @param system_key Pointer to the AES_KEY_SIZE-byte system key.
/// @return true if system_key appears anywhere in buf.
inline bool contains_key_material(const uint8_t *buf, size_t len, const uint8_t *system_key) {
  if (buf == nullptr || system_key == nullptr || len < AES_KEY_SIZE)
    return false;
  for (size_t i = 0; i + AES_KEY_SIZE <= len; i++) {
    if (memcmp(buf + i, system_key, AES_KEY_SIZE) == 0)
      return true;
  }
  return false;
}

}  // namespace home_io_control
}  // namespace esphome
