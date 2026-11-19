/// @file exchange_listen_test.cpp
/// @brief Tests for ExchangeEngine::listen(), the shared listen primitive, and the engine counters.

#include "hub_exchange.h"
#include "hub_core.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "radio_sx1276.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "exchange_fixture.h"

using namespace esphome::home_io_control;
using namespace exchange_test;

// ============================================================================
// ExchangeEngine::listen() — the shared listen primitive, exercised directly through MockRadio.
// Not yet called by any of the six wait loops (that porting happens loop by loop in later steps);
// these tests pin the primitive's own behaviour so a later port can be bisected from it.
// ============================================================================

namespace {

/// Radio double that reports a controllable, constant preamble/sync state — used to test
/// linger_on_preamble without depending on a real chip's IRQ timing.
class SignalDetectRadio : public MockRadio {
 public:
  bool is_preamble_detected() override { return preamble_detected_; }
  void set_preamble_detected(bool v) { preamble_detected_ = v; }

 private:
  bool preamble_detected_{false};
};

}  // namespace

// Pins the roll-call's own call to spec.linger_on_preamble / spec.linger_dwell_ms in
// collect_broadcast_responses() -- not just the listen() primitive those fields feed. A hardware
// capture found the two unguarded: SX1276's 5 ms per-channel dwell is shorter than a
// DISCOVER_SPE_RESP's ~10 ms air time, so the radio retuned mid-frame on nearly every reception
// and the measured found-rate collapsed from 83% to 12%; hardware-measured 2026-08-19 confirmed
// 100% once the guard was added. If either field is dropped from that call site, this test must
// fail even though CollectBroadcastResponses_DoesNotHopAfterAReception and
// Listen_LingerOnPreambleUsesLingerDwellAndSuppressesHopping both still pass.
TEST(Exchange, CollectBroadcastResponses_LingersOnPreambleInsteadOfHoppingMidFrame) {
  constexpr uint32_t WINDOW_MS = 150;

  SignalDetectRadio radio;
  radio.set_preamble_detected(true);  // asserted throughout; nothing is ever received
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  // Matches the hardware SX1276 dwell that exposed the bug: 5 ms dwell vs ~10 ms reply air time.
  tuning.sx1276_discovery_hop_slice_ms = 5;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count =
      engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP, WINDOW_MS, collected.handler());

  EXPECT_EQ(count, 0u) << "nothing was ever queued; this test only cares about the wait/hop cadence";

  // Exactly one hop precedes the whole listen -- leaving the request channel, per
  // CollectBroadcastResponses_LeavesRequestChannelImmediately. With the dwell (5 ms) shorter than
  // a reply's air time, an unguarded rotation would then hop roughly every 5 ms for the rest of
  // the 150 ms window (dozens of hops); the preamble/sync guard must suppress every one of them.
  EXPECT_EQ(radio.freq_history().size(), 1u)
      << "asserted preamble/sync must suppress hopping for the whole window, not just the pre-loop skip";

  // wait_timeouts() must alternate the per-channel dwell with a PREAMBLE_LINGER_DWELL_MS-sized
  // linger extension, mirroring Listen_LingerOnPreambleUsesLingerDwellAndSuppressesHopping's
  // canonical-pairs check but exercised through the roll-call call site instead of listen()
  // directly.
  const auto &timeouts = radio.wait_timeouts();
  size_t canonical_pairs = 0;
  for (size_t i = 0; i + 1 < timeouts.size(); i += 2) {
    if (timeouts[i] != tuning.sx1276_discovery_hop_slice_ms || timeouts[i + 1] != PREAMBLE_LINGER_DWELL_MS)
      break;
    canonical_pairs++;
  }
  EXPECT_GE(canonical_pairs, 4u) << "need several unclamped dwell/linger pairs to prove the alternation";
}

TEST(Exchange, Listen_HoldNeverRetunesAndUsesTheWholeWindowAsOneTimeout) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  ListenSpec spec;
  spec.window_ms = 300;
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;

  RadioRxPacket packet{};
  IoFrame frame{};
  auto outcome = engine.listen(spec, packet, frame,
                               [](const IoFrame *, const RadioRxPacket &) { return ReplyDisposition::ACCEPT; });

  EXPECT_EQ(outcome, ListenOutcome::ACCEPTED);
  EXPECT_TRUE(radio.freq_history().empty()) << "HOLD_REQUEST_CHANNEL must never retune";
  ASSERT_EQ(radio.wait_timeouts().size(), 1u) << "HOLD waits the whole remaining window in one call, not slices";
  expect_window_near(radio.wait_timeouts().front(), spec.window_ms);
}

