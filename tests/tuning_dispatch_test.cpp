/// @file tuning_dispatch_test.cpp
/// @brief Tests for the Home Assistant tuning dispatch logic in hub_core.cpp.
///
/// Exercises IOHomeControlComponent::update_tuning_number() and
/// update_tuning_select(), including the comma-separated discovery command
/// parser and the enum/destination/payload string decoding. Private members are
/// reachable because the host test build compiles with -fno-access-control.

#include "hub_core.h"
#include "proto_frame.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

namespace {

/// Build a component with a mock radio so radio-affecting updates have a target.
IOHomeControlComponent make_component() {
  IOHomeControlComponent comp;
  comp.radio_ = new MockRadio();
  return comp;
}

}  // namespace

// ============================================================================
// Numeric parameter dispatch
// ============================================================================

TEST(TuningDispatch, NumberUpdatesEachParameter) {
  IOHomeControlComponent comp = make_component();

  comp.update_tuning_number("sx1262_response_preamble", 96);
  EXPECT_EQ(comp.tuning_.sx1262_response_preamble, 96);

  comp.update_tuning_number("sx1262_post_tx_settle_us", 750);
  EXPECT_EQ(comp.tuning_.sx1262_post_tx_settle_us, 750);

  comp.update_tuning_number("sx1276_discovery_hop_slice_ms", 20);
  EXPECT_EQ(comp.tuning_.sx1276_discovery_hop_slice_ms, 20);

  comp.update_tuning_number("sx1262_discovery_hop_slice_ms", 250);
  EXPECT_EQ(comp.tuning_.sx1262_discovery_hop_slice_ms, 250);

  comp.update_tuning_number("lbt_max_retries", 1);
  EXPECT_EQ(comp.tuning_.lbt_max_retries, 1);

  comp.update_tuning_number("lbt_rssi_threshold_dbm", -45);
  EXPECT_EQ(comp.tuning_.lbt_rssi_threshold_dbm, -45);

  comp.update_tuning_number("pairing_discovery_wait_ms", 3000);
  EXPECT_EQ(comp.tuning_.pairing_discovery_wait_ms, 3000);

  comp.update_tuning_number("pairing_discovery_initial_dwell_ms", 500);
  EXPECT_EQ(comp.tuning_.pairing_discovery_initial_dwell_ms, 500);

  comp.update_tuning_number("pairing_key_exchange_retries", 5);
  EXPECT_EQ(comp.tuning_.pairing_key_exchange_retries, 5);
}

TEST(TuningDispatch, NumberIgnoresUnknownParameter) {
  IOHomeControlComponent comp = make_component();
  comp.update_tuning_number("does_not_exist", 123);
  // Defaults remain untouched.
  EXPECT_EQ(comp.tuning_.sx1262_response_preamble, SX1262_RESPONSE_PREAMBLE);
}

// ============================================================================
// Select parameter dispatch
// ============================================================================

TEST(TuningDispatch, SelectBandwidth) {
  IOHomeControlComponent comp = make_component();
  comp.update_tuning_select("sx1262_rx_bandwidth", "156.2");
  EXPECT_EQ(comp.tuning_.sx1262_rx_bandwidth, SX1262RxBandwidth::BW_156_2_KHZ);
}

TEST(TuningDispatch, SelectDiscoveryCommandsCommaSeparated) {
  IOHomeControlComponent comp = make_component();
  comp.update_tuning_select("pairing_discovery_commands", "0x28, 0x2E");
  ASSERT_EQ(comp.tuning_.pairing_discovery_commands.size(), 2);
  EXPECT_EQ(comp.tuning_.pairing_discovery_commands[0], DiscoveryCommand::DISCOVER);
  EXPECT_EQ(comp.tuning_.pairing_discovery_commands[1], DiscoveryCommand::DISCOVER_ALT);
}

TEST(TuningDispatch, SelectDiscoveryCommandsSingle) {
  IOHomeControlComponent comp = make_component();
  comp.update_tuning_select("pairing_discovery_commands", "0x2E");
  ASSERT_EQ(comp.tuning_.pairing_discovery_commands.size(), 1);
  EXPECT_EQ(comp.tuning_.pairing_discovery_commands[0], DiscoveryCommand::DISCOVER_ALT);
}

TEST(TuningDispatch, SelectDiscoveryCommandsDropsInvalidTokens) {
  IOHomeControlComponent comp = make_component();
  comp.update_tuning_select("pairing_discovery_commands", "0x28,bogus,0x2A");
  ASSERT_EQ(comp.tuning_.pairing_discovery_commands.size(), 2);
  EXPECT_EQ(comp.tuning_.pairing_discovery_commands[0], DiscoveryCommand::DISCOVER);
  EXPECT_EQ(comp.tuning_.pairing_discovery_commands[1], DiscoveryCommand::DISCOVER_SPE);
}

