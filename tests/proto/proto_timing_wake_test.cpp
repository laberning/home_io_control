#include <gtest/gtest.h>

#include "proto_timing.h"
#include "tuning_config.h"  // Per-chip response and start preambles

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

constexpr PreambleCase kPreambles[] = {
    {"SHORT_PREAMBLE", SHORT_PREAMBLE, TxWake::SHORT},
    {"NORMAL_START_PREAMBLE", NORMAL_START_PREAMBLE, TxWake::SHORT},
    {"COLD_BROADCAST_REPLY_PREAMBLE", COLD_BROADCAST_REPLY_PREAMBLE, TxWake::SHORT},
    {"PAIRING_DISCOVERY_PREAMBLE", PAIRING_DISCOVERY_PREAMBLE, TxWake::LONG},
    {"LONG_PREAMBLE", LONG_PREAMBLE, TxWake::LONG},
    {"SX1262_RESPONSE_PREAMBLE", SX1262_RESPONSE_PREAMBLE, TxWake::SHORT},
    {"SX1276_RESPONSE_PREAMBLE", SX1276_RESPONSE_PREAMBLE, TxWake::SHORT},
    {"LR1121_RESPONSE_PREAMBLE", LR1121_RESPONSE_PREAMBLE, TxWake::SHORT},
    {"SOFT_PHY_START_PREAMBLE", SOFT_PHY_START_PREAMBLE, TxWake::SHORT},
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
