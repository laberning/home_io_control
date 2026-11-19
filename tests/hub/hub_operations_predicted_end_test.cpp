/// @file hub_operations_predicted_end_test.cpp
/// @brief Tests for the predicted-end read of hub_operations.cpp: a moving cover that counts down is read once more,
/// with the settling attempts of the exchange engine.

#include "hub_core.h"
#include "hub_internal.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_private_registers.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"
#include "reading_store.h"
#include "stubs/radio_test_common.h"

#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include "hub_operations_fixture.h"

using namespace esphome::home_io_control;
using namespace hub_operations_test;

// ========================================================================================
// The predicted-end read: a moving cover that counts down is read once more, with the settling
// plan, a second after its count runs out.
// ========================================================================================

namespace {

/// A trusted 0x04 from ABC123 that says moving, with @p remaining_s left.
IoFrame moving_report(const TestableComponent &comp, uint16_t remaining_s) {
  IoFrame f = build_moving_status_response(comp.node_id_, 0);
  f.data[6] = static_cast<uint8_t>(remaining_s >> 8);
  f.data[7] = static_cast<uint8_t>(remaining_s & 0xFF);
  return f;
}

/// A trusted 0x04 from ABC123 that says stopped off its target (target 100 %, current 50 %), with
/// @p remaining_s at bytes 6-7.
IoFrame stopped_off_target_report(const TestableComponent &comp, uint16_t remaining_s) {
  IoFrame f = build_moving_status_response(comp.node_id_, 0);
  f.data[0] = test::STATUS_BYTE_AT_REST;
  f.data[2] = 0xC8;
  f.data[3] = 0x00;
  f.data[6] = static_cast<uint8_t>(remaining_s >> 8);
  f.data[7] = static_cast<uint8_t>(remaining_s & 0xFF);
  return f;
}

/// What the scheduler would do at the clock's current time: pop a due poll and run it against a
/// silent radio. Returns the number of transmissions it made (0 = nothing was due).
int run_due_poll_against_silence(TestableComponent &comp, MockRadio &radio) {
  const auto due = comp.poll_policy_.pop_due_device(esphome::millis());
  if (!due.has_value())
    return 0;
  const int before = radio.get_send_count();
  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    radio.queue_tx_result(false);
  EXPECT_FALSE(comp.request_device_status(*due));
  return radio.get_send_count() - before;
}

}  // namespace

TEST(HubOperations, MovingCoverIsReadOnceAtItsPredictedEndWithTheSettlingPlan) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 0);

  comp.update_device_status_(moving_report(comp, 12));
  const uint32_t t0 = esphome::millis();
  EXPECT_EQ(comp.poll_policy_.get_settle_update("ABC123"), t0 + 13000u) << "12 s left plus a second";

  // A progress read, answered: the count fell, so the aim moves with it (to the same instant, give or take).
  esphome::test_clock::advance_ms(3001);
  ASSERT_TRUE(comp.poll_policy_.pop_due_device(esphome::millis()).has_value()) << "the progress read is due";
  comp.update_device_status_(moving_report(comp, 9));
  EXPECT_EQ(comp.poll_policy_.get_settle_update("ABC123"), t0 + 3001u + 10000u) << "re-aimed from the falling count";

  // Between the two the progress reads stay single tries.
  esphome::test_clock::advance_ms(3001);
  EXPECT_EQ(run_due_poll_against_silence(comp, radio), 1) << "a progress read is one attempt";

  // The aim is reached: one read with the settling plan.
  // The silent progress read left a progress read due as well; the due aim wins over it.
  esphome::test_clock::set_ms(t0 + 13002u);
  ASSERT_NE(comp.poll_policy_.get_next_update("ABC123"), 0u);
  ASSERT_LT(comp.poll_policy_.get_next_update("ABC123"), esphome::millis()) << "a progress read is due too";
  EXPECT_EQ(static_cast<size_t>(run_due_poll_against_silence(comp, radio)), planned_settling_attempts(comp))
      << "the read at the predicted end is a settling read";
  EXPECT_GT(planned_settling_attempts(comp), 1u);
}

