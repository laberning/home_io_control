#pragma once

#include <cstdarg>

#ifdef __GNUC__
#define ESPHOME_ATTRIBUTE_FORMAT(x, y, z) __attribute__((format(printf, x, y, z)))
#else
#define ESPHOME_ATTRIBUTE_FORMAT(x, y, z)
#endif

#ifndef ESPHOME_IGNORE_FORMAT
#define ESPHOME_IGNORE_FORMAT(x, y) ((void) sizeof(x), (void) sizeof(y))
#endif

// No-op logging macros for host unit tests
#define ESP_LOGI(tag, fmt, ...) \
  do { \
  } while (0)
#define ESP_LOGD(fmt, ...) \
  do { \
  } while (0)
#define ESP_LOGW(fmt, ...) \
  do { \
  } while (0)
#define ESP_LOGE(fmt, ...) \
  do { \
  } while (0)
#define ESP_LOGCONFIG(fmt, ...) \
  do { \
  } while (0)

// Helper macros used in dump_config()
#define YESNO(b) ((b) ? "YES" : "NO")

// Real ESPHome's LOG_STR() wraps a literal as `const LogString *` (PROGMEM on AVR, a plain
// reinterpret_cast elsewhere). Host tests have no LogString type, so this just passes the
// literal through as `const char *`, matching what Component::mark_failed(const char *) expects.
#define LOG_STR(s) (s)
