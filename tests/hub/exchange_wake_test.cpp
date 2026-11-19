/// @file exchange_wake_test.cpp
/// @brief Tests for the wake belief of low-power receivers in the exchange engine: the per-try request preamble.

#include "hub_exchange.h"
#include "hub_core.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_crypto.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <vector>

#include "exchange_fixture.h"

using namespace esphome::home_io_control;
using namespace exchange_test;

// ============================================================================
// Low-power wake belief: per-try request preamble in send_and_receive()
// ============================================================================
// A low-power START request orders its tries by the target's wake belief (see
// decisions::low_power_try_preamble()). MockRadio has no queued reply unless a test adds one, so
// every try transmits and the preamble of each is read back from get_tx_configs().

TEST(WakeBelief, NoProviderKeepsTheWakeUpPreambleOnEveryTry) {
  WakeBeliefRig rig;
  rig.engine.set_target_evidence_provider({});
  EXPECT_EQ(rig.send(low_power_position_request()),
            (Preambles{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
}

TEST(WakeBelief, AwakeTargetGetsShortThenWakeUpTwice) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  const uint16_t short_preamble = mock_preamble_bytes(rig.tuning.start_preamble_ms);
  EXPECT_EQ(rig.send(low_power_position_request()),
            (Preambles{short_preamble, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.engine.get_debug().wake_belief, decisions::WakeBelief::AWAKE);
  EXPECT_EQ(rig.engine.get_debug().last_try_preamble, MOCK_WAKE_PREAMBLE);
}

TEST(WakeBelief, RecentlyHeardTargetWithoutMovingEvidenceIsAsleep) {
  // A resting VELUX SSL ignored the short preamble even right after it had answered (ADR 0040,
  // amendment): having heard from the device is no reason to lead short.
  WakeBeliefRig rig;
  rig.heard_ago(34);
  EXPECT_EQ(rig.send(low_power_position_request()),
            (Preambles{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.engine.get_debug().wake_belief, decisions::WakeBelief::ASLEEP);
}

TEST(WakeBelief, AsleepTargetGetsTheWakeUpPreambleOnEveryTry) {
  WakeBeliefRig rig;
  rig.heard_ago(60000);
  rig.moved_ago(LOW_POWER_MAX_TRAVEL_MS + 1000);
  EXPECT_EQ(rig.send(low_power_position_request()),
            (Preambles{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.engine.get_debug().wake_belief, decisions::WakeBelief::ASLEEP);
}

TEST(WakeBelief, StopIsAwakeEvenWithoutEvidence) {
  WakeBeliefRig rig;
  const uint16_t short_preamble = mock_preamble_bytes(rig.tuning.start_preamble_ms);
  EXPECT_EQ(rig.send(low_power_stop_request()), (Preambles{short_preamble, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
}

TEST(WakeBelief, UnknownDestinationIsAsleep) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);  // would be AWAKE, but the provider says it has never heard of this node
  rig.evidence_known = false;
  EXPECT_EQ(rig.send(low_power_position_request()),
            (Preambles{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.engine.get_debug().wake_belief, decisions::WakeBelief::ASLEEP);
}

TEST(WakeBelief, KillSwitchRestoresTheWakeUpPreambleOnEveryTry) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  rig.tuning.low_power_wake_belief = false;
  EXPECT_EQ(rig.send(low_power_position_request()),
            (Preambles{MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE, MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.provider_calls, 0) << "with the switch off the evidence is never even looked up";
}

TEST(WakeBelief, ExplicitPreambleOverrideWinsOnEveryTry) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  EXPECT_EQ(rig.send(low_power_position_request(), EXCHANGE_RETRY_COUNT, /*override_preamble=*/48),
            (Preambles{48, 48, 48}));
  EXPECT_EQ(rig.provider_calls, 0) << "an override is the caller's choice and is never second-guessed";
}

TEST(WakeBelief, DebugSnapshotNamesWhyNoBeliefApplied) {
  // The exchange-failure log line prints this reason in its belief= field, so a posted log says
  // whether the switch was off, the frame was not low-power, and so on — one "n/a" could not.
  using Use = ExchangeEngine::WakeBeliefUse;
  WakeBeliefRig rig;
  rig.moved_ago(1000);

  rig.send(low_power_position_request(/*low_power=*/false));
  EXPECT_EQ(rig.engine.get_debug().wake_belief_use, Use::NOT_LOW_POWER);

  rig.send(low_power_position_request(), EXCHANGE_RETRY_COUNT, /*override_preamble=*/48);
  EXPECT_EQ(rig.engine.get_debug().wake_belief_use, Use::OVERRIDE);

  rig.send(low_power_position_request(), /*max_tries=*/1);
  EXPECT_EQ(rig.engine.get_debug().wake_belief_use, Use::APPLIED) << "the try count is never a reason to skip";

  rig.tuning.low_power_wake_belief = false;
  rig.send(low_power_position_request());
  EXPECT_EQ(rig.engine.get_debug().wake_belief_use, Use::SWITCHED_OFF);
  rig.tuning.low_power_wake_belief = true;

  rig.send(low_power_position_request());
  EXPECT_EQ(rig.engine.get_debug().wake_belief_use, Use::APPLIED);

  rig.engine.set_target_evidence_provider({});
  rig.send(low_power_position_request());
  EXPECT_EQ(rig.engine.get_debug().wake_belief_use, Use::NO_PROVIDER);
}

TEST(WakeBelief, SwitchOffKeepsASingleTryExchangeOnTheWakeUpPreamble) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  rig.tuning.low_power_wake_belief = false;
  EXPECT_EQ(rig.send(low_power_position_request(), /*max_tries=*/1), (Preambles{MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.engine.get_debug().wake_belief_use, ExchangeEngine::WakeBeliefUse::SWITCHED_OFF);
}

TEST(WakeBelief, SkipReasonLabelsAreDistinctAndReadable) {
  using Use = ExchangeEngine::WakeBeliefUse;
  EXPECT_STREQ(ExchangeEngine::wake_belief_use_name(Use::NOT_LOW_POWER), "not_low_power");
  EXPECT_STREQ(ExchangeEngine::wake_belief_use_name(Use::OVERRIDE), "override");
  EXPECT_STREQ(ExchangeEngine::wake_belief_use_name(Use::SWITCHED_OFF), "off");
  EXPECT_STREQ(ExchangeEngine::wake_belief_use_name(Use::NO_PROVIDER), "no_provider");
  EXPECT_STREQ(ExchangeEngine::wake_belief_use_name(Use::APPLIED), "applied");
}

TEST(WakeBelief, AlwaysAliveTargetKeepsTheNormalStartPreambleOnEveryTry) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  const uint16_t short_preamble = mock_preamble_bytes(rig.tuning.start_preamble_ms);
  EXPECT_EQ(rig.send(low_power_position_request(/*low_power=*/false)),
            (Preambles{short_preamble, short_preamble, short_preamble}));
  EXPECT_EQ(rig.provider_calls, 0) << "only a low-power start frame has a wake-up preamble to reorder";
}

TEST(WakeBelief, NonStartFrameKeepsTheResponsePreamble) {
  // Key-transfer-shaped frame: a non-start frame that still carries CTRL1_LOW_POWER.
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  IoFrame request{};
  init_frame(request, true, false, false, /*low_power=*/true);
  set_dst(request, test::DST_ID);
  set_src(request, test::OWN_ID);
  uint8_t payload[16] = {0};
  set_cmd(request, CMD_KEY_TRANSFER, payload, sizeof(payload));
  ASSERT_FALSE(is_start(request));
  ASSERT_NE(request.ctrl1 & CTRL1_LOW_POWER, 0);

  const Preambles preambles = rig.send(request);

  ASSERT_FALSE(preambles.empty());
  for (const uint16_t preamble : preambles)
    EXPECT_EQ(preamble, SHORT_PREAMBLE) << "the driver's response preamble, never a wake belief";
  EXPECT_EQ(rig.provider_calls, 0);
}

TEST(WakeBelief, SingleTryExchangeSendsTheBeliefsFirstTry) {
  // A scheduler-owned poll is allowed one try at most ladder slots. Its likeliest moment is seconds
  // after a command or STOP, when the receiver is travelling or has just answered and ignores the
  // wake-up preamble, so it leads with the same preamble a multi-try exchange would.
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  EXPECT_EQ(rig.send(low_power_position_request(), /*max_tries=*/1),
            (Preambles{mock_preamble_bytes(rig.tuning.start_preamble_ms)}));
  EXPECT_EQ(rig.provider_calls, 1);
  EXPECT_EQ(rig.engine.get_debug().wake_belief_use, ExchangeEngine::WakeBeliefUse::APPLIED);
  EXPECT_EQ(rig.engine.get_debug().wake_belief, decisions::WakeBelief::AWAKE);
}

TEST(WakeBelief, SingleTryExchangeToARecentlyHeardTargetKeepsTheWakeUpPreamble) {
  // The settle poll after an accepted STOP: moving evidence was cleared and the STOP's reply was
  // heard a second ago. The receiver is at rest, where only the wake-up preamble reaches it.
  WakeBeliefRig rig;
  rig.heard_ago(1000);
  EXPECT_EQ(rig.send(low_power_position_request(), /*max_tries=*/1), (Preambles{MOCK_WAKE_PREAMBLE}));
  EXPECT_EQ(rig.engine.get_debug().wake_belief, decisions::WakeBelief::ASLEEP);
}

TEST(WakeBelief, TwoTryExchangeStillFollowsThePlan) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  EXPECT_EQ(rig.send(low_power_position_request(), /*max_tries=*/2),
            (Preambles{mock_preamble_bytes(rig.tuning.start_preamble_ms), MOCK_WAKE_PREAMBLE}));
}

TEST(WakeBelief, TunedWakePreambleReplacesLongPreamble) {
  WakeBeliefRig rig;
  rig.tuning.low_power_wake_ms = mock_preamble_ms(2048);
  EXPECT_EQ(rig.send(low_power_position_request()), (Preambles{2048, 2048, 2048}));
}

TEST(WakeBelief, TunedWakePreambleAppliesWithTheSwitchOff) {
  WakeBeliefRig rig;
  rig.tuning.low_power_wake_belief = false;
  rig.tuning.low_power_wake_ms = mock_preamble_ms(1536);
  EXPECT_EQ(rig.send(low_power_position_request()), (Preambles{1536, 1536, 1536}));
}

TEST(WakeBelief, SilentAwakeExchangeSpendsTheMovingEvidence) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  int spent = 0;
  rig.engine.set_wake_evidence_spent_handler([&spent](const uint8_t *dst) {
    EXPECT_EQ(memcmp(dst, test::DST_ID, NODE_ID_SIZE), 0);
    spent++;
  });
  rig.send(low_power_position_request(), /*max_tries=*/1);
  EXPECT_EQ(spent, 1) << "a receiver believed moving that answers nothing has most likely come to rest";
}

TEST(WakeBelief, SilentAwakeExchangeKeepsTheEvidenceWhileTheReportedRemainingTimeHasNotRunOut) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  rig.evidence.moving_until_ms = esphome::millis() + 5000;
  int spent = 0;
  rig.engine.set_wake_evidence_spent_handler([&spent](const uint8_t *) { spent++; });

  rig.send(low_power_position_request(), /*max_tries=*/1);
  EXPECT_EQ(spent, 0) << "by its own count it is still travelling: one missed frame says nothing";

  esphome::test_clock::advance_ms(5000);
  rig.moved_ago(1000);
  rig.send(low_power_position_request(), /*max_tries=*/1);
  EXPECT_EQ(spent, 1) << "once the count has run out a silent short try does spend the evidence";
}

TEST(WakeBelief, SilentStopLeavesTheEvidenceAlone) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  int spent = 0;
  rig.engine.set_wake_evidence_spent_handler([&spent](const uint8_t *) { spent++; });
  rig.send(low_power_stop_request());
  EXPECT_EQ(spent, 0) << "an unanswered STOP stopped nothing: the receiver may still be travelling";
}

TEST(WakeBelief, SilentAsleepExchangeLeavesTheEvidenceAlone) {
  WakeBeliefRig rig;
  int spent = 0;
  rig.engine.set_wake_evidence_spent_handler([&spent](const uint8_t *) { spent++; });
  rig.send(low_power_position_request());
  EXPECT_EQ(spent, 0) << "only an AWAKE belief has moving evidence to spend";

  rig.moved_ago(1000);
  rig.send(low_power_position_request(/*low_power=*/false));
  EXPECT_EQ(spent, 0) << "an always-alive target has no belief at all";
}

TEST(WakeBelief, EvidenceIsLookedUpOncePerExchange) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  ASSERT_EQ(rig.send(low_power_position_request()).size(), 3u);
  EXPECT_EQ(rig.provider_calls, 1) << "a belief is resolved once, not re-derived (and re-clocked) every try";
}

TEST(WakeBelief, OnlyTheClockChangesBetweenTries_FrameBytesAreIdentical) {
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  ASSERT_EQ(rig.send(low_power_position_request()).size(), 3u);
  const auto &sent = rig.radio.get_sent_data();
  ASSERT_EQ(sent.size(), 3u);
  EXPECT_EQ(sent[0], sent[1]);
  EXPECT_EQ(sent[0], sent[2]);
}

TEST(WakeBelief, ReplyOnTheWakeUpTryEndsTheExchange) {
  // The belief was wrong (the target was not listening for the short preamble): try 2's wake-up
  // preamble reaches it. The exchange succeeds after exactly two transmits.
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  rig.radio.queue_rx_hold_until_sent(2);
  rig.radio.queue_rx(test::make_rx_packet(build_status_response(test::DST_ID, test::OWN_ID)));

  IoFrame response{};
  const ExchangeOutcome outcome =
      rig.engine.send_and_receive(low_power_position_request(), response, FREQ_CH2, EXCHANGE_RETRY_COUNT);

  EXPECT_EQ(outcome, ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  ASSERT_EQ(rig.radio.get_tx_configs().size(), 2u);
  EXPECT_EQ(rig.radio.get_tx_configs()[0].preamble_len, mock_preamble_bytes(rig.tuning.start_preamble_ms));
  EXPECT_EQ(rig.radio.get_tx_configs()[1].preamble_len, MOCK_WAKE_PREAMBLE);
}

TEST(WakeBelief, DiscoverConfirmStyleCallersStillGetTheAsleepRule) {
  // request_preamble_for() is the single-shot rule (pairing's discover-confirm uses it directly):
  // it never consults a belief, whatever the evidence says.
  WakeBeliefRig rig;
  rig.moved_ago(1000);
  EXPECT_EQ(rig.engine.request_preamble_for(low_power_position_request()), MOCK_WAKE_PREAMBLE);
  EXPECT_EQ(rig.provider_calls, 0);
}