namespace {

/// Preambles of the transmissions @p radio made after its first @p already_sent.
std::vector<uint16_t> preambles_since(const MockRadio &radio, size_t already_sent) {
  std::vector<uint16_t> preambles;
  const auto &configs = radio.get_tx_configs();
  for (size_t i = already_sent; i < configs.size(); ++i)
    preambles.push_back(configs[i].preamble_len);
  return preambles;
}

}  // namespace

// \todo Untested on hardware: needs a low-power cover that reports a remaining time (ADR 0077).
TEST(HubOperations, MissedShortTryMidMoveKeepsTheShortPreambleUntilTheCountRunsOut) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(moving_report(comp, 12));
  const uint32_t t0 = esphome::millis();
  const uint16_t short_preamble = mock_preamble_bytes(comp.tuning_.start_preamble_ms);
  const IoDevice *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  // A progress read, lost: by its own count the cover is still moving, so the next one is short again.
  esphome::test_clock::advance_ms(3001);
  size_t sent = radio.get_tx_configs().size();
  EXPECT_EQ(run_due_poll_against_silence(comp, radio), 1);
  EXPECT_EQ(preambles_since(radio, sent), (std::vector<uint16_t>{short_preamble}));
  EXPECT_NE(dev->last_moving_evidence_ms, 0u) << "one missed frame is not proof that it has come to rest";

  esphome::test_clock::advance_ms(3001);
  sent = radio.get_tx_configs().size();
  EXPECT_EQ(run_due_poll_against_silence(comp, radio), 1);
  EXPECT_EQ(preambles_since(radio, sent), (std::vector<uint16_t>{short_preamble}));

  // The count has run out and the cover is silent: now it has most likely come to rest.
  esphome::test_clock::set_ms(t0 + 12000u + PREDICTED_END_MARGIN_MS + 1);
  sent = radio.get_tx_configs().size();
  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    radio.queue_tx_result(false);
  EXPECT_FALSE(comp.request_device_status("ABC123"));
  ASSERT_FALSE(preambles_since(radio, sent).empty());
  EXPECT_EQ(preambles_since(radio, sent).front(), short_preamble) << "the belief still leads short...";
  EXPECT_EQ(dev->last_moving_evidence_ms, 0u) << "...and a silent try after the count spends the evidence";
}

TEST(HubOperations, PredictedEndReadToALowPowerCoverLeadsWithTheWakeUpPreamble) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(moving_report(comp, 12));
  const uint32_t t0 = esphome::millis();
  const IoDevice *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_NE(dev->last_moving_evidence_ms, 0u) << "the belief would lead short: moving evidence is fresh";

  esphome::test_clock::set_ms(t0 + 13002u);
  const size_t sent = radio.get_tx_configs().size();
  const int attempts = run_due_poll_against_silence(comp, radio);

  EXPECT_GT(attempts, 1) << "a settling read";
  const std::vector<uint16_t> preambles = preambles_since(radio, sent);
  ASSERT_EQ(preambles.size(), static_cast<size_t>(attempts));
  for (const uint16_t preamble : preambles)
    EXPECT_EQ(preamble, mock_preamble_bytes(comp.tuning_.low_power_wake_ms)) << "at its predicted end the cover rests";
}

