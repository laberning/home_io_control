/// @file
/// @brief RadioBridle link supervision on the manual clock: heartbeat, non-blocking resync, the
///        down state and its retry, event-counter gaps, unsolicited ready, and trace lines.

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "fake_coprocessor.h"
#include "testable_radio_bridle.h"

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::bridle;
using esphome::test_clock::ManualClock;
using Link = RadioBridle::Link;

namespace {

struct Rig {
  ManualClock clock;
  FakeByteStream stream;
  FakeCoprocessor cp;
  FakeResetLine reset;
  TestableRadioBridle radio;

  /// @param trace_file Write the driver's trace when HIOC_BRIDLE_TRACE_DIR is set (a session
  ///        fixture's source); the handshake is part of it, so this is decided before init().
  explicit Rig(bool trace_file = false) : cp(stream), radio(&stream, io2g4_phy(), &reset, std::nullopt) {
    reset.on_release = [this] { cp.boot(); };
    if (trace_file)
      radio.trace_to_environment_dir();
    EXPECT_TRUE(radio.init());
  }

  static uint64_t now() { return esphome::test_clock::state().now_us; }
  static void advance_ms(uint32_t ms) { esphome::test_clock::advance_ms(ms); }

  /// One hub pass; returns the manual-clock time it took, in µs.
  uint64_t pass() {
    uint64_t const start = now();
    RadioRxPacket packet;
    radio.check_for_packet(packet);
    return now() - start;
  }
  /// Passes every `step_ms` until `done()` or `limit_ms` has gone by; returns the longest pass.
  template<class Done> uint64_t run_until(Done done, uint32_t limit_ms, uint32_t step_ms = 10) {
    uint64_t longest = 0;
    for (uint32_t t = 0; t < limit_ms && !done(); t += step_ms) {
      advance_ms(step_ms);
      longest = std::max(longest, pass());
    }
    return longest;
  }
  bool running() { return radio.link_state() == Link::RUNNING; }
  bool down() { return radio.link_state() == Link::DOWN; }
  size_t commands(MsgType type) { return cp.commands_of(type).size(); }
};

}  // namespace

// === Heartbeat ===

TEST(RadioBridleLink, ThirtySecondsOfSilenceSendsGetStatsAndAnAnswerKeepsTheLink) {
  Rig rig;
  rig.advance_ms(29000);
  rig.pass();
  EXPECT_EQ(rig.commands(MsgType::GET_STATS), 0u);
  rig.advance_ms(1500);
  rig.pass();
  EXPECT_EQ(rig.commands(MsgType::GET_STATS), 1u);
  rig.advance_ms(20);
  rig.pass();
  rig.advance_ms(200);
  rig.pass();
  EXPECT_TRUE(rig.running());
  EXPECT_EQ(rig.commands(MsgType::GET_STATS), 1u) << "the answer counted as traffic: no second heartbeat";
  EXPECT_EQ(rig.reset.pulse_starts_us.size(), 1u);
}

TEST(RadioBridleLink, EventsAloneDoNotKeepTheClaim) {
  // The co-processor counts commands, not traffic (spec §2.1): 30 s of events with no command sent still draw a
  // heartbeat.
  Rig rig;
  for (int i = 0; i < 28; i++) {
    rig.advance_ms(1000);
    rig.cp.inject_rx_overflow(1);
    rig.pass();
  }
  EXPECT_EQ(rig.commands(MsgType::GET_STATS), 0u) << "28 s since the last command";
  rig.advance_ms(2100);
  rig.cp.inject_rx_overflow(1);
  rig.pass();
  EXPECT_EQ(rig.commands(MsgType::GET_STATS), 1u);
}

TEST(RadioBridleLink, NoExtraHeartbeatWhileCommandsFlow) {
  Rig rig;
  for (int i = 0; i < 8; i++) {
    rig.advance_ms(20000);
    rig.radio.read_rssi();  // a command every 20 s
    rig.pass();
  }
  EXPECT_EQ(rig.commands(MsgType::GET_STATS), 0u);
}

