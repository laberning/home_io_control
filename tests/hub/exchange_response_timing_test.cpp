/// @file exchange_response_timing_test.cpp
/// @brief Tests for the response timing of the exchange engine: the response preamble a driver asks for, the channel
/// hop rotation and idle hop, and the response windows.

#include "hub_exchange.h"
#include "hub_core.h"
#include "proto_frame.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "radio_sx1276.h"
#include "radio_sx1262.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

#include "exchange_fixture.h"

using namespace esphome::home_io_control;
using namespace exchange_test;

// ============================================================================
// response_preamble() behavior tests
// ============================================================================
// Validate that RadioDriver::response_preamble() correctly influences the
// preamble length used by send_and_receive_ for both START and non-START frames,
// and that the SX1262 override returns the longer preamble while the default
// (SX1276-like) returns SHORT_PREAMBLE.

TEST(Exchange, ResponsePreamble_DefaultReturnsShortPreamble) {
  MockRadio radio;
  EXPECT_EQ(radio.response_preamble(), SHORT_PREAMBLE)
      << "base RadioDriver (SX1276-like) should return SHORT_PREAMBLE for response frames";
}

TEST(Exchange, ResponsePreamble_SX1262MatchesConfiguredValue) {
  MockRadioSX1262 radio;
  EXPECT_EQ(radio.response_preamble(), MOCK_RESPONSE_PREAMBLE)
      << "SX1262 should return the configured response preamble";
  // Byte-denominated like SHORT_PREAMBLE (set_packet_params_() converts to bits at the SPI
  // boundary); the 3.5 ms default is 17 bytes at 8 bit periods each, above the protocol's nominal
  // 8-byte floor.
  EXPECT_EQ(radio.response_preamble(), 17u);
  EXPECT_GE(radio.response_preamble(), SHORT_PREAMBLE);
}

TEST(Exchange, TxRxTurnaround_RealDriversDeclareExpectedCapability) {
  // Pins the per-chip capability the pairing engine uses to select its key-confirm
  // wait strategy. Instantiation only — no init(), no SPI traffic.
  MockSpi spi;
  MockPin rst, dio0, dio4, dio1, busy;
  RadioSX1276 sx1276(&spi, &rst, &dio0, &dio4, 17, 0x80);
  RadioSX1262 sx1262(&spi, &rst, &dio1, &busy, 17, 0x03);
  EXPECT_TRUE(sx1276.has_fast_tx_rx_turnaround())
      << "SX1276 (IoHomeOn) catches immediate replies via the standard exchange wait";
  EXPECT_FALSE(sx1262.has_fast_tx_rx_turnaround())
      << "SX1262 needs the dedicated key-confirm wait (slow TX->RX transition)";
}

TEST(Exchange, RadioSX1276_ApplyTuningRoutesRadioParameters) {
  // Drives the real SX1276 apply_tuning() path (no init(); register writes go to the no-op
  // MockSpi). Verifies the default is the 3.5 ms duration at 10 bit periods per byte and that a
  // tuning override propagates to the observable response_preamble(), and that set_rx_bandwidth_
  // accepts every enum option without touching hardware.
  MockSpi spi;
  MockPin rst, dio0, dio4;
  RadioSX1276 sx1276(&spi, &rst, &dio0, &dio4, 17, 0x80);

  // Default (no tuning applied): 3.5 ms is 13 bytes at 10 bit periods each.
  EXPECT_EQ(sx1276.response_preamble(), 13u);

  TuningConfig cfg{};
  cfg.response_preamble_ms = 10.0F;  // 384 bit periods: 38 bytes at 10 bit periods each
  for (auto bw : {SX1276RxBandwidth::BW_20_8_KHZ, SX1276RxBandwidth::BW_41_7_KHZ, SX1276RxBandwidth::BW_125_0_KHZ}) {
    cfg.sx1276_rx_bandwidth = bw;
    sx1276.apply_tuning(cfg);  // set_rx_bandwidth_ + set_response_preamble_ (no crash / no SPI dependency)
  }
  EXPECT_EQ(sx1276.response_preamble(), 38) << "apply_tuning must route response_preamble_ms to the driver";

  // Applying defaults again restores the default preamble.
  sx1276.apply_tuning(TuningConfig{});
  EXPECT_EQ(sx1276.response_preamble(), 13u);
}

