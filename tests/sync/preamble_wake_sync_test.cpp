/// @file preamble_wake_sync_test.cpp
/// @brief Guards the wake-up rule against a preamble tunable that could reach it by accident.
///
/// tx_wake_for() (proto_timing.h) calls a START frame's preamble a wake-up burst once it reaches
/// LONG_PREAMBLE. That is only right while no preamble tunable other than the discovery broadcast
/// can be set that high. This test parses tuning.py's _NUMBER_PARAMS at runtime (Python stays the
/// single source of truth) and pins:
///   - every `*_PREAMBLE` tunable's ceiling is below LONG_PREAMBLE, so its frames are never
///     treated as a wake-up burst;
///   - `low_power_wake_preamble`'s floor is LONG_PREAMBLE, so it is always a wake-up burst;
///   - `pairing_discovery_preamble`'s ceiling is exactly LONG_PREAMBLE, its default and its only
///     wake-up value.
/// A new preamble tunable or a raised ceiling fails here, which forces a decision about its wake
/// level.

#include "proto_timing.h"
#include "support/python_dict_parser.h"

#include <string>

using namespace esphome::home_io_control;

namespace {

constexpr const char *kTuningPy = "components/home_io_control/tuning.py";
constexpr const char *kDiscoveryPreamble = "CONF_PAIRING_DISCOVERY_PREAMBLE";
constexpr const char *kWakePreamble = "CONF_LOW_POWER_WAKE_PREAMBLE";

bool ends_with(const std::string &s, const std::string &suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

TEST(PreambleWakeSync, OnlyTheDiscoveryPreambleCanReachTheWakeBurst) {
  const auto params = test::parse_python_range_dict(kTuningPy, "_NUMBER_PARAMS");
  ASSERT_FALSE(params.empty());

  int preamble_params = 0;
  for (const auto &[key, range] : params) {
    if (!ends_with(key, "_PREAMBLE"))
      continue;
    preamble_params++;
    if (key == kDiscoveryPreamble) {
      EXPECT_EQ(range.max, LONG_PREAMBLE) << key;
    } else if (key == kWakePreamble) {
      // The resting receiver's wake-up burst: every value it can take is a wake-up, so its floor is
      // LONG_PREAMBLE and tx_wake_for() labels it LONG whatever the user sets.
      EXPECT_GE(range.min, LONG_PREAMBLE) << key;
    } else {
      EXPECT_LT(range.max, LONG_PREAMBLE) << key << " can reach the wake-up burst; decide its wake level";
      EXPECT_FALSE(is_wake_preamble(static_cast<uint16_t>(range.max))) << key;
    }
  }
  // Response (x3), cold broadcast reply, normal start, pairing discovery, low-power wake-up.
  EXPECT_EQ(preamble_params, 7);
  EXPECT_EQ(params.count(kDiscoveryPreamble), 1u);
}

TEST(PreambleWakeSync, ParserReadsSignedRangesAndUnits) {
  const auto params = test::parse_python_range_dict(kTuningPy, "_NUMBER_PARAMS");
  ASSERT_EQ(params.count("CONF_LBT_RSSI_THRESHOLD_DBM"), 1u);
  const auto &lbt = params.at("CONF_LBT_RSSI_THRESHOLD_DBM");
  EXPECT_LT(lbt.min, 0);
  EXPECT_LT(lbt.max, 0);
  EXPECT_EQ(lbt.unit, "dBm");
}

}  // namespace