TEST(RadioBridleLink, AHeartbeatWithoutAnAnswerWithin100msStartsAResync) {
  Rig rig;
  rig.cp.set_silent(true);
  rig.advance_ms(30100);
  rig.pass();  // the heartbeat goes out
  ASSERT_EQ(rig.commands(MsgType::GET_STATS), 1u);
  rig.advance_ms(99);
  rig.pass();
  EXPECT_TRUE(rig.running()) << "still within the 100 ms";
  rig.advance_ms(5);
  rig.pass();
  EXPECT_FALSE(rig.running());
}

// === Resync ===

TEST(RadioBridleLink, AResyncNeverBlocksAPassLongerThanTheResetPulse) {
  Rig rig;
  rig.cp.set_silent(true);
  rig.advance_ms(30100);
  rig.pass();
  rig.advance_ms(150);
  rig.pass();
  ASSERT_FALSE(rig.running());
  rig.cp.set_silent(false);  // the co-processor is back by the time the pulse comes
  uint64_t const longest = rig.run_until([&] { return rig.running(); }, 10000, 5);
  EXPECT_TRUE(rig.running());
  EXPECT_LE(longest, 21000u) << "the 10 ms pulse and 10 ms of settling are the only wait";
}

TEST(RadioBridleLink, AResyncConfiguresEverythingAgainIncludingTheEventMask) {
  Rig rig;
  rig.cp.set_silent(true);
  rig.advance_ms(30100);
  rig.pass();
  rig.advance_ms(150);
  rig.pass();
  rig.cp.set_silent(false);
  size_t const sets_before = rig.commands(MsgType::SET_PARAMS);
  rig.run_until([&] { return rig.running(); }, 10000, 5);
  ASSERT_TRUE(rig.running());
  EXPECT_EQ(rig.commands(MsgType::SET_PARAMS), sets_before + 2) << "the event mask, then the radio";
  EXPECT_EQ(rig.cp.event_mask(), 0x0F);
  EXPECT_EQ(rig.reset.pulse_starts_us.size(), 2u);
}

TEST(RadioBridleLink, TheChannelSurvivesAResync) {
  Rig rig;
  rig.radio.change_frequency(FREQ_CH3);
  rig.cp.set_silent(true);
  rig.advance_ms(30100);
  rig.pass();
  rig.advance_ms(150);
  rig.pass();
  rig.cp.set_silent(false);
  rig.run_until([&] { return rig.running(); }, 10000, 5);
  ASSERT_TRUE(rig.running());
  rig.pass();
  size_t const sent_by_resync = rig.cp.commands_of(MsgType::RX_ON).size();
  EXPECT_FALSE(rig.cp.receiving()) << "a resync leaves the radio idle: the next wait restores receive";
  rig.radio.set_mode_rx();
  auto rx_ons = rig.cp.commands_of(MsgType::RX_ON);
  ASSERT_EQ(rx_ons.size(), sent_by_resync + 1);
  Message m;
  ASSERT_EQ(decode_message(rx_ons.back()->message.data(), rx_ons.back()->message.size(), m), DecodeStatus::OK);
  EXPECT_EQ(std::get<RxOn>(m.body).freq_hz, 2475000000u) << "the driver's channel is put back";
  EXPECT_EQ(rig.cp.freq_hz(), 2475000000u);
}

TEST(RadioBridleLink, WhileTheLinkIsLostSendingFailsAtOnce) {
  Rig rig;
  rig.cp.set_silent(true);
  rig.advance_ms(30100);
  rig.pass();
  rig.advance_ms(150);
  rig.pass();
  ASSERT_FALSE(rig.running());
  const uint8_t frame[12] = {};
  uint64_t const start = Rig::now();
  EXPECT_EQ(rig.radio.send_packet(frame, sizeof(frame), {}).status, TxResult::Status::FAILED);
  EXPECT_LT(Rig::now() - start, 1000u);
  EXPECT_FALSE(rig.radio.run_idle_scan());
  EXPECT_EQ(rig.radio.read_rssi(), RadioBridle::RSSI_NONE_DBM);
}

TEST(RadioBridleLink, ATxDoneTimeoutStartsAResyncAndTheNextTryWaitsForIt) {
  Rig rig;
  rig.cp.set_silent(true);
  const uint8_t frame[12] = {};
  EXPECT_EQ(rig.radio.send_packet(frame, sizeof(frame), {}).status, TxResult::Status::FAILED);
  EXPECT_FALSE(rig.running());
  rig.cp.set_silent(false);
  rig.run_until([&] { return rig.running(); }, 10000, 5);
  EXPECT_TRUE(rig.running());
  EXPECT_TRUE(rig.radio.send_packet(frame, sizeof(frame), {}).sent());
}

