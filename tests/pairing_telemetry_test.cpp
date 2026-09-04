/// @file pairing_telemetry_test.cpp
/// @brief Tests for PairingTelemetry and its wiring into a full mocked pairing attempt.

#include "hub_core.h"
#include "pairing_advisor.h"
#include "pairing_telemetry.h"
#include "proto_frame.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::advisor;

namespace {

class TestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::discover_and_pair;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::system_key_;
  using IOHomeControlComponent::process_received_packet_;
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
  telemetry.record_hop();
  for (int i = 0; i < PAIRING_TELEMETRY_MAX_EVENTS + 1; i++)
    telemetry.record_tx(CMD_DISCOVER_REQ);
  ASSERT_TRUE(telemetry.truncated()) << "sanity: this attempt should have overflowed the event array";
  ASSERT_EQ(telemetry.hop_count(), 1u) << "sanity: hop was recorded before begin() reset it";

  telemetry.begin();

  EXPECT_EQ(telemetry.event_count(), 0u);
  EXPECT_EQ(telemetry.heard_count(), 0u);
  EXPECT_EQ(telemetry.hop_count(), 0u);
  EXPECT_FALSE(telemetry.truncated());
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

  // record_hop() deliberately does not add to the stored event array (see its doc comment) — only
  // TX and LBT_DEFER are stored here, so event_count() is 2, not 3.
  EXPECT_EQ(telemetry.event_count(), 2u);
  EXPECT_EQ(telemetry.hop_count(), 1u);
  EXPECT_EQ(telemetry.heard_count(), 0u) << "only RX/RX_REJECT count as heard";
  EXPECT_EQ(telemetry.lbt_retries(), 1u);
}

TEST(PairingTelemetry, HopsDoNotConsumeEventStorage) {
  // Regression test for issue #27: at a short hop slice, a multi-second discovery window produces
  // far more hops than PAIRING_TELEMETRY_MAX_EVENTS. Before the fix, record_hop() stored each one
  // in the fixed array, so a real RX arriving later in the same window could be silently dropped
  // from the *stored* events (and PairingAdvisor's event-scanning passes — 1w_traffic/channel_busy/
  // foreign_controller — could go blind) once hops alone filled it, even though heard_count() (a
  // separate counter, incremented before the capacity check) kept counting it — so this specific
  // bug affected the advisor's diagnosis, not rf_silent, which reads heard_count() directly. Assert
  // the stored-event side no longer drops a real RX after many hops.
  PairingTelemetry telemetry;
  telemetry.begin();
  for (int i = 0; i < PAIRING_TELEMETRY_MAX_EVENTS * 4; i++)
    telemetry.record_hop();
  ASSERT_EQ(telemetry.hop_count(), static_cast<uint32_t>(PAIRING_TELEMETRY_MAX_EVENTS) * 4);
  ASSERT_EQ(telemetry.event_count(), 0u) << "hops must not occupy the stored-event array";
  ASSERT_FALSE(telemetry.truncated()) << "hops alone must never trip truncation";

  const uint8_t src[NODE_ID_SIZE] = {0x11, 0x22, 0x33};
  telemetry.record_rx(build_rx_frame(CMD_DISCOVER_RESP, src), -50);

  EXPECT_EQ(telemetry.event_count(), 1u) << "a real RX after many hops must still be stored";
  EXPECT_EQ(telemetry.heard_count(), 1u);
  EXPECT_FALSE(telemetry.truncated());
}

