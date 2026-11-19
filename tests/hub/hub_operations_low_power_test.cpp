/// @file hub_operations_low_power_test.cpp
/// @brief Tests for low-power devices in hub_operations.cpp: the low_power class on the wire and the wake belief the
/// hub feeds the exchange engine.

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

#include "hub_operations_fixture.h"

using namespace esphome::home_io_control;
using namespace hub_operations_test;

// ============================================================================
// low_power class -> CTRL1_LOW_POWER bit + start-frame preamble (end-to-end)
// ============================================================================
// The per-device low_power class, declared in YAML and carried through DeviceConfig into the
// registry, must reach BOTH the transmitted frame's CTRL1_LOW_POWER bit AND the preamble the
// exchange engine picks for the start frame. A call site reverting to a hardcoded low_power
// literal (the exact regression of issue #87) has to trip one of these assertions.

TEST(HubOperations, LowPowerDeviceExecuteUsesLongPreambleAndSetsLowPowerBit) {
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);

  comp.set_device_position("ABC123", 50);

  ASSERT_GE(radio.get_tx_configs().size(), 1u) << "the EXECUTE start frame should have been transmitted";
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, MOCK_WAKE_PREAMBLE)
      << "a low_power target's directed start frame must use the wake-up preamble";
  ASSERT_FALSE(radio.get_sent_data().empty());
  EXPECT_NE(radio.get_sent_data().front()[1] & CTRL1_LOW_POWER, 0)
      << "a low_power target's frame must carry CTRL1_LOW_POWER";
}

TEST(HubOperations, DefaultDeviceExecuteUsesNormalStartPreambleAndClearsLowPowerBit) {
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/false);

  comp.set_device_position("ABC123", 50);

  ASSERT_GE(radio.get_tx_configs().size(), 1u) << "the EXECUTE start frame should have been transmitted";
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "an always-alive target's directed start frame uses `start_preamble_ms`, not MOCK_WAKE_PREAMBLE";
  ASSERT_FALSE(radio.get_sent_data().empty());
  EXPECT_EQ(radio.get_sent_data().front()[1] & CTRL1_LOW_POWER, 0)
      << "an always-alive target's frame must leave CTRL1_LOW_POWER clear";
}

TEST(HubOperations, AStatusReadMarkedStateUnknownLeadsShortDespiteARecentStoppedReport) {
  // The hub carries AttemptRequest::with_state_unknown() to the exchange: a catch-up read of a cover
  // that reported "stopped" a second ago still leads with the short attempt a moving receiver hears.
  for (const bool marked : {false, true}) {
    TestableComponent comp;
    MockRadio radio;
    setup_low_power_cover(comp, radio, /*low_power=*/true);
    IoDevice *dev = comp.get_device("ABC123");
    ASSERT_NE(dev, nullptr);
    dev->last_status = esphome::millis();  // a report from just now: the read is not UNKNOWN by age

    IoFrame request{};
    create_get_status(request, comp.node_id_, dev->node_id, /*low_power=*/true);
    auto attempts = decisions::AttemptRequest::of_kind(decisions::ReadKind::SETTLING);
    if (marked)
      attempts = attempts.with_state_unknown();
    IoFrame response{};
    comp.send_and_receive_(request, response, FREQ_CH2, attempts);

    ASSERT_GE(radio.get_tx_configs().size(), 1u);
    EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, marked ? mock_preamble_bytes(comp.tuning_.start_preamble_ms)
                                                             : mock_preamble_bytes(comp.tuning_.low_power_wake_ms))
        << (marked ? "marked: the short lead first"
                   : "unmarked: a recent report keeps the read on the wake-up preamble");
  }
}

TEST(HubOperations, LowPowerDeviceStatusPollUsesLongPreambleAndSetsLowPowerBit) {
  // create_get_status is the frame the issue #87 reporter watched time out — cover it explicitly.
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);

  comp.request_device_status("ABC123");

  ASSERT_GE(radio.get_tx_configs().size(), 2u) << "the status poll start frames should have been transmitted";
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "a read to a receiver with no news of its state leads with one short attempt (ADR 0079)";
  EXPECT_EQ(radio.get_tx_configs()[1].preamble_len, MOCK_WAKE_PREAMBLE)
      << "a low_power target's status poll must use the wake-up preamble after the short lead";
  ASSERT_FALSE(radio.get_sent_data().empty());
  EXPECT_NE(radio.get_sent_data().front()[1] & CTRL1_LOW_POWER, 0)
      << "a low_power target's status poll must carry CTRL1_LOW_POWER";
}