TEST(RadioBridleLink, ResetPulsesAreAtLeastFiveSecondsApart) {
  Rig rig;
  rig.cp.set_silent(true);
  rig.advance_ms(30100);
  rig.pass();
  rig.run_until([&] { return rig.down(); }, 60000, 20);
  auto &pulses = rig.reset.pulse_starts_us;
  ASSERT_GE(pulses.size(), 3u);
  for (size_t i = 1; i < pulses.size(); i++)
    EXPECT_GE(pulses[i] - pulses[i - 1], 5000000u) << "pulse " << i;
}

TEST(RadioBridleLink, ThreeFailedResyncsInAMinuteTakeTheLinkDown) {
  Rig rig;
  rig.cp.set_silent(true);
  rig.advance_ms(30100);
  rig.pass();
  rig.run_until([&] { return rig.down(); }, 60000, 20);
  ASSERT_TRUE(rig.down());
  EXPECT_FALSE(rig.radio.is_failed()) << "runtime link loss is not a latched failure";
  EXPECT_EQ(rig.reset.pulse_starts_us.size(), 1u + 3u) << "the boot pulse and three resync attempts";
}

TEST(RadioBridleLink, ADownLinkTriesOnceAMinuteAndRecoversWhenTheCoprocessorIsBack) {
  Rig rig;
  rig.cp.set_silent(true);
  rig.advance_ms(30100);
  rig.pass();
  rig.run_until([&] { return rig.down(); }, 60000, 20);
  ASSERT_TRUE(rig.down());
  size_t const pulses = rig.reset.pulse_starts_us.size();

  rig.run_until([] { return false; }, 50000, 100);
  EXPECT_EQ(rig.reset.pulse_starts_us.size(), pulses) << "nothing within the minute";
  EXPECT_TRUE(rig.down());

  rig.run_until([&] { return rig.reset.pulse_starts_us.size() > pulses; }, 15000, 50);
  EXPECT_EQ(rig.reset.pulse_starts_us.size(), pulses + 1) << "one attempt after the minute";
  rig.run_until([&] { return rig.down() && rig.commands(MsgType::HELLO) == 0; }, 3000, 50);  // it fails: still silent
  EXPECT_TRUE(rig.down());

  rig.cp.set_silent(false);
  rig.run_until([&] { return rig.reset.pulse_starts_us.size() > pulses + 1; }, 70000, 100);
  rig.run_until([&] { return rig.running(); }, 5000, 5);
  EXPECT_TRUE(rig.running());
  const uint8_t frame[12] = {};
  EXPECT_TRUE(rig.radio.send_packet(frame, sizeof(frame), {}).sent());
}

TEST(RadioBridleLink, ACoprocessorThatComesBackWithAnotherVersionStaysDown) {
  Rig rig;
  rig.cp.set_silent(true);
  rig.advance_ms(30100);
  rig.pass();
  rig.advance_ms(150);
  rig.pass();
  // A co-processor re-flashed meanwhile advertises another version.
  rig.cp.set_silent(false);
  rig.cp.set_advertised_minor(PROTO_MINOR + 1);
  rig.run_until([&] { return rig.down(); }, 60000, 20);
  EXPECT_TRUE(rig.down());
  EXPECT_FALSE(rig.radio.is_failed());
}

// === Host link claim (spec §2.1, §7) ===

TEST(RadioBridleLink, LinkInUseInTheMiddleOfASessionIsALostLinkAndTheResetTakesTheBoardBack) {
  Rig rig;
  rig.cp.set_link_in_use(true);
  rig.advance_ms(30100);
  rig.pass();  // the heartbeat is refused
  rig.advance_ms(10);
  rig.pass();
  EXPECT_FALSE(rig.running());
  rig.cp.set_link_in_use(false);  // the reset pulse clears the claim
  rig.run_until([&] { return rig.running(); }, 20000, 20);
  EXPECT_TRUE(rig.running());
  EXPECT_EQ(rig.reset.pulse_starts_us.size(), 2u);
}

