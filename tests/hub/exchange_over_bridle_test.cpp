/// @file
/// @brief The exchange engine over MockRadio and over RadioBridle on a scripted co-processor.
///
/// Part one is typed: the same assertion holds for an SPI radio and for the bridge, which is what
/// "the hub works unchanged over the bridge" means. Part two covers what MockRadio does not model and the
/// bridge has: the wake-up train chosen per frame, the clear-channel check seen from the co-processor, the time
/// a train and the UART round trips take, the Wi-Fi guard on the 2.4 GHz band and the trace's silence while the
/// hub waits for a reply. The band's rules themselves (windows, gap, channels) are tested over an invented band in
/// exchange_band_test.cpp and exchange_transmit_test.cpp; these tests show that the real bands, through the real
/// driver, give the engine what it needs.

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bridle_session_runs.h"
#include "bridle_stand_in.h"
#include "exchange_engine.h"
#include "exchange_fixture.h"
#include "fake_byte_stream.h"
#include "fake_coprocessor.h"
#include "log_frame.h"
#include "proto_commands.h"
#include "radio_bridle.h"
#include "radio_io2g4_phy.h"
#include "radio_stand_in.h"
#include "support/log_capture.h"
#include "test_helpers.h"
#include "testable_radio_bridle.h"

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::bridle;
using esphome::test_clock::ManualClock;

namespace {

/// Builds the stand-in a typed test runs over, and the figures that differ between the kinds.
struct OverMockRadio {
  static std::unique_ptr<RadioStandIn> make() { return std::make_unique<MockStandIn>(); }
};
struct OverBridle {
  static std::unique_ptr<RadioStandIn> make() {
    auto stand_in = std::make_unique<BridleStandIn>();
    EXPECT_TRUE(stand_in->initialised()) << "the driver did not come up";
    return stand_in;
  }
};

/// The 868 MHz bridge: a `gfsk_raw` radio, the same coding as the SPI radios, the same airtime.
struct OverBridle868 {
  static std::unique_ptr<RadioStandIn> make() {
    auto stand_in = std::make_unique<BridleStandIn868>();
    EXPECT_TRUE(stand_in->initialised()) << "the driver did not come up";
    return stand_in;
  }
};

/// A hub-side engine over a stand-in, on a manual clock. The engine's random source is scripted: the values are
/// consumed in order, then 0 (the first channel of a draw is channel 2).
template<class Kind> struct Rig {
  ManualClock clock;
  std::unique_ptr<RadioStandIn> radio;
  RadioDriver *driver;
  TuningConfig tuning;
  ExchangeEngine engine;
  std::deque<uint32_t> random_values;

  // The stand-in is made by a factory so that it comes after the clock: it needs the manual clock.
  explicit Rig(std::function<std::unique_ptr<RadioStandIn>()> make = &Kind::make)
      : radio(make()), driver(&radio->driver()), engine(&driver, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning) {
    radio->set_model_tx_airtime(true);
    engine.set_random_source([this]() -> uint32_t {
      if (random_values.empty())
        return 0;
      const uint32_t value = random_values.front();
      random_values.pop_front();
      return value;
    });
  }
  void reply(const IoFrame &frame) { radio->queue_rx(test::make_rx_packet(frame)); }
  void challenge(const uint8_t bytes[6]) { reply(exchange_test::build_challenge(test::DST_ID, test::OWN_ID, bytes)); }
  void final_reply() { reply(exchange_test::build_status_response(test::DST_ID, test::OWN_ID)); }
  /// The channel of every frame sent, in order.
  std::vector<uint32_t> sent_channels() const {
    std::vector<uint32_t> channels;
    for (const RadioTxConfig &config : radio->tx_configs())
      channels.push_back(config.freq_hz);
    return channels;
  }
};

const uint8_t CHALLENGE[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

/// The EXECUTE the exchange tests send, the one the session fixtures were recorded with.
using bridle_runs::execute_request;

IoFrame status_read(bool low_power = false) {
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, low_power);
  return request;
}

using Channels = std::vector<uint32_t>;

/// The wire bytes of a frame.
std::vector<uint8_t> frame_bytes(const IoFrame &frame) {
  uint8_t buf[FRAME_MAX_WIRE_SIZE];
  const uint8_t len = serialize(frame, buf, sizeof(buf));
  return {buf, buf + len};
}

}  // namespace

template<class Kind> class ExchangeOverRadio : public ::testing::Test {};
using RadioKinds = ::testing::Types<OverMockRadio, OverBridle, OverBridle868>;
TYPED_TEST_SUITE(ExchangeOverRadio, RadioKinds);

// === Rules both stand-ins must satisfy ===

