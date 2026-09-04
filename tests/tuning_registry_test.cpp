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
      "sx1262_response_preamble",
      "sx1262_post_tx_settle_us",
      "sx1276_response_preamble",
      "sx1276_discovery_hop_slice_ms",
      "sx1262_discovery_hop_slice_ms",
      "lr1121_response_preamble",
      "lr1121_post_tx_settle_us",
      "lr1121_discovery_hop_slice_ms",
      "cold_broadcast_reply_preamble",
      "normal_start_preamble",
      "lbt_max_retries",
      "lbt_rssi_threshold_dbm",
      "pairing_discovery_preamble",
      "pairing_discovery_wait_ms",
      "pairing_discovery_initial_dwell_ms",
      "pairing_key_exchange_retries",
      "exchange_start_response_wait_ms",
      "exchange_response_wait_ms",
      "exchange_total_budget_ms",
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

TEST(TuningRegistry, TotalParameterCountIsTwentySix) {
  EXPECT_EQ(number_param_names().size() + select_param_names().size(), 26u);
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

TEST(TuningRegistry, EveryNumberParameterAppearsInSnapshotWhenNonDefault) {
  // Regression test: tuning_config_snapshot() is a hand-written if-chain, one line per field, that
  // does not derive from this registry table — a new NUMBER_PARAMS entry can round-trip through
  // update_/get_ (registry-driven, covered above) while staying invisible in the exact diagnostic
  // log (`tuning_config_full_snapshot()`, logged at the start of every discover_and_pair() attempt)
  // a user would paste into a bug report. Walks every registered number parameter, nudges it off
  // its default by +1 (small enough not to overflow any field's storage type — uint8_t included —
  // while still differing from any real default), and asserts the snapshot names it.
  for (const TuningNumberParam *p = tuning_number_params_begin(); p != tuning_number_params_end(); ++p) {
    TuningConfig cfg{};
    p->set(cfg, p->get(cfg) + 1.0F);
    const std::string snapshot = tuning_config_snapshot(cfg);
    EXPECT_NE(snapshot.find(std::string(p->name) + "="), std::string::npos)
        << p->name << " is missing from tuning_config_snapshot() despite being non-default";
  }
}

// ============================================================================
// Select parameters round-trip every legal option
// ============================================================================

TEST(TuningRegistry, EverySelectOptionRoundTrips) {
  TestableHubComponent comp;
  MockRadio radio;
  setup_component(comp, radio);

  // The three RX-bandwidth option lists are derived from the same constexpr tables production
  // uses (via tuning_config.h accessors), so a shipping option can never be silently skipped
  // here. Rendering matches production: bandwidth_to_string() on each row's kHz value.
  auto bandwidth_options = [](BandwidthTableView table) {
    std::vector<std::string> options;
    for (size_t i = 0; i < table.size; ++i)
      options.push_back(bandwidth_to_string(table.data[i].khz));
    return options;
  };

  const std::vector<std::pair<std::string, std::vector<std::string>>> legal_options = {
      {"sx1262_rx_bandwidth", bandwidth_options(sx1262_bandwidth_table())},
      {"sx1276_rx_bandwidth", bandwidth_options(sx1276_bandwidth_table())},
      {"lr1121_rx_bandwidth", bandwidth_options(lr1121_bandwidth_table())},
      {"pairing_discovery_commands", {"0x28", "0x2E", "0x28,0x2E"}},
      {"pairing_discovery_destination", {"auto", "0x00003B", "0x00003F"}},
      {"pairing_discovery_payload", {"none", "0x00"}},
      {"pairing_discovery_low_power", {"Off", "On"}},
  };

  const std::set<std::string> bandwidth_params = {"sx1262_rx_bandwidth", "sx1276_rx_bandwidth", "lr1121_rx_bandwidth"};
  size_t bandwidth_option_count = 0;
  for (const auto &entry : legal_options) {
    ASSERT_NE(find_tuning_select(entry.first), nullptr) << "expected select parameter missing: " << entry.first;
    for (const std::string &option : entry.second) {
      comp.update_tuning_select(entry.first, option);
      EXPECT_EQ(comp.get_tuning_select_value(entry.first), option)
          << "option did not round-trip: " << entry.first << " = " << option;
      if (bandwidth_params.count(entry.first) != 0)
        ++bandwidth_option_count;
    }
  }
  // Floor guard: this test exists to prove every *table-derived* bandwidth option round-trips, so
  // the guard is pinned to the bandwidth subtotal (7 SX1262 + 5 SX1276 + 7 LR1121 = 19), not to a
  // total that also counts the hand-written non-bandwidth rows. An exact assert on the subtotal is
  // both non-vacuous if a chip table is emptied (the original >= 27-total floor was not) and stable
  // when an unrelated non-bandwidth select option is added later.
  ASSERT_EQ(bandwidth_option_count, 19u);
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
