/// @file bridle_wake_preamble_sync_test.cpp
/// @brief Pins that the default wake-up fits the preamble capacity the Bridle specification requires of a radio.
///
/// `Io868GfskPhy::radio_fits()` accepts any radio with `IO868_MIN_PREAMBLE_CAPACITY_BYTES`. The wake-up the hub
/// sends by default (`LOW_POWER_WAKE_MS`, a raw-stream preamble of 8 bit times a byte) has to fit that capacity,
/// or a conforming radio would refuse the default configuration. A longer configured wake-up is checked against
/// the radio at setup and on every change (`RadioDriver::accepts_tuning()`).

#include "proto_timing.h"
#include "radio_io868_gfsk_phy.h"
#include "tuning_config.h"

#include <cmath>

#include <gtest/gtest.h>

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::bridle;

TEST(BridleWakePreambleSync, TheDefaultWakeUpFitsTheCapacityTheSpecRequires) {
  const double bytes = LOW_POWER_WAKE_MS * IO868_LINE_RATE_BPS / 1000.0 / IO868_PREAMBLE_BITS_PER_BYTE;
  EXPECT_LE(std::lround(bytes), IO868_MIN_PREAMBLE_CAPACITY_BYTES);
}