TEST(RadioBridleLink, AResetLineThatReachesNothingFallsBackToHelloAndTakesTheFreedBoard) {
  // A reset pin is configured but not wired to the co-processor: the pulse brings no boot ready. The resync then asks
  // with hello, as the start-up handshake does, and takes the board once the other link has let go (spec §2.1).
  Rig rig;
  rig.reset.on_release = nullptr;
  rig.cp.set_link_in_use(true);  // another host took the board while this one was silent
  rig.advance_ms(30100);
  rig.pass();  // the heartbeat is refused: a lost link
  rig.advance_ms(10);
  rig.pass();
  ASSERT_FALSE(rig.running());
  size_t const hellos_before = rig.cp.commands_of(MsgType::HELLO).size();
  rig.cp.set_link_in_use(false);  // the other host closed its USB port
  rig.cp.take_over_on_next_hello();
  rig.run_until([&] { return rig.running(); }, 20000, 10);
  EXPECT_TRUE(rig.running());
  EXPECT_GT(rig.cp.commands_of(MsgType::HELLO).size(), hellos_before) << "the resync asked with hello";
}

TEST(RadioBridleLink, AnUnawaitedCommandRefusedWithLinkInUseAlsoResyncs) {
  Rig rig;
  rig.cp.set_link_in_use(true);
  rig.radio.read_rssi();
  rig.advance_ms(20);
  rig.pass();
  EXPECT_FALSE(rig.running());
}

TEST(RadioBridleLink, WithoutAResetLineAHelloTakesTheFreedBoardBack) {
  ManualClock clock;
  FakeByteStream stream;
  FakeCoprocessor cp(stream);
  TestableRadioBridle radio(&stream, io2g4_phy(), nullptr, std::nullopt);
  cp.boot();
  ASSERT_TRUE(radio.init());
  cp.set_link_in_use(true);
  esphome::test_clock::advance_ms(30100);
  RadioRxPacket packet;
  radio.check_for_packet(packet);
  esphome::test_clock::advance_ms(10);
  radio.check_for_packet(packet);
  ASSERT_NE(radio.link_state(), Link::RUNNING);
  cp.set_link_in_use(false);
  cp.take_over_on_next_hello();
  for (int t = 0; t < 12000 && radio.link_state() != Link::RUNNING;
       t += 10) {  // the first hello was refused; the next try is 5 s on
    esphome::test_clock::advance_ms(10);
    radio.check_for_packet(packet);
  }
  EXPECT_EQ(radio.link_state(), Link::RUNNING);
  EXPECT_FALSE(cp.commands_of(MsgType::HELLO).empty());
  EXPECT_NE(cp.event_mask(), 0) << "configured again after the takeover";
}

TEST(RadioBridleLink, AHubThatStartsWhileAnotherLinkHoldsTheBoardTakesItOnceFreed) {
  // No reset line: the start-up hello is refused. The hub starts with the link down instead of failing, and its retry
  // takes the board when the other link lets go (spec §2.1).
  ManualClock clock;
  FakeByteStream stream;
  FakeCoprocessor cp(stream);
  TestableRadioBridle radio(&stream, io2g4_phy(), nullptr, std::nullopt);
  cp.set_link_in_use(true);
  ASSERT_TRUE(radio.init());
  EXPECT_FALSE(radio.is_failed());
  EXPECT_EQ(radio.link_state(), Link::DOWN);
  RadioRxPacket packet;
  esphome::test_clock::advance_ms(RadioBridle::DOWN_RETRY_MS + 100);
  radio.check_for_packet(packet);  // the first retry is still refused
  EXPECT_NE(radio.link_state(), Link::RUNNING);
  cp.set_link_in_use(false);  // the PC tool closed its port
  cp.take_over_on_next_hello();
  for (int t = 0; t < 2 * static_cast<int>(RadioBridle::DOWN_RETRY_MS) && radio.link_state() != Link::RUNNING;
       t += 10) {
    esphome::test_clock::advance_ms(10);
    radio.check_for_packet(packet);
  }
  EXPECT_EQ(radio.link_state(), Link::RUNNING);
  EXPECT_NE(cp.event_mask(), 0) << "configured after the takeover";
}

