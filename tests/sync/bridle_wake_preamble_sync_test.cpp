/// @file bridle_wake_preamble_sync_test.cpp
/// @brief Pins the preamble length a Bridle 868 MHz radio must offer to the longest wake-up the hub can ask for.
///
/// `Io868GfskPhy::radio_fits()` refuses a co-processor whose radio caps `tx.preamble_bytes` below
/// `LOW_POWER_WAKE_PREAMBLE_MAX`. The wake-up preamble is a tunable (`low_power_wake_preamble`) whose range lives in
/// tuning.py; this test parses that range at runtime (Python stays the single source of truth) and fails when the
/// two drift apart, so a raised ceiling cannot reach a radio that was never checked for it.

#include "tuning_config.h"
#include "support/python_dict_parser.h"

using namespace esphome::home_io_control;

namespace {

constexpr const char *kTuningPy = "components/home_io_control/tuning.py";
constexpr const char *kWakePreamble = "CONF_LOW_POWER_WAKE_PREAMBLE";

TEST(BridleWakePreambleSync, RequiredPreambleCapacityIsTheTunableMaximum) {
  const auto params = test::parse_python_range_dict(kTuningPy, "_NUMBER_PARAMS");
  ASSERT_EQ(params.count(kWakePreamble), 1u);
  EXPECT_EQ(params.at(kWakePreamble).max, LOW_POWER_WAKE_PREAMBLE_MAX);
}

}  // namespace
