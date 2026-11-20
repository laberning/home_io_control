/// @file exchange_broadcast_test.cpp
/// @brief Tests for collect_broadcast_responses() and the channel policy of a roll call.

#include "hub_exchange.h"
#include "hub_core.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "radio_sx1276.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"
#include "support/hold_seam.h"

#include <algorithm>
#include <cstring>

#include "exchange_fixture.h"

using namespace esphome::home_io_control;
using namespace exchange_test;

// ============================================================================
// collect_broadcast_responses tests
// ============================================================================
// Exercises ExchangeEngine directly (not through IOHomeControlComponent) to isolate this
// primitive from the action layer that drives it (ManagementActions::scan_paired_devices(),
// covered separately in tests/hub/hub_management_test.cpp). A small pairing_discovery_wait_ms keeps
// host-test iteration counts meaningful: in host tests millis() advances by exactly 1 per call,
// and MockRadio::wait_for_packet() returns false immediately once its queue is empty rather than
// honouring the timeout.

TEST(Exchange, CollectBroadcastResponses_ZeroReplies) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  EXPECT_EQ(count, 0u) << "no queued replies should yield zero collected";
  EXPECT_TRUE(collected.frames.empty()) << "the handler must not be invoked when nothing arrives";
}

TEST(Exchange, CollectBroadcastResponses_WindowIsHonouredNotIgnored) {
  // Every production caller passes the same tuning value, so without this the window_ms parameter
  // could be ignored entirely and no other test would notice. A zero-length window must expire
  // before the first receive: host millis() advances one tick per call, so the deadline is already
  // in the past when the loop is first evaluated.
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP, /*window_ms=*/0,
                                                     collected.handler());

  EXPECT_EQ(count, 0u) << "a zero-length window must collect nothing even with a reply already queued";
  EXPECT_TRUE(collected.frames.empty()) << "the handler must not run after the window has expired";
  EXPECT_EQ(radio.get_send_count(), 1) << "the request is still transmitted; only the listen window is empty";
}

TEST(Exchange, CollectBroadcastResponses_OneReply) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  radio.set_last_capture_rssi(-42);

  IoFrame reply = build_spe_response(test::DST_ID, test::OWN_ID);
  radio.queue_rx(to_rx_packet(reply));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  ASSERT_EQ(count, 1u);
  ASSERT_EQ(collected.frames.size(), 1u);
  EXPECT_EQ(memcmp(collected.frames[0].src, test::DST_ID, NODE_ID_SIZE), 0)
      << "delivered reply should carry the responder's node ID";
  EXPECT_EQ(collected.rssi_dbm[0], -42) << "delivered reply should carry the RSSI of the captured packet";
}

TEST(Exchange, CollectBroadcastResponses_HoldsTheFrameLogFromTheBroadcastToTheLastReply) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  test::HoldSeam seam(radio);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));
  radio.queue_rx(to_rx_packet(build_spe_response(test::FOREIGN_ID, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP, tuning.pairing_discovery_wait_ms,
                                     collected.handler());

  EXPECT_EQ(seam.depth_at_deliver.size(), 2u);
  EXPECT_TRUE(seam.held_at_every_send_and_delivery())
      << "nothing may be logged between the broadcast and any reply of the window (ADR 0111)";
  EXPECT_TRUE(test::frame_log_released());
}

TEST(Exchange, CollectBroadcastResponses_ReleasesTheFrameLogWhenTheBroadcastFailsToSend) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  radio.queue_tx_result(false);

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  EXPECT_EQ(engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                               tuning.pairing_discovery_wait_ms, collected.handler()),
            0u);
  EXPECT_TRUE(test::frame_log_released());
}

TEST(Exchange, CollectBroadcastResponses_TwoDistinctSourcesBothCollected) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));
  radio.queue_rx(to_rx_packet(build_spe_response(test::FOREIGN_ID, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  EXPECT_EQ(count, 2u) << "two distinct responders should both be collected";
}

TEST(Exchange, CollectBroadcastResponses_SameSourceTwiceBothDelivered) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));
  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  EXPECT_EQ(count, 2u) << "the engine does not deduplicate; both deliveries reach the handler";
  EXPECT_EQ(collected.frames.size(), 2u)
      << "deduplication belongs to the caller (see ScanPairedDevicesDedupsSameSourceThroughActionLayer)";
}

TEST(Exchange, CollectBroadcastResponses_WrongCommandIgnoredWithoutAbortingCollection) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // Wrong-command frame from the same src the valid reply will use.
  IoFrame wrong_cmd{};
  init_frame(wrong_cmd, true, true, true, false);
  set_dst(wrong_cmd, test::OWN_ID);
  set_src(wrong_cmd, test::DST_ID);
  set_cmd(wrong_cmd, CMD_PRIVATE_RESP, nullptr, 0);
  radio.queue_rx(to_rx_packet(wrong_cmd));

  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  ASSERT_EQ(count, 1u) << "the wrong-command frame must be ignored, not counted or fatal";
  ASSERT_EQ(collected.frames.size(), 1u);
  EXPECT_EQ(collected.frames[0].cmd, CMD_DISCOVER_SPE_RESP) << "the delivered reply must be the valid one";
}

