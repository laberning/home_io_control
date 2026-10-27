/// @file
/// @brief The exchange engine over MockRadio and over RadioBridle on a scripted co-processor.
///
/// Part one is typed: the same assertion holds for an SPI radio and for the bridge, which is what
/// "the hub works unchanged over the bridge" means. Part two covers what MockRadio does not model
/// and the bridge has: wake-up strobes chosen per frame, and the ordering rule seen from the engine.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>

#include <gtest/gtest.h>

#include "bridle_session_runs.h"
#include "bridle_stand_in.h"
#include "exchange_engine.h"
#include "proto_commands.h"
#include "radio_stand_in.h"
#include "test_helpers.h"

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::bridle;
using esphome::test_clock::ManualClock;

namespace {

/// Builds the stand-in a typed test runs over, and what its try count is under the LONG budget.
struct OverMockRadio {
  static std::unique_ptr<RadioStandIn> make() { return std::make_unique<MockStandIn>(); }
  /// ~213 ms per LONG try on the 868 model: all three fit the budget.
  static constexpr int LONG_TRIES_IN_BUDGET = 3;
};
struct OverBridle {
  static std::unique_ptr<RadioStandIn> make() {
    auto stand_in = std::make_unique<BridleStandIn>();
    EXPECT_TRUE(stand_in->initialised()) << "the driver did not come up";
    return stand_in;
  }
  /// ~510 ms per LONG try plus the short 2.4 GHz reply window and gap: all three fit the budget.
  static constexpr int LONG_TRIES_IN_BUDGET = 3;
};

/// The 868 MHz bridge: a `gfsk_raw` radio, the same coding as the SPI radios, the same airtime.
struct OverBridle868 {
  static std::unique_ptr<RadioStandIn> make() {
    auto stand_in = std::make_unique<BridleStandIn868>();
    EXPECT_TRUE(stand_in->initialised()) << "the driver did not come up";
    return stand_in;
  }
  /// ~219 ms per LONG try (the SX1262's own air time plus the command on the UART): all three fit the budget.
  static constexpr int LONG_TRIES_IN_BUDGET = 3;
};

/// A hub-side engine over a stand-in, on a manual clock.
template<class Kind> struct Rig {
  ManualClock clock;
  std::unique_ptr<RadioStandIn> radio;
  RadioDriver *driver;
  TuningConfig tuning;
  ExchangeEngine engine;

  // The stand-in is made by a factory so that it comes after the clock: it needs the manual clock.
  explicit Rig(std::function<std::unique_ptr<RadioStandIn>()> make = &Kind::make)
      : radio(make()), driver(&radio->driver()), engine(&driver, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning) {
    radio->set_model_tx_airtime(true);
  }
  void reply(const IoFrame &frame) { radio->queue_rx(test::make_rx_packet(frame)); }
  void challenge(const uint8_t bytes[6]) { reply(test::build_challenge(test::DST_ID, test::OWN_ID, bytes)); }
  void final_reply() { reply(test::build_status_response(test::DST_ID, test::OWN_ID)); }
};

const uint8_t CHALLENGE[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

}  // namespace

template<class Kind> class ExchangeOverRadio : public ::testing::Test {};
using RadioKinds = ::testing::Types<OverMockRadio, OverBridle, OverBridle868>;
TYPED_TEST_SUITE(ExchangeOverRadio, RadioKinds);

// === Rules both stand-ins must satisfy ===

TYPED_TEST(ExchangeOverRadio, ExecuteWithChallengeAndResponseEndsWithTheStatusReply) {
  Rig<TypeParam> rig;
  rig.challenge(CHALLENGE);
  rig.final_reply();
  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, false, 100);
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  const auto sent = rig.radio->sent_frames();
  ASSERT_EQ(sent.size(), 2u) << "the request, then the answer to the challenge";
  IoFrame auth{};
  ASSERT_TRUE(parse(sent[1].data(), static_cast<uint8_t>(sent[1].size()), auth));
  EXPECT_EQ(auth.cmd, CMD_CHALLENGE_RESP);
  EXPECT_EQ(response.cmd, CMD_PRIVATE_RESP);
  for (const RadioTxConfig &cfg : rig.radio->tx_configs())
    EXPECT_EQ(cfg.freq_hz, FREQ_CH2);
}

