/// @file pairing_telemetry_test.cpp
/// @brief Tests for PairingTelemetry and its wiring into a full mocked pairing attempt.

#include "hub_core.h"
#include "pairing_telemetry.h"
#include "proto_frame.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;

namespace {

class TestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::discover_and_pair;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::system_key_;
};

RadioRxPacket frame_to_rx_packet(const IoFrame &frame, uint32_t freq_hz = FREQ_CH2) {
  RadioRxPacket pkt{};
  pkt.len = serialize(frame, pkt.data, sizeof(pkt.data));
  pkt.freq_hz = freq_hz;
  return pkt;
}

IoFrame build_discovery_response(const uint8_t src[3], const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, true, true, false);
  set_dst(f, dst);
  set_src(f, src);
  uint8_t payload[DEVICE_METADATA_SIZE] = {0x00, 0x01};  // ROLLER_SHUTTER, subtype 1
  set_cmd(f, CMD_DISCOVER_RESP, payload, sizeof(payload));
  return f;
}

IoFrame build_key_challenge(const uint8_t src[3], const uint8_t dst[3], const uint8_t challenge[6]) {
  IoFrame f{};
  init_frame(f, true, false, false, false);
  set_dst(f, dst);
  set_src(f, src);
  set_cmd(f, CMD_CHALLENGE_REQ, challenge, 6);
  return f;
}

IoFrame build_key_confirm(const uint8_t src[3], const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, false, false, false);
  set_dst(f, dst);
  set_src(f, src);
  set_cmd(f, CMD_KEY_CONFIRM, nullptr, 0);
  return f;
}

/// Minimal frame for PairingTelemetry::record_rx()/record_rx_reject() unit tests — only cmd
/// and src matter to those tests; dst and the 1W bit default to zero/2W.
IoFrame build_rx_frame(uint8_t cmd, const uint8_t src[NODE_ID_SIZE]) {
  IoFrame f{};
  init_frame(f, true, false, false, false);
  const uint8_t dst[NODE_ID_SIZE] = {0, 0, 0};
  set_dst(f, dst);
  set_src(f, src);
  set_cmd(f, cmd, nullptr, 0);
  return f;
}

/// Response to the best-effort SetConfig1 (0x6F) sent in Phase 3 — queuing this makes the
/// finalize step succeed too, so the attempt reports PairingOutcome::PAIRED rather than
/// CONFIG_FAILED (both are valid "pairing succeeded" outcomes; tests want to see PAIRED).
IoFrame build_set_config1_resp(const uint8_t src[3], const uint8_t dst[3]) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  set_dst(f, dst);
  set_src(f, src);
  set_cmd(f, CMD_SET_CONFIG1_RESP, nullptr, 0);
  return f;
}

}  // namespace

// ============================================================================
// PairingTelemetry — pure unit tests
// ============================================================================

TEST(PairingTelemetry, BeginResetsAllState) {
  PairingTelemetry telemetry;
  const uint8_t src[NODE_ID_SIZE] = {0x11, 0x22, 0x33};
  telemetry.begin();
  telemetry.record_rx(build_rx_frame(CMD_DISCOVER_RESP, src), -50);
  telemetry.set_phase(pairing::PairingState::WAIT_DISCOVER_RESPONSE);
  telemetry.increment_discovery_attempt();
  telemetry.record_lbt_defer(-40);

  telemetry.begin();

  EXPECT_EQ(telemetry.event_count(), 0u);
  EXPECT_EQ(telemetry.heard_count(), 0u);
  EXPECT_EQ(telemetry.phase(), pairing::PairingState::IDLE);
  EXPECT_EQ(telemetry.outcome(), PairingOutcome::NONE);
  EXPECT_EQ(telemetry.discovery_attempts(), 0u);
  EXPECT_EQ(telemetry.lbt_retries(), 0u);
  EXPECT_FALSE(telemetry.has_paired_device());
}