TEST(RadioBridleLink, ATakeoverReadyIsAcceptedAndConfiguredLikeABoot) {
  // The first hello after the hub's reset pulse is not needed when the boot ready is lost: init() asks hello.
  ManualClock clock;
  FakeByteStream stream;
  FakeCoprocessor cp(stream);
  TestableRadioBridle radio(&stream, io2g4_phy(), nullptr, std::nullopt);
  cp.take_over_on_next_hello();  // no boot ready is ever sent: the hub's hello takes the board
  ASSERT_TRUE(radio.init());
  EXPECT_EQ(radio.link_state(), Link::RUNNING);
  EXPECT_NE(cp.event_mask(), 0);
  EXPECT_EQ(radio.counter_gaps(), 0u) << "the takeover ready restarts the counter at 0";
}

// === Event counter and unsolicited ready ===

TEST(RadioBridleLink, TwoCounterGapsAreTolerated) {
  Rig rig;
  for (int i = 0; i < 2; i++) {
    rig.cp.skip_event_counter(2);
    rig.cp.inject_rx_overflow(1);
    rig.advance_ms(1000);
    rig.pass();
  }
  EXPECT_EQ(rig.radio.counter_gaps(), 2u);
  EXPECT_TRUE(rig.running());
}

TEST(RadioBridleLink, ThreeCounterGapsInTenSecondsStartAResync) {
  Rig rig;
  for (int i = 0; i < 3; i++) {
    rig.cp.skip_event_counter(2);
    rig.cp.inject_rx_overflow(1);
    rig.advance_ms(1000);
    rig.pass();
  }
  EXPECT_EQ(rig.radio.counter_gaps(), 3u);
  EXPECT_FALSE(rig.running());
}

TEST(RadioBridleLink, GapsFarApartAreNotRepeatedGaps) {
  Rig rig;
  for (int i = 0; i < 3; i++) {
    rig.cp.skip_event_counter(2);
    rig.cp.inject_rx_overflow(1);
    rig.advance_ms(6000);
    rig.pass();
    rig.cp.inject_rx_overflow(1);  // keep the link busy so the heartbeat stays out of it
  }
  EXPECT_TRUE(rig.running());
}

// Source of the session fixture bridle_link_unsolicited_ready (tests/corpus/sessions/).
TEST(RadioBridleLink, AnUnsolicitedReadyReconfiguresWithoutAnotherResetPulse) {
  Rig rig(/*trace_file=*/true);
  rig.radio.set_mode_rx();
  rig.pass();  // the co-processor acks the rx_on before it reboots
  size_t const sets_before = rig.commands(MsgType::SET_PARAMS);
  rig.cp.reboot(ResetCause::WATCHDOG);
  EXPECT_EQ(rig.cp.event_mask(), 0) << "the reboot cleared it";
  uint64_t const longest = rig.run_until([&] { return rig.running() && rig.cp.event_mask() != 0; }, 3000, 5);
  EXPECT_TRUE(rig.running());
  EXPECT_EQ(rig.cp.event_mask(), 0x0F) << "the event mask is set again";
  EXPECT_EQ(rig.commands(MsgType::SET_PARAMS), sets_before + 2);
  EXPECT_EQ(rig.reset.pulse_starts_us.size(), 1u) << "the co-processor had already rebooted";
  EXPECT_LT(longest, 2000u);
  rig.radio.set_mode_rx();
  RadioRxPacket packet;
  EXPECT_FALSE(rig.radio.wait_for_packet(packet, 2));
  EXPECT_TRUE(rig.cp.receiving());
}

TEST(RadioBridleLink, FramesQueuedBeforeARebootAreNotDeliveredAfterIt) {
  Rig rig;
  rig.radio.set_mode_rx();
  rig.cp.inject_rx(2450000000u, {0x01, 0x69, 0x6F, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xAA, 0xBB});
  rig.radio.is_sync_detected();  // drains the frame into the FIFO without taking it
  ASSERT_EQ(rig.radio.fifo_count(), 1u);
  rig.cp.reboot(ResetCause::SOFT);
  rig.run_until([&] { return rig.running() && rig.cp.event_mask() != 0; }, 3000, 5);
  RadioRxPacket packet;
  EXPECT_FALSE(rig.radio.check_for_packet(packet)) << "a reply from before the reboot answers nothing";
}