TYPED_TEST(ExchangeOverRadio, AStatusPollNeedsNoAuthentication) {
  Rig<TypeParam> rig;
  rig.final_reply();
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, false);
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  EXPECT_EQ(rig.radio->sent_frames().size(), 1u);
}

TYPED_TEST(ExchangeOverRadio, ASilentFirstTryIsRetriedOnceAndTheSameFrameGoesOutAgain) {
  Rig<TypeParam> rig;
  rig.radio->queue_rx_silence(1);
  rig.final_reply();
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, false);
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  const auto sent = rig.radio->sent_frames();
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0], sent[1]);
}

TYPED_TEST(ExchangeOverRadio, ABusyChannelIsWaitedOutOnceAndTheFrameGoesOutChecked) {
  Rig<TypeParam> rig;
  rig.radio->queue_channel_busy(-50, 1);  // above both bands' thresholds, then the channel clears
  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, false, 100);
  ASSERT_TRUE(rig.engine.transmit_frame(request, FREQ_CH2, SHORT_PREAMBLE));
  EXPECT_EQ(rig.radio->sent_frames().size(), 1u);
  EXPECT_EQ(rig.engine.counters().lbt_retries, 1u);
  ASSERT_EQ(rig.radio->tx_configs().size(), 1u);
  ASSERT_TRUE(rig.radio->tx_configs()[0].cca_threshold_dbm.has_value());
  EXPECT_EQ(*rig.radio->tx_configs()[0].cca_threshold_dbm, rig.driver->lbt_threshold_dbm(rig.tuning))
      << "the check uses the threshold of the radio's own band";
}

TYPED_TEST(ExchangeOverRadio, ALowPowerDeviceThatNeverAnswersEndsInsideTheExchangeBudget) {
  Rig<TypeParam> rig;
  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, /*low_power=*/true, 100);
  IoFrame response{};
  uint32_t const start = esphome::test_clock::peek_ms();
  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::FAILED);
  uint32_t const blocked_ms = esphome::test_clock::peek_ms() - start;

  EXPECT_EQ(rig.radio->send_count(), TypeParam::LONG_TRIES_IN_BUDGET);
  EXPECT_LT(blocked_ms, rig.tuning.exchange_total_budget_ms) << "the whole exchange stays inside its budget";
  for (const RadioTxConfig &cfg : rig.radio->tx_configs())
    EXPECT_EQ(cfg.wake, TxWake::LONG);
}

// === Rules only the bridge has ===

namespace {

/// The `tx` commands the co-processor received, decoded (bodies view the recorded bytes).
struct RecordedTxs {
  std::vector<FakeCoprocessor::RecordedCommand> commands;
  std::vector<Tx> decoded;
  explicit RecordedTxs(FakeCoprocessor &cp) {
    for (const FakeCoprocessor::RecordedCommand *c : cp.commands_of(MsgType::TX))
      commands.push_back(*c);
    Message m;
    for (const auto &c : commands) {
      EXPECT_EQ(decode_message(c.message.data(), c.message.size(), m), DecodeStatus::OK);
      decoded.push_back(std::get<Tx>(m.body));
    }
  }
};

}  // namespace

// Source of the session fixture bridle_exchange_execute_challenge (tests/corpus/sessions/).
TEST(ExchangeOverBridle, ExecuteWithChallengeIsAnsweredAndTheStatusReplyEndsIt) {
  Rig<OverBridle> rig;
  rig.challenge(CHALLENGE);
  rig.final_reply();
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(bridle_runs::execute_request(false), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);
}

