#include <gtest/gtest.h>

#include "proto_timing.h"

#include <cstdint>

using namespace esphome::home_io_control;

// ============================================================================
// TxWake test suite
// ============================================================================
// tx_wake_for() over every preamble this project transmits, with START set and clear: only a
// START frame carrying the wake-up burst asks for the long wake-up, any other START frame for the
// short one, and a continuation frame for none. A new preamble constant belongs in this table.
// ============================================================================

namespace {

struct PreambleCase {
  const char *name;
  uint16_t preamble;
  TxWake start_wake;  ///< Expected level with START set.
};

/// Programmed bytes of a duration at the 38400 bps line rate and a coder's bits per preamble byte:
/// 8 for a raw byte stream, 10 for the SX1276 (RadioDriver::preamble_bytes_for_ms() is the runtime form).
constexpr uint16_t bytes_for_ms(double ms, uint8_t bits_per_byte) {
  return static_cast<uint16_t>(ms * 38.4 / bits_per_byte + 0.5);
}

// Every preamble duration default, on both coders (a preamble is wake-up level by its programmed
// length, and the two coders give different lengths for one duration).
constexpr PreambleCase kPreambles[] = {
    {"SHORT_PREAMBLE", SHORT_PREAMBLE, TxWake::SHORT},
    {"START_PREAMBLE_MS (8 bits/byte)", bytes_for_ms(START_PREAMBLE_MS, 8), TxWake::SHORT},
    {"START_PREAMBLE_MS (10 bits/byte)", bytes_for_ms(START_PREAMBLE_MS, 10), TxWake::SHORT},
    {"COLD_BROADCAST_REPLY_PREAMBLE_MS (8 bits/byte)", bytes_for_ms(COLD_BROADCAST_REPLY_PREAMBLE_MS, 8),
     TxWake::SHORT},
    {"COLD_BROADCAST_REPLY_PREAMBLE_MS (10 bits/byte)", bytes_for_ms(COLD_BROADCAST_REPLY_PREAMBLE_MS, 10),
     TxWake::SHORT},
    {"RESPONSE_PREAMBLE_MS (8 bits/byte)", bytes_for_ms(RESPONSE_PREAMBLE_MS, 8), TxWake::SHORT},
    {"RESPONSE_PREAMBLE_MS (10 bits/byte)", bytes_for_ms(RESPONSE_PREAMBLE_MS, 10), TxWake::SHORT},
    {"LONG_PREAMBLE", LONG_PREAMBLE, TxWake::LONG},
    {"LOW_POWER_WAKE_MS (8 bits/byte)", bytes_for_ms(LOW_POWER_WAKE_MS, 8), TxWake::LONG},
    {"LOW_POWER_WAKE_MS (10 bits/byte)", bytes_for_ms(LOW_POWER_WAKE_MS, 10), TxWake::LONG},
};

TEST(TxWake, EveryPreambleConstantWithStartSet) {
  for (const auto &c : kPreambles)
    EXPECT_EQ(tx_wake_for(true, c.preamble), c.start_wake) << c.name;
}

TEST(TxWake, ContinuationFrameNeverWakes) {
  for (const auto &c : kPreambles)
    EXPECT_EQ(tx_wake_for(false, c.preamble), TxWake::NONE) << c.name;
}

TEST(TxWake, WakePreambleThresholdIsLongPreamble) {
  EXPECT_FALSE(is_wake_preamble(LONG_PREAMBLE - 1));
  EXPECT_TRUE(is_wake_preamble(LONG_PREAMBLE));
  EXPECT_TRUE(is_wake_preamble(UINT16_MAX));
}

// Evaluated at compile time too: the helpers are usable in constant expressions.
static_assert(tx_wake_for(true, LONG_PREAMBLE) == TxWake::LONG);
static_assert(tx_wake_for(false, LONG_PREAMBLE) == TxWake::NONE);

}  // namespace
