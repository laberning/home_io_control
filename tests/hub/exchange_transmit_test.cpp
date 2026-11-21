/// @file exchange_transmit_test.cpp
/// @brief Tests for the transmit path of the exchange engine: the transmit and overheard-frame observers, the wake
/// level a transmission is stamped with, listen before talk, the channel access of a request and the busy limit.

#include "hub_exchange.h"
#include "hub_core.h"
#include "oneway_transmitter.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_crypto.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <deque>
#include <vector>

#include "exchange_fixture.h"

using namespace esphome::home_io_control;
using namespace exchange_test;

// ============================================================================
// Transmit observer — transmit_frame() reports LBT deferrals and sent frames to the attached
// TransmitObserver (PairingTelemetry during pairing; any future TX accounting the same way).
// ============================================================================

namespace {

/// Records every TransmitObserver callback it receives.
struct RecordingTransmitObserver : TransmitObserver {
  struct Sent {
    uint8_t cmd;
    RadioTxConfig config;
    uint8_t wire_len;
  };
  std::vector<int16_t> lbt_defers;
  std::vector<Sent> sent;

  void on_lbt_defer(int16_t rssi_dbm) override { this->lbt_defers.push_back(rssi_dbm); }
  void on_transmit(const IoFrame &frame, const RadioTxConfig &config, uint8_t wire_len) override {
    this->sent.push_back({frame.cmd, config, wire_len});
  }
};

/// A hub whose engine talks to `radio`, with the node ID set so frames serialize.
void wire_observer_rig(TestableComponent &comp, MockRadio &radio) {
  comp.initialized_ = true;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
}

}  // namespace

TEST(Exchange, TransmitObserverSeesSentFrameWithItsTxShape) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  RecordingTransmitObserver observer;
  comp.exchange_engine_.set_transmit_observer(&observer);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(request, FREQ_CH3, TEST_LONG_PREAMBLE_BYTES));

  ASSERT_EQ(observer.sent.size(), 1u);
  EXPECT_EQ(observer.sent[0].cmd, request.cmd);
  EXPECT_EQ(observer.sent[0].config.freq_hz, FREQ_CH3);
  EXPECT_EQ(observer.sent[0].config.preamble_len, TEST_LONG_PREAMBLE_BYTES);
  // The length the observer is told is the one the radio was handed.
  ASSERT_EQ(radio.get_sent_data().size(), 1u);
  EXPECT_EQ(observer.sent[0].wire_len, radio.get_sent_data()[0].size());
  EXPECT_TRUE(observer.lbt_defers.empty());
}

TEST(Exchange, TransmitObserverSeesEachLbtDeferralWithItsRssi) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  RecordingTransmitObserver observer;
  comp.exchange_engine_.set_transmit_observer(&observer);

  // Two busy reads, then MockRadio's clear default: two deferrals, then the frame goes out.
  radio.queue_rssi(-50);
  radio.queue_rssi(-60);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(request, FREQ_CH2, SHORT_PREAMBLE));

  EXPECT_EQ(observer.lbt_defers, (std::vector<int16_t>{-50, -60}));
  EXPECT_EQ(observer.sent.size(), 1u);
}

TEST(Exchange, TransmitObserverNotToldAboutAFailedSend) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  RecordingTransmitObserver observer;
  comp.exchange_engine_.set_transmit_observer(&observer);

  radio.queue_tx_result(false);
  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  EXPECT_FALSE(comp.exchange_engine_.transmit_frame(request, FREQ_CH2, SHORT_PREAMBLE));

  EXPECT_TRUE(observer.sent.empty());
}

TEST(Exchange, DetachedTransmitObserverHearsNothing) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  RecordingTransmitObserver observer;
  comp.exchange_engine_.set_transmit_observer(&observer);
  comp.exchange_engine_.set_transmit_observer(nullptr);

  radio.queue_rssi(-50);
  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(request, FREQ_CH2, SHORT_PREAMBLE));

  EXPECT_TRUE(observer.lbt_defers.empty());
  EXPECT_TRUE(observer.sent.empty());
}

// ============================================================================
// Wake level — transmit_frame() stamps every transmission with tx_wake_for(START, preamble), so a
// radio that wakes receivers some other way than by preamble length reads the same decision.
// ============================================================================

TEST(Exchange, TransmitFrameStampsTheWakeLevel) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);

  IoFrame start{};
  create_execute_position(start, comp.node_id_, test::DST_ID, false, 100);
  ASSERT_TRUE(is_start(start));
  IoFrame continuation = start;
  continuation.ctrl0 &= static_cast<uint8_t>(~CTRL0_START);

  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(start, FREQ_CH2, MOCK_WAKE_PREAMBLE));
  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(start, FREQ_CH2, MOCK_START_PREAMBLE));
  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(continuation, FREQ_CH2, MOCK_WAKE_PREAMBLE));

  ASSERT_EQ(radio.get_tx_configs().size(), 3u);
  EXPECT_EQ(radio.get_tx_configs()[0].wake, TxWake::LONG);
  EXPECT_EQ(radio.get_tx_configs()[1].wake, TxWake::SHORT);
  EXPECT_EQ(radio.get_tx_configs()[2].wake, TxWake::NONE);
}

TEST(Exchange, TransmitFrameLabelsAWakeUpFromItsDurationOnEveryRadio) {
  // 1024 programmed bytes last 213 ms on a raw-stream radio and 267 ms on a 10-bit radio: neither is
  // a wake-up. 1440 raw-stream bytes and 1152 ten-bit bytes both last 300 ms, the threshold.
  struct Row {
    bool ten_bit;
    uint16_t bytes;
    TxWake expected;
  };
  const Row rows[] = {
      {false, 1024, TxWake::SHORT}, {true, 1024, TxWake::SHORT}, {false, 1439, TxWake::SHORT},
      {true, 1151, TxWake::SHORT},  {false, 1440, TxWake::LONG}, {true, 1152, TxWake::LONG},
  };
  for (const Row &row : rows) {
    SCOPED_TRACE(::testing::Message() << (row.ten_bit ? "10" : "8") << " bit periods per byte, " << row.bytes
                                      << " bytes");
    TestableComponent comp;
    MockRadio raw_stream;
    TenBitPreambleRadio ten_bit;
    MockRadio &radio = row.ten_bit ? static_cast<MockRadio &>(ten_bit) : raw_stream;
    wire_observer_rig(comp, radio);
    IoFrame start{};
    create_execute_position(start, comp.node_id_, test::DST_ID, false, 100);

    ASSERT_TRUE(comp.exchange_engine_.transmit_frame(start, FREQ_CH2, row.bytes));

    ASSERT_EQ(radio.get_tx_configs().size(), 1u);
    EXPECT_EQ(radio.get_tx_configs()[0].wake, row.expected);
  }
}