// The challenge answer goes out at once on 2.4 GHz too, even next to a busy channel: the device listens
// for it on this channel for only a few tens of ms, and the request that opened the exchange was checked.
TEST(ExchangeOverBridle, TheChallengeAnswerGoesOutWithoutAChannelCheckWhileTheRequestHadOne) {
  Rig<OverBridle> rig;
  auto &bridge = static_cast<BridleStandIn &>(*rig.radio);
  bridge.coprocessor().queue_channel_level_dbm(-100);  // the request's check finds the channel clear,
  bridge.coprocessor().set_channel_level_dbm(-40);     // every later check finds it busy
  rig.challenge(CHALLENGE);
  rig.final_reply();
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(bridle_runs::execute_request(false), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  RecordedTxs txs(bridge.coprocessor());
  ASSERT_EQ(txs.decoded.size(), 2u) << "the request, then the answer to the challenge";
  EXPECT_TRUE(txs.decoded[0].cca) << "the request opens the exchange: checked";
  EXPECT_EQ(txs.decoded[0].cca_threshold_dbm, rig.driver->lbt_threshold_dbm(rig.tuning));
  EXPECT_FALSE(txs.decoded[1].cca) << "the challenge answer continues the exchange: never checked";
  EXPECT_EQ(rig.engine.counters().lbt_retries, 0u);
}

// Source of the session fixture bridle_exchange_long_strobe.
TEST(ExchangeOverBridle, TheStartFrameToALowPowerDeviceGetsTheLongTrainAndItsContinuationsNone) {
  Rig<OverBridle> rig;
  rig.challenge(CHALLENGE);
  rig.final_reply();
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(bridle_runs::execute_request(true), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  auto &bridge = static_cast<BridleStandIn &>(*rig.radio);
  RecordedTxs txs(bridge.coprocessor());
  ASSERT_EQ(txs.decoded.size(), 2u);
  EXPECT_TRUE(txs.decoded[0].has_strobe);
  EXPECT_EQ(txs.decoded[0].strobe.count, 510);
  EXPECT_FALSE(txs.decoded[1].has_strobe) << "the challenge response continues an exchange: no wake-up";
}

// Source of the session fixture bridle_868_exchange_execute_challenge.
TEST(ExchangeOverBridle868, ExecuteWithChallengeIsAnsweredAndTheStatusReplyEndsIt) {
  Rig<OverBridle868> rig;
  rig.challenge(CHALLENGE);
  rig.final_reply();
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(bridle_runs::execute_request(false), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  EXPECT_EQ(rig.radio->send_count(), 2) << "the request and the answer to the challenge";
}

// Source of the session fixture bridle_868_exchange_long_preamble: no strobe on this band, the wake-up is
// the preamble of the first try.
TEST(ExchangeOverBridle868, TheStartFrameToALowPowerDeviceGoesOutBehindTheLongPreambleOnly) {
  Rig<OverBridle868> rig;
  rig.challenge(CHALLENGE);
  rig.final_reply();
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(bridle_runs::execute_request(true), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  const auto &configs = rig.radio->tx_configs();
  ASSERT_EQ(configs.size(), 2u);
  EXPECT_EQ(configs[0].wake, TxWake::LONG);
  EXPECT_EQ(configs[0].preamble_len, 1024);
  EXPECT_NE(configs[1].wake, TxWake::LONG) << "the challenge answer continues an exchange: no wake-up";
}

TEST(ExchangeOverBridle, AKeyExtractionBroadcastReplyIsAShortTrainNotALongOne) {
  Rig<OverBridle> rig;
  IoFrame frame{};
  create_execute_position(frame, test::OWN_ID, test::DST_ID, /*low_power=*/false, 100);  // a START frame
  ASSERT_TRUE(rig.engine.transmit_frame(frame, FREQ_CH2, COLD_BROADCAST_REPLY_PREAMBLE));
  auto &bridge = static_cast<BridleStandIn &>(*rig.radio);
  RecordedTxs txs(bridge.coprocessor());
  ASSERT_EQ(txs.decoded.size(), 1u);
  EXPECT_EQ(rig.radio->tx_configs()[0].wake, TxWake::SHORT);
  ASSERT_TRUE(txs.decoded[0].has_strobe);
  EXPECT_EQ(txs.decoded[0].strobe.count, 8) << "an 80-byte preamble is no wake-up burst";
}

TEST(ExchangeOverBridle, AnAnswerThatArrivedBeforeOurTxDoneIsNotTakenForTheReply) {
  // The co-processor here reports a frame during our own transmission on purpose, which the trace
  // lint would (rightly) flag: this test writes no trace.
  Rig<OverBridle> rig([] { return std::make_unique<BridleStandIn>(/*trace_to_file=*/false); });
  auto &bridge = static_cast<BridleStandIn &>(*rig.radio);
  bridge.bridle().set_mode_rx();
  // A stale final reply lands while the request is on the air; taken for the answer it would end
  // the exchange without any challenge. The real challenge and reply follow the transmission.
  IoFrame stale = test::build_status_response(test::DST_ID, test::OWN_ID);
  RadioRxPacket stale_packet = test::make_rx_packet(stale);
  {
    std::vector<uint8_t> psdu(IO2G4_PSDU_HEADER, IO2G4_PSDU_HEADER + 3);
    psdu.insert(psdu.end(), stale_packet.data, stale_packet.data + stale_packet.len);
    psdu.push_back(0xAA);
    psdu.push_back(0xBB);
    bridge.coprocessor().inject_rx(2450000000u, psdu, CrcState::OK, 200);
  }
  rig.challenge(CHALLENGE);
  rig.final_reply();

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, false, 100);
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  EXPECT_EQ(rig.radio->sent_frames().size(), 2u) << "the exchange went through the challenge";
  EXPECT_EQ(bridge.bridle().dropped_before_tx_done(), 1u);
}

// The longest wake-up the hub can be asked for: a 4096-byte preamble, 859 ms of air per try. A third try does
// not fit the exchange budget, so the engine sends two, over the bridge exactly as over an SPI radio. (The engine
// counts a try's air time against the budget but not the reply window that follows it, so the second try ends up to
// one window after `exchange_total_budget_ms`: a property of the engine, the same on every radio.)
template<class Kind> void long_wake_4096() {
  Rig<Kind> rig;
  rig.tuning.low_power_wake_preamble = 4096;
  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, /*low_power=*/true, 100);
  IoFrame response{};
  uint32_t const start = esphome::test_clock::peek_ms();
  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.radio->send_count(), 2) << "a third try of 859 ms does not fit what is left of the budget";
  const auto times = rig.radio->send_times_ms();
  ASSERT_EQ(times.size(), 2u);
  EXPECT_GE(times[1] - times[0], 859u) << "the second try starts after the first one's air time";
  EXPECT_LE(esphome::test_clock::peek_ms() - start,
            rig.tuning.exchange_total_budget_ms + rig.tuning.exchange_start_response_wait_ms);
  for (const RadioTxConfig &cfg : rig.radio->tx_configs())
    EXPECT_EQ(cfg.preamble_len, 4096);
}

TEST(ExchangeOverBridle868, ALongWakeUpPreambleOf4096BytesGetsOnlyTheTriesThatFitTheBudget) {
  long_wake_4096<OverBridle868>();
}
TEST(ExchangeOverMockRadio, ALongWakeUpPreambleOf4096BytesGetsOnlyTheTriesThatFitTheBudget) {
  long_wake_4096<OverMockRadio>();
}

// Three LONG tries of 1024 bytes (the default wake-up) fit, with the command's UART time on top of the air time.
TEST(ExchangeOverBridle868, ThreeLongTriesOf1024BytesFitTheBudgetAndEachTakesItsModelledAirTime) {
  Rig<OverBridle868> rig;
  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, /*low_power=*/true, 100);
  IoFrame response{};
  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::FAILED);
  ASSERT_EQ(rig.radio->send_count(), 3);
  const auto times = rig.radio->send_times_ms();
  for (size_t i = 1; i < times.size(); i++)
    EXPECT_GE(times[i] - times[i - 1], 213u) << "a 1024-byte preamble alone is 213 ms on air";
}