// === Unawaited commands: ack, refusal, loss ===

namespace {

/// A 12-byte io frame to send on channel 2.
std::vector<uint8_t> some_frame() {
  std::vector<uint8_t> f;
  for (uint8_t i = 0; i < 12; i++)
    f.push_back(static_cast<uint8_t>(0x20 + i));
  return f;
}

RadioTxConfig channel_2() {
  RadioTxConfig cfg{};
  cfg.freq_hz = FREQ_CH2;
  return cfg;
}

}  // namespace

TEST(RadioBridleLink, AnAckClearsTheEntryOfTheCommandItAnswers) {
  Rig rig;
  rig.radio.set_mode_rx();
  EXPECT_EQ(rig.radio.pending_count(), 1u);
  rig.pass();
  EXPECT_EQ(rig.radio.pending_count(), 0u);
  EXPECT_TRUE(rig.running());
  EXPECT_EQ(rig.radio.stale_replies(), 0u) << "an ack for a command we sent is not a stale reply";
}

TEST(RadioBridleLink, ARefusedRxScanIsRetriedAfterTheHoldOffAndNotBefore) {
  Rig rig;
  rig.cp.refuse_next_rx_scan();
  EXPECT_TRUE(rig.radio.run_idle_scan());
  rig.pass();
  EXPECT_FALSE(rig.radio.scanning()) << "the refusal undoes the optimistic state";
  EXPECT_FALSE(rig.radio.rx_on_active());
  EXPECT_EQ(rig.radio.unawaited_errors(), 1u);
  EXPECT_TRUE(rig.running()) << "a refusal is not a lost link";

  // During the hold-off the hub hops from the host, so the radio receives and follows change_frequency().
  EXPECT_FALSE(rig.radio.run_idle_scan());
  rig.pass();
  EXPECT_EQ(rig.commands(MsgType::RX_ON), 1u);
  EXPECT_TRUE(rig.radio.rx_on_active());
  rig.radio.change_frequency(FREQ_CH3);
  rig.pass();
  EXPECT_EQ(rig.commands(MsgType::RX_ON), 2u) << "a hop during the hold-off is an rx_on, not a recorded frequency";
  rig.advance_ms(RadioBridle::SCAN_RETRY_MS - 100);
  EXPECT_FALSE(rig.radio.run_idle_scan());
  EXPECT_EQ(rig.commands(MsgType::RX_SCAN), 1u) << "no retry loop";
  rig.advance_ms(200);
  EXPECT_TRUE(rig.radio.run_idle_scan());
  EXPECT_EQ(rig.commands(MsgType::RX_SCAN), 2u);
  rig.pass();
  EXPECT_TRUE(rig.radio.scanning()) << "the second try was accepted";
}

TEST(RadioBridleLink, ARefusedRxOnClearsReceiveAndIsNotSentAgainBeforeTheHoldOff) {
  Rig rig;
  rig.cp.reject_next(MsgType::RX_ON, ErrorCode::PARAM_RANGE, field::rx_on::FREQ_HZ);
  rig.radio.set_mode_rx();
  rig.pass();
  EXPECT_FALSE(rig.radio.rx_on_active());
  RadioRxPacket packet;
  EXPECT_FALSE(rig.radio.wait_for_packet(packet, 0));
  EXPECT_EQ(rig.commands(MsgType::RX_ON), 1u) << "the wait does not repeat the refused command at once";
  rig.advance_ms(RadioBridle::SCAN_RETRY_MS + 1);
  EXPECT_FALSE(rig.radio.wait_for_packet(packet, 0));
  EXPECT_EQ(rig.commands(MsgType::RX_ON), 2u);
  EXPECT_TRUE(rig.radio.rx_on_active());
}

TEST(RadioBridleLink, ARefusalThatALaterTxOvertookChangesNoState) {
  Rig rig;
  rig.cp.reject_next(MsgType::RX_ON, ErrorCode::PARAM_RANGE, field::rx_on::FREQ_HZ);
  rig.radio.set_mode_rx();
  const auto frame = some_frame();
  ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), channel_2()).sent());
  rig.pass();
  EXPECT_EQ(rig.radio.unawaited_errors(), 1u) << "the refusal arrived";
  EXPECT_TRUE(rig.radio.rx_on_active()) << "but the tx's then_rx describes the radio now";
  EXPECT_TRUE(rig.running());
}

