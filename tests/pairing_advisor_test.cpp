/// @file pairing_advisor_test.cpp
/// @brief Tests for PairingAdvisor's read-only diagnosis of a completed pairing attempt.

#include "pairing_advisor.h"
#include "pairing_telemetry.h"
#include "proto_constants.h"
#include "proto_frame.h"
#include "proto_timing.h"

#include "support/test_helpers.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::advisor;

namespace {

/// Build a frame for telemetry tests. `is_2w=false` sets the CTRL0 1W protocol bit.
IoFrame build_frame(bool is_2w, const uint8_t src[NODE_ID_SIZE], const uint8_t dst[NODE_ID_SIZE], uint8_t cmd) {
  IoFrame f{};
  init_frame(f, is_2w, false, false, false);
  set_dst(f, dst);
  set_src(f, src);
  set_cmd(f, cmd, nullptr, 0);
  return f;
}

const uint8_t OWN_ID[NODE_ID_SIZE] = {0x11, 0x22, 0x33};
const uint8_t REMOTE_SRC[NODE_ID_SIZE] = {0x2F, 0x9A, 0x98};
const uint8_t OTHER_CONTROLLER_ID[NODE_ID_SIZE] = {0x99, 0x88, 0x77};
const uint8_t DEVICE_SRC[NODE_ID_SIZE] = {0x44, 0x55, 0x66};

}  // namespace

// ============================================================================
// ONE_WAY_PAIRING_TRAFFIC
// ============================================================================

TEST(PairingAdvisor, OneWayPairingTraffic_Cmd0x20_IssueTwentySevenFrameShape) {
  // F5 00 00 00 3F 2F 9A 98 20 ... — CTRL0 bit5=1 (1W), dst=00003F, cmd=0x20 (WRITE_PRIVATE).
  PairingTelemetry telemetry;
  telemetry.begin();
  IoFrame f = build_frame(false, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_WRITE_PRIVATE);
  telemetry.record_rx_reject(f, -48);

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  ASSERT_EQ(count, 1u);
  EXPECT_EQ(advice[0].code, PairingAdviceCode::ONE_WAY_PAIRING_TRAFFIC);
  EXPECT_EQ(memcmp(advice[0].subject_node, REMOTE_SRC, NODE_ID_SIZE), 0);
  EXPECT_EQ(advice[0].rssi, -48);
}

TEST(PairingAdvisor, OneWayPairingTraffic_Cmd0x39_OneWayRemove) {
  PairingTelemetry telemetry;
  telemetry.begin();
  IoFrame f = build_frame(false, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_ONEWAY_REMOVE);
  telemetry.record_rx_reject(f, -50);

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  ASSERT_EQ(count, 1u);
  EXPECT_EQ(advice[0].code, PairingAdviceCode::ONE_WAY_PAIRING_TRAFFIC);
}

TEST(PairingAdvisor, OneWayPairingTraffic_Cmd0x2E_AlternateDiscovery1W) {
  PairingTelemetry telemetry;
  telemetry.begin();
  IoFrame f = build_frame(false, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_DISCOVER_ALT_REQ);
  telemetry.record_rx_reject(f, -52);

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  ASSERT_EQ(count, 1u);
  EXPECT_EQ(advice[0].code, PairingAdviceCode::ONE_WAY_PAIRING_TRAFFIC);
}

TEST(PairingAdvisor, OneWayPairingTraffic_NotTriggeredWhenNotBroadcastDst) {
  PairingTelemetry telemetry;
  telemetry.begin();
  // 1W bit set, but addressed to a normal device, not the 1W broadcast address.
  IoFrame f = build_frame(false, REMOTE_SRC, DEVICE_SRC, CMD_WRITE_PRIVATE);
  telemetry.record_rx_reject(f, -48);

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  EXPECT_EQ(count, 0u);
}

TEST(PairingAdvisor, OneWayPairingTraffic_NotTriggeredForUnrelatedCommand) {
  PairingTelemetry telemetry;
  telemetry.begin();
  // 1W bit set, broadcast dst, but not one of the three grounded 1W-pairing command bytes.
  IoFrame f = build_frame(false, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_EXECUTE);
  telemetry.record_rx_reject(f, -48);

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  EXPECT_EQ(count, 0u);
}

TEST(PairingAdvisor, OneWayPairingTraffic_NotTriggeredWhen2WFlag) {
  PairingTelemetry telemetry;
  telemetry.begin();
  // Same cmd/dst as the 1W case but the 2W protocol flag — must not be misclassified.
  IoFrame f = build_frame(true, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_WRITE_PRIVATE);
  telemetry.record_rx_reject(f, -48);

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  EXPECT_EQ(count, 0u);
}