// === Trace files ===

TEST(ExchangeOverBridle, WithATraceDirectoryTheStandInWritesItsTraceWhenItGoesAway) {
  namespace fs = std::filesystem;
  fs::path const dir = fs::temp_directory_path() / "bridle_driver_trace_test";
  fs::remove_all(dir);
  ASSERT_EQ(setenv("HIOC_BRIDLE_TRACE_DIR", dir.c_str(), 1), 0);
  {
    Rig<OverBridle> rig;
    rig.final_reply();
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, false);
    IoFrame response{};
    ASSERT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  }
  unsetenv("HIOC_BRIDLE_TRACE_DIR");

  fs::path const file = dir / "ExchangeOverBridle.WithATraceDirectoryTheStandInWritesItsTraceWhenItGoesAway.log";
  ASSERT_TRUE(fs::exists(file));
  std::ifstream in(file);
  std::string line;
  size_t sent = 0;
  size_t received = 0;
  while (std::getline(in, line)) {
    EXPECT_EQ(line.rfind("[bridle] ", 0), 0u);
    sent += line.rfind("[bridle] > ", 0) == 0;
    received += line.rfind("[bridle] < ", 0) == 0;
  }
  EXPECT_GE(sent, 3u) << "the configuration and the tx";
  EXPECT_GE(received, 3u) << "the ready, the acknowledgements and the reply";
  fs::remove_all(dir);
}

