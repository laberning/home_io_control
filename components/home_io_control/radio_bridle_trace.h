/// @file
/// @brief Bridle trace lines: one line per message, the format session recordings are made from.
/// @ingroup hioc_radio
///
/// A host that logs every message it exchanges with the co-processor lets a conversation be
/// checked and replayed later (spec §12.1). This header renders one message as such lines:
///
///     [bridle] > 1234 0711004c00
///
/// direction, the host's millisecond clock, and the decoded message (`msg_type · seq · body ·
/// crc`) in hex. A message over 128 bytes is split over numbered parts (`k/n`) so that no line
/// exceeds what common embedded loggers keep. Compiled only under `IOHOME_BRIDLE`.

#pragma once

// IOHOME_BRIDLE is only visible after something pulls in esphome/core/defines.h (ESPHome codegen's
// cg.add_define() lands there, not as a compiler -D flag): this include must run before the #ifdef,
// or the file silently compiles to nothing in a real build although the host tests (which pass
// -DIOHOME_BRIDLE) are fine.
#include "esphome/core/hal.h"

#ifdef IOHOME_BRIDLE

#include <cstddef>
#include <cstdint>

namespace esphome::home_io_control::bridle {

/// Marks a trace line; the log prefix in front of it (level, tag) is ignored by readers.
static constexpr char TRACE_MARKER[] = "[bridle]";
/// Message bytes per line; a longer message is split (spec §12.1, ESP-IDF truncates at 512 bytes).
static constexpr size_t TRACE_PART_BYTES = 128;
/// A buffer size that holds any trace line: marker, direction, clock, `k/n`, 256 hex digits.
static constexpr size_t TRACE_LINE_MAX = 320;

/// Which way a message travelled.
enum class TraceDirection : char {
  HOST_TO_COPROC = '>',
  COPROC_TO_HOST = '<',
};

/// Number of lines a message of `message_size` bytes takes (at least 1).
[[nodiscard]] size_t trace_part_count(size_t message_size);

/// @brief Write line `part` (0-based) of a message into `out`.
///
/// All parts of one message carry the same direction and clock. A single-line message has no
/// `k/n` field.
/// @return Characters written, excluding the terminator; 0 when `part` is out of range or `cap`
///         is smaller than `TRACE_LINE_MAX`.
size_t format_trace_part(char *out, size_t cap, TraceDirection direction, uint32_t host_ms, const uint8_t *message,
                         size_t message_size, size_t part);

}  // namespace esphome::home_io_control::bridle

#endif  // IOHOME_BRIDLE