TEST(PairingTelemetry, RecentOneWaySightingIsRecordedAsRxEvent) {
  PairingTelemetry telemetry;
  telemetry.begin();
  const uint8_t src[NODE_ID_SIZE] = {0x2F, 0x9A, 0x98};
  const uint8_t dst[NODE_ID_SIZE] = {0x00, 0x00, 0x3F};
  RecentOneWayPairingSighting sighting{};
  memcpy(sighting.src, src, NODE_ID_SIZE);
  memcpy(sighting.dst, dst, NODE_ID_SIZE);
  sighting.cmd = CMD_WRITE_PRIVATE;
  sighting.rssi = -48;
  sighting.seen_ms = 1;

  telemetry.record_recent_one_way_sighting(sighting);

  ASSERT_EQ(telemetry.event_count(), 1u);
  EXPECT_EQ(telemetry.heard_count(), 1u) << "a recent sighting counts as heard, same as a live RX";
  const PairingTelemetryEvent &event = telemetry.events()[0];
  EXPECT_EQ(event.kind, PairingTelemetryEventKind::RX);
  EXPECT_EQ(event.cmd, CMD_WRITE_PRIVATE);
  EXPECT_TRUE(event.oneway);
  EXPECT_EQ(event.aux, 1u) << "must be marked as a seeded pre-window sighting, not a live capture";
  EXPECT_EQ(event.rssi, -48) << "the real RSSI must be preserved, not fabricated as 0";
  EXPECT_EQ(memcmp(event.src_node, src, NODE_ID_SIZE), 0);
  EXPECT_EQ(memcmp(event.dst_node, dst, NODE_ID_SIZE), 0);
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

TEST(PairingTelemetry, ResultStringDoesNotTruncateWorstCaseFields) {
  // Regression test for a buffer-size bug: the frozen `v1;` string was silently truncated by
  // snprintf for realistic worst-case field widths, chopping the trailing advice= field. Builds
  // the longest outcome/phase/type names, saturated counters, and all three advice codes, then
  // asserts the string is complete (not truncated) and ends with the full advice value.
  PairingTelemetry telemetry;
  telemetry.begin();

  const uint8_t src[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
  // Saturate discovery_attempts_ and lbt_retries_ (both uint8_t; COUNTER_SATURATION_MAX = 0xFF).
  for (int i = 0; i < 300; i++) {
    telemetry.increment_discovery_attempt();
    telemetry.record_lbt_defer(-40);
  }
  // Saturate heard_count_ (uint16_t) well past the fixed event-storage cap.
  for (int i = 0; i < 70000; i++)
    telemetry.record_rx(build_rx_frame(CMD_DISCOVER_RESP, src), -60);

  telemetry.set_outcome(PairingOutcome::INVALID_RESPONSE);             // "invalid_response" — longest outcome name
  telemetry.set_phase(pairing::PairingState::WAIT_DISCOVER_RESPONSE);  // "wait_discover_response" — longest phase name
  const uint8_t node_id[NODE_ID_SIZE] = {0x30, 0xE1, 0xF2};
  telemetry.set_paired_device(node_id, DeviceType::HEATING_TEMPERATURE_INTERFACE);  // longest type name
  const std::string advice = "1w_traffic,channel_busy,foreign_controller";          // all three advice codes
  telemetry.set_advice_codes(advice);

  const std::string result = telemetry.result_sensor_string();

  EXPECT_EQ(telemetry.discovery_attempts(), 0xFFu);
  EXPECT_EQ(telemetry.lbt_retries(), 0xFFu);
  EXPECT_EQ(telemetry.heard_count(), 65535u) << "heard_count_ (uint16_t) should saturate, not wrap";
  ASSERT_GE(result.size(), advice.size() + 8) << "sanity: result string should contain the advice field";
  EXPECT_EQ(result.substr(result.size() - advice.size()), advice)
      << "the rendered string must not be truncated before the trailing advice= value: " << result;
  EXPECT_NE(result.find("advice=" + advice), std::string::npos) << "full result: " << result;
}

TEST(PairingTelemetry, TruncatedIsReportedEvenWhenHeardCountDoesNotExceedEventCount) {
  // Regression test: log_summary()'s "(truncated)" marker used to compare heard_count_ (RX/
  // RX_REJECT only) against event_count_ (all kinds), which misses truncation whenever most
  // dropped/stored events aren't RX/RX_REJECT — e.g. an attempt dominated by TX/PHASE events.
  PairingTelemetry telemetry;
  telemetry.begin();

  // Fill exact capacity with non-"heard" TX events, then overflow with more TX events.
  for (int i = 0; i < PAIRING_TELEMETRY_MAX_EVENTS + 4; i++)
    telemetry.record_tx(CMD_DISCOVER_REQ);

  EXPECT_EQ(telemetry.event_count(), PAIRING_TELEMETRY_MAX_EVENTS);
  EXPECT_EQ(telemetry.heard_count(), 0u) << "TX events never count toward heard_count_";
  ASSERT_LE(telemetry.heard_count(), telemetry.event_count())
      << "sanity: the old buggy heuristic (heard_count_ > event_count_) would say 'not truncated' here";
  EXPECT_TRUE(telemetry.truncated()) << "events were dropped past capacity; truncated() must say so "
                                        "regardless of which kind of event was dropped";
}

TEST(PairingTelemetry, NotTruncatedWhenAllEventsFitInStorage) {
  PairingTelemetry telemetry;
  telemetry.begin();
  telemetry.record_tx(CMD_DISCOVER_REQ);
  telemetry.record_hop();

  EXPECT_FALSE(telemetry.truncated());
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

TEST(PairingTelemetry, RecentOneWaySightingSeedsDiscoveryTelemetryAndSuppressesRfSilent) {
  // End-to-end regression test for issue #27 (miljaar): a 1W pairing gesture overheard shortly
  // before discover_and_pair() is called must still show up as heard/1w_traffic, not rf_silent,
  // even though nothing answers the discovery broadcast itself.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  const uint8_t remote_src[NODE_ID_SIZE] = {0x2F, 0x9A, 0x98};
  IoFrame gesture{};
  init_frame(gesture, /*is_2w=*/false, false, false, false);
  set_dst(gesture, BROADCAST_DISCOVER_ALT);
  set_src(gesture, remote_src);
  set_cmd(gesture, CMD_WRITE_PRIVATE, nullptr, 0);
  comp.process_received_packet_(frame_to_rx_packet(gesture));

  // Nothing queued for the discovery attempt itself — it times out with NO_RESPONSE, same as
  // DiscoveryTimeoutReportsNoResponseOutcome below, but the pre-window sighting should still
  // register.
  EXPECT_FALSE(comp.discover_and_pair());

  const PairingTelemetry &telemetry = comp.pairing_telemetry();
  EXPECT_EQ(telemetry.outcome(), PairingOutcome::NO_RESPONSE);
  EXPECT_GT(telemetry.heard_count(), 0u) << "the pre-window 1W gesture must count as heard";

  PairingAdvice advice[PAIRING_ADVICE_MAX];
  const uint8_t count = analyze_pairing_telemetry(telemetry, comp.node_id_, advice);
  bool has_1w = false;
  bool has_rf_silent = false;
  for (uint8_t i = 0; i < count; i++) {
    has_1w |= advice[i].code == PairingAdviceCode::ONE_WAY_PAIRING_TRAFFIC;
    has_rf_silent |= advice[i].code == PairingAdviceCode::RF_SILENT;
  }
  EXPECT_TRUE(has_1w) << "the advisor should point at the 1W gesture, not RF silence";
  EXPECT_FALSE(has_rf_silent);
}

TEST(PairingTelemetry, StaleOneWaySightingOutsideWindowIsIgnored) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  const uint8_t remote_src[NODE_ID_SIZE] = {0x2F, 0x9A, 0x98};
  IoFrame gesture{};
  init_frame(gesture, /*is_2w=*/false, false, false, false);
  set_dst(gesture, BROADCAST_DISCOVER_ALT);
  set_src(gesture, remote_src);
  set_cmd(gesture, CMD_WRITE_PRIVATE, nullptr, 0);
  comp.process_received_packet_(frame_to_rx_packet(gesture));

  // Age the sighting past the window before pairing — it must not be seeded once stale.
  test::burn_millis(PAIRING_RECENT_ONE_WAY_SIGHTING_WINDOW_MS + 1);

  EXPECT_FALSE(comp.discover_and_pair());

  const PairingTelemetry &telemetry = comp.pairing_telemetry();
  EXPECT_EQ(telemetry.heard_count(), 0u) << "a sighting older than the window must not be seeded";
}

TEST(PairingTelemetry, OrdinaryOneWayTrafficDoesNotSeedDiscoveryTelemetry) {
  // False-positive-direction counterpart to RecentOneWaySightingSeedsDiscoveryTelemetryAndSuppressesRfSilent:
  // a 1W frame that does *not* match the pairing-gesture shape (decisions::is_one_way_pairing_gesture()
  // — here, CMD_EXECUTE instead of one of the three gesture command bytes) must not be treated as
  // "the motor is in 1W learning mode" evidence. Ordinary 1W remote traffic (someone operating a
  // cover) is common and must not manufacture a false 1w_traffic diagnosis on every later pairing
  // attempt within the window.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);

  const uint8_t remote_src[NODE_ID_SIZE] = {0x2F, 0x9A, 0x98};
  IoFrame ordinary{};
  init_frame(ordinary, /*is_2w=*/false, false, false, false);
  set_dst(ordinary, BROADCAST_DISCOVER_ALT);
  set_src(ordinary, remote_src);
  set_cmd(ordinary, CMD_EXECUTE, nullptr, 0);
  comp.process_received_packet_(frame_to_rx_packet(ordinary));

  EXPECT_FALSE(comp.discover_and_pair());

  const PairingTelemetry &telemetry = comp.pairing_telemetry();
  EXPECT_EQ(telemetry.heard_count(), 0u) << "a non-gesture 1W frame must not seed telemetry";
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
