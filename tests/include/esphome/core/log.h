#pragma once

#include <cstdarg>

// x is the archetype (printf/scanf), y the 1-based index of the format string, z the index of the
// first argument to check against it. The archetype must come from the caller, not be hardcoded
// here — `format(printf, x, y, z)` would pass four arguments to an attribute that takes three,
// making every use a compile error. That went unnoticed while the macro had no users; `sink()`
// below is now one, so it stays honest.
#ifdef __GNUC__
#define ESPHOME_ATTRIBUTE_FORMAT(x, y, z) __attribute__((format(x, y, z)))
#else
#define ESPHOME_ATTRIBUTE_FORMAT(x, y, z)
#endif

#ifndef ESPHOME_IGNORE_FORMAT
#define ESPHOME_IGNORE_FORMAT(x, y) ((void) sizeof(x), (void) sizeof(y))
#endif

// No-op logging macros for host unit tests. The arguments are named in a dead `if (false)`
// branch so the compiler still sees every logged variable as *used*: without this, any value
// computed solely to be logged looks unused on host while being live in firmware. That used to
// force -Wno-unused-variable into HOST_CXXFLAGS; it no longer does, because of this stub.
// Do NOT collapse these back to a plain `do { } while (0)` — that silently reintroduces the
// whole unused-variable warning class on host.
//
// The branch has to be *dead*, not just cheap: logged arguments must stay unevaluated, as they
// were before this stub existed. A live call would run every logged expression on every log line
// — firing side effects and passing non-trivially-copyable arguments through `...` for real — and
// the optimizer is not available to save us, since the host build is -O0 (only the ASan variant
// is -O1). `if (false)` still type-checks the call and marks its operands used, but the constant
// condition folds in the front end, so no call reaches codegen even unoptimized.
//
// The sink also carries a printf format attribute, so the host build type-checks every ESP_LOG*
// format string against its arguments — the class of bug that cost this project 8 files' worth of
// %u/PRIu32 fixes when the ESP-IDF toolchain moved. Note that host and firmware disagree on the
// width of size_t/uint32_t, so use the width-agnostic specifiers (%zu, PRIu32) rather than %u to
// satisfy both.
namespace esphome {
namespace host_log_stub {
inline void sink(const char *, const char *, ...) ESPHOME_ATTRIBUTE_FORMAT(printf, 2, 3);
inline void sink(const char *, const char *, ...) {}
}  // namespace host_log_stub
}  // namespace esphome

#define IOHOME_HOST_LOG_STUB(tag, ...) \
  do { \
    if (false) \
      ::esphome::host_log_stub::sink(tag, __VA_ARGS__); \
  } while (0)

#define ESP_LOGI(tag, ...) IOHOME_HOST_LOG_STUB(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) IOHOME_HOST_LOG_STUB(tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) IOHOME_HOST_LOG_STUB(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) IOHOME_HOST_LOG_STUB(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) IOHOME_HOST_LOG_STUB(tag, __VA_ARGS__)
#define ESP_LOGCONFIG(tag, ...) IOHOME_HOST_LOG_STUB(tag, __VA_ARGS__)

// Helper macros used in dump_config()
#define YESNO(b) ((b) ? "YES" : "NO")

// Real ESPHome's LOG_STR() wraps a literal as `const LogString *` (PROGMEM on AVR, a plain
// reinterpret_cast elsewhere). Host tests have no LogString type, so this just passes the
// literal through as `const char *`, matching what Component::mark_failed(const char *) expects.
#define LOG_STR(s) (s)
