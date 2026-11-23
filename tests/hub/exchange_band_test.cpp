/// @file exchange_band_test.cpp
/// @brief The exchange engine reads the radio's band (ADR 0132): the reply windows and the gap of a plan, the
/// tuning override, the late-answer warning and the air frequency in the log lines. The band is invented
/// (tests/support/test_bands.h): 80 ms start window, 70 ms low-power window, 45 ms gap, an answer late after 40 ms.

#include "hub_core.h"
#include "hub_exchange.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include "exchange_fixture.h"
#include "support/hold_seam.h"
#include "support/log_capture.h"
#include "stubs/radio_test_common.h"
#include "test_bands.h"
#include "test_helpers.h"

#include <cstring>
#include <string>
#include <vector>

using namespace esphome::home_io_control;
using namespace exchange_test;

namespace {

/// An exchange engine on a ManualClock over a MockRadio on the invented band.
struct BandRig {
  esphome::test_clock::ManualClock clock;
  MockRadio radio;
  RadioDriver *radio_ptr{&radio};
  TuningConfig tuning{};
  ExchangeEngine engine{&radio_ptr, test::OWN_ID, test::TEST_SYSTEM_KEY, &tuning};

  BandRig() {
    radio.set_band(test_bands::second_band());
    radio.set_model_tx_airtime(true);
  }

  /// One exchange of @p kind to a silent target.
  void run_silent(decisions::ReadKind kind, bool low_power) {
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, low_power);
    IoFrame response{};
    EXPECT_EQ(engine.send_and_receive(request, response, FREQ_CH2, read_of(kind)), ExchangeOutcome::FAILED);
  }

  /// One try's transmit time to a target of this power class, as the engine estimates it.
  uint32_t tx_ms(bool low_power) const {
    IoFrame request{};
    create_get_status(request, test::OWN_ID, test::DST_ID, low_power);
    return engine.tx_time_ms(request, FREQ_CH2, engine.request_preamble_for(request));
  }

  /// Time from each transmit to the next, in ms.
  std::vector<uint32_t> spacing_ms() const {
    const auto &t = radio.send_times_ms();
    std::vector<uint32_t> spacing;
    for (size_t i = 0; i + 1 < t.size(); i++)
      spacing.push_back(t[i + 1] - t[i]);
    return spacing;
  }
};

}  // namespace

TEST(ExchangeBand, ACommandToASilentLowPowerTargetListensTheBandsStartWindowWithTheBandsGap) {
  BandRig rig;
  rig.run_silent(decisions::ReadKind::COMMAND, true);

  EXPECT_EQ(rig.radio.wait_timeouts(), (std::vector<uint32_t>{80, 80, 80}));
  for (const uint32_t spacing : rig.spacing_ms()) {
    EXPECT_GE(spacing, rig.tx_ms(true) + 80u + 45u) << "the window and the band's gap pass between two tries";
    EXPECT_LT(spacing, rig.tx_ms(true) + 80u + EXCHANGE_RETRY_DELAY_MS) << "not the 868 MHz gap";
  }
}

TEST(ExchangeBand, ASettlingReadOfASilentLowPowerTargetListensTheBandsLowPowerWindowWithoutAGap) {
  BandRig rig;
  rig.run_silent(decisions::ReadKind::SETTLING, true);

  EXPECT_EQ(rig.radio.wait_timeouts(), (std::vector<uint32_t>{70, 70, 70}));
}

TEST(ExchangeBand, AProgressPollIsOneAttemptOfTheBandsStartWindow) {
  BandRig rig;
  rig.run_silent(decisions::ReadKind::PROGRESS, false);

  EXPECT_EQ(rig.radio.wait_timeouts(), (std::vector<uint32_t>{80}));
}

TEST(ExchangeBand, TheTuningKeyReplacesTheBandsStartWindowOnTheEngine) {
  BandRig rig;
  rig.tuning.exchange_start_response_wait_ms = 90;
  rig.run_silent(decisions::ReadKind::COMMAND, true);

  EXPECT_EQ(rig.radio.wait_timeouts(), (std::vector<uint32_t>{90, 90, 90}));
}

TEST(ExchangeBand, ARequestThatNamesOnlyTheTriesTakesTheBandsStartWindowAndGap) {
  BandRig rig;
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, false);
  IoFrame response{};
  EXPECT_EQ(rig.engine.send_and_receive(request, response, FREQ_CH2, ExchangeOptions{2}), ExchangeOutcome::FAILED);

  EXPECT_EQ(rig.radio.wait_timeouts(), (std::vector<uint32_t>{80, 80}));
  ASSERT_EQ(rig.spacing_ms().size(), 1u);
  EXPECT_GE(rig.spacing_ms()[0], rig.tx_ms(false) + 80u + 45u);
  EXPECT_LT(rig.spacing_ms()[0], rig.tx_ms(false) + 80u + EXCHANGE_RETRY_DELAY_MS) << "not the 868 MHz gap";
}