TEST(ExchangeOverBridle, WithoutATraceDirectoryNothingIsTracedOrWritten) {
  unsetenv("HIOC_BRIDLE_TRACE_DIR");
  Rig<OverBridle> rig;
  auto &bridge = static_cast<BridleStandIn &>(*rig.radio);
  EXPECT_TRUE(bridge.bridle().trace_lines.empty());
}

// === Retry policy: the band decides the channel, the window and the gap ===

// === First try away from the hub's own Wi-Fi ===

TEST(ExchangeWifiAvoidance, TheFirstTryLeavesAChannelInsideTheWifiGuardAndTheRetriesCoverTheRest) {
  Rig<OverBridle> rig;
  rig.engine.set_avoid_frequency_provider([] { return decisions::wifi_channel_centre_hz(8); });  // 2447 MHz
  rig.radio->queue_rx_silence(2);  // deaf on the first two tries
  rig.final_reply();
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, false);
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  const auto &configs = rig.radio->tx_configs();
  ASSERT_EQ(configs.size(), 3u);
  EXPECT_EQ(configs[0].freq_hz, FREQ_CH3) << "the default 2450 MHz is inside the Wi-Fi guard";
  EXPECT_EQ(configs[1].freq_hz, FREQ_CH1) << "the retries rotate from the channel try 0 used";
  EXPECT_EQ(configs[2].freq_hz, FREQ_CH2) << "so the three tries cover all three channels";
}

TEST(ExchangeWifiAvoidance, ALastGoodChannelOutsideTheGuardIsKept) {
  Rig<OverBridle> rig;
  rig.engine.set_avoid_frequency_provider([] { return decisions::wifi_channel_centre_hz(1); });  // 2412 MHz
  rig.final_reply();
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, false);
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  EXPECT_EQ(rig.radio->tx_configs()[0].freq_hz, FREQ_CH2);
}

TEST(ExchangeWifiAvoidance, NoWifiOrNoProviderKeepsTheDefaultChannel) {
  for (const bool with_provider : {false, true}) {
    Rig<OverBridle> rig;
    if (with_provider)
      rig.engine.set_avoid_frequency_provider([] { return decisions::wifi_channel_centre_hz(0); });
    rig.final_reply();
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, false);
    IoFrame response{};
    ASSERT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);
    EXPECT_EQ(rig.radio->tx_configs()[0].freq_hz, FREQ_CH2);
  }
}