// ============================================================================
// Listen before talk — transmit_frame() asks the radio to send only on a clear TX channel, up to
// LBT_MAX_RETRIES times, then sends once without the check; a failed send ends it at once. This is the
// access of the senders that keep a fixed channel; a request from send_and_receive() takes a free channel.
// ============================================================================

TEST(Exchange, LbtBusyThenClearSendsOnTheSecondCheck) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  radio.queue_rssi(-50);  // busy once, then MockRadio's clear default

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(request, FREQ_CH2, SHORT_PREAMBLE));

  EXPECT_EQ(radio.get_send_count(), 1);
  EXPECT_EQ(comp.exchange_engine_.counters().lbt_retries, 1u);
  ASSERT_EQ(radio.get_tx_configs().size(), 1u);
  EXPECT_EQ(radio.get_tx_configs()[0].cca_threshold_dbm, comp.exchange_engine_.busy_limit_dbm())
      << "the frame that went out was sent with the check";
}

TEST(Exchange, LbtBusyOnEveryCheckSendsOnceWithoutIt) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  RecordingTransmitObserver observer;
  comp.exchange_engine_.set_transmit_observer(&observer);
  radio.set_rssi_default(-50);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(request, FREQ_CH2, SHORT_PREAMBLE));

  const uint8_t checks = LBT_MAX_RETRIES;
  EXPECT_EQ(comp.exchange_engine_.counters().lbt_retries, checks);
  EXPECT_EQ(observer.lbt_defers.size(), checks);
  EXPECT_EQ(radio.rssi_read_freqs().size(), checks) << "the forced send takes no reading";
  ASSERT_EQ(radio.get_tx_configs().size(), 1u);
  EXPECT_FALSE(radio.get_tx_configs()[0].cca_threshold_dbm.has_value()) << "the last send is forced";
}

TEST(Exchange, LbtFailedSendIsNotRetried) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  radio.queue_tx_result(false);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  EXPECT_FALSE(comp.exchange_engine_.transmit_frame(request, FREQ_CH2, SHORT_PREAMBLE));

  EXPECT_EQ(radio.get_send_count(), 1) << "FAILED is not CHANNEL_BUSY: no further attempt";
  EXPECT_EQ(comp.exchange_engine_.counters().lbt_retries, 0u);
}

// ============================================================================
// A one-way burst through the real send path. Every copy opens a session (CTRL0 START), so every copy gets the
// listen-before-talk check of a fixed-channel sender: up to LBT_MAX_RETRIES checks LBT_RETRY_DELAY_MS apart, then
// the copy goes out regardless. A busy channel delays a copy by at most the sum of its checks and never drops it;
// the gap of the burst (ONEWAY_BURST_INTERVAL_MS) comes on top.
// ============================================================================

namespace {

/// A 1W STOP burst through the hub's engine: the transmitter is wired as the hub wires it.
struct OneWayBurstRig {
  OneWayBurstRig() : transmitter(make_transmit(), &comp.tuning_, make_preamble_bytes()) {
    wire_observer_rig(comp, radio);
    comp.exchange_engine_.set_transmit_observer(&observer);
    radio.set_rssi_default(-120);
    esphome::test_hal::reset_delays();
    EXPECT_TRUE(create_1w_execute_command(frame, test::OWN_ID, DeviceType::VERTICAL_EXTERIOR_AWNING, CoverCommand::STOP,
                                          0x0102, test::TEST_SYSTEM_KEY));
  }

  OneWayTransmitFn make_transmit() {
    return [this](const IoFrame &f, uint32_t freq, uint16_t preamble) {
      return comp.exchange_engine_.transmit_frame(f, freq, preamble);
    };
  }
  OneWayPreambleBytesFn make_preamble_bytes() {
    return [this](float ms) { return comp.exchange_engine_.preamble_bytes_for_ms(ms); };
  }

  TestableComponent comp;
  MockRadio radio;
  RecordingTransmitObserver observer;
  OneWayTransmitter transmitter;
  IoFrame frame{};
};

/// Sum of the delays the burst waited, in ms.
uint32_t total_delay_ms() {
  uint32_t total = 0;
  for (const uint32_t ms : esphome::test_hal::recorded_delays())
    total += ms;
  return total;
}

}  // namespace

TEST(OneWayBurstSendPath, EveryCopyOpensASessionSoEveryCopyIsCheckedBeforeItGoesOut) {
  OneWayBurstRig rig;
  ASSERT_TRUE(is_start(rig.frame)) << "a 1W copy carries START";

  ASSERT_TRUE(rig.transmitter.send_burst(rig.frame, OneWayPowerClass::ALWAYS_ALIVE));

  ASSERT_EQ(rig.radio.get_tx_configs().size(), ONEWAY_BURST_REPEATS);
  for (const auto &tx_config : rig.radio.get_tx_configs()) {
    ASSERT_TRUE(tx_config.cca_threshold_dbm.has_value()) << "a free channel is sent on with the check, never without";
    EXPECT_EQ(*tx_config.cca_threshold_dbm, rig.comp.exchange_engine_.busy_limit_dbm());
  }
  EXPECT_EQ(rig.comp.exchange_engine_.counters().lbt_retries, 0u);
}