TEST(TuningDispatch, SelectDestinationExplicitAndAuto) {
  IOHomeControlComponent comp = make_component();

  comp.update_tuning_select("pairing_discovery_destination", "0x00003F");
  EXPECT_FALSE(comp.tuning_.pairing_discovery_destination_auto);
  EXPECT_EQ(comp.tuning_.pairing_discovery_destination[2], ADDRESS_SUFFIX_BROADCAST);

  comp.update_tuning_select("pairing_discovery_destination", "0x00003B");
  EXPECT_FALSE(comp.tuning_.pairing_discovery_destination_auto);
  EXPECT_EQ(comp.tuning_.pairing_discovery_destination[2], ADDRESS_SUFFIX_DISCOVERY);

  comp.update_tuning_select("pairing_discovery_destination", "auto");
  EXPECT_TRUE(comp.tuning_.pairing_discovery_destination_auto);
}

TEST(TuningDispatch, SelectPayload) {
  IOHomeControlComponent comp = make_component();

  comp.update_tuning_select("pairing_discovery_payload", "0x00");
  EXPECT_TRUE(comp.tuning_.pairing_discovery_payload_enabled);
  EXPECT_EQ(comp.tuning_.pairing_discovery_payload, 0x00);

  comp.update_tuning_select("pairing_discovery_payload", "none");
  EXPECT_FALSE(comp.tuning_.pairing_discovery_payload_enabled);
}

TEST(TuningDispatch, SelectLowPower) {
  IOHomeControlComponent comp = make_component();

  comp.update_tuning_select("pairing_discovery_low_power", "On");
  EXPECT_TRUE(comp.tuning_.pairing_discovery_low_power);

  comp.update_tuning_select("pairing_discovery_low_power", "Off");
  EXPECT_FALSE(comp.tuning_.pairing_discovery_low_power);
}

// ============================================================================
// Initial-state seeding (get_*_value), used to publish HA entity boot values
// ============================================================================

TEST(TuningDispatch, GetNumberValueReturnsDefaultsThenUpdates) {
  IOHomeControlComponent comp = make_component();

  // Defaults come from the single-source constants in proto_frame.h.
  EXPECT_FLOAT_EQ(comp.get_tuning_number_value("sx1262_post_tx_settle_us"), SX1262_POST_TX_SETTLE_US);
  EXPECT_FLOAT_EQ(comp.get_tuning_number_value("sx1276_discovery_hop_slice_ms"), SX1276_DISCOVERY_HOP_SLICE_MS);
  EXPECT_FLOAT_EQ(comp.get_tuning_number_value("pairing_key_exchange_retries"), PAIRING_KEY_EXCHANGE_RETRIES);

  comp.update_tuning_number("sx1262_post_tx_settle_us", 750);
  EXPECT_FLOAT_EQ(comp.get_tuning_number_value("sx1262_post_tx_settle_us"), 750);
}

TEST(TuningDispatch, GetNumberValueUnknownReturnsZero) {
  IOHomeControlComponent comp = make_component();
  EXPECT_FLOAT_EQ(comp.get_tuning_number_value("nope"), 0.0F);
}

TEST(TuningDispatch, GetSelectValueMatchesOptionStrings) {
  IOHomeControlComponent comp = make_component();

  // Defaults.
  EXPECT_EQ(comp.get_tuning_select_value("sx1262_rx_bandwidth"), "117.3");
  EXPECT_EQ(comp.get_tuning_select_value("pairing_discovery_commands"), "0x28");
  EXPECT_EQ(comp.get_tuning_select_value("pairing_discovery_destination"), "auto");
  EXPECT_EQ(comp.get_tuning_select_value("pairing_discovery_payload"), "none");
  EXPECT_EQ(comp.get_tuning_select_value("pairing_discovery_low_power"), "Off");

  // Round-trip: a value set via update_ is reported back as the same option string.
  comp.update_tuning_select("sx1262_rx_bandwidth", "156.2");
  EXPECT_EQ(comp.get_tuning_select_value("sx1262_rx_bandwidth"), "156.2");

  comp.update_tuning_select("pairing_discovery_destination", "0x00003F");
  EXPECT_EQ(comp.get_tuning_select_value("pairing_discovery_destination"), "0x00003F");

  // A multi-command preset round-trips as the same comma-separated dropdown option.
  comp.update_tuning_select("pairing_discovery_commands", "0x28,0x2E");
  EXPECT_EQ(comp.get_tuning_select_value("pairing_discovery_commands"), "0x28,0x2E");
}

TEST(TuningDispatch, GetSelectValueUnknownReturnsEmpty) {
  IOHomeControlComponent comp = make_component();
  EXPECT_EQ(comp.get_tuning_select_value("nope"), "");
}
