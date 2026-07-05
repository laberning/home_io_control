#include "tuning_config.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

// ============================================================================
// TuningConfig default and override tests
// ============================================================================

TEST(TuningConfig, DefaultsMatchPlan) {
  TuningConfig cfg{};
  EXPECT_EQ(cfg.sx1262_rx_bandwidth, SX1262RxBandwidth::BW_117_3_KHZ);
  EXPECT_EQ(cfg.sx1262_response_preamble, 64);
  EXPECT_EQ(cfg.sx1262_post_tx_settle_us, 500);
  EXPECT_EQ(cfg.sx1276_discovery_hop_slice_ms, 5);
  EXPECT_EQ(cfg.sx1262_discovery_hop_slice_ms, 200);
  EXPECT_EQ(cfg.lbt_max_retries, 5);
  EXPECT_EQ(cfg.lbt_rssi_threshold_dbm, -90);
  ASSERT_EQ(cfg.pairing_discovery_commands.size(), 1);
  EXPECT_EQ(static_cast<uint8_t>(cfg.pairing_discovery_commands[0]), 0x28);
  EXPECT_TRUE(cfg.pairing_discovery_destination_auto);
  EXPECT_FALSE(cfg.pairing_discovery_payload_enabled);
  EXPECT_FALSE(cfg.pairing_discovery_low_power);
  EXPECT_EQ(cfg.pairing_discovery_wait_ms, 2000);
  EXPECT_EQ(cfg.pairing_discovery_initial_dwell_ms, 300);
  EXPECT_EQ(cfg.pairing_key_exchange_retries, 3);
  EXPECT_FALSE(cfg.active);
}

TEST(TuningConfig, BandwidthStringRoundTrip) {
  // from_string accepts bare numbers and (tolerantly) a kHz suffix in any case.
  EXPECT_EQ(sx1262_bandwidth_from_string("58.6"), SX1262RxBandwidth::BW_58_6_KHZ);
  EXPECT_EQ(sx1262_bandwidth_from_string("78.2kHz"), SX1262RxBandwidth::BW_78_2_KHZ);
  EXPECT_EQ(sx1262_bandwidth_from_string("117.3"), SX1262RxBandwidth::BW_117_3_KHZ);
  EXPECT_EQ(sx1262_bandwidth_from_string("156.2 kHz"), SX1262RxBandwidth::BW_156_2_KHZ);
  EXPECT_EQ(sx1262_bandwidth_from_string("187.2KHZ"), SX1262RxBandwidth::BW_187_2_KHZ);
  // 93.8 kHz is a real SX1262 bandwidth but intentionally not exposed as an option.
  EXPECT_FALSE(sx1262_bandwidth_from_string("93.8").has_value());
  // to_string emits a bare kHz number (the unit lives in the entity name).
  EXPECT_EQ(sx1262_bandwidth_to_string(SX1262RxBandwidth::BW_58_6_KHZ), "58.6");
  EXPECT_EQ(sx1262_bandwidth_to_string(SX1262RxBandwidth::BW_117_3_KHZ), "117.3");
  EXPECT_FLOAT_EQ(sx1262_bandwidth_to_khz(SX1262RxBandwidth::BW_58_6_KHZ), 58.6f);
  EXPECT_FLOAT_EQ(sx1262_bandwidth_to_khz(SX1262RxBandwidth::BW_117_3_KHZ), 117.3f);
  EXPECT_FLOAT_EQ(sx1262_bandwidth_to_khz(SX1262RxBandwidth::BW_187_2_KHZ), 187.2f);
}

TEST(TuningConfig, DiscoveryCommandStringRoundTrip) {
  auto cmd28 = discovery_command_from_string("0x28");
  ASSERT_TRUE(cmd28.has_value());
  EXPECT_EQ(discovery_command_to_string(cmd28.value()), "0x28");
  EXPECT_EQ(discovery_command_from_string("0x2A").value(), DiscoveryCommand::DISCOVER_SPE);
  EXPECT_EQ(discovery_command_from_string("0x2E").value(), DiscoveryCommand::DISCOVER_ALT);
  EXPECT_FALSE(discovery_command_from_string("0xFF").has_value());
}

TEST(TuningConfig, CommandsToStringFormat) {
  std::vector<DiscoveryCommand> cmds{DiscoveryCommand::DISCOVER, DiscoveryCommand::DISCOVER_ALT};
  EXPECT_EQ(discovery_commands_to_string(cmds), "[0x28,0x2E]");
}

