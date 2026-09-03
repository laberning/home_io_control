#pragma once

/// @file redaction.h
/// @brief Key-material redaction helpers shared by every surface that can emit frame bytes
/// or debug text (frame logging today; telemetry and diagnostic reports later).
/// @ingroup hioc_protocol
///
/// The one deliberate, narrow exception to this file's masking is `IOHOME_UNSAFE_LOG_KEY_MATERIAL`
/// (log_frame.h::render_frame_hex_redacted()) — a maintainer-only, opt-in-only build flag for
/// capturing a genuine pairing exchange for `tests/corpus/`. See that function's doxygen for the
/// full rules before ever defining it.

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
///
/// CMD_CHALLENGE_REQ (0x3C) and CMD_CHALLENGE_RESP (0x3D) are masked for the same reason even
/// though neither carries the key itself: create_hmac() builds the 0x3D payload by AES-128-ECB
/// encrypting an IV built from the 0x3C challenge and the challenged frame's own (plaintext)
/// transcript under the system key, then truncating to 6 bytes (proto_crypto.cpp). Logging both
/// frames publishes a known-plaintext/known-ciphertext pair for that block cipher under the
/// system key, so they get the same treatment as literal key material.
///
/// CMD_ONEWAY_ADD_CONTROLLER (0x30) is the 1W equivalent of CMD_KEY_TRANSFER. Its payload is not
/// literally the key — it is the key wrapped with the public TRANSFER_KEY — but the unwrap needs
/// nothing secret: the IV derives only from the sender's node address, which is plaintext in the
/// same frame's header. So publishing those bytes is equivalent to publishing the key, and they
/// are masked for the same reason 0x32 is. The one deliberate exception is the adoption report in
/// oneway_key_adoption.cpp, which formats the *decoded* key directly rather than going
/// through any frame-rendering path.
/// @param cmd Frame command byte.
/// @return true if the payload of a frame with this command must be masked.
inline bool command_carries_key_material(uint8_t cmd) {
  return cmd == CMD_KEY_TRANSFER || cmd == CMD_CHALLENGE_REQ || cmd == CMD_CHALLENGE_RESP ||
         cmd == CMD_ONEWAY_ADD_CONTROLLER;
}

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