TEST(Exchange, SendAndReceive_LowPowerStartFrameUsesLongPreamble) {
  // A START frame whose target is a low-power device (CTRL1_LOW_POWER set) keeps MOCK_WAKE_PREAMBLE:
  // the long preamble is the wake-up burst for a duty-cycled receiver.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, /*low_power=*/true, 100);
  ASSERT_TRUE(is_start(request)) << "execute command should have START flag set";
  ASSERT_NE(request.ctrl1 & CTRL1_LOW_POWER, 0) << "low_power=true must set CTRL1_LOW_POWER";

  // No RX → times out after retries, but we can inspect the TX config
  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  ASSERT_GE(radio.get_tx_configs().size(), 1u) << "at least one TX should have been attempted";
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, MOCK_WAKE_PREAMBLE)
      << "a low-power START frame must use MOCK_WAKE_PREAMBLE to wake the target";
}

TEST(Exchange, SendAndReceive_NormalStartFrameUsesNormalStartPreamble) {
  // A START frame to a non-low-power (always-alive) target uses the runtime-tunable
  // `start_preamble_ms`, not the 1024-byte wake-up burst.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, /*low_power=*/false, 100);
  ASSERT_TRUE(is_start(request)) << "execute command should have START flag set";
  ASSERT_EQ(request.ctrl1 & CTRL1_LOW_POWER, 0) << "low_power=false must leave CTRL1_LOW_POWER clear";

  // No RX → times out after retries, but we can inspect the TX config
  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  ASSERT_GE(radio.get_tx_configs().size(), 1u) << "at least one TX should have been attempted";
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "a normal START frame must use `start_preamble_ms`, not MOCK_WAKE_PREAMBLE";
}

TEST(Exchange, SendAndReceive_NonStartFrameUsesResponsePreamble_Default) {
  // Non-START frames on the default radio (SX1276-like) should use SHORT_PREAMBLE.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadio radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  // Build a non-START frame (key_transfer style: 2W, no start, no end)
  IoFrame request{};
  init_frame(request, true, false, false, false);
  set_dst(request, test::DST_ID);
  set_src(request, comp.node_id_);
  uint8_t payload[16] = {0};
  set_cmd(request, CMD_KEY_TRANSFER, payload, sizeof(payload));
  ASSERT_FALSE(is_start(request)) << "key_transfer should NOT have START flag";

  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  ASSERT_GE(radio.get_tx_configs().size(), 1u);
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, SHORT_PREAMBLE)
      << "non-START frame on default radio should use SHORT_PREAMBLE";
}

TEST(Exchange, SendAndReceive_NonStartFrameUsesResponsePreamble_SX1262) {
  // Non-START frames on SX1262 should use MOCK_RESPONSE_PREAMBLE, not MOCK_WAKE_PREAMBLE.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadioSX1262 radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  // Build a non-START frame (key_transfer style)
  IoFrame request{};
  init_frame(request, true, false, false, false);
  set_dst(request, test::DST_ID);
  set_src(request, comp.node_id_);
  uint8_t payload[16] = {0};
  set_cmd(request, CMD_KEY_TRANSFER, payload, sizeof(payload));
  ASSERT_FALSE(is_start(request));

  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  ASSERT_GE(radio.get_tx_configs().size(), 1u);
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, MOCK_RESPONSE_PREAMBLE)
      << "non-START frame on SX1262 should use MOCK_RESPONSE_PREAMBLE";
}

namespace {

/// The preamble the first transmit of `request` carries, on a hub driving `radio`.
uint16_t first_preamble_on(MockRadio &radio, const IoFrame &request) {
  TestableComponent comp;
  comp.initialized_ = true;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);
  return radio.get_tx_configs().empty() ? 0 : radio.get_tx_configs()[0].preamble_len;
}

}  // namespace