TEST(OneWayBurstSendPath, ABusyChannelBeforeTheSecondCopyDelaysThatCopyOnceAndLosesNone) {
  OneWayBurstRig rig;
  rig.radio.queue_rssi(-120);  // copy 1: free
  rig.radio.queue_rssi(-50);   // copy 2, first check: busy; the next reading is the free default

  ASSERT_TRUE(rig.transmitter.send_burst(rig.frame, OneWayPowerClass::ALWAYS_ALIVE));

  EXPECT_EQ(rig.observer.sent.size(), ONEWAY_BURST_REPEATS) << "all four copies went out";
  EXPECT_EQ(rig.comp.exchange_engine_.counters().lbt_retries, 1u);
  ASSERT_EQ(rig.observer.lbt_defers.size(), 1u);
  EXPECT_EQ(rig.observer.lbt_defers[0], -50);
  // gap, gap + one back-off before copy 2's second check, gap: the back-off sits inside the second copy's wait.
  const std::vector<uint32_t> expected_delays = {ONEWAY_BURST_INTERVAL_MS, LBT_RETRY_DELAY_MS, ONEWAY_BURST_INTERVAL_MS,
                                                 ONEWAY_BURST_INTERVAL_MS};
  EXPECT_EQ(esphome::test_hal::recorded_delays(), expected_delays);
  for (const auto &tx_config : rig.radio.get_tx_configs())
    EXPECT_TRUE(tx_config.cca_threshold_dbm.has_value()) << "the copy that went out after the back-off was checked";
}

TEST(OneWayBurstSendPath, AChannelThatStaysBusyDelaysEachCopyByAllItsChecksThenSendsItUnchecked) {
  OneWayBurstRig rig;
  rig.radio.set_rssi_default(-50);  // busy on every check

  ASSERT_TRUE(rig.transmitter.send_burst(rig.frame, OneWayPowerClass::ALWAYS_ALIVE));

  const uint32_t checks_per_copy = LBT_MAX_RETRIES;
  EXPECT_EQ(rig.observer.sent.size(), ONEWAY_BURST_REPEATS) << "the copy still goes out after the last check";
  EXPECT_EQ(rig.comp.exchange_engine_.counters().lbt_retries, checks_per_copy * ONEWAY_BURST_REPEATS);
  ASSERT_EQ(rig.radio.get_tx_configs().size(), ONEWAY_BURST_REPEATS);
  for (const auto &tx_config : rig.radio.get_tx_configs())
    EXPECT_FALSE(tx_config.cca_threshold_dbm.has_value()) << "the send after the last check is not checked again";
  // At most 25 ms per copy on top of the three gaps of the burst.
  EXPECT_EQ(total_delay_ms(), (ONEWAY_BURST_REPEATS - 1) * ONEWAY_BURST_INTERVAL_MS +
                                  ONEWAY_BURST_REPEATS * checks_per_copy * LBT_RETRY_DELAY_MS);
}

TEST(OneWayBurstSendPath, ACopyTheRadioFailsIsNotRetriedAndTheNextCopyStillGoesOut) {
  OneWayBurstRig rig;
  rig.radio.queue_tx_result(true);
  rig.radio.queue_tx_result(false);  // copy 2: the radio cannot send it

  EXPECT_TRUE(rig.transmitter.send_burst(rig.frame, OneWayPowerClass::ALWAYS_ALIVE))
      << "a partial burst counts as sent";

  EXPECT_EQ(rig.radio.get_send_count(), ONEWAY_BURST_REPEATS)
      << "one attempt per copy: a failure is not a busy channel";
  EXPECT_EQ(rig.observer.sent.size(), ONEWAY_BURST_REPEATS - 1u) << "the failed copy never reached the air";
  EXPECT_EQ(rig.comp.exchange_engine_.counters().lbt_retries, 0u);
}

TEST(OneWayBurstSendPath, ABurstOfWakeUpCopiesIsStampedWithTheLongWakeLevelAndTheFlagCopyAlone) {
  OneWayBurstRig rig;

  ASSERT_TRUE(rig.transmitter.send_burst(rig.frame, OneWayPowerClass::LOW_POWER));

  ASSERT_EQ(rig.radio.get_tx_configs().size(), ONEWAY_BURST_REPEATS);
  EXPECT_EQ(rig.radio.get_tx_configs()[0].wake, TxWake::LONG);
  EXPECT_EQ(rig.radio.get_tx_configs()[0].preamble_len, rig.comp.exchange_engine_.wake_preamble())
      << "the wake-up copy lasts low_power_wake_ms through the radio's conversion";
  for (size_t copy = 1; copy < ONEWAY_BURST_REPEATS; copy++)
    EXPECT_EQ(rig.radio.get_tx_configs()[copy].wake, TxWake::SHORT) << "copy " << copy;
}

// Only a frame that opens a session (CTRL0 START) is checked; one without START answers within a
// session and goes out at once (ADR 0045, issue #178: a single 5 ms back-off before a 0x3D lost the
// exchange on an OXIMO 50 io).
TEST(Exchange, LbtSkippedForAFrameWithoutStart) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  RecordingTransmitObserver observer;
  comp.exchange_engine_.set_transmit_observer(&observer);
  radio.set_rssi_default(-50);  // busy on every check

  IoFrame continuation{};
  create_execute_position(continuation, comp.node_id_, test::DST_ID, false, 100);
  continuation.ctrl0 &= static_cast<uint8_t>(~CTRL0_START);
  ASSERT_FALSE(is_start(continuation));
  ASSERT_TRUE(comp.exchange_engine_.transmit_frame(continuation, FREQ_CH2, SHORT_PREAMBLE));

  EXPECT_EQ(radio.get_send_count(), 1);
  EXPECT_TRUE(radio.rssi_read_freqs().empty()) << "no channel reading for a frame without START";
  EXPECT_EQ(comp.exchange_engine_.counters().lbt_retries, 0u);
  EXPECT_TRUE(observer.lbt_defers.empty());
  ASSERT_EQ(radio.get_tx_configs().size(), 1u);
  EXPECT_FALSE(radio.get_tx_configs()[0].cca_threshold_dbm.has_value());
}