// ============================================================================
// Low-power wake belief — evidence stamps and hub -> engine wiring
// ============================================================================
// A low_power device's start-frame preamble follows its wake belief (decisions::wake_belief()):
// the hub supplies the evidence (last_moving_evidence_ms / last_seen_ms), the engine picks the
// preamble per try. These tests run on a manual clock so ages are exact.

namespace {

void queue_frame(MockRadio &radio, const IoFrame &frame) { radio.queue_rx(test::make_rx_packet(frame)); }

/// Preamble of the transmit at `index` (0-based, in send order) — fails the test if there was none.
uint16_t preamble_of_tx(const MockRadio &radio, size_t index) {
  EXPECT_LT(index, radio.get_tx_configs().size()) << "expected a transmit #" << index;
  return index < radio.get_tx_configs().size() ? radio.get_tx_configs()[index].preamble_len : 0;
}

}  // namespace

// --- evidence: which paths stamp / clear last_moving_evidence_ms -------------------------------

TEST(HubOperations, AcceptedMoveWithoutAckStampsMovingEvidenceEvenWithoutAnOptimisticOverlay) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->optimistic_state = false;  // no overlay: the accept itself is the only signal of a move
  queue_frame(radio, build_challenge_request(dev->node_id, comp.node_id_));  // authenticated, never closed

  ASSERT_TRUE(comp.set_device_position("ABC123", 40)) << "an authenticated command is accepted";

  EXPECT_NE(dev->last_moving_evidence_ms, 0u);
}

TEST(HubOperations, FailedMoveDoesNotStampMovingEvidence) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->optimistic_state = false;

  ASSERT_FALSE(comp.set_device_position("ABC123", 40)) << "no reply at all: the command failed";

  EXPECT_EQ(dev->last_moving_evidence_ms, 0u) << "a command the device never accepted proves nothing";
}

TEST(HubOperations, StopAcceptedWithoutAStatusLeavesTheEvidenceAsItWas) {
  // A challenged STOP with no closing reply has no status to say whether the motor is still winding
  // down, so the evidence is left alone; the settle poll decides (a silent one spends it).
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  note_moving_evidence(*dev, esphome::millis());  // it was travelling
  const uint32_t stamp = dev->last_moving_evidence_ms;
  queue_frame(radio, build_challenge_request(dev->node_id, comp.node_id_));

  ASSERT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::STOP));

  EXPECT_EQ(dev->last_moving_evidence_ms, stamp);
}

TEST(HubOperations, StopAckThatReportsMovingKeepsTheSettlePollShortFirst) {
  // Field log (VELUX SSL, ADR 0040 amendment): the ack to an accepted STOP still reported "moving"
  // while the motor wound down, and the settle poll a second later drew nothing on three wake-up
  // tries. The ack's own flag keeps the evidence, so that poll leads with the short preamble.
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  note_moving_evidence(*dev, esphome::millis());
  queue_frame(radio, build_moving_status_response(comp.node_id_, /*remaining_s=*/0));

  ASSERT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::STOP));
  EXPECT_NE(dev->last_moving_evidence_ms, 0u) << "the motor said it is still moving";

  esphome::test_clock::advance_ms(STOP_SETTLE_POLL_CAP_MS);
  const size_t before = radio.get_tx_configs().size();
  comp.request_device_status("ABC123");

  ASSERT_EQ(radio.get_tx_configs().size() - before, static_cast<size_t>(LOW_POWER_SETTLING_ATTEMPTS));
  EXPECT_EQ(preamble_of_tx(radio, before), mock_preamble_bytes(comp.tuning_.start_preamble_ms));
  EXPECT_EQ(preamble_of_tx(radio, before + 1), MOCK_WAKE_PREAMBLE);
  EXPECT_EQ(preamble_of_tx(radio, before + 2), MOCK_WAKE_PREAMBLE);
}

TEST(HubOperations, FailedStopKeepsMovingEvidence) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  note_moving_evidence(*dev, esphome::millis());

  ASSERT_FALSE(comp.execute_device_command_("ABC123", CoverCommand::STOP)) << "no reply: the STOP failed";

  EXPECT_NE(dev->last_moving_evidence_ms, 0u) << "the receiver was never told to stop, so it may still be moving";
}

TEST(HubOperations, StopAckThatReportsStoppedLeavesNoEvidenceDespiteStalePositions) {
  // The ack to our own STOP is decoded with trust_position=false, so dev.target/position are stale
  // values from an earlier poll that have not converged, and normalize_stopped_state() reads that as
  // "still moving" (stamping evidence). The accepted STOP must still end with none, or a STOP that
  // worked would leave the receiver looking like it is travelling for LOW_POWER_MAX_TRAVEL_MS.
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->target = 100.0f;
  dev->position = 30.0f;
  note_moving_evidence(*dev, esphome::millis());
  queue_frame(radio, build_status_response(comp.node_id_));  // STATUS_STOPPED set

  ASSERT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::STOP));

  EXPECT_EQ(dev->last_moving_evidence_ms, 0u);
}

