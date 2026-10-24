/// @file
/// @brief The hub's exchange engine against an emulated 2.4 GHz blind over the real co-processor core.
/// @ingroup hioc_hub
///
/// Six cases, each asserting attempts and timing from what went on the simulated air (the frames the
/// hub's driver sent, their wake trains and their times), not only the end result. The blind is a model
/// (tests/support/io_blind_emulator.h); its limits are the ones measured on one real blind under bench
/// conditions, so a case about them says "this blind".

#include "fullstack_rig.h"

#include "hub_core.h"
#include "hub_internal.h"
#include "stubs/radio_test_common.h"

using namespace esphome::home_io_control;
// `bridle::` alone is ambiguous here: the global ::bridle (the Bridle release) is visible too.
namespace hioc_bridle = esphome::home_io_control::bridle;
using esphome::test_clock::ManualClock;
using fullstack::EmulatorEvent;
using fullstack::IoBlindEmulator;
using fullstack::Rig;

namespace {

const std::string kBlindId = node_id_to_string(test::DST_ID);
constexpr uint16_t RAW_CLOSED = 0xC800;
constexpr uint64_t LONG_TRAIN_FRAMES = 510;
constexpr uint64_t SHORT_TRAIN_FRAMES = 8;

IoFrame status_request(bool low_power = true) {
  IoFrame request{};
  create_get_status(request, test::OWN_ID, test::DST_ID, low_power);
  return request;
}

IoFrame execute_closed(bool low_power = true) {
  IoFrame request{};
  create_execute_position(request, test::OWN_ID, test::DST_ID, low_power, 100);
  return request;
}

IoBlindEmulator::Config blind_at(double position_pct) {
  IoBlindEmulator::Config config = fullstack::default_blind_config();
  config.position_pct = position_pct;
  return config;
}

}  // namespace

// === 1. A status read of a resting blind ===