TEST(RadioBridleLink, ErrorNotConfiguredToAnUnawaitedCommandStartsAResync) {
  Rig rig;
  rig.cp.reject_next(MsgType::RX_ON, ErrorCode::NOT_CONFIGURED);
  rig.radio.set_mode_rx();
  rig.pass();
  EXPECT_FALSE(rig.running());
  EXPECT_EQ(rig.radio.pending_count(), 0u);
  rig.run_until([&] { return rig.running(); }, 10000, 5);
  EXPECT_TRUE(rig.running()) << "the configuration is sent again";
}

TEST(RadioBridleLink, ErrorNotConfiguredToAnAwaitedCommandStartsAResync) {
  Rig rig;
  rig.cp.reject_next(MsgType::GET_RSSI, ErrorCode::NOT_CONFIGURED);
  EXPECT_EQ(rig.radio.read_rssi(), RadioBridle::RSSI_NONE_DBM);
  EXPECT_FALSE(rig.running());
}

TEST(RadioBridleLink, ErrorNotReceivingToGetRssiIsNoValueAndNoResync) {
  Rig rig;
  EXPECT_EQ(rig.radio.read_rssi(), RadioBridle::RSSI_NONE_DBM) << "the radio is idle";
  EXPECT_TRUE(rig.running());
  EXPECT_EQ(rig.cp.errors_sent(ErrorCode::NOT_RECEIVING), 1u);
}

TEST(RadioBridleLink, AMissingAckResyncsTheLinkAfterTheAckTimeout) {
  Rig rig;
  rig.cp.drop_next_ack();
  rig.radio.set_mode_rx();
  rig.advance_ms(RadioBridle::ACK_TIMEOUT_MS - 50);
  rig.pass();
  EXPECT_TRUE(rig.running());
  rig.advance_ms(100);
  rig.pass();
  EXPECT_FALSE(rig.running());
}

TEST(RadioBridleLink, AnAckNamingALaterCommandThanTheHeadMeansTheHeadWasLost) {
  Rig rig;
  rig.cp.drop_next_ack();
  rig.radio.set_mode_rx();
  rig.radio.change_frequency(FREQ_CH3);  // a second rx_on; only its ack arrives
  EXPECT_EQ(rig.radio.pending_count(), 2u);
  rig.pass();
  EXPECT_FALSE(rig.running());
}

TEST(RadioBridleLink, AnUnsolicitedRadioFaultResyncsTheLink) {
  Rig rig;
  rig.cp.inject_radio_fault(3);
  rig.pass();
  EXPECT_FALSE(rig.running());
  EXPECT_EQ(rig.radio.stale_replies(), 0u) << "an error that answers no command is not a stale reply";
}

TEST(RadioBridleLink, AFullPendingTableWaitsForTheHeadsReplyBeforeSending) {
  Rig rig;
  rig.radio.set_mode_rx();
  // More retunes than the table holds, with no pass in between: the acks are in the stream, unread.
  for (size_t i = 0; i < RadioBridle::PENDING_DEPTH + 1; i++)
    rig.radio.change_frequency(i % 2 == 0 ? FREQ_CH3 : FREQ_CH1);
  EXPECT_TRUE(rig.running());
  EXPECT_EQ(rig.commands(MsgType::RX_ON), RadioBridle::PENDING_DEPTH + 2) << "every command went out";
  rig.pass();
  EXPECT_EQ(rig.radio.pending_count(), 0u);
}

TEST(RadioBridleLink, AFullPendingTableWithNoReplyResyncsAfterTheAckTimeout) {
  Rig rig;
  rig.cp.set_silent(true);
  rig.radio.set_mode_rx();
  for (size_t i = 0; i < RadioBridle::PENDING_DEPTH + 1; i++)
    rig.radio.change_frequency(i % 2 == 0 ? FREQ_CH3 : FREQ_CH1);
  EXPECT_FALSE(rig.running());
  EXPECT_EQ(rig.commands(MsgType::RX_ON), RadioBridle::PENDING_DEPTH) << "the command that did not fit never went out";
}