TEST(Exchange, TheRequestsChannelIsCheckedButNotThe0x3DOfAnAuthenticatedExchange) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  radio.set_channel_rssi(FREQ_CH2, -50);  // the first channel is busy; the others are free

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
  uint8_t chal_data[HMAC_SIZE] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  radio.queue_rx(to_rx_packet(build_challenge(test::DST_ID, comp.node_id_, chal_data)));
  radio.queue_rx(to_rx_packet(build_status_response(test::DST_ID, comp.node_id_)));

  IoFrame response{};
  ASSERT_EQ(comp.send_and_receive_(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  EXPECT_EQ(comp.exchange_engine_.counters().lbt_retries, 1u) << "only the request's checks are counted";
  ASSERT_EQ(radio.get_sent_data().size(), 2u) << "request + 0x3D";
  IoFrame sent{};
  const auto &raw = radio.get_sent_data()[1];
  ASSERT_TRUE(parse(raw.data(), static_cast<uint8_t>(raw.size()), sent));
  EXPECT_EQ(sent.cmd, CMD_CHALLENGE_RESP);
  ASSERT_EQ(radio.get_tx_configs().size(), 2u);
  EXPECT_TRUE(radio.get_tx_configs()[0].cca_threshold_dbm.has_value());
  EXPECT_FALSE(radio.get_tx_configs()[1].cca_threshold_dbm.has_value()) << "the 0x3D is never checked";
  EXPECT_EQ(radio.get_tx_configs()[1].freq_hz, radio.get_tx_configs()[0].freq_hz)
      << "the answer goes out on the channel the request took";
}

// ============================================================================
// Overheard-frame observer: a frame the first-response or final-response wait ignores as unrelated
// is reported to the attached OverheardFrameObserver, and the exchange itself runs as without it.
// ============================================================================

namespace {

/// What MockRadio's capture-lifecycle emulation stamps on every delivered frame.
constexpr int16_t OVERHEARD_TEST_RSSI_DBM = -55;

struct RecordingOverheardObserver : OverheardFrameObserver {
  std::vector<IoFrame> frames;
  std::vector<uint32_t> times;
  std::vector<int16_t> rssis;
  void on_overheard_frame(const IoFrame &frame, const RadioRxPacket &, int16_t rssi_dbm, uint32_t now) override {
    this->frames.push_back(frame);
    this->rssis.push_back(rssi_dbm);
    this->times.push_back(now);
  }
};

/// A wall remote's CLOSE press: a 1W frame to a device class.
IoFrame remote_press_frame() {
  const uint8_t remote[NODE_ID_SIZE] = {0x9D, 0x60, 0x85};
  const uint8_t roller_class[NODE_ID_SIZE] = {0x00, 0x01, 0xBF};
  const uint8_t close[3] = {0x00, 0x00, 0xC8};
  IoFrame f{};
  init_frame(f, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  set_src(f, remote);
  set_dst(f, roller_class);
  set_cmd(f, CMD_EXECUTE, close, sizeof(close));
  return f;
}

/// What an exchange did, for comparing a run with and without an overheard frame.
struct ExchangeRun {
  ExchangeOutcome outcome;
  size_t send_count;
  std::vector<uint32_t> send_times_ms;
  uint32_t ended_ms;
  bool operator==(const ExchangeRun &o) const {
    return outcome == o.outcome && send_count == o.send_count && send_times_ms == o.send_times_ms &&
           ended_ms == o.ended_ms;
  }
};

/// One exchange where `noise` (if any) is queued at the given point: before the first reply (a direct
/// answer follows) or between the challenge and the final reply.
ExchangeRun run_with_noise(const IoFrame *noise, bool challenged, RecordingOverheardObserver &observer) {
  ResendRig rig;
  rig.radio.set_emulate_capture_lifecycle(true);
  rig.engine.set_overheard_frame_observer(&observer);
  if (challenged) {
    rig.queue_challenge();
    if (noise != nullptr)
      rig.queue(*noise);
    rig.queue_final_reply();
  } else {
    if (noise != nullptr)
      rig.queue(*noise);
    rig.queue(build_status_response(test::DST_ID, test::OWN_ID));
  }
  const ExchangeOutcome outcome = rig.send(stop_request());
  return {outcome, rig.radio.get_sent_data().size(), rig.radio.send_times_ms(), esphome::millis()};
}

}  // namespace

TEST(ExchangeOverheard, RemotePressInTheFirstResponseWaitReachesTheObserverOnce) {
  const IoFrame press = remote_press_frame();
  RecordingOverheardObserver quiet;
  RecordingOverheardObserver heard;
  const ExchangeRun without = run_with_noise(nullptr, false, quiet);
  const ExchangeRun with = run_with_noise(&press, false, heard);

  ASSERT_EQ(heard.frames.size(), 1u);
  EXPECT_EQ(heard.frames[0].cmd, CMD_EXECUTE);
  EXPECT_EQ(heard.rssis[0], OVERHEARD_TEST_RSSI_DBM) << "the observer gets the RSSI of the reception";
  EXPECT_EQ(memcmp(heard.frames[0].src, press.src, NODE_ID_SIZE), 0);
  EXPECT_TRUE(quiet.frames.empty());
  EXPECT_EQ(with.outcome, ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  EXPECT_TRUE(with == without) << "the exchange's outcome, sends and timing do not depend on the frame";
}

TEST(ExchangeOverheard, RemotePressInTheFinalResponseWaitReachesTheObserverOnce) {
  const IoFrame press = remote_press_frame();
  RecordingOverheardObserver quiet;
  RecordingOverheardObserver heard;
  const ExchangeRun without = run_with_noise(nullptr, true, quiet);
  const ExchangeRun with = run_with_noise(&press, true, heard);

  ASSERT_EQ(heard.frames.size(), 1u);
  EXPECT_EQ(heard.frames[0].cmd, CMD_EXECUTE);
  EXPECT_TRUE(quiet.frames.empty());
  EXPECT_TRUE(with == without);
}

TEST(ExchangeOverheard, ForeignTwoWayFrameAlsoReachesTheObserverBecauseTheEngineDoesNotFilter) {
  const IoFrame foreign = build_status_response(test::FOREIGN_ID, test::OWN_ID);
  RecordingOverheardObserver heard;
  const ExchangeRun run = run_with_noise(&foreign, false, heard);

  ASSERT_EQ(heard.frames.size(), 1u);
  EXPECT_EQ(heard.frames[0].cmd, CMD_PRIVATE_RESP);
  EXPECT_EQ(run.outcome, ExchangeOutcome::SUCCESS_WITH_RESPONSE);
}

TEST(ExchangeOverheard, FramesTheExchangeAcceptsAreNotReported) {
  RecordingOverheardObserver heard;
  const ExchangeRun run = run_with_noise(nullptr, true, heard);

  EXPECT_EQ(run.outcome, ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  EXPECT_TRUE(heard.frames.empty());
}

TEST(ExchangeOverheard, InboundAuthenticationWaitDoesNotReport) {
  // challenge_sender() serves a device-initiated command and the pairing key pull; its wait is
  // not an exchange wait, so a remote press heard there is not handed to the observer.
  ResendRig rig;
  RecordingOverheardObserver heard;
  rig.engine.set_overheard_frame_observer(&heard);
  rig.queue(remote_press_frame());

  IoFrame request{};
  create_execute_position(request, test::DST_ID, test::OWN_ID, false, 100);
  EXPECT_NE(rig.engine.challenge_sender(request, FREQ_CH2, test::TEST_SYSTEM_KEY),
            ExchangeEngine::ChallengeResult::VERIFIED);
  EXPECT_TRUE(heard.frames.empty());
}

// ============================================================================
// Channel access of a request (ADR 0096): a request from send_and_receive() is sent only onto a
// channel its check found free; on a busy result it moves to another channel after a random, growing
// back-off, and when its access time is spent the attempt ends as "channel busy" with nothing sent.
// ============================================================================

namespace {

/// A standalone engine on a manual clock whose random source is scripted: the values are consumed in
/// order, then 0. The draws of one attempt are the first channel, then per busy check the back-off and
/// the next channel.
struct AccessRig {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr{&radio};
  TuningConfig tuning{};
  ExchangeEngine engine{&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning};
  std::deque<uint32_t> random_values;
  RecordingTransmitObserver transmitted;
  RecordingOverheardObserver heard;

  AccessRig() {
    engine.set_random_source([this]() -> uint32_t {
      if (random_values.empty())
        return 0;
      const uint32_t value = random_values.front();
      random_values.pop_front();
      return value;
    });
    engine.set_transmit_observer(&transmitted);
    engine.set_overheard_frame_observer(&heard);
  }

  /// A status read of a mains or low-power target.
  static IoFrame status_read(bool low_power) {
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, low_power);
    return request;
  }

  /// One exchange with @p options to a target that never answers.
  ExchangeOutcome run(bool low_power, const ExchangeOptions &options) {
    IoFrame response{};
    return engine.send_and_receive(status_read(low_power), response, FREQ_CH2, options);
  }

  /// The channel of every frame sent, in order.
  std::vector<uint32_t> sent_channels() const {
    std::vector<uint32_t> channels;
    for (const auto &config : radio.get_tx_configs())
      channels.push_back(config.freq_hz);
    return channels;
  }
};

using Channels = std::vector<uint32_t>;

}  // namespace

TEST(ChannelAccess, AFreeChannelIsSentOnAtOnceAfterTheFiveReadings) {
  AccessRig rig;
  rig.run(false, ExchangeOptions{1});

  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH2}));
  EXPECT_EQ(rig.radio.rssi_read_freqs().size(), static_cast<size_t>(CHANNEL_CHECK_READINGS))
      << "four readings from the engine and the radio's own right before the first symbol";
  EXPECT_EQ(rig.engine.counters().lbt_retries, 0u);
  EXPECT_EQ(rig.engine.counters().channel_busy_attempts, 0u);
  EXPECT_TRUE(rig.radio.freq_history().empty()) << "no retune: the radio was already on the chosen channel";
}