TEST(FullStackExchange, AStatusReadOfARestingBlindDecodesToItsPositionOnTheFirstTry) {
  ManualClock clock;
  Rig rig(blind_at(47.0));
  ASSERT_TRUE(rig.initialised());
  IoFrame response{};
  ASSERT_EQ(rig.engine().send_and_receive(status_request(), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  ASSERT_EQ(rig.heard().size(), 1u) << "one attempt";
  EXPECT_EQ(rig.heard()[0].wake_frames, LONG_TRAIN_FRAMES)
      << "a low-power device with no wake evidence gets the long train";
  ASSERT_EQ(rig.events().size(), 1u);
  EXPECT_EQ(rig.events()[0].kind, EmulatorEvent::Kind::STATUS_ANSWERED);

  const test::DecodedStatus status(response, kBlindId);
  ASSERT_NE(status.device, nullptr);
  EXPECT_TRUE(status.device->is_stopped);
  const double raw = status.device->position * 512.0;
  EXPECT_NEAR(raw, rig.blind().current_raw(rig.heard()[0].start_us), STATUS_POS_TOLERANCE_RAW);
}

// === 2. A status read of a moving blind ===

TEST(FullStackExchange, AStatusReadOfAMovingBlindDecodesToMovingWithItsTargetAndTheHintPlusOneSecond) {
  ManualClock clock;
  Rig rig(blind_at(0.0));
  ASSERT_TRUE(rig.initialised());
  rig.blind().set_off(RAW_CLOSED, rig.sim_now_us());  // a remote sent it for the closed limit just now
  IoFrame response{};
  ASSERT_EQ(rig.engine().send_and_receive(status_request(), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  ASSERT_EQ(rig.heard().size(), 1u);
  const uint16_t remaining_s = static_cast<uint16_t>((response.data[6] << 8) | response.data[7]);
  EXPECT_GE(remaining_s, 70u);
  EXPECT_LE(remaining_s, 72u) << "a 72 s travel, read within the first second of it";

  // The hint can only shorten the poll delay, so a configured interval above it lets it show.
  const test::DecodedStatus status(response, kBlindId, 120000);
  ASSERT_NE(status.device, nullptr);
  EXPECT_FALSE(status.device->is_stopped);
  EXPECT_FLOAT_EQ(status.device->target, 100.0f);
  EXPECT_LT(status.device->position, 5.0f) << "it has only just set off";
  EXPECT_EQ(status.next_poll_delay_ms(), (remaining_s + 1u) * 1000u)
      << "the hub adds a second to the blind's own figure";
}

// === 3. An EXECUTE with a right answer to the challenge ===

TEST(FullStackExchange, AnExecuteWithARightAnswerIsConfirmedOnTheFirstTryAndTheAnswerComesInsideTheBlindsWindow) {
  ManualClock clock;
  Rig rig(blind_at(0.0));
  ASSERT_TRUE(rig.initialised());
  IoFrame response{};
  ASSERT_EQ(rig.engine().send_and_receive(execute_closed(), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  EXPECT_EQ(response.cmd, CMD_PRIVATE_RESP);

  ASSERT_EQ(rig.heard().size(), 2u) << "the request once, then the answer to the challenge";
  EXPECT_EQ(rig.heard()[0].wake_frames, LONG_TRAIN_FRAMES);
  EXPECT_EQ(rig.heard()[1].wake_frames, 0u) << "the answer goes out without a train";
  ASSERT_EQ(rig.events().size(), 2u);
  EXPECT_EQ(rig.events()[0].kind, EmulatorEvent::Kind::EXECUTE_CHALLENGED);
  EXPECT_EQ(rig.events()[1].kind, EmulatorEvent::Kind::ANSWER_ACCEPTED);
  EXPECT_LT(rig.events()[1].gap_us, rig.blind().config().answer_window_us) << "inside this blind's 46 ms";
  EXPECT_EQ(rig.blind().accepted_executes(), 1u);
  EXPECT_TRUE(rig.blind().moving(rig.sim_now_us())) << "the blind set off";
}

// === 4. An EXECUTE whose answer is held 47 ms ===

TEST(FullStackExchange, AnAnswerHeldFor47MsIsIgnoredByThisBlindAndTheExchangeEndsWithoutAHang) {
  ManualClock clock;
  Rig rig(blind_at(0.0));
  ASSERT_TRUE(rig.initialised());
  // From the moment the hub's request has left, the host link is 47 ms slow: the answer to the challenge
  // starts that much later than the hub would have sent it.
  rig.set_on_hub_frame([&rig](const fullstack::HeardFrame &frame) {
    if (frame.io.size() > 8 && frame.io[8] == CMD_EXECUTE)
      rig.bridge().set_uplink_delay_us(47000);
  });
  IoFrame response{};
  const ExchangeOutcome outcome = rig.engine().send_and_receive(execute_closed(), response, FREQ_CH2);

  EXPECT_NE(outcome, ExchangeOutcome::SUCCESS_WITH_RESPONSE) << "the blind never confirms";
  EXPECT_EQ(rig.blind().accepted_executes(), 0u);
  size_t too_late = 0;
  for (const EmulatorEvent &event : rig.events()) {
    EXPECT_NE(event.kind, EmulatorEvent::Kind::ANSWER_ACCEPTED);
    if (event.kind == EmulatorEvent::Kind::ANSWER_TOO_LATE) {
      too_late++;
      EXPECT_GT(event.gap_us, 47000u);
    }
  }
  EXPECT_GE(too_late, 1u) << "the blind heard the answer and ignored it";
  EXPECT_LT(rig.sim_now_us(), 30u * 1000u * 1000u) << "the exchange is bounded";
}

// === 5. A short train to a resting blind ===

TEST(FullStackExchange, AShortTrainToARestingBlindIsNotHeardAndTheLongTrainRetryIs) {
  ManualClock clock;
  esphome::test_clock::advance_ms(10000);  // evidence stamps count from boot; 0 means never
  Rig rig(blind_at(30.0));
  ASSERT_TRUE(rig.initialised());
  // The hub believes this low-power blind is travelling (moving evidence a second old), so its first try
  // leads with the short train; the blind rests, so it does not hear it.
  const uint32_t moving_since = esphome::test_clock::peek_ms() - 1000;
  rig.engine().set_target_evidence_provider([moving_since](const uint8_t *, decisions::TargetEvidence &out) {
    out = {moving_since, moving_since, false};
    return true;
  });
  IoFrame response{};
  ASSERT_EQ(rig.engine().send_and_receive(status_request(), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  ASSERT_EQ(rig.heard().size(), 2u) << "a missed first try and a retry";
  EXPECT_EQ(rig.heard()[0].wake_frames, SHORT_TRAIN_FRAMES);
  EXPECT_EQ(rig.heard()[1].wake_frames, LONG_TRAIN_FRAMES);
  ASSERT_EQ(rig.events().size(), 2u);
  EXPECT_EQ(rig.events()[0].kind, EmulatorEvent::Kind::SLEPT_THROUGH);
  EXPECT_EQ(rig.events()[1].kind, EmulatorEvent::Kind::STATUS_ANSWERED);
  // What the miss costs: the short try is 8 strobes and the frame (about 9 ms on air), and the next try's train
  // starts about 110 ms after it ended on this run (the band's reply window plus its retry gap).
  const uint64_t failed_try_us = rig.heard()[0].end_us - rig.heard()[0].first_wake_start_us;
  const uint64_t until_retry_us = rig.heard()[1].first_wake_start_us - rig.heard()[0].end_us;
  EXPECT_LT(failed_try_us, 12000u);
  EXPECT_GT(until_retry_us, 100000u);
  EXPECT_LT(until_retry_us, 130000u);
}

// === 6. The first request is lost ===

TEST(FullStackExchange, TheHubMovesToAnotherChannelAfterAMissAndTheSecondTryIsAnswered) {
  ManualClock clock;
  Rig rig(blind_at(50.0));
  ASSERT_TRUE(rig.initialised());
  rig.blind().lose_next_frames(1);
  IoFrame response{};
  ASSERT_EQ(rig.engine().send_and_receive(status_request(), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);

  ASSERT_EQ(rig.heard().size(), 2u);
  ASSERT_EQ(rig.events().size(), 2u);
  EXPECT_EQ(rig.events()[0].kind, EmulatorEvent::Kind::LOST);
  EXPECT_EQ(rig.events()[1].kind, EmulatorEvent::Kind::STATUS_ANSWERED);
  // The retry goes out on the next channel, the way a VELUX controller retries.
  EXPECT_EQ(rig.heard()[0].freq_hz, hioc_bridle::IO2G4_CH2_HZ);
  EXPECT_EQ(rig.heard()[1].freq_hz, hioc_bridle::IO2G4_CH3_HZ);
  ASSERT_EQ(rig.radio().tx_configs.size(), 2u);
  EXPECT_EQ(rig.radio().tx_configs[0].freq_hz, FREQ_CH2);
  EXPECT_EQ(rig.radio().tx_configs[1].freq_hz, FREQ_CH3);
}

TEST(FullStackExchange, ABlindThatHearsOnlyOneChannelIsAnsweredOnTheRetryAndTheNextCommandStartsThere) {
  ManualClock clock;
  IoBlindEmulator::Config config = blind_at(50.0);
  config.only_freq_hz = hioc_bridle::IO2G4_CH3_HZ;
  Rig rig(config);
  ASSERT_TRUE(rig.initialised());
  IoFrame response{};
  ASSERT_EQ(rig.engine().send_and_receive(status_request(), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  ASSERT_EQ(rig.heard().size(), 2u);
  EXPECT_EQ(rig.events()[0].kind, EmulatorEvent::Kind::LOST) << "deaf on the first channel";
  EXPECT_EQ(rig.events()[1].kind, EmulatorEvent::Kind::STATUS_ANSWERED);
  EXPECT_EQ(rig.heard()[1].freq_hz, hioc_bridle::IO2G4_CH3_HZ);

  ASSERT_EQ(rig.engine().send_and_receive(status_request(), response, FREQ_CH2),
            ExchangeOutcome::SUCCESS_WITH_RESPONSE);
  ASSERT_EQ(rig.heard().size(), 3u) << "the next command is answered on its first try";
  EXPECT_EQ(rig.heard()[2].freq_hz, hioc_bridle::IO2G4_CH3_HZ);
}

// === Determinism ===

TEST(FullStackExchange, TheSameExchangeRunTwiceHasTheSameFramesAtTheSameTimes) {
  auto run = [] {
    ManualClock clock;
    Rig rig(blind_at(0.0));
    EXPECT_TRUE(rig.initialised());
    IoFrame response{};
    EXPECT_EQ(rig.engine().send_and_receive(execute_closed(), response, FREQ_CH2),
              ExchangeOutcome::SUCCESS_WITH_RESPONSE);
    std::vector<uint64_t> trace;
    for (const fullstack::HeardFrame &frame : rig.heard()) {
      trace.push_back(frame.start_us);
      trace.push_back(frame.end_us);
    }
    for (const EmulatorEvent &event : rig.events())
      trace.push_back(event.gap_us + event.reply_gap_us);
    trace.push_back(rig.sim_now_us());
    return trace;
  };
  EXPECT_EQ(run(), run());
}
