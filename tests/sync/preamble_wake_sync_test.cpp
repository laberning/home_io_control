/// @file preamble_wake_sync_test.cpp
/// @brief Guards the wake-up rule against a preamble tunable that could reach it by accident.
///
/// tx_wake_for() (proto_timing.h) calls a START frame's preamble a wake-up burst once its
/// programmed length reaches LONG_PREAMBLE bytes. A preamble is set as a duration (ADR 0097) and a
/// driver turns it into bytes with its own bits per preamble byte (8 on the software-PHY radios, 10
/// on the SX1276), so the same duration is a different byte count per radio. This test parses
/// tuning.py's _NUMBER_PARAMS at runtime (Python stays the single source of truth) and pins, for
/// both coders:
///   - every preamble duration tunable except the wake-up and the discovery one has a ceiling below
///     LONG_PREAMBLE bytes on every radio, so its frames are never treated as a wake-up burst;
///   - `low_power_wake_ms`'s floor is at least LONG_PREAMBLE bytes on every radio, so it is always
///     a wake-up burst;
///   - `pairing_discovery_preamble_ms`'s ceiling reaches LONG_PREAMBLE bytes on every radio, its
///     only wake-up values (it has no default of its own: discovery follows the start preamble);
///   - `low_power_wake_ms`'s default, LOW_POWER_WAKE_MS, is a value its YAML range accepts, on its
///     step.
/// A new preamble tunable or a raised ceiling fails here, which forces a decision about its wake
/// level.

#include "proto_timing.h"
#include "tuning_config.h"
#include "support/python_dict_parser.h"

#include <cmath>
#include <string>

using namespace esphome::home_io_control;

namespace {

constexpr const char *kTuningPy = "components/home_io_control/tuning.py";
constexpr const char *kDiscoveryPreamble = "CONF_PAIRING_DISCOVERY_PREAMBLE_MS";
constexpr const char *kWakePreamble = "CONF_LOW_POWER_WAKE_MS";

/// The two coders' bits per preamble byte: the software-PHY radios send 8, the SX1276 sends 10.
constexpr double kFewestBitsPerByte = 8.0;
constexpr double kMostBitsPerByte = 10.0;

/// Programmed bytes of `ms` at the protocol line rate (38400 bps).
double bytes_at(double ms, double bits_per_byte) { return ms * 38.4 / bits_per_byte; }

bool is_preamble_knob(const std::string &key) {
  return key.find("PREAMBLE") != std::string::npos || key == kWakePreamble;
}

TEST(PreambleWakeSync, OnlyTheWakeAndDiscoveryPreamblesCanReachTheWakeBurst) {
  const auto params = test::parse_python_range_dict(kTuningPy, "_NUMBER_PARAMS");
  ASSERT_FALSE(params.empty());

  int preamble_params = 0;
  for (const auto &[key, range] : params) {
    if (!is_preamble_knob(key))
      continue;
    preamble_params++;
    EXPECT_EQ(range.unit, "ms") << key << " is set as a duration";
    if (key == kDiscoveryPreamble) {
      // Its only wake-up values are the top of its range, on either coder.
      EXPECT_GE(bytes_at(range.max, kMostBitsPerByte), LONG_PREAMBLE) << key;
    } else if (key == kWakePreamble) {
      // The resting receiver's wake-up burst: every value it can take is a wake-up on either
      // coder, so tx_wake_for() labels it LONG whatever the user sets.
      EXPECT_GE(bytes_at(range.min, kMostBitsPerByte), LONG_PREAMBLE) << key;
    } else {
      EXPECT_LT(bytes_at(range.max, kFewestBitsPerByte), LONG_PREAMBLE)
          << key << " can reach the wake-up burst; decide its wake level";
    }
  }
  // Response, cold broadcast reply, start, pairing discovery, low-power wake-up.
  EXPECT_EQ(preamble_params, 5);
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

TEST(PreambleWakeSync, ParserReadsFractionalBounds) {
  const auto params = test::parse_python_range_dict(kTuningPy, "_NUMBER_PARAMS");
  ASSERT_EQ(params.count("CONF_RESPONSE_PREAMBLE_MS"), 1u);
  const auto &response = params.at("CONF_RESPONSE_PREAMBLE_MS");
  EXPECT_NE(response.min, std::floor(response.min)) << "the floor is a fraction of a millisecond";
  EXPECT_EQ(response.step, 0.5);
}

TEST(PreambleWakeSync, EveryDurationDefaultIsAValidValueOfItsKnob) {
  // ADR 0095 and 0097: the default of each duration knob must be accepted by YAML as it stands, on
  // the number entity's step.
  const auto params = test::parse_python_range_dict(kTuningPy, "_NUMBER_PARAMS");
  const TuningConfig defaults{};
  const struct {
    const char *key;
    float value;
    float compiled_default;
  } knobs[] = {
      {"CONF_LOW_POWER_WAKE_MS", defaults.low_power_wake_ms, LOW_POWER_WAKE_MS},
      {"CONF_START_PREAMBLE_MS", defaults.start_preamble_ms, START_PREAMBLE_MS},
      {"CONF_RESPONSE_PREAMBLE_MS", defaults.response_preamble_ms, RESPONSE_PREAMBLE_MS},
      {"CONF_COLD_BROADCAST_REPLY_PREAMBLE_MS", defaults.cold_broadcast_reply_preamble_ms,
       COLD_BROADCAST_REPLY_PREAMBLE_MS},
  };
  for (const auto &knob : knobs) {
    ASSERT_EQ(params.count(knob.key), 1u) << knob.key;
    const auto &range = params.at(knob.key);
    EXPECT_EQ(knob.value, knob.compiled_default) << knob.key;
    EXPECT_GE(knob.value, range.min) << knob.key;
    EXPECT_LE(knob.value, range.max) << knob.key;
    ASSERT_GT(range.step, 0) << knob.key;
    const double steps = (knob.value - range.min) / range.step;
    EXPECT_NEAR(steps, std::round(steps), 1e-9) << knob.key << " is on the number entity's step";
  }
}

}  // namespace