TEST(ChannelAccess, TheFirstChannelOfARequestIsTheRandomSourcesChoice) {
  for (const uint32_t random : {0u, 1u, 2u}) {
    AccessRig rig;
    rig.random_values = {random};
    rig.run(false, ExchangeOptions{1});
    ASSERT_EQ(rig.sent_channels().size(), 1u);
    EXPECT_EQ(rig.sent_channels()[0], decisions::REQUEST_CHANNELS[random]) << "random=" << random;
  }
}

TEST(ChannelAccess, EveryAttemptOfARequestTakesADifferentChannelThanTheOneBefore) {
  AccessRig rig;
  rig.random_values = {0, 0, 1};  // attempt 1: CH2; attempts 2 and 3 draw among the two channels they may take
  rig.run(false, ExchangeOptions{3});

  const Channels sent = rig.sent_channels();
  ASSERT_EQ(sent.size(), 3u);
  EXPECT_NE(sent[0], sent[1]);
  EXPECT_NE(sent[1], sent[2]);
  EXPECT_EQ(sent, (Channels{FREQ_CH2, FREQ_CH1, FREQ_CH3})) << "the draws 0, 0 and 1 (modulo the two candidates)";
}

TEST(ChannelAccess, ABusyChannelMovesTheRequestToAnotherOneAndTheReplyIsHeardThere) {
  AccessRig rig;
  rig.radio.set_channel_rssi(FREQ_CH2, -50);  // the first channel is busy, the others are free
  rig.radio.queue_rx_silence(1);              // what the back-off hears
  const IoFrame reply = build_status_response(test::DST_ID, test::OWN_ID);
  uint8_t raw[64];
  RadioRxPacket pkt{};
  pkt.len = serialize(reply, raw, sizeof(raw));
  memcpy(pkt.data, raw, pkt.len);
  pkt.freq_hz = FREQ_CH1;
  rig.radio.queue_rx(pkt);

  IoFrame response{};
  ASSERT_EQ(rig.engine.send_and_receive(AccessRig::status_read(false), response, FREQ_CH2, ExchangeOptions{1}),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH1})) << "nothing was sent onto the busy channel";
  EXPECT_EQ(rig.engine.counters().lbt_retries, 1u);
  EXPECT_EQ(rig.transmitted.lbt_defers, std::vector<int16_t>{-50});
  EXPECT_EQ(rig.radio.get_current_freq(), FREQ_CH1) << "the reply wait stayed on the channel the request used";
  EXPECT_EQ(rig.radio.freq_history(), (Channels{FREQ_CH1})) << "one retune, to the new channel, before the send";
}

