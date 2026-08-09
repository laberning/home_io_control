/// @file tuning_registry_test.cpp
/// @brief Tests for the table-driven tuning parameter registry.
///
/// Pins the exact set of registered parameters (the cross-check anchor for
/// scripts/check-tuning-sync.py) and verifies that every number parameter and every
/// legal select option round-trips through the hub's update_/get_ dispatch, which now
/// routes through tuning_registry.h.

#include "hub_core.h"
#include "tuning_registry.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

using namespace esphome::home_io_control;
using test::TestableHubComponent;

namespace {

/// Set up a component with a mock radio so radio-affecting updates have a target.
void setup_component(TestableHubComponent &comp, MockRadio &radio) { comp.radio_ = &radio; }

std::set<std::string> number_param_names() {
  std::set<std::string> names;
  for (const TuningNumberParam *p = tuning_number_params_begin(); p != tuning_number_params_end(); ++p)
    names.insert(p->name);
  return names;
}

std::set<std::string> select_param_names() {
  std::set<std::string> names;
  for (const TuningSelectParam *p = tuning_select_params_begin(); p != tuning_select_params_end(); ++p)
    names.insert(p->name);
  return names;
}

}  // namespace

// ============================================================================
// Table contents: the exact parameter inventory (cross-language sync anchor)
// ============================================================================

TEST(TuningRegistry, NumberTableContainsExactlyExpectedParameters) {
  const std::set<std::string> expected = {
      "sx1262_response_preamble",      "sx1262_post_tx_settle_us",      "sx1276_response_preamble",
      "sx1276_discovery_hop_slice_ms", "sx1262_discovery_hop_slice_ms", "lr1121_response_preamble",
      "lr1121_post_tx_settle_us",      "lr1121_discovery_hop_slice_ms", "lbt_max_retries",
      "lbt_rssi_threshold_dbm",        "pairing_discovery_wait_ms",     "pairing_discovery_initial_dwell_ms",
      "pairing_key_exchange_retries",
  };
  EXPECT_EQ(number_param_names(), expected) << "number table drifted from the expected inventory";
}

TEST(TuningRegistry, SelectTableContainsExactlyExpectedParameters) {
  const std::set<std::string> expected = {
      "sx1262_rx_bandwidth",         "sx1276_rx_bandwidth",           "lr1121_rx_bandwidth",
      "pairing_discovery_commands",  "pairing_discovery_destination", "pairing_discovery_payload",
      "pairing_discovery_low_power",
  };
  EXPECT_EQ(select_param_names(), expected) << "select table drifted from the expected inventory";
}

TEST(TuningRegistry, TotalParameterCountIsTwenty) {
  EXPECT_EQ(number_param_names().size() + select_param_names().size(), 20u);
}

// ============================================================================
// Number parameters round-trip through update_/get_
// ============================================================================

TEST(TuningRegistry, EveryNumberParameterRoundTrips) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);

  // Distinct, small positive values that fit every field's storage type (uint8/int16/uint16).
  float value = 11.0F;
  for (const TuningNumberParam *p = tuning_number_params_begin(); p != tuning_number_params_end(); ++p) {
    comp.update_tuning_number(p->name, value);
    EXPECT_FLOAT_EQ(comp.get_tuning_number_value(p->name), value) << "parameter did not round-trip: " << p->name;
    value += 7.0F;
  }
}

// ============================================================================
// Select parameters round-trip every legal option
// ============================================================================

TEST(TuningRegistry, EverySelectOptionRoundTrips) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);

  const std::vector<std::pair<std::string, std::vector<std::string>>> legal_options = {
      {"sx1262_rx_bandwidth", {"58.6", "78.2", "117.3", "156.2", "187.2"}},
      {"sx1276_rx_bandwidth", {"20.8", "41.7", "62.5", "83.3", "125.0"}},
      {"lr1121_rx_bandwidth", {"39.0", "46.9", "58.6", "78.2", "117.3", "156.2", "187.2"}},
      {"pairing_discovery_commands", {"0x28", "0x2E", "0x28,0x2E"}},
      {"pairing_discovery_destination", {"auto", "0x00003B", "0x00003F"}},
      {"pairing_discovery_payload", {"none", "0x00"}},
      {"pairing_discovery_low_power", {"Off", "On"}},
  };

  for (const auto &entry : legal_options) {
    ASSERT_NE(find_tuning_select(entry.first), nullptr) << "expected select parameter missing: " << entry.first;
    for (const std::string &option : entry.second) {
      comp.update_tuning_select(entry.first, option);
      EXPECT_EQ(comp.get_tuning_select_value(entry.first), option)
          << "option did not round-trip: " << entry.first << " = " << option;
    }
  }
}

// ============================================================================
// Unknown parameter names are safe no-ops
// ============================================================================

TEST(TuningRegistry, UnknownNamesDoNotCrashAndReturnDefaults) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);

  EXPECT_EQ(find_tuning_number("does_not_exist"), nullptr);
  EXPECT_EQ(find_tuning_select("does_not_exist"), nullptr);

  // update_ on unknown names must be safe no-ops.
  comp.update_tuning_number("does_not_exist", 42.0F);
  comp.update_tuning_select("does_not_exist", "whatever");

  EXPECT_FLOAT_EQ(comp.get_tuning_number_value("does_not_exist"), 0.0F);
  EXPECT_EQ(comp.get_tuning_select_value("does_not_exist"), "");
}