TEST(Exchange, Listen_RotateAllChannelsVisitsAllThreeChannels) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  ListenSpec spec;
  spec.window_ms = 150;
  spec.policy = ListenPolicy::ROTATE_ALL_CHANNELS;
  spec.dwell_ms = 5;

  RadioRxPacket packet{};
  IoFrame frame{};
  // Nothing queued: every slice times out, so the loop hops for the whole window.
  auto outcome = engine.listen(spec, packet, frame,
                               [](const IoFrame *, const RadioRxPacket &) { return ReplyDisposition::IGNORE; });

  EXPECT_EQ(outcome, ListenOutcome::TIMED_OUT);
  const auto &history = radio.freq_history();
  EXPECT_NE(std::find(history.begin(), history.end(), FREQ_CH1), history.end());
  EXPECT_NE(std::find(history.begin(), history.end(), FREQ_CH2), history.end());
  EXPECT_NE(std::find(history.begin(), history.end(), FREQ_CH3), history.end());
}

TEST(Exchange, Listen_RotateSkippingRequestHopsFirstAndNeverLandsOnRequestChannel) {
  const uint32_t request_channels[] = {FREQ_CH1, FREQ_CH2, FREQ_CH3};
  for (uint32_t request_freq : request_channels) {
    MockRadio radio;
    RadioDriver *radio_ptr = &radio;
    TuningConfig tuning = make_broadcast_test_tuning();
    ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

    ListenSpec spec;
    spec.window_ms = 20;
    spec.policy = ListenPolicy::ROTATE_SKIPPING_REQUEST;
    spec.request_freq = request_freq;
    spec.dwell_ms = 1;  // force several iterations within the window
    spec.hop_after_ignored_frame = false;

    RadioRxPacket packet{};
    IoFrame frame{};
    engine.listen(spec, packet, frame, [](const IoFrame *, const RadioRxPacket &) { return ReplyDisposition::IGNORE; });

    ASSERT_FALSE(radio.call_log().empty());
    EXPECT_EQ(radio.call_log().front(), MockRadio::CallKind::kHop)
        << "request_freq=" << request_freq << ": must retune before the first listen";
    for (uint32_t visited : radio.freq_history()) {
      EXPECT_NE(visited, request_freq) << "request_freq=" << request_freq << ": must never land on it";
    }
  }
}

TEST(Exchange, Listen_AcceptStopsImmediately) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));
  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));  // must never be consumed

  ListenSpec spec;
  spec.window_ms = 300;
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;

  int calls = 0;
  RadioRxPacket packet{};
  IoFrame frame{};
  auto outcome = engine.listen(spec, packet, frame, [&](const IoFrame *, const RadioRxPacket &) {
    calls++;
    return ReplyDisposition::ACCEPT;
  });

  EXPECT_EQ(outcome, ListenOutcome::ACCEPTED);
  EXPECT_EQ(calls, 1) << "ACCEPT on the first packet must stop the listen before the second is ever read";
  EXPECT_EQ(frame.cmd, CMD_DISCOVER_SPE_RESP) << "the caller's frame must hold the accepted frame";
}

TEST(Exchange, Listen_AbortStopsImmediatelyAndIsDistinguishableFromAccept) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  ListenSpec spec;
  spec.window_ms = 300;
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;

  RadioRxPacket packet{};
  IoFrame frame{};
  auto outcome = engine.listen(spec, packet, frame,
                               [](const IoFrame *, const RadioRxPacket &) { return ReplyDisposition::ABORT; });

  EXPECT_EQ(outcome, ListenOutcome::ABORTED);
  EXPECT_NE(outcome, ListenOutcome::ACCEPTED);
}

TEST(Exchange, Listen_IgnoreRunsToDeadline) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  ListenSpec spec;
  spec.window_ms = 300;
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;

  int calls = 0;
  RadioRxPacket packet{};
  IoFrame frame{};
  auto outcome = engine.listen(spec, packet, frame, [&](const IoFrame *, const RadioRxPacket &) {
    calls++;
    return ReplyDisposition::IGNORE;
  });

  EXPECT_EQ(outcome, ListenOutcome::TIMED_OUT) << "an always-IGNORE handler must run the listen to the deadline";
  EXPECT_EQ(calls, 1) << "the one queued packet was still handed to the handler on the way to timing out";
}