TEST(TuningConfig, CommandsToCsvFormat) {
  EXPECT_EQ(discovery_commands_to_csv({}), "");
  EXPECT_EQ(discovery_commands_to_csv({DiscoveryCommand::DISCOVER}), "0x28");
  std::vector<DiscoveryCommand> cmds{DiscoveryCommand::DISCOVER, DiscoveryCommand::DISCOVER_SPE,
                                     DiscoveryCommand::DISCOVER_ALT};
  EXPECT_EQ(discovery_commands_to_csv(cmds), "0x28,0x2A,0x2E");
}

TEST(TuningConfig, DestinationResolver_Auto) {
  const uint8_t *dst = resolve_discovery_destination(0x28, true, BROADCAST_DISCOVER_ALT);
  EXPECT_EQ(dst[0], 0x00);
  EXPECT_EQ(dst[1], 0x00);
  EXPECT_EQ(dst[2], 0x3B);

  const uint8_t *dst2a = resolve_discovery_destination(0x2A, true, BROADCAST_DISCOVER_ALT);
  EXPECT_EQ(dst2a[2], 0x3B);

  const uint8_t *dst2e = resolve_discovery_destination(0x2E, true, BROADCAST_DISCOVER);
  EXPECT_EQ(dst2e[2], 0x3F);

  // Unknown command falls back to the standard 2W discovery broadcast.
  const uint8_t *dst_unknown = resolve_discovery_destination(0xFF, true, BROADCAST_DISCOVER_ALT);
  EXPECT_EQ(dst_unknown[2], 0x3B);
}

TEST(TuningConfig, DestinationResolver_Explicit) {
  const uint8_t *dst = resolve_discovery_destination(0x28, false, BROADCAST_DISCOVER_ALT);
  EXPECT_EQ(dst[2], 0x3F);
}

TEST(TuningConfig, SnapshotEmptyForDefaults) {
  TuningConfig cfg{};
  EXPECT_EQ(tuning_config_snapshot(cfg), "");
  EXPECT_EQ(tuning_config_full_snapshot(cfg), "Tuning: defaults active");
}

TEST(TuningConfig, SnapshotIncludesNonDefaults) {
  TuningConfig cfg{};
  cfg.sx1262_post_tx_settle_us = 750;
  cfg.pairing_discovery_commands = {DiscoveryCommand::DISCOVER, DiscoveryCommand::DISCOVER_ALT};
  cfg.pairing_discovery_payload_enabled = true;
  cfg.pairing_discovery_payload = 0x00;
  cfg.pairing_discovery_low_power = true;
  cfg.lbt_max_retries = 1;

  std::string snapshot = tuning_config_snapshot(cfg);
  EXPECT_NE(snapshot.find("sx1262_post_tx_settle_us=750"), std::string::npos);
  EXPECT_NE(snapshot.find("pairing_discovery_commands=[0x28,0x2E]"), std::string::npos);
  EXPECT_NE(snapshot.find("pairing_discovery_payload=0x00"), std::string::npos);
  EXPECT_NE(snapshot.find("pairing_discovery_low_power=true"), std::string::npos);
  EXPECT_NE(snapshot.find("lbt_max_retries=1"), std::string::npos);
}

TEST(TuningConfig, SnapshotOmitsUnchangedDefaults) {
  TuningConfig cfg{};
  cfg.sx1262_response_preamble = 96;  // only this override
  std::string snapshot = tuning_config_snapshot(cfg);
  EXPECT_NE(snapshot.find("sx1262_response_preamble=96"), std::string::npos);
  EXPECT_EQ(snapshot.find("sx1262_post_tx_settle_us"), std::string::npos);
  EXPECT_EQ(snapshot.find("lbt_max_retries"), std::string::npos);
}

TEST(TuningConfig, DestinationStringFormatting) {
  EXPECT_EQ(discovery_destination_to_string(true, BROADCAST_DISCOVER), "auto");
  EXPECT_EQ(discovery_destination_to_string(false, BROADCAST_DISCOVER), "0x00003B");
  EXPECT_EQ(discovery_destination_to_string(false, BROADCAST_DISCOVER_ALT), "0x00003F");
}

TEST(TuningConfig, PayloadStringFormatting) {
  EXPECT_EQ(discovery_payload_to_string(false, 0x00), "none");
  EXPECT_EQ(discovery_payload_to_string(true, 0x00), "0x00");
}

TEST(TuningConfig, UpdateLogLineFormat) {
  EXPECT_EQ(tuning_update_log_line("sx1262_post_tx_settle_us", "750"),
            "Tuning updated via HA: sx1262_post_tx_settle_us=750");
}