TEST(RadioBridleLink, ThePendingTableIsEmptyAfterAResync) {
  Rig rig;
  rig.cp.set_silent(true);
  rig.radio.set_mode_rx();
  rig.radio.set_mode_standby();
  EXPECT_EQ(rig.radio.pending_count(), 2u);
  rig.advance_ms(RadioBridle::ACK_TIMEOUT_MS + 10);
  rig.pass();
  EXPECT_FALSE(rig.running());
  EXPECT_EQ(rig.radio.pending_count(), 0u);
  rig.cp.set_silent(false);
  rig.run_until([&] { return rig.running(); }, 10000, 5);
  ASSERT_TRUE(rig.running());
  EXPECT_EQ(rig.radio.pending_count(), 0u);
  EXPECT_EQ(rig.radio.unawaited_errors(), 0u);
}

TEST(RadioBridleLink, AResyncSendsNoCommandBeyondTheConfiguration) {
  Rig rig;
  rig.radio.set_mode_rx();
  rig.pass();
  rig.cp.set_silent(true);
  rig.advance_ms(30100);
  rig.pass();
  rig.advance_ms(150);
  rig.pass();
  rig.cp.set_silent(false);
  size_t const rx_ons = rig.commands(MsgType::RX_ON);
  size_t const idles = rig.commands(MsgType::IDLE);
  rig.run_until([&] { return rig.running(); }, 10000, 5);
  ASSERT_TRUE(rig.running());
  EXPECT_EQ(rig.commands(MsgType::RX_ON), rx_ons) << "the next wait restores receive, not the resync";
  EXPECT_EQ(rig.commands(MsgType::IDLE), idles);
}

// === Trace ===

TEST(RadioBridleLink, TraceLinesRecordEveryMessageInBothDirections) {
  Rig rig;
  rig.radio.set_trace(true);
  rig.radio.set_mode_rx();
  rig.radio.set_mode_rx();
  rig.radio.read_rssi();
  ASSERT_GE(rig.radio.trace_lines.size(), 4u);
  size_t out = 0;
  size_t in = 0;
  for (const std::string &line : rig.radio.trace_lines) {
    EXPECT_EQ(line.rfind("[bridle] ", 0), 0u) << line;
    out += line.rfind("[bridle] > ", 0) == 0;
    in += line.rfind("[bridle] < ", 0) == 0;
  }
  EXPECT_EQ(out, 3u) << "rx_on twice and the get_rssi";
  EXPECT_EQ(in, 3u) << "the two acks and the rssi reply";
  // The rssi reply is vector `rssi`: its trace line carries the vector's type byte.
  char type_hex[8];
  std::snprintf(type_hex, sizeof(type_hex), " %02x", static_cast<unsigned>(MsgType::RSSI));
  EXPECT_NE(rig.radio.trace_lines.back().find(type_hex), std::string::npos) << "the rssi message type";
}

TEST(RadioBridleLink, AMessageOver128BytesIsTracedInParts) {
  Rig rig;
  rig.radio.set_trace(true);
  rig.radio.set_mode_rx();
  rig.pass();  // the ack of the rx_on
  rig.radio.trace_lines.clear();
  rig.cp.inject_log(LogLevel::INFO, std::string(140, 'x'));
  rig.pass();
  ASSERT_EQ(rig.radio.trace_lines.size(), 2u);
  EXPECT_NE(rig.radio.trace_lines[0].find(" 1/2 "), std::string::npos);
  EXPECT_NE(rig.radio.trace_lines[1].find(" 2/2 "), std::string::npos);
}

TEST(RadioBridleLink, WithoutTheOptionNothingIsTraced) {
  Rig rig;
  rig.radio.set_mode_rx();
  rig.pass();
  EXPECT_TRUE(rig.radio.trace_lines.empty());
}

TEST(RadioBridleLink, TheHeartbeatsLogDroppedCounterIsKeptForTheDump) {
  Rig rig;
  rig.advance_ms(30100);
  rig.pass();
  rig.advance_ms(50);
  rig.pass();
  EXPECT_TRUE(rig.radio.have_stats());
  EXPECT_EQ(rig.radio.log_dropped(), 0u);
  rig.radio.dump_debug();
}