TEST(ChannelAccess, TheBackOffIsRandomAndTheHubKeepsReceivingThroughIt) {
  AccessRig rig;
  rig.radio.set_channel_rssi(FREQ_CH2, -50);
  rig.random_values = {0, 2, 0};  // first channel CH2; back-off 1 + 2 % 4 = 3 ms; next channel CH1
  rig.run(false, ExchangeOptions{1});

  ASSERT_FALSE(rig.radio.wait_timeouts().empty());
  EXPECT_EQ(rig.radio.wait_timeouts()[0], 3u) << "the first wait of the exchange is the back-off, through listen()";
  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH1}));
}

TEST(ChannelAccess, ARemoteFrameHeardDuringTheBackOffReachesTheRemoteHandling) {
  AccessRig rig;
  rig.radio.set_emulate_capture_lifecycle(true);
  rig.radio.set_channel_rssi(FREQ_CH2, -50);  // the remote's burst keeps channel 2 busy
  rig.radio.queue_rx(to_rx_packet(remote_press_frame()));

  rig.run(false, ExchangeOptions{1});

  ASSERT_EQ(rig.heard.frames.size(), 1u) << "the press heard while backing off is handed over once";
  EXPECT_EQ(rig.heard.frames[0].cmd, CMD_EXECUTE);
  EXPECT_EQ(rig.heard.rssis[0], OVERHEARD_TEST_RSSI_DBM);
  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH1})) << "and the request still goes out, on another channel";
}

TEST(ChannelAccess, AChannelThatNeverClearsEndsTheAttemptAsBusyAndSendsNothing) {
  AccessRig rig;
  rig.radio.set_rssi_default(-50);  // every channel is busy
  const uint32_t begin_ms = esphome::test_clock::peek_ms();

  EXPECT_EQ(rig.run(false, ExchangeOptions{1}), ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.radio.get_send_count(), 0) << "never onto a channel measured busy";
  EXPECT_EQ(rig.engine.counters().channel_busy_attempts, 1u);
  EXPECT_GT(rig.engine.counters().lbt_retries, 1u) << "it kept checking, on other channels, until the time was spent";
  EXPECT_STREQ(rig.engine.get_debug().stage, "channel_busy");
  const uint32_t spent_ms = esphome::test_clock::peek_ms() - begin_ms;
  EXPECT_GE(spent_ms, CHANNEL_ACCESS_BUDGET_MS);
  EXPECT_LE(spent_ms, CHANNEL_ACCESS_BUDGET_MS + CHANNEL_CHECK_TIME_MS + 2) << "and no longer than about 100 ms";
  const Channels &measured = rig.radio.rssi_read_freqs();
  EXPECT_GE(std::set<uint32_t>(measured.begin(), measured.end()).size(), 2u) << "it moved on to other channels";
}

TEST(ChannelAccess, ABusyAttemptCountsAsAFailedAttemptOfItsPlanAndTheNextOneStartsAfterTheGap) {
  AccessRig rig;
  rig.radio.set_rssi_default(-50);

  EXPECT_EQ(rig.run(false, ExchangeOptions{EXCHANGE_RETRY_COUNT}), ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.engine.counters().channel_busy_attempts, static_cast<uint32_t>(EXCHANGE_RETRY_COUNT));
  EXPECT_EQ(rig.engine.counters().retransmits, static_cast<uint32_t>(EXCHANGE_RETRY_COUNT - 1));
  EXPECT_EQ(rig.radio.get_send_count(), 0);
}

TEST(ChannelAccess, ALowPowerRequestStaysOnChannel2AndWaitsLongerForItToClear) {
  AccessRig rig;
  rig.random_values = {1, 1, 1, 1, 1, 1, 1, 1};  // would move an ordinary request
  rig.radio.set_channel_rssi(FREQ_CH2, -50);
  const uint32_t begin_ms = esphome::test_clock::peek_ms();

  EXPECT_EQ(rig.run(true, ExchangeOptions{1}), ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.radio.get_send_count(), 0);
  const Channels &measured = rig.radio.rssi_read_freqs();
  EXPECT_EQ(std::set<uint32_t>(measured.begin(), measured.end()), std::set<uint32_t>{FREQ_CH2});
  EXPECT_TRUE(rig.radio.freq_history().empty()) << "it never leaves channel 2";
  const uint32_t spent_ms = esphome::test_clock::peek_ms() - begin_ms;
  EXPECT_GE(spent_ms, LOW_POWER_CHANNEL_ACCESS_BUDGET_MS);
  EXPECT_LE(spent_ms, LOW_POWER_CHANNEL_ACCESS_BUDGET_MS + CHANNEL_CHECK_TIME_MS + 2);
}

TEST(ChannelAccess, ALowPowerRequestSendsOnChannel2OnceItClears) {
  AccessRig rig;
  rig.random_values = {1, 1, 1, 1};
  rig.run(true, ExchangeOptions{1});
  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH2}));
}