TYPED_TEST(ExchangeOverRadio, ExecuteWithChallengeAndResponseEndsWithTheStatusReply) {
  Rig<TypeParam> rig;
  rig.challenge(CHALLENGE);
  rig.final_reply();
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(execute_request(false), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  const auto sent = rig.radio->sent_frames();
  ASSERT_EQ(sent.size(), 2u) << "the request, then the answer to the challenge";
  IoFrame auth{};
  ASSERT_TRUE(parse(sent[1].data(), static_cast<uint8_t>(sent[1].size()), auth));
  EXPECT_EQ(auth.cmd, CMD_CHALLENGE_RESP);
  EXPECT_EQ(response.cmd, CMD_PRIVATE_RESP);
  const Channels channels = rig.sent_channels();
  ASSERT_EQ(channels.size(), 2u);
  EXPECT_EQ(channels[0], FREQ_CH2) << "the random source's first choice";
  EXPECT_EQ(channels[1], channels[0]) << "the answer goes out on the channel of the request it answers";
}

TYPED_TEST(ExchangeOverRadio, AStatusPollNeedsNoAuthentication) {
  Rig<TypeParam> rig;
  rig.final_reply();
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(status_read(), response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  EXPECT_EQ(rig.radio->sent_frames().size(), 1u);
}

TYPED_TEST(ExchangeOverRadio, ASilentFirstTryIsRetriedOnAnotherChannelAndTheSameFrameGoesOutAgain) {
  Rig<TypeParam> rig;
  rig.radio->queue_rx_silence(1);
  rig.final_reply();
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(status_read(), response, FREQ_CH2, ExchangeOptions{3}),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  const auto sent = rig.radio->sent_frames();
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0], sent[1]);
  const Channels channels = rig.sent_channels();
  ASSERT_EQ(channels.size(), 2u);
  EXPECT_NE(channels[0], channels[1]) << "a retry uses another channel than the try before it";
}

TYPED_TEST(ExchangeOverRadio, EveryTryOfASilentRequestUsesAnotherChannelThanTheOneBefore) {
  Rig<TypeParam> rig;
  rig.random_values = {0, 0, 1};  // the draws among the channels each try may take
  IoFrame response{};
  EXPECT_EQ(rig.engine.send_and_receive(status_read(), response, FREQ_CH2, ExchangeOptions{3}),
            ExchangeOutcome::FAILED);
  const Channels channels = rig.sent_channels();
  ASSERT_EQ(channels.size(), 3u);
  EXPECT_NE(channels[0], channels[1]);
  EXPECT_NE(channels[1], channels[2]);
  EXPECT_EQ(channels, (Channels{FREQ_CH2, FREQ_CH1, FREQ_CH3}));
}

TYPED_TEST(ExchangeOverRadio, AFrameOfASessionFollowsTheRequestsChannelWhateverTheRandomSource) {
  Rig<TypeParam> rig;
  rig.random_values = {1};  // the request draws the second channel of the order: channel 1
  rig.challenge(CHALLENGE);
  rig.final_reply();
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(execute_request(false), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH1, FREQ_CH1}));
}

TYPED_TEST(ExchangeOverRadio, ABusyChannelIsWaitedOutOnceAndTheFrameGoesOutChecked) {
  Rig<TypeParam> rig;
  rig.radio->queue_channel_busy(-50, 1);  // above both bands' limits, then the channel clears
  ASSERT_TRUE(rig.engine.transmit_frame(execute_request(false), FREQ_CH2, SHORT_PREAMBLE));
  EXPECT_EQ(rig.radio->sent_frames().size(), 1u);
  EXPECT_EQ(rig.engine.counters().lbt_retries, 1u);
  ASSERT_EQ(rig.radio->tx_configs().size(), 1u);
  ASSERT_TRUE(rig.radio->tx_configs()[0].cca_threshold_dbm.has_value());
  EXPECT_EQ(*rig.radio->tx_configs()[0].cca_threshold_dbm, rig.engine.busy_limit_dbm())
      << "the check uses the limit of the radio's own band";
  EXPECT_EQ(rig.engine.busy_limit_dbm(), rig.driver->band().busy_limit_until_quiet_level_dbm);
}

TYPED_TEST(ExchangeOverRadio, AReadsFirstReplyWindowIsTheBandsOwnUnlessTheTuningSaysOtherwise) {
  Rig<TypeParam> rig;
  IoFrame response{};
  rig.engine.send_and_receive(status_read(), response, FREQ_CH2, 1);
  ASSERT_FALSE(rig.radio->wait_timeouts().empty());
  EXPECT_EQ(rig.radio->wait_timeouts().front(), rig.driver->band().start_reply_window_ms);

  rig.tuning.exchange_start_response_wait_ms = 777;
  const size_t before = rig.radio->wait_timeouts().size();
  rig.engine.send_and_receive(status_read(), response, FREQ_CH2, 1);
  const auto &timeouts = rig.radio->wait_timeouts();
  EXPECT_NE(std::find(timeouts.begin() + static_cast<std::ptrdiff_t>(before), timeouts.end(), 777u), timeouts.end())
      << "the tuning key overrides the band";
}