TEST(Exchange, CollectBroadcastResponses_WrongDestinationIgnored) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // Addressed to some other hub, not us.
  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::FOREIGN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  EXPECT_EQ(count, 0u) << "a reply not addressed to us must be ignored";
}

TEST(Exchange, CollectBroadcastResponses_NoCapacityLimitAtEngineLayer) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // Three distinct responders: the engine stores nothing, so all three reach the handler and any
  // limit is the caller's to impose.
  const uint8_t src_a[3] = {0x01, 0x01, 0x01};
  const uint8_t src_b[3] = {0x02, 0x02, 0x02};
  const uint8_t src_c[3] = {0x03, 0x03, 0x03};
  radio.queue_rx(to_rx_packet(build_spe_response(src_a, test::OWN_ID)));
  radio.queue_rx(to_rx_packet(build_spe_response(src_b, test::OWN_ID)));
  radio.queue_rx(to_rx_packet(build_spe_response(src_c, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  EXPECT_EQ(count, 3u) << "the engine imposes no cap of its own";
  EXPECT_EQ(collected.frames.size(), 3u) << "every distinct responder must reach the handler";
}

TEST(Exchange, CollectBroadcastResponses_TransmitFailureReturnsZero) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);
  radio.queue_tx_result(false);

  // Even a queued reply must not be collected if the initial transmit never went out.
  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  EXPECT_EQ(count, 0u) << "a failed initial transmit must short-circuit with zero replies";
  EXPECT_TRUE(collected.frames.empty()) << "the handler must not be invoked when the transmit failed";
  EXPECT_EQ(radio.get_send_count(), 1) << "exactly one transmit attempt, no retry";
}

// ============================================================================
// Roll-call channel policy: collect_broadcast_responses()'s default policy
// (ROTATE_SKIPPING_REQUEST) leaves the request channel before its first listen, because a Somfy
// always-alive reply is seen there only 1 of 149 times. The two tests below call without the new
// `policy` argument, so they pin that default rather than a fixed universal behaviour — a caller
// that passes ROTATE_ALL_CHANNELS instead (see below) gets the opposite.
// ============================================================================

TEST(Exchange, CollectBroadcastResponses_LeavesRequestChannelImmediately) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // A reply queued for the very first wait_for_packet() call is what this test needs to isolate
  // "before the first listen" — but collect_broadcast_responses() keeps collecting for the whole
  // window even after a match (it doesn't return on the first reply), so old code eventually hops
  // too once the queue runs dry, and freq_history() alone can't tell "hopped before listening
  // ever started" from "hopped after the queued reply was consumed". call_log() can: it's the
  // interleaved order of wait/hop calls, so the very first entry reveals whether a hop preceded
  // the first listen (new code, unconditional pre-listen hop) or a listen came first (old code,
  // which only ever hops after a wait_for_packet() call already returned false).
  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP, tuning.pairing_discovery_wait_ms,
                                     collected.handler());

  ASSERT_FALSE(radio.call_log().empty());
  EXPECT_EQ(radio.call_log().front(), MockRadio::CallKind::kHop)
      << "the engine must retune before its first listen, not after it";
  ASSERT_FALSE(radio.freq_history().empty());
  EXPECT_NE(radio.freq_history().front(), FREQ_CH2) << "the request channel must not be the first channel listened on";
}

TEST(Exchange, CollectBroadcastResponses_NeverListensOnRequestChannel) {
  const uint32_t request_channels[] = {FREQ_CH1, FREQ_CH2, FREQ_CH3};
  for (uint32_t request_freq : request_channels) {
    MockRadio radio;
    RadioDriver *radio_ptr = &radio;
    TuningConfig tuning = make_broadcast_test_tuning();
    tuning.pairing_discovery_wait_ms = 20;
    // collect_broadcast_responses() leaves spec.dwell_ms at 0, so listen() asks MockRadio's
    // hop_dwell_ms() (which reads this field) — force many iterations within the window.
    tuning.sx1276_discovery_hop_slice_ms = 1;
    ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

    IoFrame request = build_spe_request(test::OWN_ID);
    CollectedReplies collected;
    engine.collect_broadcast_responses(request, request_freq, CMD_DISCOVER_SPE_RESP, tuning.pairing_discovery_wait_ms,
                                       collected.handler());

    for (uint32_t visited : radio.freq_history()) {
      EXPECT_NE(visited, request_freq) << "request_freq=" << request_freq
                                       << ": the rotation must never land back on the request channel";
    }
    const auto &history = radio.freq_history();
    const bool saw_both_others =
        std::find(history.begin(), history.end(), request_freq == FREQ_CH1 ? FREQ_CH2 : FREQ_CH1) != history.end() &&
        std::find(history.begin(), history.end(), request_freq == FREQ_CH3 ? FREQ_CH2 : FREQ_CH3) != history.end();
    EXPECT_TRUE(saw_both_others) << "request_freq=" << request_freq
                                 << ": both non-request channels should appear over several hops";
  }
}

