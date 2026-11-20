/// @file
/// @brief Records the lines the code under test logs, for tests that check a line is logged (and how often).
///
/// The host log stub (tests/include/esphome/core/log.h) never formats a line and never evaluates its
/// arguments, so a capture holds each line's level, tag and format string only: enough to count the
/// warnings a call issues, not to read their text.
#pragma once

#include "esphome/core/log.h"

#include <algorithm>
#include <string>
#include <vector>

namespace test {

/// Records every line logged while it lives; the previous recorder (if any) is restored on destruction.
class LogCapture : public esphome::host_log_stub::Recorder {
 public:
  /// One logged line, unformatted.
  struct Line {
    char level{};        ///< 'E', 'W', 'I', 'D', 'V' or 'C'.
    std::string tag;     ///< The line's tag.
    std::string format;  ///< The format string, arguments not filled in.
  };

  LogCapture() : previous_(esphome::host_log_stub::recorder()) { esphome::host_log_stub::recorder() = this; }
  ~LogCapture() override { esphome::host_log_stub::recorder() = previous_; }
  LogCapture(const LogCapture &) = delete;
  LogCapture &operator=(const LogCapture &) = delete;
  LogCapture(LogCapture &&) = delete;
  LogCapture &operator=(LogCapture &&) = delete;

  void record(char level, const char *tag, const char *format) override {
    lines_.push_back({level, tag != nullptr ? tag : "", format != nullptr ? format : ""});
  }

  /// @return The lines recorded so far, in order.
  [[nodiscard]] const std::vector<Line> &lines() const { return lines_; }

  /// @param level The level to count ('W' for warnings).
  /// @return How many lines of that level were recorded.
  [[nodiscard]] size_t count(char level) const {
    return static_cast<size_t>(
        std::count_if(lines_.begin(), lines_.end(), [level](const Line &line) { return line.level == level; }));
  }

  /// Forget the lines recorded so far (e.g. those of a test's setup).
  void clear() { lines_.clear(); }

 private:
  esphome::host_log_stub::Recorder *previous_;
  std::vector<Line> lines_;
};

}  // namespace test