// ADR 0097: every kind of frame carries its preamble as a duration, converted by the radio, so the
// same setting is fewer programmed bytes on a radio that sends 10 bit periods per byte and lasts
// the same on air.
TEST(Exchange, EveryFrameKindCarriesTheSameDurationOnATenBitRadioAsOnARawStreamRadio) {
  IoFrame normal{};
  create_execute_position(normal, test::OWN_ID, test::DST_ID, /*low_power=*/false, 100);
  IoFrame wake{};
  create_execute_position(wake, test::OWN_ID, test::DST_ID, /*low_power=*/true, 100);
  IoFrame continuation{};
  init_frame(continuation, true, false, false, false);
  set_dst(continuation, test::DST_ID);
  set_src(continuation, test::OWN_ID);
  uint8_t payload[16] = {0};
  set_cmd(continuation, CMD_KEY_TRANSFER, payload, sizeof(payload));

  struct Kind {
    const char *name;
    const IoFrame *frame;
    uint16_t raw_stream_bytes;
    uint16_t ten_bit_bytes;
    float ms;
  };
  const Kind kinds[] = {
      {"start frame to a mains target", &normal, 48, 38, START_PREAMBLE_MS},
      {"start frame to a low-power target", &wake, 2472, 1978, LOW_POWER_WAKE_MS},
      {"continuation frame", &continuation, 17, 13, RESPONSE_PREAMBLE_MS},
  };
  for (const Kind &kind : kinds) {
    MockRadioSX1262 raw_stream;
    TenBitPreambleRadio ten_bit;
    EXPECT_EQ(first_preamble_on(raw_stream, *kind.frame), kind.raw_stream_bytes) << kind.name << " (8 bits/byte)";
    EXPECT_EQ(first_preamble_on(ten_bit, *kind.frame), kind.ten_bit_bytes) << kind.name << " (10 bits/byte)";
    RadioTxConfig raw_cfg;
    raw_cfg.preamble_len = kind.raw_stream_bytes;
    RadioTxConfig ten_cfg;
    ten_cfg.preamble_len = kind.ten_bit_bytes;
    EXPECT_NEAR(raw_stream.tx_air_time_us(13, raw_cfg) / 1000.0, ten_bit.tx_air_time_us(13, ten_cfg) / 1000.0, 1.0)
        << kind.name << ": the same time on air";
  }
}

TEST(Exchange, ATunedStartDurationMovesEveryRadioToItsOwnByteCount) {
  IoFrame normal{};
  create_execute_position(normal, test::OWN_ID, test::DST_ID, /*low_power=*/false, 100);
  // start_preamble_ms is read at send time, so a live change (the Home Assistant number) applies to the next frame.
  for (const float ms : {5.0F, 20.0F}) {
    MockRadioSX1262 raw_stream;
    TenBitPreambleRadio ten_bit;
    TestableComponent comp;
    comp.initialized_ = true;
    comp.tuning_.start_preamble_ms = ms;
    memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
    memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
    IoFrame response{};
    comp.radio_ = &raw_stream;
    comp.send_and_receive_(normal, response, FREQ_CH2);
    comp.radio_ = &ten_bit;
    comp.send_and_receive_(normal, response, FREQ_CH2);
    ASSERT_FALSE(raw_stream.get_tx_configs().empty());
    ASSERT_FALSE(ten_bit.get_tx_configs().empty());
    EXPECT_EQ(raw_stream.get_tx_configs()[0].preamble_len, raw_stream.preamble_bytes_for_ms(ms)) << ms;
    EXPECT_EQ(ten_bit.get_tx_configs()[0].preamble_len, ten_bit.preamble_bytes_for_ms(ms)) << ms;
    EXPECT_GT(raw_stream.get_tx_configs()[0].preamble_len, ten_bit.get_tx_configs()[0].preamble_len) << ms;
  }
}