TEST(PairingTelemetry, RecordRxStoresEventFields) {
  PairingTelemetry telemetry;
  telemetry.begin();
  const uint8_t src[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  telemetry.record_rx(build_rx_frame(CMD_DISCOVER_RESP, src), -55);

  ASSERT_EQ(telemetry.event_count(), 1u);
  EXPECT_EQ(telemetry.heard_count(), 1u);
  const PairingTelemetryEvent &event = telemetry.events()[0];
  EXPECT_EQ(event.kind, PairingTelemetryEventKind::RX);
  EXPECT_EQ(event.cmd, CMD_DISCOVER_RESP);
  EXPECT_EQ(memcmp(event.src_node, src, NODE_ID_SIZE), 0);
  EXPECT_EQ(event.rssi, -55);
}

TEST(PairingTelemetry, RecordRxRejectCountsTowardHeardNotJustEventCount) {
  PairingTelemetry telemetry;
  telemetry.begin();
  const uint8_t src[NODE_ID_SIZE] = {0x01, 0x02, 0x03};
  telemetry.record_rx_reject(build_rx_frame(CMD_PRIVATE_RESP, src), -70);

  EXPECT_EQ(telemetry.event_count(), 1u);
  EXPECT_EQ(telemetry.heard_count(), 1u);
  EXPECT_EQ(telemetry.events()[0].kind, PairingTelemetryEventKind::RX_REJECT);
}

TEST(PairingTelemetry, TxAndLbtDeferAndHopDoNotCountTowardHeard) {
  PairingTelemetry telemetry;
  telemetry.begin();
  telemetry.record_tx(CMD_DISCOVER_REQ);
  telemetry.record_lbt_defer(-30);
  telemetry.record_hop();

  EXPECT_EQ(telemetry.event_count(), 3u);
  EXPECT_EQ(telemetry.heard_count(), 0u) << "only RX/RX_REJECT count as heard";
  EXPECT_EQ(telemetry.lbt_retries(), 1u);
}

TEST(PairingTelemetry, EventOverflowTruncatesStorageButHeardKeepsCountingAll) {
  PairingTelemetry telemetry;
  telemetry.begin();
  const uint8_t src[NODE_ID_SIZE] = {0x00, 0x00, 0x01};
  constexpr int kTotalEvents = PAIRING_TELEMETRY_MAX_EVENTS + 8;
  for (int i = 0; i < kTotalEvents; i++)
    telemetry.record_rx(build_rx_frame(CMD_DISCOVER_RESP, src), -60);

  EXPECT_EQ(telemetry.event_count(), PAIRING_TELEMETRY_MAX_EVENTS) << "storage caps at the fixed array size";
  EXPECT_EQ(telemetry.heard_count(), static_cast<uint16_t>(kTotalEvents))
      << "heard_count keeps counting past the storage cap";
}

TEST(PairingTelemetry, SetOutcomeAndPairedDeviceFeedResultString) {
  PairingTelemetry telemetry;
  telemetry.begin();
  telemetry.increment_discovery_attempt();
  telemetry.record_lbt_defer(-40);
  const uint8_t node_id[NODE_ID_SIZE] = {0x30, 0xE1, 0xF2};
  telemetry.set_paired_device(node_id, DeviceType::AWNING);
  telemetry.set_outcome(PairingOutcome::PAIRED);

  const std::string result = telemetry.result_sensor_string();
  EXPECT_EQ(result.rfind("v1; outcome=paired;", 0), 0u) << "must start with the frozen v1; prefix: " << result;
  EXPECT_NE(result.find("node=30E1F2"), std::string::npos) << result;
  EXPECT_NE(result.find("type=awning"), std::string::npos) << result;
  EXPECT_NE(result.find("attempts=1"), std::string::npos) << result;
  EXPECT_NE(result.find("lbt=1"), std::string::npos) << result;
}

TEST(PairingTelemetry, ResultStringUsesPlaceholdersWhenNoDevicePaired) {
  PairingTelemetry telemetry;
  telemetry.begin();
  telemetry.set_outcome(PairingOutcome::NO_RESPONSE);

  const std::string result = telemetry.result_sensor_string();
  EXPECT_NE(result.find("outcome=no_response"), std::string::npos) << result;
  EXPECT_NE(result.find("node=-"), std::string::npos) << result;
  EXPECT_NE(result.find("type=-"), std::string::npos) << result;
}

// ============================================================================
// End-to-end: discover_and_pair() populates telemetry correctly
// ============================================================================

TEST(PairingTelemetry, HappyPathReportsPairedOutcomeAndPairedDevice) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  const uint8_t device_bytes[NODE_ID_SIZE] = {test::DST_ID[0], test::DST_ID[1], test::DST_ID[2]};
  radio.queue_rx(frame_to_rx_packet(build_discovery_response(device_bytes, comp.node_id_)));
  uint8_t challenge[HMAC_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  radio.queue_rx(frame_to_rx_packet(build_key_challenge(device_bytes, comp.node_id_, challenge)));
  radio.queue_rx(frame_to_rx_packet(build_key_confirm(device_bytes, comp.node_id_)));
  radio.queue_rx(frame_to_rx_packet(build_set_config1_resp(device_bytes, comp.node_id_)));

  ASSERT_TRUE(comp.discover_and_pair());

  const PairingTelemetry &telemetry = comp.pairing_telemetry();
  EXPECT_EQ(telemetry.outcome(), PairingOutcome::PAIRED);
  EXPECT_EQ(telemetry.phase(), pairing::PairingState::COMPLETE);
  ASSERT_TRUE(telemetry.has_paired_device());
  EXPECT_EQ(memcmp(telemetry.paired_node_id(), device_bytes, NODE_ID_SIZE), 0);
  EXPECT_GE(telemetry.discovery_attempts(), 1u);
  EXPECT_GT(telemetry.event_count(), 0u) << "discovery/challenge/confirm should have recorded RX events";

  const std::string result = telemetry.result_sensor_string();
  EXPECT_EQ(result.rfind("v1; outcome=paired;", 0), 0u) << result;
}

TEST(PairingTelemetry, DiscoveryTimeoutReportsNoResponseOutcome) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  EXPECT_FALSE(comp.discover_and_pair());

  const PairingTelemetry &telemetry = comp.pairing_telemetry();
  EXPECT_EQ(telemetry.outcome(), PairingOutcome::NO_RESPONSE);
  EXPECT_FALSE(telemetry.has_paired_device());
  EXPECT_NE(telemetry.result_sensor_string().find("outcome=no_response"), std::string::npos);
}

