/// @file
/// @brief Bridle trace line formatting.
/// @ingroup hioc_radio

// IOHOME_BRIDLE is only visible after something pulls in esphome/core/defines.h (ESPHome codegen's
// cg.add_define() lands there, not as a compiler -D flag): this include must run before the #ifdef,
// or the file silently compiles to nothing in a real build although the host tests (which pass
// -DIOHOME_BRIDLE) are fine.
#include "esphome/core/hal.h"

#ifdef IOHOME_BRIDLE

#include "radio_bridle_trace.h"

#include <cinttypes>
#include <cstdio>

namespace esphome::home_io_control::bridle {

size_t trace_part_count(size_t message_size) {
  size_t const parts = (message_size + TRACE_PART_BYTES - 1) / TRACE_PART_BYTES;
  return parts == 0 ? 1 : parts;
}

size_t format_trace_part(char *out, size_t cap, TraceDirection direction, uint32_t host_ms, const uint8_t *message,
                         size_t message_size, size_t part) {
  size_t const parts = trace_part_count(message_size);
  if (part >= parts || cap < TRACE_LINE_MAX)
    return 0;
  int written = std::snprintf(out, cap, "%s %c %" PRIu32 " ", TRACE_MARKER, static_cast<char>(direction), host_ms);
  if (parts > 1) {
    written += std::snprintf(out + written, cap - static_cast<size_t>(written), "%u/%u ",
                             static_cast<unsigned>(part + 1), static_cast<unsigned>(parts));
  }
  static constexpr char HEX[] = "0123456789abcdef";
  size_t const first = part * TRACE_PART_BYTES;
  size_t const last = message_size < first + TRACE_PART_BYTES ? message_size : first + TRACE_PART_BYTES;
  auto pos = static_cast<size_t>(written);
  // A nibble is 4 bits: the shift and mask ARE the hex encoding.
  // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
  for (size_t i = first; i < last; i++) {
    out[pos++] = HEX[message[i] >> 4];
    out[pos++] = HEX[message[i] & 0x0F];
  }
  // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
  out[pos] = '\0';
  return pos;
}

}  // namespace esphome::home_io_control::bridle

#endif  // IOHOME_BRIDLE