TEST(Exchange, SendAndReceive_AuthResponseUsesSX1262Preamble) {
  // When SX1262 radio handles a challenge, the 0x3D auth response should use the longer preamble.
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadioSX1262 radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  // Queue a challenge from device
  uint8_t chal_data[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  uint8_t raw_chal[64];
  uint8_t len_chal = serialize(challenge, raw_chal, sizeof(raw_chal));
  RadioRxPacket chal_pkt{};
  chal_pkt.len = len_chal;
  memcpy(chal_pkt.data, raw_chal, len_chal);
  radio.queue_rx(chal_pkt);

  // No final response — will timeout, but we can inspect the auth response TX config
  IoFrame response{};
  comp.send_and_receive_(request, response, FREQ_CH2);

  // TX 0 = request (`start_preamble_ms` — start frame to a non-low-power target)
  // TX 1 = auth response (should use MOCK_RESPONSE_PREAMBLE)
  ASSERT_GE(radio.get_tx_configs().size(), 2u) << "should have sent at least request + auth response";
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "initial request (START, low_power=false) should use `start_preamble_ms`";
  EXPECT_EQ(radio.get_tx_configs()[1].preamble_len, MOCK_RESPONSE_PREAMBLE)
      << "auth response on SX1262 should use MOCK_RESPONSE_PREAMBLE";
}

TEST(Exchange, SendAndReceive_RequestPreambleOverrideAppliesToRequestOnly) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadioSX1262 radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, /*low_power=*/true, 100);

  uint8_t chal_data[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);
  uint8_t raw_chal[64];
  RadioRxPacket chal_pkt{};
  chal_pkt.len = serialize(challenge, raw_chal, sizeof(raw_chal));
  memcpy(chal_pkt.data, raw_chal, chal_pkt.len);
  radio.queue_rx(chal_pkt);

  IoFrame response{};
  comp.exchange_engine_.send_and_receive(request, response, FREQ_CH2, ExchangeOptions{}.with_preamble(32));

  ASSERT_GE(radio.get_tx_configs().size(), 2u);
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, 32u)
      << "the override replaces the rule's MOCK_WAKE_PREAMBLE for a low-power start frame";
  EXPECT_EQ(radio.get_tx_configs()[1].preamble_len, MOCK_RESPONSE_PREAMBLE)
      << "the 0x3D challenge response keeps the driver's response preamble";
}

// answer_challenge() is the one place that builds and sends a 0x3D, shared by the normal
// inbound-challenge path (handle_authentication_()) and pairing's post-0x32 challenge answer
// (wait_for_key_confirm_(), pairing_engine.cpp), so both send the exact same shape. This test
// drives it directly, bypassing send_and_receive_() entirely, to pin its own contract independent
// of the outbound retry loop around it.
TEST(Exchange, AnswerChallengeSendsExactly0x3DOverTheRequestTranscript) {
  TestableComponent comp;
  comp.initialized_ = true;
  MockRadioSX1262 radio;
  comp.radio_ = &radio;
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  IoFrame request{};
  create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);

  uint8_t chal_data[HMAC_SIZE] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  IoFrame challenge = build_challenge(test::DST_ID, comp.node_id_, chal_data);

  ASSERT_TRUE(comp.exchange_engine_.answer_challenge(request, challenge, FREQ_CH2))
      << "answer_challenge should build and transmit the 0x3D";

  ASSERT_EQ(radio.get_sent_data().size(), 1u) << "answer_challenge should transmit exactly one frame";
  IoFrame sent{};
  const auto &raw = radio.get_sent_data()[0];
  ASSERT_TRUE(parse(raw.data(), static_cast<uint8_t>(raw.size()), sent)) << "transmitted frame must parse cleanly";

  IoFrame expected{};
  ASSERT_TRUE(create_challenge_resp(expected, request.dst, comp.node_id_, chal_data, request, comp.system_key_))
      << "reference create_challenge_resp() call should succeed";
  uint8_t expected_raw[64];
  const uint8_t expected_len = serialize(expected, expected_raw, sizeof(expected_raw));
  ASSERT_EQ(raw.size(), expected_len) << "answer_challenge must send exactly create_challenge_resp()'s bytes";
  EXPECT_EQ(0, memcmp(raw.data(), expected_raw, expected_len))
      << "answer_challenge must send exactly create_challenge_resp()'s bytes";

  ASSERT_EQ(radio.get_tx_configs().size(), 1u);
  EXPECT_EQ(radio.get_tx_configs()[0].preamble_len, MOCK_RESPONSE_PREAMBLE)
      << "answer_challenge should use the driver's response_preamble(), like handle_authentication_() did";
}