TEST(Exchange, Listen_UnparsablePacketReachesHandlerWithNullParsed) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // Shorter than FRAME_MIN_SIZE: parse() must reject it outright.
  RadioRxPacket garbage{};
  garbage.len = 4;
  radio.queue_rx(garbage);

  ListenSpec spec;
  spec.window_ms = 300;
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;

  bool saw_null_parsed = false;
  RadioRxPacket packet{};
  IoFrame frame{};
  auto outcome = engine.listen(spec, packet, frame, [&](const IoFrame *parsed, const RadioRxPacket &pkt) {
    saw_null_parsed = parsed == nullptr;
    EXPECT_EQ(pkt.len, 4u) << "the raw packet must still reach the handler even though it didn't parse";
    return ReplyDisposition::ACCEPT;
  });

  EXPECT_EQ(outcome, ListenOutcome::ACCEPTED);
  EXPECT_TRUE(saw_null_parsed) << "a packet that fails parse() must reach the handler with parsed == nullptr";
}

TEST(Exchange, Listen_HopAfterIgnoredFrameFalseStaysPutAfterAReception) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));
  radio.queue_rx_silence(5);

  ListenSpec spec;
  spec.window_ms = 20;
  spec.policy = ListenPolicy::ROTATE_ALL_CHANNELS;
  spec.dwell_ms = 1;
  spec.hop_after_ignored_frame = false;

  RadioRxPacket packet{};
  IoFrame frame{};
  engine.listen(spec, packet, frame, [](const IoFrame *, const RadioRxPacket &) { return ReplyDisposition::IGNORE; });

  ASSERT_GE(radio.call_log().size(), 2u) << "need the reception plus at least one following slice";
  EXPECT_EQ(radio.call_log()[0], MockRadio::CallKind::kWait) << "ROTATE_ALL_CHANNELS starts listening immediately";
  EXPECT_EQ(radio.call_log()[1], MockRadio::CallKind::kWait)
      << "hop_after_ignored_frame=false: no hop between the reception and the next slice";
}

TEST(Exchange, Listen_LingerOnPreambleUsesLingerDwellAndSuppressesHopping) {
  SignalDetectRadio radio;
  radio.set_preamble_detected(true);  // asserted throughout; nothing is ever received
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  ListenSpec spec;
  spec.window_ms = 150;
  spec.policy = ListenPolicy::ROTATE_ALL_CHANNELS;
  spec.dwell_ms = 5;
  spec.linger_on_preamble = true;
  spec.linger_dwell_ms = 15;

  RadioRxPacket packet{};
  IoFrame frame{};
  auto outcome = engine.listen(spec, packet, frame,
                               [](const IoFrame *, const RadioRxPacket &) { return ReplyDisposition::IGNORE; });

  EXPECT_EQ(outcome, ListenOutcome::TIMED_OUT);
  EXPECT_TRUE(radio.freq_history().empty()) << "the preamble guard must suppress every hop for the whole window";

  // Same clamped-tail caveat as the pairing-engine version of this property: walk pairs only as
  // long as both timeouts come through unclamped by the window's edge.
  const auto &timeouts = radio.wait_timeouts();
  size_t canonical_pairs = 0;
  for (size_t i = 0; i + 1 < timeouts.size(); i += 2) {
    if (timeouts[i] != spec.dwell_ms || timeouts[i + 1] != spec.linger_dwell_ms)
      break;
    canonical_pairs++;
  }
  EXPECT_GE(canonical_pairs, 4u) << "need several unclamped dwell/extension pairs to prove the alternation";
}

TEST(Exchange, Listen_OnHopFiresExactlyOncePerHop) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  ListenSpec spec;
  spec.window_ms = 150;
  spec.policy = ListenPolicy::ROTATE_ALL_CHANNELS;
  spec.dwell_ms = 5;
  int hop_calls = 0;
  spec.on_hop = [&]() { hop_calls++; };

  RadioRxPacket packet{};
  IoFrame frame{};
  engine.listen(spec, packet, frame, [](const IoFrame *, const RadioRxPacket &) { return ReplyDisposition::IGNORE; });

  ASSERT_FALSE(radio.freq_history().empty());
  EXPECT_EQ(static_cast<size_t>(hop_calls), radio.freq_history().size())
      << "on_hop must fire exactly once per change_frequency() call, no more and no less";
}

// ============================================================================
// Exchange-engine counters. Pure additive telemetry: free-running, engine-wide, no behavior
// change. Internal-only for now — see the Counters doc comment in exchange_engine.h for why
// there's no sensor/log consumer yet. One test per increment site.
// ============================================================================

TEST(Exchange, CountersTrackLbtRetries) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // Two busy reads (>= CHANNEL_LIMIT_UNTIL_QUIET_LEVEL_DBM) force two retries; the queue then runs dry and
  // read_rssi() falls back to MockRadio's default (-120 dBm, well clear), so the third read lets
  // transmit_frame() proceed without a third retry being counted.
  radio.queue_rssi(-50);
  radio.queue_rssi(-50);

  IoFrame correct = build_status_response(test::DST_ID, comp.node_id_);
  radio.queue_rx(to_rx_packet(correct));

  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  EXPECT_EQ(comp.exchange_engine_.counters().lbt_retries, 2u);
}