TEST(HubOperations, ReadAfterASilentPredictedEndReadLeadsShortThenWakes) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(moving_report(comp, 12));
  const uint32_t t0 = esphome::millis();
  const IoDevice *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  const uint16_t short_preamble = mock_preamble_bytes(comp.tuning_.start_preamble_ms);
  const uint16_t wake_preamble = mock_preamble_bytes(comp.tuning_.low_power_wake_ms);

  esphome::test_clock::set_ms(t0 + 13002u);
  ASSERT_GT(run_due_poll_against_silence(comp, radio), 1);
  EXPECT_NE(dev->last_moving_evidence_ms, 0u)
      << "the wake-only read took the belief out of the exchange: it spends nothing";

  // Still moving past its count, or at rest: the next read covers both, and its silence decides.
  const uint32_t next = comp.poll_policy_.get_next_update("ABC123");
  ASSERT_NE(next, 0u) << "a missed read is retried";
  esphome::test_clock::set_ms(next + 1);
  const size_t sent = radio.get_tx_configs().size();
  const int attempts = run_due_poll_against_silence(comp, radio);
  ASSERT_GT(attempts, 1) << "the read after a missed one is a settling read";
  const std::vector<uint16_t> preambles = preambles_since(radio, sent);
  EXPECT_EQ(preambles.front(), short_preamble) << "a cover still travelling past its count hears the first try";
  for (size_t i = 1; i < preambles.size(); ++i)
    EXPECT_EQ(preambles[i], wake_preamble) << "a resting cover hears the later ones";
  EXPECT_EQ(dev->last_moving_evidence_ms, 0u) << "silent after its count: the evidence is spent";
}

TEST(HubOperations, PredictedEndReadToAMainsCoverKeepsTheNormalStartPreamble) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/false);
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(moving_report(comp, 12));
  const uint32_t t0 = esphome::millis();

  esphome::test_clock::set_ms(t0 + 13002u);
  const size_t sent = radio.get_tx_configs().size();
  ASSERT_GT(run_due_poll_against_silence(comp, radio), 0);
  for (const uint16_t preamble : preambles_since(radio, sent))
    EXPECT_EQ(preamble, mock_preamble_bytes(comp.tuning_.start_preamble_ms));
}

TEST(HubOperations, StaleRemainingTimeFallsBackToSingleTryProgressReads) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 0);

  for (int i = 0; i <= PREDICTED_END_STALE_REPORTS; ++i) {
    comp.update_device_status_(moving_report(comp, 12));  // the count never falls
    esphome::test_clock::advance_ms(3001);
  }

  EXPECT_EQ(comp.poll_policy_.get_settle_update("ABC123"), 0u) << "a frozen count is not a prediction";
  comp.poll_policy_.set_next_update("ABC123", esphome::millis() - 1);
  EXPECT_EQ(run_due_poll_against_silence(comp, radio), 1);

  comp.update_device_status_(moving_report(comp, 11));
  EXPECT_NE(comp.poll_policy_.get_settle_update("ABC123"), 0u) << "a count that falls again is aimed at again";
}

TEST(HubOperations, EndReadOfAShortOrZeroCountRetriesALostReplyAndProgressReadsStaySingleTries) {
  for (const uint16_t remaining : {uint16_t{2}, uint16_t{1}, uint16_t{0}}) {
    SCOPED_TRACE(remaining);
    esphome::test_clock::ManualClock clock(50000);
    TestableComponent comp;
    MockRadio radio;
    setup_cover_component(comp, radio);
    comp.begin_status_poll_tracking_("ABC123", 0);

    // The first count teaches the hub that the cover counts down (a count of 0 is only believed after that);
    // the progress read it leaves due is one attempt.
    comp.update_device_status_(moving_report(comp, 12));
    comp.poll_policy_.set_next_update("ABC123", esphome::millis() - 1);
    EXPECT_EQ(run_due_poll_against_silence(comp, radio), 1) << "a progress read is one attempt";

    comp.update_device_status_(moving_report(comp, remaining));
    const uint32_t t0 = esphome::millis();
    EXPECT_EQ(comp.poll_policy_.get_settle_update("ABC123"), t0 + remaining * 1000u + 1000u);

    // The end of the count: one read with the settling plan, repeated while the reply is lost.
    esphome::test_clock::advance_ms(remaining * 1000u + 1001u);
    EXPECT_EQ(static_cast<size_t>(run_due_poll_against_silence(comp, radio)), planned_settling_attempts(comp))
        << "a lost reply at the end of the count is retried";
    EXPECT_GT(planned_settling_attempts(comp), 1u);
  }
}