// ----------------------------------------------------------------------------
// The late-answer warning (ADR 0111: the lines sit after the wait, never inside it)
// ----------------------------------------------------------------------------

namespace {

/// A hub on a MockRadio on the invented band whose challenged exchange is logged with its timing.
struct LateAnswerRig {
  esphome::test_clock::ManualClock clock;
  TestableComponent comp;
  MockRadio radio;
  test::LogCapture log;
  std::vector<size_t> at_deliver;
  test::HoldSeam seam{radio, [] {}, [this] { at_deliver.push_back(log.lines().size()); }};

  explicit LateAnswerRig(uint16_t warn_ms) {
    IoBand band = test_bands::second_band();
    band.late_answer_warn_ms = warn_ms;
    radio.set_band(band);
    radio.set_model_tx_airtime(true);
    comp.initialized_ = true;
    comp.radio_ = &radio;
    memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
    memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
  }

  /// One authenticated exchange that the target answers.
  void run_challenged() {
    IoFrame request{};
    create_execute_position(request, comp.node_id_, test::DST_ID, false, 100);
    const uint8_t chal_data[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    radio.queue_rx(to_rx_packet(build_challenge(test::DST_ID, comp.node_id_, chal_data)));
    radio.queue_rx(to_rx_packet(build_status_response(test::DST_ID, comp.node_id_)));
    IoFrame response{};
    ASSERT_EQ(comp.send_and_receive_(request, response, FREQ_CH2), ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  }

  /// The lines logged after the last reply was handed over.
  std::vector<test::LogCapture::Line> after_last_reply() const {
    return {log.lines().begin() + static_cast<std::ptrdiff_t>(at_deliver.back()), log.lines().end()};
  }
};

}  // namespace

TEST(ExchangeBand, AnAnswerAboveTheBandsThresholdIsReportedRightAfterTheAuthChallengeLine) {
  LateAnswerRig rig(1);  // 1 ms: the modelled airtime of the 0x3D is above it
  rig.run_challenged();

  const auto after = rig.after_last_reply();
  ASSERT_GE(after.size(), 2u);
  EXPECT_EQ(after[0].format.rfind("Auth challenge try=%d wait_ms", 0), 0u) << "the deferred line first";
  EXPECT_EQ(after[1].level, 'W');
  EXPECT_NE(after[1].format.find("answered late"), std::string::npos);
  EXPECT_EQ(rig.log.count('W'), 1u);
}

TEST(ExchangeBand, AnAnswerBelowTheBandsThresholdIsNotReported) {
  LateAnswerRig rig(60000);
  rig.run_challenged();

  EXPECT_EQ(rig.log.count('W'), 0u);
}

TEST(ExchangeBand, ABandWithAThresholdOfZeroNeverReportsALateAnswer) {
  LateAnswerRig rig(0);
  rig.run_challenged();

  EXPECT_EQ(rig.log.count('W'), 0u);
}

// ----------------------------------------------------------------------------
// Log lines print the air frequency
// ----------------------------------------------------------------------------

TEST(ExchangeBand, ABusyChannelLineShowsTheAirFrequencyNotTheChannelsIdentity) {
  BandRig rig;
  rig.radio.set_channel_rssi(FREQ_CH1, -30);
  rig.radio.set_channel_rssi(FREQ_CH2, -30);
  rig.radio.set_channel_rssi(FREQ_CH3, -30);
  test::LogCapture log(/*with_text=*/true);

  rig.run_silent(decisions::ReadKind::PROGRESS, false);

  size_t busy_lines = 0;
  for (const auto &line : log.lines()) {
    if (line.format.rfind("Channel busy (RSSI", 0) != 0 && line.format.rfind("LBT: channel busy", 0) != 0)
      continue;
    busy_lines++;
    const bool air = line.text.find(std::to_string(test_bands::SECOND_BAND_CH2_HZ) + " Hz") != std::string::npos ||
                     line.text.find(std::to_string(test_bands::SECOND_BAND_CH1_HZ) + " Hz") != std::string::npos ||
                     line.text.find(std::to_string(test_bands::SECOND_BAND_CH3_HZ) + " Hz") != std::string::npos;
    EXPECT_TRUE(air) << line.text;
    EXPECT_EQ(line.text.find("868"), std::string::npos) << line.text;
  }
  EXPECT_GT(busy_lines, 0u) << "the scenario must reach a busy-channel line";
}