TEST(ExchangeWifiAvoidance, A868MhzBridgeIsUnaffected) {
  Rig<OverBridle868> rig;
  rig.engine.set_avoid_frequency_provider([] { return decisions::wifi_channel_centre_hz(8); });
  rig.final_reply();
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, false);
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  EXPECT_EQ(rig.radio->tx_configs()[0].freq_hz, FREQ_CH2);
}

TEST(ExchangeRetryPolicy, A2g4RetryMovesToAnotherChannelAndTheNextExchangeStartsWhereItWasAnswered) {
  Rig<OverBridle> rig;
  rig.radio->queue_rx_silence(1);  // deaf on the first channel
  rig.final_reply();
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, false);
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  const auto &configs = rig.radio->tx_configs();
  ASSERT_EQ(configs.size(), 2u);
  EXPECT_EQ(configs[0].freq_hz, FREQ_CH2) << "the first exchange starts on the default channel";
  EXPECT_EQ(configs[1].freq_hz, FREQ_CH3) << "the retry moves on";
  EXPECT_EQ(rig.engine.last_answered_freq(), FREQ_CH3);

  rig.final_reply();
  ASSERT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  ASSERT_EQ(configs.size(), 3u);
  EXPECT_EQ(configs[2].freq_hz, FREQ_CH3) << "the next exchange starts on the channel that last carried an answer";
}

TEST(ExchangeRetryPolicy, EveryFrameOfATryGoesOutOnThatTrysChannel) {
  Rig<OverBridle> rig;
  rig.radio->queue_rx_silence(1);
  rig.challenge(CHALLENGE);
  rig.final_reply();
  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, false, 100);
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  const auto &configs = rig.radio->tx_configs();
  ASSERT_EQ(configs.size(), 3u) << "request, retried request, answer to the challenge";
  EXPECT_EQ(configs[0].freq_hz, FREQ_CH2);
  EXPECT_EQ(configs[1].freq_hz, FREQ_CH3);
  EXPECT_EQ(configs[2].freq_hz, FREQ_CH3) << "the 0x3D follows the request it answers";
}

TEST(ExchangeRetryPolicy, ThreeTries2g4WalkAllThreeChannels) {
  Rig<OverBridle> rig;
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, false);
  IoFrame response{};
  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2), ExchangeOutcome::FAILED);
  const auto &configs = rig.radio->tx_configs();
  ASSERT_EQ(configs.size(), 3u);
  EXPECT_EQ(configs[0].freq_hz, FREQ_CH2);
  EXPECT_EQ(configs[1].freq_hz, FREQ_CH3);
  EXPECT_EQ(configs[2].freq_hz, FREQ_CH1);
}

TEST(ExchangeRetryPolicy, The2g4ReplyWindowIsTheBandsOwn) {
  Rig<OverBridle> rig;
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, false);
  IoFrame response{};
  rig.engine.send_and_receive(request, response, FREQ_CH2, 1);
  ASSERT_FALSE(rig.radio->wait_timeouts().empty());
  EXPECT_EQ(rig.radio->wait_timeouts().front(), rig.tuning.io2g4_exchange_start_response_wait_ms);
}

TEST(ExchangeRetryPolicy, The868BridgeKeepsTheHubsReplyWindowAndChannel) {
  Rig<OverBridle868> rig;
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, false);
  IoFrame response{};
  rig.engine.send_and_receive(request, response, FREQ_CH2);
  ASSERT_FALSE(rig.radio->wait_timeouts().empty());
  EXPECT_EQ(rig.radio->wait_timeouts().front(), rig.tuning.exchange_start_response_wait_ms);
  for (const RadioTxConfig &cfg : rig.radio->tx_configs())
    EXPECT_EQ(cfg.freq_hz, FREQ_CH2);
}

TEST(ExchangeRetryPolicy, TheMockRadioKeepsTheChannelOnEveryTry) {
  Rig<OverMockRadio> rig;
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, false);
  IoFrame response{};
  rig.engine.send_and_receive(request, response, FREQ_CH2);
  for (const RadioTxConfig &cfg : rig.radio->tx_configs())
    EXPECT_EQ(cfg.freq_hz, FREQ_CH2);
}