TYPED_TEST(ExchangeOverRadio, ALowPowerDeviceThatNeverAnswersKeepsItsThreeTriesWhateverTheyCost) {
  Rig<TypeParam> rig;
  IoFrame response{};
  EXPECT_EQ(rig.engine.send_and_receive(execute_request(true), response, FREQ_CH2,
                                        exchange_test::read_of(decisions::ReadKind::COMMAND)),
            ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.radio->send_count(), 3) << "a wake-up exchange is bounded by its tries, not by the budget (ADR 0095)";
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

// Also the source of the session fixture bridle_exchange_execute_challenge (tests/corpus/sessions/).
// No channel check precedes a 0x3D: the device listens for the answer on the request's channel for a few tens of
// ms, and the request that opened the exchange was checked. The co-processor sees no `get_rssi` between the request's
// `tx` and the answer's, and the answer's `tx` carries no check, while the request's did.
TEST(ExchangeOverBridle, NoChannelCheckPrecedesTheChallengeAnswerWhileTheRequestHadOne) {
  Rig<OverBridle> rig;
  auto &bridge = static_cast<BridleStandIn &>(*rig.radio);
  rig.challenge(CHALLENGE);
  rig.final_reply();
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(execute_request(false), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  RecordedTxs txs(bridge.coprocessor());
  ASSERT_EQ(txs.decoded.size(), 2u) << "the request, then the answer to the challenge";
  EXPECT_TRUE(txs.decoded[0].cca) << "the request opens the exchange: checked";
  EXPECT_EQ(txs.decoded[0].cca_threshold_dbm, rig.engine.busy_limit_dbm());
  EXPECT_FALSE(txs.decoded[1].cca) << "the challenge answer continues the exchange: never checked";

  const uint64_t request_at = txs.commands[0].t_us;
  const uint64_t answer_at = txs.commands[1].t_us;
  const auto reads = bridge.coprocessor().commands_of(MsgType::GET_RSSI);
  EXPECT_GE(reads.size(), static_cast<size_t>(CHANNEL_CHECK_READINGS) - 1u) << "the request's readings";
  for (const FakeCoprocessor::RecordedCommand *read : reads) {
    EXPECT_TRUE(read->t_us < request_at || read->t_us > answer_at)
        << "a reading at " << read->t_us << " us lies between the request (" << request_at << ") and the answer ("
        << answer_at << ")";
  }
  EXPECT_EQ(rig.engine.counters().lbt_retries, 0u);
}

// A preamble the co-processor holds for its `sync_timeout_us` would keep an unforced retune waiting and the reading
// behind it refused as `radio_busy`; the channel check's forced retune drops it, so the request is checked and sent.
TEST(ExchangeOverBridle, ARequestIsCheckedAndSentWhileTheCoprocessorHoldsAPreambleOnAnotherChannel) {
  Rig<OverBridle> rig;
  auto &bridge = static_cast<BridleStandIn &>(*rig.radio);
  rig.challenge(CHALLENGE);
  rig.final_reply();
  bridge.driver().change_frequency(FREQ_CH1);
  ASSERT_TRUE(bridge.driver().run_idle_scan());
  bridge.coprocessor().hold_preamble(1000000);

  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(execute_request(false), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  EXPECT_EQ(rig.engine.counters().no_reading_attempts, 0u);
  EXPECT_EQ(rig.engine.counters().lbt_retries, 0u);
}

// A retune the co-processor refuses leaves the radio on the old channel, and the `get_rssi` right behind it reports
// that channel's level. A level is used only when the retune before it was accepted (ADR 0134), whichever caller
// asked: the channel check, the quiet-level learning of the idle hops and the ranking of a forced send.
TEST(ExchangeOverBridle, ARequestIsNotSentOnAChannelWhoseRetuneTheCoprocessorRefused) {
  Rig<OverBridle> rig;
  auto &bridge = static_cast<BridleStandIn &>(*rig.radio);
  bridge.driver().set_mode_rx();
  ASSERT_TRUE(bridge.driver().read_rssi().has_value()) << "the radio receives; its rx_on is through";
  bridge.coprocessor().set_channel_rssi_dbm(-50);  // the busy channel the radio stays on: its level is not CH2's
  bridge.coprocessor().reject_every_rx_on_to(rig.driver->band().air_hz_of(FREQ_CH2), ErrorCode::OUT_OF_RANGE);

  const ExchangeEngine::ChannelTransmit opened = rig.engine.open_session(status_read(), 8, 0);

  EXPECT_EQ(opened.status, decisions::ChannelAccessStatus::NO_READING) << "the level read was another channel's";
  EXPECT_EQ(rig.radio->send_count(), 0);
  EXPECT_EQ(rig.engine.counters().no_reading_attempts, 1u);
  EXPECT_EQ(rig.engine.counters().channel_busy_attempts, 0u) << "nobody measured the channel busy";
  EXPECT_EQ(rig.engine.counters().lbt_retries, 0u) << "the other channel's level did not count as a busy check";
}

TEST(ExchangeOverBridle, ARequestIsSentOnAChannelTheCoprocessorRetunedTo) {
  Rig<OverBridle> rig;
  auto &bridge = static_cast<BridleStandIn &>(*rig.radio);
  bridge.driver().set_mode_rx();
  bridge.coprocessor().set_channel_rssi_dbm(-120);

  EXPECT_EQ(rig.engine.open_session(status_read(), 8, 0).status, decisions::ChannelAccessStatus::SENT);
}

// The forced send ranks the channels by their readings (CH2, CH1, CH3). CH1 is the channel the co-processor refused to
// tune to: the quiet level it then reports is the level of the channel the radio stayed on, which was quieter a moment
// ago. CH1 has no reading, so it is not ranked and the request goes out on CH2, the quietest channel measured.
TEST(ExchangeOverBridle, AForcedSendDoesNotRankAChannelWhoseRetuneTheCoprocessorRefused) {
  // The checks alternate between CH2 and CH3 and never draw CH1: the value drawn for the channel of each check, and
  // between them for its back-off.
  auto start = [](auto &rig) {
    for (int check = 0; check < 200; check++) {
      rig.random_values.push_back(check % 2 == 0 ? 0 : 1);
      rig.random_values.push_back(0);
    }
    auto &bridge = static_cast<BridleStandIn &>(*rig.radio);
    bridge.driver().set_mode_rx();
    EXPECT_TRUE(bridge.driver().read_rssi().has_value()) << "the radio receives; its rx_on is through";
    bridge.coprocessor().set_channel_rssi_dbm(-50);  // every channel busy
    return &bridge;
  };
  IoFrame response{};

  // How many readings the checks take before the ranking, with nothing refused and no forced send.
  const size_t check_readings = [&] {
    Rig<OverBridle> dry;
    BridleStandIn *dry_bridge = start(dry);
    const size_t before = dry_bridge->coprocessor().commands_of(MsgType::GET_RSSI).size();
    dry.engine.send_and_receive(status_read(), response, FREQ_CH2, ExchangeOptions{1});
    EXPECT_EQ(dry.radio->send_count(), 0);
    return dry_bridge->coprocessor().commands_of(MsgType::GET_RSSI).size() - before;
  }();

  Rig<OverBridle> rig;
  rig.tuning.lbt_force_when_busy = true;
  BridleStandIn *bridge = start(rig);
  bridge->coprocessor().reject_every_rx_on_to(rig.driver->band().air_hz_of(FREQ_CH1), ErrorCode::OUT_OF_RANGE);
  for (size_t reading = 0; reading < check_readings; reading++)
    bridge->coprocessor().queue_channel_level_dbm(-50);
  bridge->coprocessor().queue_channel_level_dbm(-50);   // the ranking's reading of CH2
  bridge->coprocessor().queue_channel_level_dbm(-120);  // what the radio reports after the refused retune to CH1
  rig.engine.send_and_receive(status_read(), response, FREQ_CH2, ExchangeOptions{1});

  ASSERT_EQ(rig.sent_channels().size(), 1u) << "the request went out on the quietest channel that has a reading";
  EXPECT_EQ(rig.sent_channels()[0], FREQ_CH2);
  EXPECT_EQ(rig.engine.counters().no_reading_attempts, 0u);
}

// The idle hop's reading teaches the board's quiet level. One taken right behind a refused hop is the level of the
// channel the radio stayed on, so it teaches nothing; the same hops with every retune accepted do. The refusal comes
// late, while the reading behind it waits for its reply, so that the driver has not undone the hop's `rx_on` yet when
// it asks for the reading: only the rule that a reading needs an accepted retune keeps it out of the quiet level.
TEST(ExchangeOverBridle, TheQuietLevelIsNotLearnedFromAReadingBehindARefusedHop) {
  constexpr uint64_t READING_US = 500;  // the co-processor's time to read
  // After the next hop has started its reading, before that reading's reply (replies keep command order).
  constexpr uint64_t REFUSAL_DELAY_US = HOP_TIME_US + 1 + (READING_US / 2);
  auto learned_limit_dbm = [](bool refuse) {
    Rig<OverBridle> rig;
    auto &bridge = static_cast<BridleStandIn &>(*rig.radio);
    bridge.coprocessor().set_channel_rssi_dbm(-113);
    bridge.coprocessor().set_rssi_reply_delay_us(READING_US);
    rig.engine.reset_hop_timestamp();
    const int blocks = static_cast<int>(QUIET_LEVEL_MIN_READINGS) * 3 * 2;
    auto idle_hop = [&rig] {
      esphome::test_clock::advance_us(HOP_TIME_US + 1);
      rig.engine.maybe_hop();
    };
    for (int block = 0; block < blocks; block++) {
      bridge.driver().set_mode_rx();
      for (int hop = 1; hop < QUIET_LEVEL_READING_EVERY_N_HOPS - 1; hop++)
        idle_hop();
      if (refuse)
        bridge.coprocessor().reject_next(MsgType::RX_ON, ErrorCode::OUT_OF_RANGE, 1, 0, REFUSAL_DELAY_US);
      idle_hop();  // the hop whose retune is refused
      idle_hop();  // the hop that takes the reading right behind it
    }
    return rig.engine.busy_limit_dbm();
  };
  int16_t const unlearned = Rig<OverBridle>().engine.busy_limit_dbm();
  EXPECT_NE(learned_limit_dbm(false), unlearned) << "the same hops with every retune accepted do learn";
  EXPECT_EQ(learned_limit_dbm(true), unlearned);
}

// Source of the session fixture bridle_exchange_long_strobe.
TEST(ExchangeOverBridle, TheStartFrameToALowPowerDeviceGetsTheLongTrainAndItsContinuationsNone) {
  Rig<OverBridle> rig;
  rig.challenge(CHALLENGE);
  rig.final_reply();
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(execute_request(true), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  auto &bridge = static_cast<BridleStandIn &>(*rig.radio);
  RecordedTxs txs(bridge.coprocessor());
  ASSERT_EQ(txs.decoded.size(), 2u);
  EXPECT_TRUE(txs.decoded[0].has_strobe);
  EXPECT_EQ(txs.decoded[0].strobe.count, 510);
  EXPECT_FALSE(txs.decoded[1].has_strobe) << "the challenge response continues an exchange: no wake-up";
}

// Source of the session fixture bridle_868_exchange_execute_challenge: the exchange to an always-alive device behind
// the short preamble, the answer to the challenge a second send with no wake-up.
TEST(ExchangeOverBridle868, ExecuteWithChallengeIsAnsweredAndTheStatusReplyEndsIt) {
  Rig<OverBridle868> rig;
  rig.challenge(CHALLENGE);
  rig.final_reply();
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(execute_request(false), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  const auto &configs = rig.radio->tx_configs();
  ASSERT_EQ(configs.size(), 2u) << "the request and the answer to the challenge";
  for (const RadioTxConfig &config : configs)
    EXPECT_NE(config.wake, TxWake::LONG);
}

// Source of the session fixture bridle_868_exchange_long_preamble. No strobe on the 868 MHz band: the wake-up is the
// preamble of the first try.
TEST(ExchangeOverBridle868, TheStartFrameToALowPowerDeviceGoesOutBehindTheLongPreambleOnly) {
  Rig<OverBridle868> rig;
  rig.challenge(CHALLENGE);
  rig.final_reply();
  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(execute_request(true), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  const auto &configs = rig.radio->tx_configs();
  ASSERT_EQ(configs.size(), 2u);
  EXPECT_EQ(configs[0].wake, TxWake::LONG);
  EXPECT_EQ(configs[0].preamble_len, rig.engine.preamble_bytes_for_ms(rig.tuning.low_power_wake_ms));
  EXPECT_NE(configs[1].wake, TxWake::LONG) << "the challenge answer continues an exchange: no wake-up";
}

TEST(ExchangeOverBridle, AKeyExtractionBroadcastReplyIsAShortTrainNotALongOne) {
  Rig<OverBridle> rig;
  ASSERT_TRUE(rig.engine.transmit_frame(execute_request(false), FREQ_CH2,
                                        rig.engine.preamble_bytes_for_ms(rig.tuning.cold_broadcast_reply_preamble_ms)));
  auto &bridge = static_cast<BridleStandIn &>(*rig.radio);
  RecordedTxs txs(bridge.coprocessor());
  ASSERT_EQ(txs.decoded.size(), 1u);
  EXPECT_EQ(rig.radio->tx_configs()[0].wake, TxWake::SHORT);
  ASSERT_TRUE(txs.decoded[0].has_strobe);
  EXPECT_EQ(txs.decoded[0].strobe.count, 8) << "a cold-broadcast preamble is no wake-up burst";
}

TEST(ExchangeOverBridle, AnAnswerThatArrivedBeforeOurTxDoneIsNotTakenForTheReply) {
  // The co-processor here reports a frame during our own transmission on purpose, which the trace
  // lint would (rightly) flag: this test writes no trace.
  Rig<OverBridle> rig([] { return std::make_unique<BridleStandIn>(/*trace_to_file=*/false); });
  auto &bridge = static_cast<BridleStandIn &>(*rig.radio);
  bridge.bridle().set_mode_rx();
  // A stale final reply lands while the request is on the air; taken for the answer it would end
  // the exchange without any challenge. The real challenge and reply follow the transmission.
  RadioRxPacket stale_packet = test::make_rx_packet(exchange_test::build_status_response(test::DST_ID, test::OWN_ID));
  stale_packet.freq_hz = FREQ_CH2;
  bridge.queue_rx_timed_after_send(stale_packet, /*send_index=*/0, /*offset_us=*/200);
  rig.challenge(CHALLENGE);
  rig.final_reply();

  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(execute_request(false), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  EXPECT_EQ(rig.radio->sent_frames().size(), 2u) << "the exchange went through the challenge";
  EXPECT_EQ(bridge.bridle().dropped_before_tx_done(), 1u);
}

// === The time a try takes over the bridge counts in the engine's budget ===

// The engine's estimate of a try (tx_time_ms(), what try_fits_budget_() and the attempt plan add up) is never below
// the time the bridge really takes on the manual clock: the wake-up train, the `tx` command and its `tx_done` on the
// UART. A bridge whose train was left out of the estimate would let a try start that the budget cannot hold.
TEST(ExchangeOverBridle, TheEstimateOfATryNeverFallsShortOfTheTimeTheTrainAndTheUartTake) {
  for (const bool low_power : {false, true}) {
    Rig<OverBridle> rig;
    const IoFrame request = execute_request(low_power);
    const uint16_t preamble = rig.engine.request_preamble_for(request);
    const uint32_t estimate_ms = rig.engine.tx_time_ms(request, FREQ_CH2, preamble);

    const uint32_t start_ms = esphome::test_clock::peek_ms();
    ASSERT_TRUE(rig.engine.transmit_frame(request, FREQ_CH2, preamble));
    const uint32_t took_ms = esphome::test_clock::peek_ms() - start_ms;

    EXPECT_GE(estimate_ms, took_ms) << "low_power=" << low_power;
    if (low_power) {
      EXPECT_GE(estimate_ms, 510u) << "the train of 510 frames, one a millisecond";
      EXPECT_GE(took_ms, 510u);
    }
  }
}

// A silent read to an always-alive device over the bridge ends inside the budget its plan was cut to: the plan counts
// the `tx` command and its `tx_done` on the UART in every try, so the tries it lets in never run past the budget.
template<class Kind> void silent_read_ends_inside_its_budget() {
  Rig<Kind> rig;
  const IoFrame request = status_read();
  // A budget that holds a few tries of this band: each costs its transmit time, the engine's overhead and the window.
  const uint32_t attempt_ms = rig.engine.tx_time_ms(request, FREQ_CH2, rig.engine.request_preamble_for(request)) +
                              EXCHANGE_ATTEMPT_OVERHEAD_MS + rig.driver->band().start_reply_window_ms;
  rig.tuning.exchange_total_budget_ms = 4 * attempt_ms;
  const decisions::AttemptPlan plan = rig.engine.plan_attempts(decisions::ReadKind::SETTLING, false, request, FREQ_CH2,
                                                               rig.engine.request_preamble_for(request));
  ASSERT_GE(plan.max_attempts, 2) << "the budget holds more than one try";

  const uint32_t start_ms = esphome::test_clock::peek_ms();
  IoFrame response{};
  EXPECT_EQ(
      rig.engine.send_and_receive(request, response, FREQ_CH2, exchange_test::read_of(decisions::ReadKind::SETTLING)),
      ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.radio->send_count(), plan.max_attempts);
  EXPECT_LE(esphome::test_clock::peek_ms() - start_ms, rig.tuning.exchange_total_budget_ms);
}

TEST(ExchangeOverBridle, ASilentReadEndsInsideTheBudgetItsPlanWasCutTo) {
  silent_read_ends_inside_its_budget<OverBridle>();
}
TEST(ExchangeOverBridle868, ASilentReadEndsInsideTheBudgetItsPlanWasCutTo) {
  silent_read_ends_inside_its_budget<OverBridle868>();
}

// The default wake-up (515 ms) keeps each try's air time on the 868 MHz bridge, the command's UART time on top.
TEST(ExchangeOverBridle868, ThreeLongTriesEachTakeTheirModelledAirTime) {
  Rig<OverBridle868> rig;
  IoFrame response{};
  EXPECT_EQ(rig.engine.send_and_receive(execute_request(true), response, FREQ_CH2,
                                        exchange_test::read_of(decisions::ReadKind::COMMAND)),
            ExchangeOutcome::FAILED);
  ASSERT_EQ(rig.radio->send_count(), 3);
  const auto times = rig.radio->send_times_ms();
  for (size_t i = 1; i < times.size(); i++)
    EXPECT_GE(times[i] - times[i - 1], static_cast<uint32_t>(rig.tuning.low_power_wake_ms))
        << "a wake-up preamble alone is " << rig.tuning.low_power_wake_ms << " ms on air";
}

// The longest wake-up the key allows keeps its three tries over the bridge exactly as over an SPI radio: a wake-up
// exchange is bounded by its tries (ADR 0095), however long they are.
template<class Kind> void longest_wake_keeps_three_tries() {
  Rig<Kind> rig;
  rig.tuning.low_power_wake_ms = 1000;
  IoFrame response{};
  EXPECT_EQ(rig.engine.send_and_receive(execute_request(true), response, FREQ_CH2,
                                        exchange_test::read_of(decisions::ReadKind::COMMAND)),
            ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.radio->send_count(), 3);
  const auto times = rig.radio->send_times_ms();
  ASSERT_EQ(times.size(), 3u);
  EXPECT_GE(times[1] - times[0], 1000u) << "the second try starts after the first one's air time";
  EXPECT_GE(times[2] - times[1], 1000u);
}

TEST(ExchangeOverBridle868, TheLongestWakeUpKeepsItsThreeTries) { longest_wake_keeps_three_tries<OverBridle868>(); }
TEST(ExchangeOverMockRadio, TheLongestWakeUpKeepsItsThreeTries) { longest_wake_keeps_three_tries<OverMockRadio>(); }

// === The Wi-Fi guard on the 2.4 GHz band (ADR 0132) ===

// Wi-Fi channel 8 (2447 MHz) sits 3 MHz from the 2450 MHz io channel: no request goes out on it while the other two
// are allowed, whatever the random source draws.
TEST(ExchangeOverBridle, ARequestNeverGoesOutOnTheChannelInsideTheWifiGuardWhileAnotherIsAllowed) {
  for (const uint32_t random : {0u, 1u, 2u, 3u, 4u, 5u}) {
    Rig<OverBridle> rig;
    rig.engine.set_avoid_frequency_provider([] { return decisions::wifi_channel_centre_hz(8); });
    rig.random_values = {random, random, random};
    IoFrame response{};
    EXPECT_EQ(rig.engine.send_and_receive(status_read(), response, FREQ_CH2, ExchangeOptions{3}),
              ExchangeOutcome::FAILED);
    const Channels channels = rig.sent_channels();
    ASSERT_EQ(channels.size(), 3u) << "random=" << random;
    for (const uint32_t channel : channels)
      EXPECT_NE(channel, FREQ_CH2) << "random=" << random << ": 2450 MHz is inside the guard";
    EXPECT_NE(channels[0], channels[1]);
    EXPECT_NE(channels[1], channels[2]);
  }
}

TEST(ExchangeOverBridle, ALowPowerRequestAlsoStaysOutOfTheGuardOnABandThatWakesOnEveryChannel) {
  Rig<OverBridle> rig;
  rig.engine.set_avoid_frequency_provider([] { return decisions::wifi_channel_centre_hz(8); });
  IoFrame response{};
  EXPECT_EQ(rig.engine.send_and_receive(execute_request(true), response, FREQ_CH2,
                                        exchange_test::read_of(decisions::ReadKind::COMMAND)),
            ExchangeOutcome::FAILED);
  ASSERT_EQ(rig.sent_channels().size(), 3u);
  for (const uint32_t channel : rig.sent_channels())
    EXPECT_NE(channel, FREQ_CH2);
}

TEST(ExchangeOverBridle, WithoutWifiTheDefaultDrawIsChannel2) {
  Rig<OverBridle> rig;
  rig.engine.set_avoid_frequency_provider([] { return decisions::wifi_channel_centre_hz(0); });  // not connected: 0
  IoFrame response{};
  rig.engine.send_and_receive(status_read(), response, FREQ_CH2, 1);
  EXPECT_EQ(rig.sent_channels(), Channels{FREQ_CH2});
}

TEST(ExchangeOverBridle868, ANoWifiChannelMovesARequestOnThe868Band) {
  for (int32_t wifi_channel = 1; wifi_channel <= 14; wifi_channel++) {
    for (const uint32_t random : {0u, 1u, 2u}) {
      Rig<OverBridle868> rig;
      rig.engine.set_avoid_frequency_provider(
          [wifi_channel] { return decisions::wifi_channel_centre_hz(wifi_channel); });
      rig.random_values = {random};
      IoFrame response{};
      rig.engine.send_and_receive(status_read(), response, FREQ_CH2, 1);
      ASSERT_EQ(rig.sent_channels().size(), 1u);
      EXPECT_EQ(rig.sent_channels()[0], decisions::REQUEST_CHANNELS[random]) << wifi_channel << "/" << random;
    }
  }
}

// === The trace prints nothing while the hub waits for a reply (ADR 0111) ===

namespace {

/// The driver as the firmware builds it, tracing through its default path (the frame log's hold), under an engine.
struct TracedEngineRig {
  ManualClock clock;
  FakeByteStream stream;
  FakeCoprocessor cp;
  FakeResetLine reset;
  RadioBridle radio;
  RadioDriver *driver{&radio};
  TuningConfig tuning;
  ExchangeEngine engine;

  TracedEngineRig()
      : cp(stream),
        radio(&stream, io2g4_phy(), &reset, std::nullopt),
        engine(&driver, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning) {
    reset.on_release = [this] { cp.boot(); };
    radio.set_trace(true);
    cp.set_channel_rssi_dbm(
        -100);  // a quiet channel: the hub's checks read it, the idle radio is told to receive first
  }
};

size_t trace_lines_in(const test::LogCapture &log) {
  return static_cast<size_t>(std::count_if(log.lines().begin(), log.lines().end(),
                                           [](const auto &line) { return line.text.rfind(TRACE_MARKER, 0) == 0; }));
}

}  // namespace

// While an exchange runs, every Bridle trace line is held back: the console is not written between a challenge
// and the answer to it, where a blocked log UART would make the answer late. The lines follow when the exchange ends,
// all of them, without the overflow note of a hold that was too small.
TEST(ExchangeOverBridle, TheTracePrintsNothingBetweenAChallengeAndItsAnswerAndAllLinesFollowAtTheEnd) {
  TracedEngineRig rig;
  ASSERT_TRUE(rig.radio.init());
  test::LogCapture log(/*with_text=*/true);

  size_t lines_at_the_request = 0;
  size_t lines_at_the_answer = 0;
  size_t transmissions = 0;
  rig.cp.set_tx_observer([&](const std::vector<uint8_t> &, uint64_t, uint64_t end_us) {
    (transmissions++ == 0 ? lines_at_the_request : lines_at_the_answer) = trace_lines_in(log);
    if (transmissions == 1) {
      const IoFrame challenge = exchange_test::build_challenge(test::DST_ID, test::OWN_ID, CHALLENGE);
      rig.cp.inject_rx_frame(2450000000u, Bridge2g4::reply_payload(frame_bytes(challenge)), FcsState::OK,
                             end_us + Bridge2g4::REPLY_DELAY_US - FakeCoprocessor::now_us());
    } else {
      const IoFrame reply = exchange_test::build_status_response(test::DST_ID, test::OWN_ID);
      rig.cp.inject_rx_frame(2450000000u, Bridge2g4::reply_payload(frame_bytes(reply)), FcsState::OK,
                             end_us + Bridge2g4::REPLY_DELAY_US - FakeCoprocessor::now_us());
    }
  });

  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(execute_request(false), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  EXPECT_EQ(transmissions, 2u);
  EXPECT_EQ(lines_at_the_request, 0u);
  EXPECT_EQ(lines_at_the_answer, 0u) << "nothing printed while the challenge's answer was sent";
  EXPECT_GE(trace_lines_in(log), 8u) << "the request's, the challenge's, the answer's and the final reply's messages";
  EXPECT_LE(trace_lines_in(log), TRACE_HELD_LINES);
  EXPECT_FALSE(std::any_of(log.lines().begin(), log.lines().end(), [](const auto &line) {
    return line.text.find("hold buffer full") != std::string::npos;
  })) << "the hold took every line, the request's channel check included";
#ifdef IOHOME_FRAME_LOG
  // The frame log's TX and RX lines of the request, the challenge, the answer and the final reply share the hold.
  EXPECT_EQ(
      std::count_if(log.lines().begin(), log.lines().end(), [](const auto &line) { return line.tag == "io_frame"; }),
      4);
#endif
}

// The hold is sized for an authenticated exchange whose request the co-processor refuses once for a busy channel
// (ADR 0047): the first channel check, the refused `tx` with its `error`, the co-processor's `log` event with the
// warning that prints it, a second channel check on another channel and the exchange. All of it is held and follows
// at the end, without the overflow note.
TEST(ExchangeOverBridle, AnExchangeWhoseRequestTheCoprocessorRefusesOnceKeepsEveryHeldLine) {
  TracedEngineRig rig;
  ASSERT_TRUE(rig.radio.init());
  // Between exchanges the hub scans the band, last tuned to another channel than the request's, so the request's
  // channel check starts with a retune. The scan's `ack` is drained here, as the hub's loop does.
  rig.radio.change_frequency(FREQ_CH1);
  ASSERT_TRUE(rig.radio.run_idle_scan());
  RadioRxPacket idle{};
  EXPECT_FALSE(rig.radio.check_for_packet(idle));
  test::LogCapture log(/*with_text=*/true);

  // The hub's own readings find the channel clear; the co-processor's reading with the `tx` finds it busy. The
  // readings after that (the second channel check) find a quiet channel.
  rig.cp.set_channel_rssi_dbm(-100);
  for (uint8_t reading = 1; reading < CHANNEL_CHECK_READINGS; reading++)
    rig.cp.queue_channel_level_dbm(-100);
  rig.cp.queue_channel_level_dbm(-60);
  // The `log` event a co-processor writes with its refusal; where it falls in the hold does not change the count.
  rig.cp.inject_log(LogLevel::WARN, "tx refused: channel busy");
  size_t transmissions = 0;
  rig.cp.set_tx_observer([&](const std::vector<uint8_t> &, uint64_t, uint64_t end_us) {
    const IoFrame frame = transmissions++ == 0 ? exchange_test::build_challenge(test::DST_ID, test::OWN_ID, CHALLENGE)
                                               : exchange_test::build_status_response(test::DST_ID, test::OWN_ID);
    // A co-processor reports each frame's start before the frame, on the channel the request went out on.
    const uint64_t delay_us = end_us + Bridge2g4::REPLY_DELAY_US - FakeCoprocessor::now_us();
    rig.cp.inject_rx_start(rig.cp.freq_hz(), RxStage::SYNC, delay_us);
    rig.cp.inject_rx_frame(rig.cp.freq_hz(), Bridge2g4::reply_payload(frame_bytes(frame)), FcsState::OK, delay_us);
  });

  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(execute_request(false), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  EXPECT_EQ(rig.cp.errors_sent(ErrorCode::CHANNEL_BUSY), 1u) << "the co-processor refused the first `tx`";
  EXPECT_EQ(transmissions, 2u) << "the request on a second channel and the answer";
  const size_t coproc_lines = static_cast<size_t>(std::count_if(
      log.lines().begin(), log.lines().end(), [](const auto &line) { return line.text.rfind("[coproc] ", 0) == 0; }));
  EXPECT_EQ(coproc_lines, 1u) << "the warning that prints the co-processor's `log` event";
  EXPECT_EQ(trace_lines_in(log) + coproc_lines,
            (2 * TRACE_CHANNEL_CHECK_LINES) + TRACE_REFUSAL_LINES + TRACE_EXCHANGE_LINES)
      << "two channel checks, the refusal and the exchange";
  EXPECT_FALSE(std::any_of(log.lines().begin(), log.lines().end(), [](const auto &line) {
    return line.text.find("hold buffer full") != std::string::npos;
  })) << "the hold took every line of the refused try and of the exchange";
#ifdef IOHOME_FRAME_LOG
  EXPECT_EQ(
      std::count_if(log.lines().begin(), log.lines().end(), [](const auto &line) { return line.tag == "io_frame"; }), 4)
      << "a refused `tx` logs no frame line";
#endif
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
    IoFrame response{};
    ASSERT_EQ(rig.engine.send_and_receive(status_read(), response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);
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