TEST(Exchange, CountersTrackRetransmits) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // Try 1's request TX itself fails (mirrors SendAndReceive_AllTransmitFails' pattern), which
  // forces a retry without needing to simulate an entire wait-window timeout via the RX queue —
  // MockRadio's HOLD-policy busy-loop drains queued RX entries near-instantly regardless of which
  // logical "try" is current, so queue_rx_silence() can't stand in for a whole-window timeout here.
  radio.queue_tx_result(false);  // try 1: request TX fails.

  IoFrame correct = build_status_response(test::DST_ID, comp.node_id_);
  radio.queue_rx(to_rx_packet(correct));  // try 2: TX defaults to success, and this is the response.

  IoFrame response{};
  bool ok = comp.send_and_receive_(request, response, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE;

  EXPECT_TRUE(ok) << "sanity: the second try must be the one that succeeds";
  EXPECT_EQ(comp.exchange_engine_.counters().retransmits, 1u) << "one retry beyond the first attempt";
}

TEST(Exchange, CountersTrackChallengeRoundTripsBothDirections) {
  // Outbound direction: a device challenges our command; handle_authentication_() answers it.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  uint8_t chal_data[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  radio.queue_rx(to_rx_packet(challenge));

  IoFrame final_resp = build_status_response(test::DST_ID, comp.node_id_);
  radio.queue_rx(to_rx_packet(final_resp));

  IoFrame response{};
  bool outbound_ok = comp.send_and_receive_(request, response, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE;
  ASSERT_TRUE(outbound_ok);
  EXPECT_EQ(comp.exchange_engine_.counters().challenge_round_trips, 1u)
      << "a device challenging our outbound command must count as one round trip";

  // Inbound direction: we challenge a device's unsolicited command; authenticate_request_()
  // verifies it. Same TestableComponent/counters — this is the second increment site, not a
  // second engine, so the count accumulates on top of the outbound one above.
  RespondOnChallengeMockRadio inbound_radio;
  comp.radio_ = &inbound_radio;
  IoFrame status_update = build_status_update_from_device(test::DST_ID, comp.node_id_);
  inbound_radio.arm(status_update, test::TEST_SYSTEM_KEY, /*valid=*/true);

  bool inbound_ok = comp.authenticate_request_(status_update, FREQ_CH2);
  ASSERT_TRUE(inbound_ok);
  EXPECT_EQ(comp.exchange_engine_.counters().challenge_round_trips, 2u)
      << "both challenge directions must feed the same counter";
}

TEST(Exchange, CountersTrackParseFailures) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // Shorter than FRAME_MIN_SIZE: parse() must reject it outright (mirrors
  // Listen_UnparsablePacketReachesHandlerWithNullParsed's stimulus).
  RadioRxPacket garbage{};
  garbage.len = 4;
  radio.queue_rx(garbage);

  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);  // expected to fail overall; only the counter matters here

  EXPECT_EQ(comp.exchange_engine_.counters().parse_failures, 1u);
}

TEST(Exchange, CountersResetZeroesEveryField) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // Drive all four counters nonzero before reset, mirroring each field's own CountersTrack* test
  // above: two busy RSSI reads for lbt_retries, a failed request TX for retransmits, a queued
  // challenge for challenge_round_trips, and a garbage packet for parse_failures.
  radio.queue_rssi(-50);
  radio.queue_rssi(-50);
  radio.queue_tx_result(false);  // try 1: request TX fails, forcing a retry.

  uint8_t chal_data[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  radio.queue_rx(to_rx_packet(challenge));

  RadioRxPacket garbage{};
  garbage.len = 4;
  radio.queue_rx(garbage);

  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  ASSERT_GT(comp.exchange_engine_.counters().lbt_retries, 0u) << "sanity: lbt_retries must be nonzero before reset";
  ASSERT_GT(comp.exchange_engine_.counters().retransmits, 0u) << "sanity: retransmits must be nonzero before reset";
  ASSERT_GT(comp.exchange_engine_.counters().challenge_round_trips, 0u)
      << "sanity: challenge_round_trips must be nonzero before reset";
  ASSERT_GT(comp.exchange_engine_.counters().parse_failures, 0u)
      << "sanity: parse_failures must be nonzero before reset";

  comp.exchange_engine_.reset_counters();

  const auto &c = comp.exchange_engine_.counters();
  EXPECT_EQ(c.lbt_retries, 0u);
  EXPECT_EQ(c.retransmits, 0u);
  EXPECT_EQ(c.challenge_round_trips, 0u);
  EXPECT_EQ(c.parse_failures, 0u);
}