TEST(ChannelAccess, ABusyAttemptSpendsNoWakeEvidence) {
  AccessRig rig;
  decisions::TargetEvidence evidence{};
  evidence.last_moving_evidence_ms = esphome::millis() - 1000;  // believed moving: AWAKE
  rig.engine.set_target_evidence_provider([&evidence](const uint8_t *, decisions::TargetEvidence &out) {
    out = evidence;
    return true;
  });
  int spent = 0;
  rig.engine.set_wake_evidence_spent_handler([&spent](const uint8_t *) { spent++; });
  rig.radio.set_rssi_default(-50);

  rig.run(true, ExchangeOptions{1});
  EXPECT_EQ(rig.engine.counters().channel_busy_attempts, 1u);
  EXPECT_EQ(spent, 0) << "a request that was never sent says nothing about the receiver";

  rig.radio.set_rssi_default(-120);
  rig.run(true, ExchangeOptions{1});
  EXPECT_EQ(spent, 1) << "the same silence after a send onto a free channel does spend it";
}

TEST(ChannelAccess, AForcedSendGoesOntoTheQuietestChannelAndIsNoEvidenceWhenSilent) {
  AccessRig rig;
  rig.tuning.lbt_force_when_busy = true;
  rig.radio.set_channel_rssi(FREQ_CH2, -50);
  rig.radio.set_channel_rssi(FREQ_CH1, -70);
  rig.radio.set_channel_rssi(FREQ_CH3, -60);
  decisions::TargetEvidence evidence{};
  evidence.last_moving_evidence_ms = esphome::millis() - 1000;
  rig.engine.set_target_evidence_provider([&evidence](const uint8_t *, decisions::TargetEvidence &out) {
    out = evidence;
    return true;
  });
  int spent = 0;
  rig.engine.set_wake_evidence_spent_handler([&spent](const uint8_t *) { spent++; });

  rig.run(false, ExchangeOptions{1});

  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH1})) << "every channel read busy; CH1 was the quietest";
  ASSERT_EQ(rig.radio.get_tx_configs().size(), 1u);
  EXPECT_FALSE(rig.radio.get_tx_configs()[0].cca_threshold_dbm.has_value()) << "forced: no check";
  EXPECT_EQ(rig.engine.counters().channel_busy_attempts, 0u) << "it was sent, so it is not a busy attempt";
  EXPECT_EQ(spent, 0) << "a collision on a busy channel explains the silence";
}

TEST(ChannelAccess, AForcedLowPowerSendStaysOnChannel2) {
  AccessRig rig;
  rig.tuning.lbt_force_when_busy = true;
  rig.radio.set_channel_rssi(FREQ_CH2, -50);
  rig.radio.set_channel_rssi(FREQ_CH1, -110);
  rig.radio.set_channel_rssi(FREQ_CH3, -110);

  rig.run(true, ExchangeOptions{1});

  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH2}));
  const Channels &measured = rig.radio.rssi_read_freqs();
  EXPECT_EQ(std::set<uint32_t>(measured.begin(), measured.end()), std::set<uint32_t>{FREQ_CH2})
      << "the other channels are never measured: a low-power request may not use them";
}

TEST(ChannelAccess, ACallerThatNeedsItsChannelKeepsTheFixedChannelAccess) {
  AccessRig rig;
  rig.random_values = {1, 1, 1};
  rig.radio.set_rssi_default(-50);  // busy on every check

  rig.run(false, ExchangeOptions{1}.on_fixed_channel());

  EXPECT_EQ(rig.sent_channels(), (Channels{FREQ_CH2})) << "the caller's channel, whatever the random source says";
  EXPECT_EQ(rig.engine.counters().lbt_retries, static_cast<uint32_t>(LBT_MAX_RETRIES)) << "five checks, 5 ms apart";
  ASSERT_EQ(rig.radio.get_tx_configs().size(), 1u);
  EXPECT_FALSE(rig.radio.get_tx_configs()[0].cca_threshold_dbm.has_value()) << "then one send without a check";
}

TEST(ChannelAccess, TheHubInstallsItsRandomSourceSoARequestFollowsIt) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  esphome::test_random::set(2);

  IoFrame request{};
  create_get_status(request, comp.node_id_, test::DST_ID, false);
  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2, 1);
  esphome::test_random::set(0);

  ASSERT_EQ(radio.get_tx_configs().size(), 1u);
  EXPECT_EQ(radio.get_tx_configs()[0].freq_hz, FREQ_CH3);
}

TEST(ChannelAccess, ABusyExchangeFailsThroughTheHubWithTheChannelBusyStage) {
  TestableComponent comp;
  MockRadio radio;
  wire_observer_rig(comp, radio);
  radio.set_rssi_default(-50);

  IoFrame request{};
  create_get_status(request, comp.node_id_, test::DST_ID, false);
  IoFrame response{};
  EXPECT_EQ(comp.send_and_receive_(request, response, FREQ_CH2, 1), ExchangeOutcome::FAILED);

  EXPECT_EQ(radio.get_send_count(), 0);
  EXPECT_STREQ(comp.exchange_engine_.get_debug().stage, "channel_busy");
}

// ============================================================================
// The busy limit judged against the board's own quiet level (ADR 0096): the idle hop takes a reading
// at every QUIET_LEVEL_READING_EVERY_N_HOPS-th hop, the limit follows from the quietest channel, and
// both channel checks (requests, start frames of fixed-channel senders) read that one limit.
// ============================================================================

namespace {

/// One idle hop of @p rig: the dwell elapses and maybe_hop() runs. Returns the channel the receiver
/// sat on right before it.
uint32_t idle_hop(AccessRig &rig) {
  const uint32_t before = rig.radio.get_current_freq();
  esphome::test_clock::advance_us(HOP_TIME_US + 1);
  rig.engine.maybe_hop();
  return before;
}

/// Hop idle until every channel has enough readings, with the radio hearing @p level_dbm.
void learn_quiet_level(AccessRig &rig, int16_t level_dbm) {
  rig.radio.set_rssi_default(level_dbm);
  const int hops = static_cast<int>(QUIET_LEVEL_MIN_READINGS) * 3 * QUIET_LEVEL_READING_EVERY_N_HOPS + 100;
  for (int i = 0; i < hops; i++)
    idle_hop(rig);
}

/// A frame that opens a session, so transmit_frame() checks the channel before it.
IoFrame start_frame() {
  IoFrame frame{};
  create_execute_position(frame, test::OWN_ID, test::DST_ID, false, 100);
  return frame;
}

}  // namespace

