/// @file log_helpers_test.cpp
/// @brief Tests for the formatting helpers in log_helpers.h.

#include "log_helpers.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

// ============================================================================
// LogHelpers test suite
// ============================================================================
// The pure formatters behind every hub log line and both key-recovery reports. Includes only
// log_helpers.h, so it also proves that header compiles without the hub. The log-emitting helpers
// (log_multiline_result(), log_component_capture()) are not covered here: the host ESP_LOG stub
// discards its arguments, so their output is unobservable.

TEST(LogHelpers, FormatKeyHexIsUppercaseUnseparatedAndZeroPadded) {
  constexpr uint8_t key[AES_KEY_SIZE] = {0x00, 0x01, 0x0A, 0xAB, 0xCD, 0xEF, 0xF0, 0xFF,
                                         0x10, 0x20, 0x30, 0x40, 0x5A, 0x6B, 0x7C, 0x8D};
  EXPECT_EQ(detail::format_key_hex(key), "00010AABCDEFF0FF102030405A6B7C8D")
      << "a pasted system_key must round-trip: uppercase, two digits per byte, no separators";
}

TEST(LogHelpers, FormatNameAndHexRendersNameAndByte) {
  EXPECT_EQ(detail::format_name_and_hex("execute", 0x00), "execute(0x00)");
  EXPECT_EQ(detail::format_name_and_hex("status_update", 0xAB), "status_update(0xAB)");
}