// Real VELUX low-power roll-call replies have been observed landing on the request channel, so a
// caller whose responders may do that passes ROTATE_ALL_CHANNELS instead of relying on the
// skipping default.
TEST(Exchange, CollectBroadcastResponses_RotateAllListensOnTheRequestChannel) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // A reply on the very first wait_for_packet() call: only reachable if the first listen stays on
  // the request channel instead of hopping away from it first.
  RadioRxPacket reply_pkt = to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID));
  reply_pkt.freq_hz = FREQ_CH2;
  radio.queue_rx(reply_pkt);

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count =
      engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP, tuning.pairing_discovery_wait_ms,
                                         collected.handler(), ListenPolicy::ROTATE_ALL_CHANNELS);

  EXPECT_EQ(count, 1u) << "ROTATE_ALL_CHANNELS must catch a reply on the request channel";
  ASSERT_FALSE(radio.call_log().empty());
  EXPECT_EQ(radio.call_log().front(), MockRadio::CallKind::kWait)
      << "ROTATE_ALL_CHANNELS starts listening immediately, unlike the skipping default";
  EXPECT_NE(std::find(radio.freq_history().begin(), radio.freq_history().end(), FREQ_CH2), radio.freq_history().end())
      << "the request frequency must still appear in the rotation";
}

TEST(Exchange, CollectBroadcastResponses_ReportsReceiveChannel) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // Transmit on CH2, but the queued reply reports arriving on CH1.
  RadioRxPacket reply_pkt = to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID));
  reply_pkt.freq_hz = FREQ_CH1;
  radio.queue_rx(reply_pkt);

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  ASSERT_EQ(count, 1u);
  ASSERT_EQ(collected.rx_freq_hz.size(), 1u);
  EXPECT_EQ(collected.rx_freq_hz[0], FREQ_CH1) << "the reply's own receive channel must be reported, not the TX one";
}

TEST(Exchange, CollectBroadcastResponses_ReportsLatencyAfterTransmit) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // One silent slice, then a reply: host millis() advances one tick per call, so after_tx_ms must
  // be strictly positive by the time the reply is delivered.
  radio.queue_rx_silence(1);
  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  uint8_t count = engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP,
                                                     tuning.pairing_discovery_wait_ms, collected.handler());

  ASSERT_EQ(count, 1u);
  ASSERT_EQ(collected.after_tx_ms.size(), 1u);
  EXPECT_GT(collected.after_tx_ms[0], 0u) << "latency must be measured from the transmit completing, not read as 0";
}

// Pins a property no existing test asserts directly: staying put after a reception is
// deliberate — the hub is demonstrably on a channel this device population uses, and replies
// arrive spread across the whole window — not merely "the mock happened to still be on the right
// channel". call_log() (not freq_history()) is what can show this: it is the interleaved
// wait/hop call order, so it can distinguish "no hop between this reception and the next slice"
// from "a hop happened and the rotation later landed back here".
TEST(Exchange, CollectBroadcastResponses_DoesNotHopAfterAReception) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  tuning.pairing_discovery_wait_ms = 20;
  // collect_broadcast_responses() leaves spec.dwell_ms at 0, so listen() asks MockRadio's
  // hop_dwell_ms() (which reads this field) — force several slices within the window.
  tuning.sx1276_discovery_hop_slice_ms = 1;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  // One matching reply lands on the very first listen after the pre-loop hop, then silence for
  // the rest of the window.
  radio.queue_rx(to_rx_packet(build_spe_response(test::DST_ID, test::OWN_ID)));
  radio.queue_rx_silence(5);

  IoFrame request = build_spe_request(test::OWN_ID);
  CollectedReplies collected;
  engine.collect_broadcast_responses(request, FREQ_CH2, CMD_DISCOVER_SPE_RESP, tuning.pairing_discovery_wait_ms,
                                     collected.handler());

  ASSERT_EQ(collected.frames.size(), 1u) << "the reply must have been collected for this test to say anything";
  ASSERT_GE(radio.call_log().size(), 3u) << "need the reception plus at least one following slice";
  EXPECT_EQ(radio.call_log()[0], MockRadio::CallKind::kHop) << "exactly one hop precedes the first listen";
  EXPECT_EQ(radio.call_log()[1], MockRadio::CallKind::kWait) << "the first listen is the one that receives the reply";
  EXPECT_EQ(radio.call_log()[2], MockRadio::CallKind::kWait)
      << "no hop between the reception and the next slice: the roll-call stays on a channel that just answered";
}