TEST(HubOperations, DecodedStatusTracksMovingEvidence) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  queue_frame(radio, build_status_response(comp.node_id_));  // stopped
  ASSERT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(dev->last_moving_evidence_ms, 0u);

  queue_frame(radio, build_moving_status_response(comp.node_id_, /*remaining_s=*/5));
  ASSERT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_NE(dev->last_moving_evidence_ms, 0u) << "a decoded 'not stopped' status is evidence of travel";

  queue_frame(radio, build_status_response(comp.node_id_));  // it arrived and stopped
  ASSERT_TRUE(comp.request_device_status("ABC123"));
  EXPECT_EQ(dev->last_moving_evidence_ms, 0u) << "a decoded 'stopped' status spends the evidence";
}

// --- wiring: the belief the engine derives from the hub's evidence ----------------------------

TEST(HubOperations, HaMoveToARestingLowPowerDeviceLeadsWithTheWakeUpPreamble) {
  // IOHomeCover::control() applies the optimistic overlay *before* the command is dispatched. That
  // prediction must not count as moving evidence, or a resting, duty-cycling receiver would be
  // treated as awake by the very command that has to wake it.
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 40.0f));

  comp.set_device_position("ABC123", 40);

  EXPECT_EQ(preamble_of_tx(radio, 0), MOCK_WAKE_PREAMBLE);
}

TEST(HubOperations, StopToALowPowerDeviceAtRestLeadsWithTheShortPreamble) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);

  comp.execute_device_command_("ABC123", CoverCommand::STOP);

  EXPECT_EQ(preamble_of_tx(radio, 0), mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "a STOP goes to a receiver that is moving, whatever the stamps say";
}

TEST(HubOperations, SettlePollAfterAnAcceptedMoveIsASingleTryLeadingWithTheShortPreamble) {
  // The tracked settle poll gets one try (its backoff ladder is its retry), and that try follows the
  // wake belief: seconds after an accepted move the receiver is travelling, and a travelling VELUX
  // solar receiver ignores the wake-up preamble (ADR 0040 D2).
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  queue_frame(radio, build_challenge_request(dev->node_id, comp.node_id_));
  ASSERT_TRUE(comp.set_device_position("ABC123", 40));
  ASSERT_NE(dev->last_moving_evidence_ms, 0u) << "precondition: the belief would be AWAKE";
  esphome::test_clock::advance_ms(3000);
  comp.begin_status_poll_tracking_("ABC123", 0);
  const size_t before = radio.get_tx_configs().size();

  comp.request_device_status("ABC123");

  ASSERT_EQ(radio.get_tx_configs().size() - before, 1u) << "scheduler-owned poll: a single try";
  EXPECT_EQ(preamble_of_tx(radio, before), mock_preamble_bytes(comp.tuning_.start_preamble_ms));
}

TEST(HubOperations, SettlePollAfterAnAcceptedStopLeadsShortThenGetsEveryTryOnTheWakeUpPreamble) {
  // After an accepted STOP nothing moves and the user's reversal waits on this poll, so it gets the
  // full budget instead of betting on one sample (a settling read, ADR 0074). The STOP cleared the
  // moving evidence and no status report has come since, so the receiver's state is unknown: one short
  // attempt reaches it while it still winds down, then every wake-up try, which a resting receiver
  // hears, even though its reply was just heard (ADR 0040, amendment; ADR 0079).
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  queue_frame(radio, build_challenge_request(dev->node_id, comp.node_id_));
  ASSERT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::STOP));
  ASSERT_EQ(dev->last_moving_evidence_ms, 0u) << "precondition: no moving evidence before or after the STOP";
  ASSERT_NE(dev->last_seen_ms, 0u) << "precondition: the STOP's challenge was heard";
  esphome::test_clock::advance_ms(STOP_SETTLE_POLL_CAP_MS);
  const size_t before = radio.get_tx_configs().size();

  comp.request_device_status("ABC123");

  ASSERT_EQ(radio.get_tx_configs().size() - before, static_cast<size_t>(LOW_POWER_SETTLING_ATTEMPTS) + 1);
  EXPECT_EQ(preamble_of_tx(radio, before), mock_preamble_bytes(comp.tuning_.start_preamble_ms));
  EXPECT_EQ(preamble_of_tx(radio, before + 1), MOCK_WAKE_PREAMBLE);
  EXPECT_EQ(preamble_of_tx(radio, before + 2), MOCK_WAKE_PREAMBLE);
  EXPECT_EQ(preamble_of_tx(radio, before + 3), MOCK_WAKE_PREAMBLE);
}

