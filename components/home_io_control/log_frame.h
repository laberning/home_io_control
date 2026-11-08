#pragma once

/// @file log_frame.h
/// @brief Shared frame logging helpers for IO-Homecontrol.
/// @ingroup hioc_protocol

#include "proto_frame.h"
#include "redaction.h"

#include "esphome/core/log.h"
#include <cinttypes>
#include <cstdarg>
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
///
/// @warning `IOHOME_UNSAFE_LOG_KEY_MATERIAL` disables the masking below entirely. It exists
/// solely so a maintainer can capture a genuine pairing exchange for `tests/corpus/` (see
/// `tests/corpus/README.md`'s key-hygiene section) when the normal `ingest.py --rekey` flow has
/// no other way to obtain the raw bytes. Never set it outside a deliberate, local,
/// own-hardware capture session: build with it, capture the one exchange you need, `--rekey`
/// immediately, then rebuild without it. Never commit a config with it enabled, never use it on
/// a device whose logs anyone else can see, and never paste output captured under it into an
/// issue/chat before re-keying. hub_core.cpp's setup() logs a loud warning every boot while this
/// is defined specifically so an accidentally-left-on build can't stay quiet.
/// @param data Raw serialized frame bytes (header + payload, no CRC).
/// @param len Total length of data.
/// @param out Buffer to write the rendered text into.
/// @param out_size Size of out.
inline void render_frame_hex_redacted(const uint8_t *data, uint8_t len, char *out, size_t out_size) {
  if (out_size == 0)
    return;
#ifndef IOHOME_UNSAFE_LOG_KEY_MATERIAL
  if (len >= FRAME_MIN_SIZE && command_carries_key_material(data[FRAME_CMD_OFFSET])) {
    // Sized for FRAME_MIN_SIZE bytes of "XX " hex (not the general FRAME_LOG_HEX_BUFFER_SIZE) so
    // GCC's -Wformat-truncation can prove the snprintf() below never truncates into `out`.
    // +4 rather than +1: bytes_to_hex()'s loop guard (`pos + 4 < out_size`) needs headroom beyond
    // the last byte's own 3 chars, not just space for the content + null terminator.
    char header_hex[(FRAME_MIN_SIZE * 3) + 4];
    bytes_to_hex(data, FRAME_MIN_SIZE, header_hex, sizeof(header_hex));
    snprintf(out, out_size, "%s[%u bytes masked]", header_hex, static_cast<unsigned>(len - FRAME_MIN_SIZE));
    return;
  }
#endif
  bytes_to_hex(data, len, out, out_size);
}

// === Held frame-log lines ===

/// @brief Log level of one frame-log line, kept with the line while it is held.
enum class FrameLogLevel : uint8_t { WARN, INFO, DEBUG };

/// Lines one hold can keep. A held window covers one received frame and the answer to it: the
/// driver's reception lines (at most four, on a failed decode) plus the answer's TX line and the
/// driver's re-arm line.
inline constexpr size_t FRAME_LOG_HELD_LINES = 6;
/// The overflow note flush() prints when lines did not fit.
inline constexpr size_t FRAME_LOG_NOTE_SIZE = 64;
/// One rendered frame-log line: the hex dump plus its prefix, frequency, command and flags.
inline constexpr size_t FRAME_LOG_LINE_SIZE = FRAME_LOG_HEX_BUFFER_SIZE + 96;

/// @brief Keeps frame-log lines back while the hub answers a device, and prints them afterwards.
///
/// Printing a log line takes milliseconds, and some receivers drop the hub's answer to their
/// challenge when it comes that much later (issue #163). While held, write() stores each line
/// instead of printing it; flush() prints the stored lines in order. Not holding, write() prints
/// at once. A line that does not fit while held is counted, not printed, and flush() reports the
/// count, so nothing is printed inside the window.
///
/// Kept outside the IOHOME_FRAME_LOG guard so its behaviour is unit-testable; firmware builds use
/// the one instance behind frame_log_hold() / frame_log_release().
class FrameLogBuffer {
 public:
  /// Prints one line. `tag` is the logging tag the line was written with.
  using Sink = void (*)(FrameLogLevel level, const char *tag, const char *text);

  explicit FrameLogBuffer(Sink sink) : sink_(sink) {}

  /// Start keeping lines back. Holding again while held changes nothing.
  void hold() { this->holding_ = true; }
  /// @return true while lines are kept back.
  [[nodiscard]] bool holding() const { return this->holding_; }
  /// @return number of lines currently kept back.
  [[nodiscard]] uint8_t held() const { return this->count_; }