// ============================================================================
// hop_frequency(): the bare CH1→CH2→CH3→CH1 rotation, asserted directly so the skip_freq default
// argument (used above) can be trusted to reproduce it exactly.
// ============================================================================

TEST(Exchange, HopFrequencyWithoutSkipIsUnchanged) {
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  radio.change_frequency(FREQ_CH1);
  engine.hop_frequency();
  EXPECT_EQ(radio.get_current_freq(), FREQ_CH2);

  engine.hop_frequency();
  EXPECT_EQ(radio.get_current_freq(), FREQ_CH3);

  engine.hop_frequency();
  EXPECT_EQ(radio.get_current_freq(), FREQ_CH1);
}

// ============================================================================
// maybe_hop(): the idle-path hop. maybe_hop() is time-gated; RadioDriver::reception_in_progress()
// additionally gates it so a hop cannot retune under an arriving frame and destroy it via
// change_frequency()'s IRQ/DIO-latch clear (issue #81).
// ============================================================================

TEST(Exchange, MaybeHopDoesNotHopWhileTheDwellHasNotYetElapsed) {
  // Boundary case for MaybeHopHopsOnceTheDwellElapses below: exchange_engine.cpp's maybe_hop()
  // compares with <=, so exactly HOP_TIME_US elapsed must not be enough on its own.
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  engine.reset_hop_timestamp();
  esphome::test_clock::advance_us(HOP_TIME_US);
  engine.maybe_hop();

  EXPECT_TRUE(radio.freq_history().empty()) << "exactly HOP_TIME_US elapsed must not be past the dwell yet";
}

TEST(Exchange, MaybeHopHopsOnceTheDwellElapses) {
  // Baseline: pins that the reception_in_progress() guard did not break the ordinary path.
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  engine.reset_hop_timestamp();
  esphome::test_clock::advance_us(HOP_TIME_US + 1);
  engine.maybe_hop();

  EXPECT_EQ(radio.freq_history().size(), 1u) << "the dwell elapsed and nothing was arriving, so the hop must fire";
}

TEST(Exchange, MaybeHopDoesNotHopWhileAFrameIsArriving) {
  // The core assertion of the whole step: a reception in progress must outrank the dwell timer.
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  engine.reset_hop_timestamp();
  esphome::test_clock::advance_us(HOP_TIME_US + 1);
  radio.note_reception_from_test();
  engine.maybe_hop();

  EXPECT_TRUE(radio.freq_history().empty()) << "a frame arriving on this channel must suppress the hop entirely";
}

TEST(Exchange, MaybeHopHopsAsSoonAsTheReceptionClears) {
  // Pins both that the holdoff expires on its own and that the deferred hop is not lost — it
  // happens on the very next call, not never.
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  engine.reset_hop_timestamp();
  esphome::test_clock::advance_us(HOP_TIME_US + 1);
  radio.note_reception_from_test();
  engine.maybe_hop();
  ASSERT_TRUE(radio.freq_history().empty()) << "sanity: the first call must still have been suppressed";

  esphome::test_clock::advance_us(RX_HOP_HOLDOFF_US + 1);
  engine.maybe_hop();

  EXPECT_EQ(radio.freq_history().size(), 1u) << "the holdoff expired, so the deferred hop must now fire";
}