TEST(HubOperations, ZeroFromACoverThatNeverCountedPlansNoEndRead) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 0);

  comp.update_device_status_(moving_report(comp, 0));
  EXPECT_EQ(comp.poll_policy_.get_settle_update("ABC123"), 0u) << "a 0 from a cover that never counted is no count";
  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), esphome::millis() + DEFAULT_SETTLE_POLL_DELAY_MS);
  esphome::test_clock::advance_ms(DEFAULT_SETTLE_POLL_DELAY_MS + 1);
  EXPECT_EQ(run_due_poll_against_silence(comp, radio), 1) << "the usual single-try progress read";
}

TEST(HubOperations, AWithdrawnAimLeavesTheProgressReadOfTheReportThatWithdrewIt) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 0);

  // A count of 1 aims at +2 s; the progress read due then is dropped in favour of the aim.
  comp.update_device_status_(moving_report(comp, 12));
  comp.update_device_status_(moving_report(comp, 1));
  const uint32_t t0 = esphome::millis();
  comp.poll_policy_.set_next_update("ABC123", t0 + 1500u);
  esphome::test_clock::set_ms(t0 + 1501u);
  EXPECT_EQ(run_due_poll_against_silence(comp, radio), 0) << "the progress read right before the aim is dropped";
  ASSERT_EQ(comp.poll_policy_.get_next_update("ABC123"), 0u);

  // A report without a count withdraws the aim before it falls due; that report plans the next read.
  comp.update_device_status_(moving_report(comp, 0xFFFF));  // all bits set: no count
  EXPECT_EQ(comp.poll_policy_.get_settle_update("ABC123"), 0u);
  EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), esphome::millis() + DEFAULT_SETTLE_POLL_DELAY_MS)
      << "the cover is still read";
  esphome::test_clock::advance_ms(DEFAULT_SETTLE_POLL_DELAY_MS + 1);
  EXPECT_EQ(run_due_poll_against_silence(comp, radio), 1) << "as a single-try progress read";
}

TEST(HubOperations, StoppedOffTargetConfirmingReadKeepsTheDefaultDelayAndWinsOverAnyCount) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_cover_component(comp, radio);
  comp.begin_status_poll_tracking_("ABC123", 0);
  comp.update_device_status_(moving_report(comp, 12));  // teaches the hub that the cover counts down
  ASSERT_NE(comp.poll_policy_.get_settle_update("ABC123"), 0u);

  // Stopped off target with a zero count (and, in the next round, a short one): the confirming read
  // still comes after the default delay and is the only read planned.
  for (const uint16_t remaining : {uint16_t{0}, uint16_t{1}}) {
    comp.poll_policy_.set_next_update("ABC123", 0);
    comp.update_device_status_(stopped_off_target_report(comp, remaining));
    EXPECT_EQ(comp.poll_policy_.get_next_update("ABC123"), esphome::millis() + DEFAULT_SETTLE_POLL_DELAY_MS)
        << "a stopped report is not scheduled from its remaining time, remaining=" << remaining;
    EXPECT_EQ(comp.poll_policy_.get_settle_update("ABC123"), 0u) << "no predicted-end read next to the confirming one";
    comp.update_device_status_(moving_report(comp, 12));  // moves again: rearm for the next round
  }

  comp.poll_policy_.set_next_update("ABC123", 0);
  comp.update_device_status_(stopped_off_target_report(comp, 0));
  esphome::test_clock::advance_ms(DEFAULT_SETTLE_POLL_DELAY_MS + 1);
  EXPECT_EQ(static_cast<size_t>(run_due_poll_against_silence(comp, radio)), planned_settling_attempts(comp))
      << "the confirming read is a settling read";
}