TEST(HubOperations, OnlyTheFirstPollAfterAStopGetsTheStopBudget) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  queue_frame(radio, build_challenge_request(dev->node_id, comp.node_id_));
  ASSERT_TRUE(comp.execute_device_command_("ABC123", CoverCommand::STOP));
  esphome::test_clock::advance_ms(STOP_SETTLE_POLL_CAP_MS);
  comp.request_device_status("ABC123");  // the post-STOP poll, all tries miss
  // Reset the streak so the ladder's own grace band cannot hand the next poll three tries either.
  comp.poll_policy_.clear_failure_streaks("ABC123");
  const size_t before = radio.get_tx_configs().size();

  comp.request_device_status("ABC123");

  EXPECT_EQ(radio.get_tx_configs().size() - before, static_cast<size_t>(SCHEDULED_POLL_MAX_TRIES));
}

TEST(HubOperations, SettlePollAfterAFailedStopDoesNotGetTheStopBudget) {
  // A STOP nobody answered has not stopped anything: the receiver may still be travelling, so the
  // normal ladder applies (its first retry slot already gets the full budget).
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  ASSERT_FALSE(comp.execute_device_command_("ABC123", CoverCommand::STOP));
  EXPECT_FALSE(comp.poll_policy_.take_stop_settle("ABC123"));
}

TEST(HubOperations, LadderSlotWithFullRetriesLeadsWithTheShortPreambleWhileBelievedAwake) {
  // The first slot after a missed settle poll gets the full retry budget, so the belief applies:
  // short / wake-up / wake-up for a receiver believed to be travelling.
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  note_moving_evidence(*dev, esphome::millis());
  comp.begin_status_poll_tracking_("ABC123", 2000);
  comp.poll_policy_.on_exchange_failed("ABC123", /*auth_like=*/false, esphome::millis());
  ASSERT_EQ(comp.poll_policy_.get_status_poll_failures("ABC123"), 1u);

  comp.request_device_status("ABC123");

  ASSERT_EQ(radio.get_tx_configs().size(), 3u);
  EXPECT_EQ(preamble_of_tx(radio, 0), mock_preamble_bytes(comp.tuning_.start_preamble_ms));
  EXPECT_EQ(preamble_of_tx(radio, 1), MOCK_WAKE_PREAMBLE);
  EXPECT_EQ(preamble_of_tx(radio, 2), MOCK_WAKE_PREAMBLE);
}

TEST(HubOperations, SilentPollWhileBelievedMovingSpendsTheMovingEvidence) {
  // A short movement ends within seconds, long before LOW_POWER_MAX_TRAVEL_MS: the receiver is at
  // rest and ignores the short preamble. A poll that draws nothing at all spends the evidence, so the
  // next exchange leads with the wake-up preamble instead of losing its first try again.
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  note_moving_evidence(*dev, esphome::millis());

  comp.request_device_status("ABC123");
  EXPECT_EQ(preamble_of_tx(radio, 0), mock_preamble_bytes(comp.tuning_.start_preamble_ms));
  EXPECT_EQ(dev->last_moving_evidence_ms, 0u) << "silence while believed moving spends the evidence";

  const size_t before = radio.get_tx_configs().size();
  esphome::test_clock::advance_ms(1000);
  comp.set_device_position("ABC123", 40);
  EXPECT_EQ(preamble_of_tx(radio, before), MOCK_WAKE_PREAMBLE);
}