TEST(Exchange, DeferredHopDoesNotRestartTheDwellTimer) {
  // Pins the "last_hop_us_ deliberately left alone" decision (exchange_engine.cpp maybe_hop()).
  // A wrong implementation that stamped last_hop_us_ = micros() on the suppressed call too (e.g.
  // by moving the update above the reception check) would still pass
  // MaybeHopHopsAsSoonAsTheReceptionClears above, because RX_HOP_HOLDOFF_US already exceeds
  // HOP_TIME_US on its own. Clearing the holdoff directly rather than waiting out its own timer
  // isolates the property: once the dwell has genuinely elapsed, a single further microsecond
  // must be enough to hop.
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;
  TuningConfig tuning = make_broadcast_test_tuning();
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  engine.reset_hop_timestamp();
  esphome::test_clock::advance_us(HOP_TIME_US + 1);
  radio.note_reception_from_test();
  engine.maybe_hop();
  ASSERT_TRUE(radio.freq_history().empty()) << "sanity: the first call must still have been suppressed";

  radio.clear_reception_from_test();
  esphome::test_clock::advance_us(1);
  engine.maybe_hop();

  EXPECT_EQ(radio.freq_history().size(), 1u)
      << "the dwell had already elapsed before the suppressed call; one more microsecond after the "
         "reception clears must be enough to hop, proving last_hop_us_ was not touched while the "
         "hop was held off";
}

// ============================================================================
// Response windows: a start frame wakes a sleeping device, so it gets the *longer* budget.
//
// These were fixed constants, and RESPONSE_START_WAIT_MS (300 ms) was shorter than
// RESPONSE_WAIT_MS (500 ms) despite its own comment promising "longer" — backwards for the one
// case where a low-power target may have been asleep until the 213 ms wake-up preamble reached it.
// Field captures of a solar RS100 measured replies from 29 ms to 3052 ms on the same device; see
// RESPONSE_START_WAIT_MS.
// ============================================================================

TEST(Exchange, StartFrameBudgetsTheStartResponseWindow) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;

  TuningConfig tuning;
  tuning.exchange_start_response_wait_ms = 1750;
  tuning.exchange_response_wait_ms = 250;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, true, 100);
  ASSERT_TRUE(is_start(request)) << "an execute command is a start frame";

  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);  // nothing queued → runs out its retries

  ASSERT_FALSE(radio.wait_timeouts().empty());
  EXPECT_EQ(radio.wait_timeouts().front(), 1750u) << "the first slice must be exactly the configured window";
}

TEST(Exchange, ContinuationFrameBudgetsTheShorterResponseWindow) {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr = &radio;

  TuningConfig tuning;
  tuning.exchange_start_response_wait_ms = 1750;
  tuning.exchange_response_wait_ms = 250;
  ExchangeEngine engine(&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning);

  IoFrame request{};
  init_frame(request, true, false, false, false);
  set_dst(request, test::DST_ID);
  set_src(request, test::OWN_ID);
  uint8_t payload[16] = {0};
  set_cmd(request, CMD_KEY_TRANSFER, payload, sizeof(payload));
  ASSERT_FALSE(is_start(request));

  IoFrame response{};
  engine.send_and_receive(request, response, FREQ_CH2);

  ASSERT_FALSE(radio.wait_timeouts().empty());
  EXPECT_EQ(radio.wait_timeouts().front(), 250u) << "the first slice must be exactly the configured window";
}

TEST(Exchange, ResponseWindowDefaultsComeFromTheProtocolConstants) {
  // No ordering constraint between the two windows is asserted on purpose. An earlier version of
  // this test required the start window to be the longer of the two, on the theory that a
  // just-woken device is the slowest to answer. Measurement disproved that: the device replies
  // within milliseconds of the carrier dropping or not at all (see RESPONSE_START_WAIT_MS), so
  // both windows only need to clear a few tens of milliseconds and their relative order carries
  // no meaning worth pinning.
  TuningConfig tuning;
  EXPECT_FALSE(tuning.exchange_start_response_wait_ms.has_value()) << "unset: the band gives the window";
  EXPECT_EQ(decisions::start_reply_window_ms(tuning, IO_868_BAND), RESPONSE_START_WAIT_MS);
  EXPECT_EQ(tuning.exchange_response_wait_ms, RESPONSE_WAIT_MS);
  EXPECT_EQ(tuning.exchange_total_budget_ms, EXCHANGE_TOTAL_BUDGET_MS);
}
