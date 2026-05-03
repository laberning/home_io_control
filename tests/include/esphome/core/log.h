#pragma once

#include <cstdarg>

#ifdef __GNUC__
#define ESPHOME_ATTRIBUTE_FORMAT(x, y, z) __attribute__((format(printf, x, y, z)))
#else
#define ESPHOME_ATTRIBUTE_FORMAT(x, y, z)
#endif

// NOLINT macros
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