TEST(PairingAdvisor, OneWayPairingTraffic_RecentSightingAloneTriggersAdviceNotRfSilent) {
  // Regression test for issue #27 (miljaar): a PROG press completed just before "Discover & Pair"
  // is pressed produced heard=0/rf_silent even though the radio heard the gesture just fine, only
  // moments before the telemetry window opened. PairingEngine::discover_and_pair() seeds telemetry
  // with exactly this kind of pre-window sighting (record_recent_one_way_sighting()) — confirm the
  // advisor treats that seeded event the same as a live one: ONE_WAY_PAIRING_TRAFFIC, not RF_SILENT,
  // and heard_count() reflects it.
  PairingTelemetry telemetry;
  telemetry.begin();
  RecentOneWayPairingSighting sighting{};
  memcpy(sighting.src, REMOTE_SRC, NODE_ID_SIZE);
  memcpy(sighting.dst, BROADCAST_DISCOVER_ALT, NODE_ID_SIZE);
  sighting.cmd = CMD_WRITE_PRIVATE;
  sighting.rssi = -48;
  sighting.seen_ms = 1;
  telemetry.record_recent_one_way_sighting(sighting);
  telemetry.record_hop();  // the live discovery window itself heard nothing new

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  ASSERT_EQ(count, 1u);
  EXPECT_EQ(advice[0].code, PairingAdviceCode::ONE_WAY_PAIRING_TRAFFIC);
  EXPECT_EQ(memcmp(advice[0].subject_node, REMOTE_SRC, NODE_ID_SIZE), 0);
  EXPECT_EQ(advice[0].rssi, -48) << "the seeded event's real RSSI should carry through, not a fabricated 0";
  EXPECT_EQ(telemetry.heard_count(), 1u);
}

// ============================================================================
// CHANNEL_BUSY_LBT_DELAYED
// ============================================================================

TEST(PairingAdvisor, ChannelBusy_SeededSightingExcludedFromLiveContentionEvidence) {
  // A seeded pre-window sighting must not be usable as evidence of *live* channel contention: it
  // predates this attempt by up to PAIRING_RECENT_ONE_WAY_SIGHTING_WINDOW_MS, so pairing it with
  // one live occurrence of the same source to cross the ">= 2 occurrences" channel_busy threshold
  // — and reporting the seeded event's RSSI as if it were a live beacon — would be fabricated
  // evidence. Confirm the seeded event is excluded from both the occurrence count and from being
  // selected as the advice's subject, even with LBT retries exhausted.
  PairingTelemetry telemetry;
  telemetry.begin();
  for (uint8_t i = 0; i < LBT_MAX_RETRIES; i++)
    telemetry.record_lbt_defer(-50);
  RecentOneWayPairingSighting sighting{};
  memcpy(sighting.src, REMOTE_SRC, NODE_ID_SIZE);
  memcpy(sighting.dst, BROADCAST_DISCOVER_ALT, NODE_ID_SIZE);
  sighting.cmd = CMD_WRITE_PRIVATE;
  sighting.rssi = -30;  // deliberately distinct from the live sighting's RSSI below
  sighting.seen_ms = 1;
  telemetry.record_recent_one_way_sighting(sighting);
  // Exactly one *live* sighting of the same source — "repeating" requires two, and the seeded one
  // must not count toward that.
  IoFrame live = build_frame(false, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_DISCOVER_ALT_REQ);
  telemetry.record_rx_reject(live, -49);

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  for (uint8_t i = 0; i < count; i++)
    EXPECT_NE(advice[i].code, PairingAdviceCode::CHANNEL_BUSY_LBT_DELAYED)
        << "a seeded sighting plus a single live one must not look like repeating live contention";
}

TEST(PairingAdvisor, ChannelBusy_RepeatingStrongBeaconWithExhaustedLbtRetries) {
  PairingTelemetry telemetry;
  telemetry.begin();
  for (uint8_t i = 0; i < LBT_MAX_RETRIES; i++)
    telemetry.record_lbt_defer(-50);
  IoFrame f1 = build_frame(false, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_DISCOVER_ALT_REQ);
  IoFrame f2 = build_frame(false, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_DISCOVER_ALT_REQ);
  telemetry.record_rx_reject(f1, -49);
  telemetry.record_rx_reject(f2, -50);

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  bool found = false;
  for (uint8_t i = 0; i < count; i++) {
    if (advice[i].code == PairingAdviceCode::CHANNEL_BUSY_LBT_DELAYED) {
      found = true;
      EXPECT_EQ(memcmp(advice[i].subject_node, REMOTE_SRC, NODE_ID_SIZE), 0);
    }
  }
  EXPECT_TRUE(found);
}