  /// Print `text` now, or keep it until the next flush() while holding.
  /// @param level Level the line is printed at.
  /// @param tag Must outlive the hold: a string literal, as every logging tag here is.
  /// @param text The rendered line; copied (and truncated to FRAME_LOG_LINE_SIZE - 1) when held.
  void write(FrameLogLevel level, const char *tag, const char *text) {
    if (!this->holding_) {
      this->sink_(level, tag, text);
      return;
    }
    if (this->count_ >= FRAME_LOG_HELD_LINES) {
      if (this->dropped_ < UINT8_MAX)
        this->dropped_++;
      return;
    }
    Line &line = this->lines_[this->count_++];
    line.level = level;
    line.tag = tag;
    snprintf(line.text, sizeof(line.text), "%s", text);
  }

  /// Print the kept lines in the order they were written, then a note if any did not fit. Keeps
  /// holding.
  void flush() {
    for (uint8_t i = 0; i < this->count_; i++)
      this->sink_(this->lines_[i].level, this->lines_[i].tag, this->lines_[i].text);
    this->count_ = 0;
    if (this->dropped_ > 0) {
      char note[FRAME_LOG_NOTE_SIZE];
      snprintf(note, sizeof(note), "%u frame-log lines not printed (hold buffer full)", this->dropped_);
      this->sink_(FrameLogLevel::WARN, "io_frame", note);
      this->dropped_ = 0;
    }
  }

  /// Print the kept lines and stop holding.
  void release() {
    this->flush();
    this->holding_ = false;
  }

 private:
  /// One kept line.
  struct Line {
    FrameLogLevel level{FrameLogLevel::INFO};
    const char *tag{""};
    char text[FRAME_LOG_LINE_SIZE]{};
  };

  Sink sink_;
  bool holding_{false};
  uint8_t count_{0};
  uint8_t dropped_{0};
  Line lines_[FRAME_LOG_HELD_LINES]{};
};

#ifdef IOHOME_FRAME_LOG
/// @brief Print one frame-log line through ESPHome's logger at its level.
inline void frame_log_print(FrameLogLevel level, const char *tag, const char *text) {
  switch (level) {
    case FrameLogLevel::WARN:
      ESP_LOGW(tag, "%s", text);
      break;
    case FrameLogLevel::INFO:
      ESP_LOGI(tag, "%s", text);
      break;
    case FrameLogLevel::DEBUG:
      ESP_LOGD(tag, "%s", text);
      break;
  }
}

/// @brief The one hold buffer every frame-log line goes through (about 2 KB, frame-log builds only).
inline FrameLogBuffer &frame_log_buffer() {
  static FrameLogBuffer buffer(frame_log_print);
  return buffer;
}

/// @brief Write a printf-style frame-log line; held while frame_log_hold() is in effect.
/// Driver-level reception/transmission diagnostics use this instead of ESP_LOGx, so they are held
/// with the frame lines they belong to.
inline void frame_log_line(FrameLogLevel level, const char *tag, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
inline void frame_log_line(FrameLogLevel level, const char *tag, const char *format, ...) {
  char text[FRAME_LOG_LINE_SIZE];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  frame_log_buffer().write(level, tag, text);
}

/// @brief Hold frame-log lines until frame_log_release() (see FrameLogBuffer).
inline void frame_log_hold() { frame_log_buffer().hold(); }
/// @brief Print the held lines; keep holding.
inline void frame_log_flush() { frame_log_buffer().flush(); }
/// @brief Print the held lines and stop holding.
inline void frame_log_release() { frame_log_buffer().release(); }

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
    frame_log_line(FrameLogLevel::INFO, "io_frame", "%s [%u bytes] freq=%" PRIu32 " preamble=%u cmd=%s %s: %s", prefix,
                   len, freq, preamble, cmd_str, flags, hex);
  else
    frame_log_line(FrameLogLevel::INFO, "io_frame", "%s [%u bytes] freq=%" PRIu32 " cmd=%s %s: %s", prefix, len, freq,
                   cmd_str, flags, hex);
}
#else
// Without the frame log nothing is ever held, so the exchange engine's calls compile to nothing.
inline void frame_log_hold() {}
inline void frame_log_flush() {}
inline void frame_log_release() {}
#endif

}  // namespace home_io_control
}  // namespace esphome