TEST(BusyLimit, IdleHopTakesOneReadingAtEveryTenthHopOnTheChannelItSatOn) {
  AccessRig rig;
  rig.engine.reset_hop_timestamp();
  std::vector<uint32_t> before_hop;
  for (int hop = 1; hop <= 2 * QUIET_LEVEL_READING_EVERY_N_HOPS; hop++) {
    before_hop.push_back(idle_hop(rig));
    const size_t expected_readings = static_cast<size_t>(hop / QUIET_LEVEL_READING_EVERY_N_HOPS);
    EXPECT_EQ(rig.radio.rssi_read_freqs().size(), expected_readings) << "after hop " << hop;
  }
  EXPECT_EQ(rig.radio.rssi_read_freqs(), (std::vector<uint32_t>{before_hop[QUIET_LEVEL_READING_EVERY_N_HOPS - 1],
                                                                before_hop[2 * QUIET_LEVEL_READING_EVERY_N_HOPS - 1]}));
}

TEST(BusyLimit, NoReadingIsTakenWhileAReceptionIsInProgress) {
  AccessRig rig;
  rig.engine.reset_hop_timestamp();
  for (int hop = 1; hop < QUIET_LEVEL_READING_EVERY_N_HOPS; hop++)
    idle_hop(rig);

  rig.radio.note_reception_from_test();
  idle_hop(rig);
  EXPECT_TRUE(rig.radio.rssi_read_freqs().empty()) << "the hop is held, so no reading is taken either";

  rig.radio.clear_reception_from_test();
  idle_hop(rig);
  EXPECT_EQ(rig.radio.rssi_read_freqs().size(), 1u) << "the tenth hop reads once the reception has cleared";
}

TEST(BusyLimit, TheIdleHopHopsAsOftenAsBefore) {
  AccessRig rig;
  rig.engine.reset_hop_timestamp();
  for (int hop = 0; hop < 3 * QUIET_LEVEL_READING_EVERY_N_HOPS; hop++)
    idle_hop(rig);
  EXPECT_EQ(rig.radio.freq_history().size(), static_cast<size_t>(3 * QUIET_LEVEL_READING_EVERY_N_HOPS));
}

TEST(BusyLimit, IsMinusEightyFiveUntilEveryChannelHasEnoughReadings) {
  AccessRig rig;
  EXPECT_EQ(rig.engine.busy_limit_dbm(), -85);
  rig.radio.set_rssi_default(-110);
  rig.engine.reset_hop_timestamp();
  for (int i = 0; i < 20 * QUIET_LEVEL_READING_EVERY_N_HOPS; i++)
    idle_hop(rig);
  EXPECT_EQ(rig.engine.busy_limit_dbm(), -85) << "a few readings per channel are no quiet level";
}

TEST(BusyLimit, ALearnedQuietBoardKeepsTheLowerBound) {
  AccessRig rig;
  rig.engine.reset_hop_timestamp();
  learn_quiet_level(rig, -113);  // bench V2
  EXPECT_EQ(rig.engine.busy_limit_dbm(), -90);
}

TEST(BusyLimit, ARequestOnANoisyBoardFindsAFreeChannelOnceTheLimitIsLearned) {
  AccessRig rig;
  rig.engine.reset_hop_timestamp();
  rig.radio.set_rssi_default(-80);
  EXPECT_EQ(rig.run(false, ExchangeOptions{1}), ExchangeOutcome::FAILED) << "-80 is busy against the -85 start limit";
  ASSERT_EQ(rig.radio.get_send_count(), 0);

  learn_quiet_level(rig, -89);  // a board that idles near the level of issue #228
  ASSERT_EQ(rig.engine.busy_limit_dbm(), -75);
  rig.radio.set_rssi_default(-80);
  rig.run(false, ExchangeOptions{1});
  EXPECT_EQ(rig.radio.get_send_count(), 1) << "-80 is idle noise against a learned -75";
}

TEST(BusyLimit, TheStartFrameOfAFixedChannelSenderUsesTheLearnedLimitToo) {
  AccessRig rig;
  rig.engine.reset_hop_timestamp();
  learn_quiet_level(rig, -89);
  rig.radio.set_rssi_default(-80);

  ASSERT_TRUE(rig.engine.transmit_frame(start_frame(), FREQ_CH2, SHORT_PREAMBLE));

  EXPECT_EQ(rig.engine.counters().lbt_retries, 0u) << "-80 dBm is below the learned -75";
  ASSERT_EQ(rig.radio.get_tx_configs().size(), 1u);
  EXPECT_EQ(rig.radio.get_tx_configs()[0].cca_threshold_dbm, -75);
}

TEST(BusyLimit, ALearnedLimitStillHoldsBackForARealTransmission) {
  AccessRig rig;
  rig.engine.reset_hop_timestamp();
  learn_quiet_level(rig, -113);
  rig.radio.set_rssi_default(-70);  // the weakest device frame seen on the bench
  EXPECT_EQ(rig.run(false, ExchangeOptions{1}), ExchangeOutcome::FAILED);
  EXPECT_EQ(rig.radio.get_send_count(), 0);
}

TEST(BusyLimit, AFixedThresholdIsTheLimitAndNothingIsLearned) {
  AccessRig rig;
  rig.tuning.lbt_rssi_threshold_dbm = -95;
  rig.engine.reset_hop_timestamp();
  rig.radio.set_rssi_default(-110);
  for (int i = 0; i < 50 * QUIET_LEVEL_READING_EVERY_N_HOPS; i++)
    idle_hop(rig);

  EXPECT_TRUE(rig.radio.rssi_read_freqs().empty()) << "no readings: the limit is not learned";
  EXPECT_EQ(rig.engine.busy_limit_dbm(), -95);

  rig.radio.set_rssi_default(-96);
  ASSERT_TRUE(rig.engine.transmit_frame(start_frame(), FREQ_CH2, SHORT_PREAMBLE));
  EXPECT_EQ(rig.radio.get_tx_configs()[0].cca_threshold_dbm, -95);
}