TEST(HubOperations, SpentEvidenceStaysSpentUntilMovementIsStampedAgain) {
  // Pins a known trade-off: a travelling receiver that misses one short try looks like one at rest,
  // so its evidence is spent. Nothing re-promotes it until something stamps movement again -- a decoded
  // status that says "moving", an overheard remote, or a movement command the device accepts. The
  // reads that follow still lead with one short attempt (the state is unknown, ADR 0079) before the
  // wake-up attempts, so a receiver that is in fact still moving is reached by them. If a field report
  // shows positions freezing mid-travel, this is the behaviour to revisit.
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  note_moving_evidence(*dev, esphome::millis());

  comp.request_device_status("ABC123");  // silent: spends the evidence
  ASSERT_EQ(dev->last_moving_evidence_ms, 0u);

  for (int poll = 0; poll < 2; poll++) {
    const size_t before = radio.get_tx_configs().size();
    esphome::test_clock::advance_ms(1000);
    comp.request_device_status("ABC123");
    EXPECT_EQ(preamble_of_tx(radio, before), mock_preamble_bytes(comp.tuning_.start_preamble_ms))
        << "poll " << poll << " after the evidence was spent: the short lead";
    EXPECT_EQ(preamble_of_tx(radio, before + 1), MOCK_WAKE_PREAMBLE)
        << "poll " << poll << " after the evidence was spent: then the wake-up preamble";
    EXPECT_EQ(dev->last_moving_evidence_ms, 0u) << "silence does not bring the evidence back";
  }

  note_moving_evidence(*dev, esphome::millis());  // what a decoded moving status or accepted move does
  const size_t before = radio.get_tx_configs().size();
  comp.request_device_status("ABC123");
  EXPECT_EQ(preamble_of_tx(radio, before), mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "fresh movement evidence leads short again";
}

TEST(HubOperations, MovingEvidenceExpiresAfterMaxTravel) {
  esphome::test_clock::ManualClock clock(500000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  note_moving_evidence(*dev, esphome::millis());

  esphome::test_clock::advance_ms(LOW_POWER_MAX_TRAVEL_MS - 1000);
  comp.request_device_status("ABC123");
  EXPECT_EQ(preamble_of_tx(radio, 0), mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "still inside the travel window";

  const size_t before = radio.get_tx_configs().size();
  esphome::test_clock::advance_ms(2000);  // now past LOW_POWER_MAX_TRAVEL_MS since the move
  comp.request_device_status("ABC123");
  EXPECT_EQ(preamble_of_tx(radio, before), mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "no news since the move and the window is over: its state is unknown, so the read leads short";
  EXPECT_EQ(preamble_of_tx(radio, before + 1), MOCK_WAKE_PREAMBLE)
      << "then the wake-up preamble a resting receiver hears";
}

TEST(HubOperations, RecentReplyAloneKeepsTheWakeUpPreamble) {
  esphome::test_clock::ManualClock clock(500000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  queue_frame(radio, build_status_response(comp.node_id_));
  ASSERT_TRUE(comp.request_device_status("ABC123")) << "a real reply: stamps last_seen_ms";
  const size_t after_reply = radio.get_tx_configs().size();

  esphome::test_clock::advance_ms(100);
  comp.request_device_status("ABC123");  // heard from 100 ms ago, reported stopped
  EXPECT_EQ(preamble_of_tx(radio, after_reply), MOCK_WAKE_PREAMBLE)
      << "a resting receiver ignores the short preamble even right after it answered";
}

TEST(HubOperations, UnregisteredDestinationIsTreatedAsAsleep) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  const uint8_t stranger[NODE_ID_SIZE] = {0x12, 0x34, 0x56};
  IoFrame request{};
  ASSERT_TRUE(create_get_status(request, comp.node_id_, stranger, /*low_power=*/true));
  IoFrame response{};

  comp.exchange_engine_.send_and_receive(request, response, FREQ_CH2);

  ASSERT_EQ(radio.get_tx_configs().size(), 3u);
  for (const auto &config : radio.get_tx_configs())
    EXPECT_EQ(config.preamble_len, MOCK_WAKE_PREAMBLE) << "no record of this node, so nothing says it is awake";
}

TEST(HubOperations, AlwaysAliveDeviceKeepsTheNormalStartPreambleOnEveryTryDespiteEvidence) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/false);
  note_moving_evidence(*comp.get_device("ABC123"), esphome::millis());

  comp.request_device_status("ABC123");

  ASSERT_EQ(radio.get_tx_configs().size(), planned_settling_attempts(comp));
  for (const auto &config : radio.get_tx_configs())
    EXPECT_EQ(config.preamble_len, mock_preamble_bytes(comp.tuning_.start_preamble_ms));
}

TEST(HubOperations, WakeBeliefSwitchOffRestoresTheWakeUpPreambleForALowPowerDevice) {
  esphome::test_clock::ManualClock clock(50000);
  TestableComponent comp;
  MockRadio radio;
  setup_low_power_cover(comp, radio, /*low_power=*/true);
  comp.tuning_.low_power_wake_belief = false;
  note_moving_evidence(*comp.get_device("ABC123"), esphome::millis());

  comp.request_device_status("ABC123");

  ASSERT_EQ(radio.get_tx_configs().size(), 3u);
  for (const auto &config : radio.get_tx_configs())
    EXPECT_EQ(config.preamble_len, MOCK_WAKE_PREAMBLE);
}