TEST(PairingAdvisor, ChannelBusy_BoundarySingleWeakBeaconDoesNotTriggerAdvice) {
  PairingTelemetry telemetry;
  telemetry.begin();
  for (uint8_t i = 0; i < LBT_MAX_RETRIES; i++)
    telemetry.record_lbt_defer(-50);
  // Only a single sighting of this source — not "repeating" — must not trigger the advice.
  IoFrame f = build_frame(false, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_DISCOVER_ALT_REQ);
  telemetry.record_rx_reject(f, -49);

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  for (uint8_t i = 0; i < count; i++)
    EXPECT_NE(advice[i].code, PairingAdviceCode::CHANNEL_BUSY_LBT_DELAYED);
}

TEST(PairingAdvisor, ChannelBusy_NotTriggeredWhenLbtRetriesBelowThreshold) {
  PairingTelemetry telemetry;
  telemetry.begin();
  telemetry.record_lbt_defer(-50);  // well below LBT_MAX_RETRIES
  IoFrame f1 = build_frame(false, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_DISCOVER_ALT_REQ);
  IoFrame f2 = build_frame(false, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_DISCOVER_ALT_REQ);
  telemetry.record_rx_reject(f1, -49);
  telemetry.record_rx_reject(f2, -50);

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  for (uint8_t i = 0; i < count; i++)
    EXPECT_NE(advice[i].code, PairingAdviceCode::CHANNEL_BUSY_LBT_DELAYED);
}

TEST(PairingAdvisor, ChannelBusy_BoundaryAdjacentToLbtMaxRetries) {
  // One retry short of the threshold must not trigger; exactly at the threshold must.
  PairingTelemetry below;
  below.begin();
  for (uint8_t i = 0; i < LBT_MAX_RETRIES - 1; i++)
    below.record_lbt_defer(-50);
  IoFrame f1 = build_frame(false, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_DISCOVER_ALT_REQ);
  IoFrame f2 = build_frame(false, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_DISCOVER_ALT_REQ);
  below.record_rx_reject(f1, -49);
  below.record_rx_reject(f2, -50);
  PairingAdvice below_advice[PAIRING_ADVICE_MAX];
  const uint8_t below_count = analyze_pairing_telemetry(below, OWN_ID, below_advice);
  for (uint8_t i = 0; i < below_count; i++)
    EXPECT_NE(below_advice[i].code, PairingAdviceCode::CHANNEL_BUSY_LBT_DELAYED)
        << "LBT_MAX_RETRIES - 1 must not be treated as exhausted";

  PairingTelemetry at;
  at.begin();
  for (uint8_t i = 0; i < LBT_MAX_RETRIES; i++)
    at.record_lbt_defer(-50);
  IoFrame f3 = build_frame(false, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_DISCOVER_ALT_REQ);
  IoFrame f4 = build_frame(false, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_DISCOVER_ALT_REQ);
  at.record_rx_reject(f3, -49);
  at.record_rx_reject(f4, -50);
  PairingAdvice at_advice[PAIRING_ADVICE_MAX];
  const uint8_t at_count = analyze_pairing_telemetry(at, OWN_ID, at_advice);
  bool found = false;
  for (uint8_t i = 0; i < at_count; i++)
    found |= at_advice[i].code == PairingAdviceCode::CHANNEL_BUSY_LBT_DELAYED;
  EXPECT_TRUE(found) << "LBT_MAX_RETRIES exactly must be treated as exhausted";
}

TEST(PairingAdvisor, ForeignControllerPairing_NotTriggeredWhenOwnNodeIdIsUnprovisioned) {
  // An all-zero own_node_id means "not provisioned yet" — must not produce a false advisory.
  PairingTelemetry telemetry;
  telemetry.begin();
  const uint8_t unprovisioned[NODE_ID_SIZE] = {0, 0, 0};
  IoFrame f = build_frame(true, DEVICE_SRC, OTHER_CONTROLLER_ID, CMD_DISCOVER_RESP);
  telemetry.record_rx_reject(f, -60);

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, unprovisioned, advice);

  for (uint8_t i = 0; i < count; i++)
    EXPECT_NE(advice[i].code, PairingAdviceCode::FOREIGN_CONTROLLER_PAIRING);
}

// ============================================================================
// FOREIGN_CONTROLLER_PAIRING
// ============================================================================

TEST(PairingAdvisor, ForeignControllerPairing_DiscoverRespAddressedToDifferentController) {
  PairingTelemetry telemetry;
  telemetry.begin();
  IoFrame f = build_frame(true, DEVICE_SRC, OTHER_CONTROLLER_ID, CMD_DISCOVER_RESP);
  telemetry.record_rx_reject(f, -60);

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  ASSERT_EQ(count, 1u);
  EXPECT_EQ(advice[0].code, PairingAdviceCode::FOREIGN_CONTROLLER_PAIRING);
  EXPECT_EQ(memcmp(advice[0].subject_node, DEVICE_SRC, NODE_ID_SIZE), 0);
}

