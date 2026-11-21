#include <gtest/gtest.h>

#include "proto_timing.h"

#include <cstdint>

using namespace esphome::home_io_control;

// ============================================================================
// TxWake test suite
// ============================================================================
// tx_wake_for() over every preamble duration this project transmits, with START set and clear: only
// a START frame whose preamble lasts at least WAKE_PREAMBLE_MIN_MS asks for the long wake-up, any
// other START frame for the short one, and a continuation frame for none. A new preamble constant
// belongs in this table.
// ============================================================================

namespace {

struct PreambleCase {
  const char *name;
  float preamble_ms;
  TxWake start_wake;  ///< Expected level with START set.
};

// Every preamble duration default and the wake-up floor. The label follows the duration alone, so
// one row serves every radio (the exchange engine converts programmed bytes to a duration first).
constexpr PreambleCase kPreambles[] = {
    {"START_PREAMBLE_MS", START_PREAMBLE_MS, TxWake::SHORT},
    {"COLD_BROADCAST_REPLY_PREAMBLE_MS", COLD_BROADCAST_REPLY_PREAMBLE_MS, TxWake::SHORT},
    {"RESPONSE_PREAMBLE_MS", RESPONSE_PREAMBLE_MS, TxWake::SHORT},
    {"a 70 ms preamble, the ceiling of the other tunables", 70.0F, TxWake::SHORT},
    {"213 ms, 1024 bytes on a raw-stream radio", 213.3F, TxWake::SHORT},
    {"just below the threshold", 299.9F, TxWake::SHORT},
    {"the wake-up floor, 300 ms", 300.0F, TxWake::LONG},
    {"LOW_POWER_WAKE_MS", LOW_POWER_WAKE_MS, TxWake::LONG},
    {"the wake-up ceiling, 1000 ms", 1000.0F, TxWake::LONG},
};

TEST(TxWake, EveryPreambleDurationWithStartSet) {
  for (const auto &c : kPreambles)
    EXPECT_EQ(tx_wake_for(true, c.preamble_ms), c.start_wake) << c.name;
}

TEST(TxWake, ContinuationFrameNeverWakes) {
  for (const auto &c : kPreambles)
    EXPECT_EQ(tx_wake_for(false, c.preamble_ms), TxWake::NONE) << c.name;
}

TEST(TxWake, WakePreambleThresholdIs300Milliseconds) {
  EXPECT_EQ(WAKE_PREAMBLE_MIN_MS, 300.0F);
  EXPECT_FALSE(is_wake_preamble_ms(299.9F));
  EXPECT_TRUE(is_wake_preamble_ms(300.0F));
  EXPECT_TRUE(is_wake_preamble_ms(65535.0F));
}

// Evaluated at compile time too: the helpers are usable in constant expressions.
static_assert(tx_wake_for(true, 300.0F) == TxWake::LONG);
static_assert(tx_wake_for(true, 299.0F) == TxWake::SHORT);
static_assert(tx_wake_for(false, 300.0F) == TxWake::NONE);

}  // namespace