TEST(PairingTelemetry, TrafficWithNoValidDiscoveryResponseReportsInvalidResponseOutcome) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  // Traffic is seen (so this isn't a timeout), but it never parses as a valid CMD_DISCOVER_RESP.
  // Discovery retries several times (PAIRING_DISCOVERY_MAX_ATTEMPTS); queue enough garbage so
  // every retry still sees traffic — otherwise a later attempt with nothing queued would report
  // NO_RESPONSE and overwrite the INVALID disposition from the earlier attempt.
  const uint8_t device_bytes[NODE_ID_SIZE] = {test::DST_ID[0], test::DST_ID[1], test::DST_ID[2]};
  IoFrame garbage = build_key_confirm(device_bytes, comp.node_id_);
  for (int i = 0; i < 5; i++)
    radio.queue_rx(frame_to_rx_packet(garbage));

  EXPECT_FALSE(comp.discover_and_pair());

  const PairingTelemetry &telemetry = comp.pairing_telemetry();
  EXPECT_EQ(telemetry.outcome(), PairingOutcome::INVALID_RESPONSE);
  EXPECT_NE(telemetry.result_sensor_string().find("outcome=invalid_response"), std::string::npos);
}

TEST(PairingTelemetry, MissingSetConfig1ResponseReportsConfigFailedOutcomeButStillPairs) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  const uint8_t device_bytes[NODE_ID_SIZE] = {test::DST_ID[0], test::DST_ID[1], test::DST_ID[2]};
  radio.queue_rx(frame_to_rx_packet(build_discovery_response(device_bytes, comp.node_id_)));
  uint8_t challenge[HMAC_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  radio.queue_rx(frame_to_rx_packet(build_key_challenge(device_bytes, comp.node_id_, challenge)));
  radio.queue_rx(frame_to_rx_packet(build_key_confirm(device_bytes, comp.node_id_)));
  // No SetConfig1 response queued — finalize_pairing_configuration_() times out.

  ASSERT_TRUE(comp.discover_and_pair()) << "key exchange succeeded, so pairing still reports success";

  const PairingTelemetry &telemetry = comp.pairing_telemetry();
  EXPECT_EQ(telemetry.outcome(), PairingOutcome::CONFIG_FAILED);
  ASSERT_TRUE(telemetry.has_paired_device()) << "device is still registered even though config failed";
  EXPECT_NE(telemetry.result_sensor_string().find("outcome=config_failed"), std::string::npos);
}

TEST(PairingTelemetry, KeyExchangeFailureReportsKeyExchangeFailedOutcome) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  const uint8_t device_bytes[NODE_ID_SIZE] = {test::DST_ID[0], test::DST_ID[1], test::DST_ID[2]};
  radio.queue_rx(frame_to_rx_packet(build_discovery_response(device_bytes, comp.node_id_)));
  // No key challenge queued — key exchange phase will time out.

  EXPECT_FALSE(comp.discover_and_pair());

  const PairingTelemetry &telemetry = comp.pairing_telemetry();
  EXPECT_EQ(telemetry.outcome(), PairingOutcome::KEY_EXCHANGE_FAILED);
  EXPECT_NE(telemetry.result_sensor_string().find("outcome=key_exchange_failed"), std::string::npos);
}