TEST(PairingAdvisor, ForeignControllerPairing_NotTriggeredWhenAddressedToUs) {
  PairingTelemetry telemetry;
  telemetry.begin();
  IoFrame f = build_frame(true, DEVICE_SRC, OWN_ID, CMD_DISCOVER_RESP);
  telemetry.record_rx(f, -60);

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  EXPECT_EQ(count, 0u);
}

// ============================================================================
// RF_SILENT
// ============================================================================

TEST(PairingAdvisor, RfSilent_NothingHeardAtAll) {
  PairingTelemetry telemetry;
  telemetry.begin();
  telemetry.record_hop();  // hops/phases don't count as "heard"

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  ASSERT_EQ(count, 1u);
  EXPECT_EQ(advice[0].code, PairingAdviceCode::RF_SILENT);
}

TEST(PairingAdvisor, RfSilent_NotTriggeredWhenSomethingWasHeard) {
  PairingTelemetry telemetry;
  telemetry.begin();
  IoFrame f = build_frame(true, DEVICE_SRC, OWN_ID, CMD_DISCOVER_RESP);
  telemetry.record_rx(f, -60);

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  for (uint8_t i = 0; i < count; i++)
    EXPECT_NE(advice[i].code, PairingAdviceCode::RF_SILENT);
}

// ============================================================================
// Combined scenarios
// ============================================================================

TEST(PairingAdvisor, CombinedScenario_OneWayTrafficAndChannelBusyBothReported) {
  PairingTelemetry telemetry;
  telemetry.begin();
  for (uint8_t i = 0; i < LBT_MAX_RETRIES; i++)
    telemetry.record_lbt_defer(-50);
  IoFrame f1 = build_frame(false, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_WRITE_PRIVATE);
  IoFrame f2 = build_frame(false, REMOTE_SRC, BROADCAST_DISCOVER_ALT, CMD_WRITE_PRIVATE);
  telemetry.record_rx_reject(f1, -48);
  telemetry.record_rx_reject(f2, -49);

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  bool has_1w = false;
  bool has_busy = false;
  for (uint8_t i = 0; i < count; i++) {
    has_1w |= advice[i].code == PairingAdviceCode::ONE_WAY_PAIRING_TRAFFIC;
    has_busy |= advice[i].code == PairingAdviceCode::CHANNEL_BUSY_LBT_DELAYED;
  }
  EXPECT_TRUE(has_1w);
  EXPECT_TRUE(has_busy);
}

TEST(PairingAdvisor, NoAdviceWhenAttemptLooksNormal) {
  PairingTelemetry telemetry;
  telemetry.begin();
  IoFrame f = build_frame(true, DEVICE_SRC, OWN_ID, CMD_DISCOVER_RESP);
  telemetry.record_rx(f, -60);

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, OWN_ID, advice);

  EXPECT_EQ(count, 0u);
}

// ============================================================================
// Rendering
// ============================================================================

TEST(PairingAdvisor, CodeNamesAreStableShortStrings) {
  EXPECT_STREQ(pairing_advice_code_name(PairingAdviceCode::NONE), "none");
  EXPECT_STREQ(pairing_advice_code_name(PairingAdviceCode::ONE_WAY_PAIRING_TRAFFIC), "1w_traffic");
  EXPECT_STREQ(pairing_advice_code_name(PairingAdviceCode::CHANNEL_BUSY_LBT_DELAYED), "channel_busy");
  EXPECT_STREQ(pairing_advice_code_name(PairingAdviceCode::FOREIGN_CONTROLLER_PAIRING), "foreign_controller");
  EXPECT_STREQ(pairing_advice_code_name(PairingAdviceCode::RF_SILENT), "rf_silent");
}

TEST(PairingAdvisor, OneWayPairingMessageIsActionableAndIncludesSrcNode) {
  PairingAdvice advice{};
  advice.code = PairingAdviceCode::ONE_WAY_PAIRING_TRAFFIC;
  memcpy(advice.subject_node, REMOTE_SRC, NODE_ID_SIZE);

  const std::string message = pairing_advice_message(advice);
  EXPECT_NE(message.find("1W"), std::string::npos);
  EXPECT_NE(message.find("Double Power Cut"), std::string::npos);
  EXPECT_NE(message.find(node_id_to_string(REMOTE_SRC)), std::string::npos);
}

TEST(PairingAdvisor, NoneAdviceMessageIsEmpty) {
  PairingAdvice advice{};
  advice.code = PairingAdviceCode::NONE;
  EXPECT_TRUE(pairing_advice_message(advice).empty());
}
