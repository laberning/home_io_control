/// @file
/// @brief The blind model on its own: what it answers, when, and what it does not hear. No simulator.
/// @ingroup hioc_hub

#include <gtest/gtest.h>

#include "corpus_generated.h"
#include "corpus_test_helpers.h"
#include "io_blind_emulator.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_frame.h"
#include "radio_io2g4_phy.h"
#include "test_helpers.h"

using namespace esphome::home_io_control;
// `bridle::` alone is ambiguous here: the global ::bridle (the Bridle release) is visible too.
namespace hioc_bridle = esphome::home_io_control::bridle;
using fullstack::EmulatedReply;
using fullstack::EmulatorEvent;
using fullstack::HeardFrame;
using fullstack::IoBlindEmulator;

namespace {

constexpr uint64_t REQUEST_END_US = 1000000;

IoBlindEmulator::Config config() { return fullstack::default_blind_config(); }

HeardFrame heard(const IoFrame &f, uint64_t end_us, uint32_t wake_frames) {
  HeardFrame h;
  uint8_t buf[FRAME_MAX_SIZE];
  const uint8_t n = serialize(f, buf, sizeof(buf));
  h.io.assign(buf, buf + n);
  h.end_us = end_us;
  h.start_us = end_us - 1000;
  h.freq_hz = hioc_bridle::IO2G4_CH2_HZ;
  h.wake_frames = wake_frames;
  return h;
}

IoFrame status_read() {
  IoFrame f{};
  create_get_status(f, test::OWN_ID, test::DST_ID, true);
  return f;
}

IoFrame execute_closed() {
  IoFrame f{};
  create_execute_position(f, test::OWN_ID, test::DST_ID, true, 100);
  return f;
}

/// The 0x3D the hub would send for `challenge` to `request`, with CTRL1 LOW_POWER set when asked.
HeardFrame answer(const EmulatedReply &challenge, const IoFrame &request, uint64_t start_after_challenge_us,
                  const uint8_t *key = test::TEST_SYSTEM_KEY, bool low_power = false) {
  IoFrame c{};
  parse(challenge.io.data(), static_cast<uint8_t>(challenge.io.size()), c);
  IoFrame f{};
  create_challenge_resp(f, test::DST_ID, test::OWN_ID, c.data, request, key);
  if (low_power)
    f.ctrl1 |= CTRL1_LOW_POWER;
  HeardFrame h = heard(f, challenge.end_us() + start_after_challenge_us + 1000, 0);
  h.start_us = challenge.end_us() + start_after_challenge_us;
  return h;
}

uint16_t u16(const std::vector<uint8_t> &data, size_t at) {
  return static_cast<uint16_t>((data[at] << 8) | data[at + 1]);
}

}  // namespace

TEST(IoBlindEmulator, ARestingBlindAnswersAStatusReadWithItsPositionAndOneSecondRemaining) {
  IoBlindEmulator blind(config());
  const auto replies = blind.hear(heard(status_read(), REQUEST_END_US, 510));
  ASSERT_EQ(replies.size(), 1u);
  IoFrame f{};
  ASSERT_TRUE(parse(replies[0].io.data(), static_cast<uint8_t>(replies[0].io.size()), f));
  EXPECT_EQ(f.cmd, CMD_PRIVATE_RESP);
  EXPECT_EQ(f.ctrl0, 0x96) << "an end frame with 14 data bytes";
  ASSERT_EQ(f.data_len, 14);
  const std::vector<uint8_t> data(f.data, f.data + f.data_len);
  EXPECT_EQ(data[0], 0x05) << "stopped";
  EXPECT_EQ(data[1], 0x60);
  EXPECT_EQ(u16(data, 2), 0x6400);
  EXPECT_EQ(u16(data, 4), 0x6400);
  EXPECT_EQ(u16(data, 6), 1);
  EXPECT_EQ((std::vector<uint8_t>(data.begin() + 11, data.end())), (std::vector<uint8_t>{0x01, 0x00, 0x00}));
}

TEST(IoBlindEmulator, TheReplyStartsSoThatItsSfdEnds2800UsAfterTheRequest) {
  IoBlindEmulator blind(config());
  const auto replies = blind.hear(heard(status_read(), REQUEST_END_US, 510));
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].start_us + 160, REQUEST_END_US + 2800);
  EXPECT_EQ(replies[0].freq_hz, hioc_bridle::IO2G4_CH2_HZ) << "on the carrier it was asked on";
  EXPECT_EQ(blind.events().back().reply_gap_us, 2640u);
}

TEST(IoBlindEmulator, AMovingBlindsRemainingSecondsFallAndItsPositionRises) {
  IoBlindEmulator blind(config());
  blind.set_off(0xC800, 0);
  uint16_t last_remaining = 0xFFFF;
  uint16_t last_position = 0;
  for (int step = 0; step < 10; step++) {
    const std::vector<uint8_t> data = blind.status_data(static_cast<uint64_t>(step) * 2500000u);
    EXPECT_EQ(data[0], 0x04);
    EXPECT_EQ(u16(data, 2), 0xC800);
    EXPECT_LT(u16(data, 6), last_remaining);
    EXPECT_GE(u16(data, 4), last_position);
    last_remaining = u16(data, 6);
    last_position = u16(data, 4);
  }
  const std::vector<uint8_t> arrived = blind.status_data(80u * 1000000u);
  EXPECT_EQ(arrived[0], 0x05);
  EXPECT_EQ(u16(arrived, 4), 0xC800);
  EXPECT_EQ(u16(arrived, 6), 1);
}

TEST(IoBlindEmulator, AShortTrainIsNotHeardByASleepingBlindAndALongOneIs) {
  IoBlindEmulator blind(config());
  EXPECT_TRUE(blind.hear(heard(status_read(), REQUEST_END_US, 8)).empty());
  EXPECT_EQ(blind.events().back().kind, EmulatorEvent::Kind::SLEPT_THROUGH);
  EXPECT_EQ(blind.hear(heard(status_read(), REQUEST_END_US + 1000000, 510)).size(), 1u);
}

TEST(IoBlindEmulator, AShortTrainIsHeardWhileTheBlindTravelsAndForAWhileAfterAFrameAddressedToIt) {
  IoBlindEmulator blind(config());
  blind.set_off(0xC800, REQUEST_END_US - 2000000);
  EXPECT_EQ(blind.hear(heard(status_read(), REQUEST_END_US, 8)).size(), 1u) << "travelling";

  IoBlindEmulator resting(config());
  EXPECT_EQ(resting.hear(heard(status_read(), REQUEST_END_US, 510)).size(), 1u);
  EXPECT_EQ(resting.hear(heard(status_read(), REQUEST_END_US + 4000000, 8)).size(), 1u) << "addressed 4 s ago";
  EXPECT_TRUE(resting.hear(heard(status_read(), REQUEST_END_US + 20000000, 8)).empty()) << "asleep again";
}

TEST(IoBlindEmulator, ARightAnswerWithinTheWindowIsConfirmedAndMovesTheBlind) {
  IoBlindEmulator blind(config());
  const IoFrame request = execute_closed();
  const auto challenge = blind.hear(heard(request, REQUEST_END_US, 510));
  ASSERT_EQ(challenge.size(), 1u);
  IoFrame c{};
  ASSERT_TRUE(parse(challenge[0].io.data(), static_cast<uint8_t>(challenge[0].io.size()), c));
  EXPECT_EQ(c.cmd, CMD_CHALLENGE_REQ);
  EXPECT_EQ(c.data_len, 6);

  const auto ack = blind.hear(answer(challenge[0], request, 46000));
  ASSERT_EQ(ack.size(), 1u);
  EXPECT_EQ(blind.events().back().kind, EmulatorEvent::Kind::ANSWER_ACCEPTED);
  EXPECT_EQ(blind.events().back().gap_us, 46000u) << "the edge itself is still inside";
  EXPECT_EQ(blind.accepted_executes(), 1u);
  EXPECT_TRUE(blind.moving(challenge[0].end_us() + 46000 + 10000000));
}

TEST(IoBlindEmulator, AnAnswerOneMicrosecondPastTheWindowIsIgnored) {
  IoBlindEmulator blind(config());
  const IoFrame request = execute_closed();
  const auto challenge = blind.hear(heard(request, REQUEST_END_US, 510));
  EXPECT_TRUE(blind.hear(answer(challenge[0], request, 46001)).empty());
  EXPECT_EQ(blind.events().back().kind, EmulatorEvent::Kind::ANSWER_TOO_LATE);
  EXPECT_EQ(blind.accepted_executes(), 0u);
}

TEST(IoBlindEmulator, AnAnswerUnderAWrongKeyIsIgnored) {
  IoBlindEmulator blind(config());
  const IoFrame request = execute_closed();
  const auto challenge = blind.hear(heard(request, REQUEST_END_US, 510));
  uint8_t wrong[16] = {};
  EXPECT_TRUE(blind.hear(answer(challenge[0], request, 10000, wrong)).empty());
  EXPECT_EQ(blind.events().back().kind, EmulatorEvent::Kind::ANSWER_WRONG);
}

TEST(IoBlindEmulator, AFrameForAnotherNodeAndALostFrameDrawNothing) {
  IoBlindEmulator blind(config());
  IoFrame other{};
  create_get_status(other, test::OWN_ID, test::OWN_ID, true);
  EXPECT_TRUE(blind.hear(heard(other, REQUEST_END_US, 510)).empty());
  EXPECT_TRUE(blind.events().empty());
  blind.lose_next_frames(1);
  EXPECT_TRUE(blind.hear(heard(status_read(), REQUEST_END_US, 510)).empty());
  EXPECT_EQ(blind.events().back().kind, EmulatorEvent::Kind::LOST);
  EXPECT_EQ(blind.hear(heard(status_read(), REQUEST_END_US + 1000000, 510)).size(), 1u);
}

TEST(IoBlindEmulator, TheChallengesAreTheSameOnEveryRun) {
  IoBlindEmulator a(config());
  IoBlindEmulator b(config());
  const IoFrame request = execute_closed();
  EXPECT_EQ(a.hear(heard(request, REQUEST_END_US, 510))[0].io, b.hear(heard(request, REQUEST_END_US, 510))[0].io);
}

TEST(IoBlindEmulator, TheModelAcceptsAnAnswerWithOrWithoutTheLowPowerBitBecauseNoRealBlindHasBeenTested) {
  for (const bool low_power : {false, true}) {
    IoBlindEmulator blind(config());
    const IoFrame request = execute_closed();
    const auto challenge = blind.hear(heard(request, REQUEST_END_US, 510));
    ASSERT_EQ(challenge.size(), 1u);
    EXPECT_EQ(blind.hear(answer(challenge[0], request, 10000, test::TEST_SYSTEM_KEY, low_power)).size(), 1u)
        << "low_power=" << low_power << ": a decision of the model, not evidence about a real blind";
    EXPECT_EQ(blind.events().back().kind, EmulatorEvent::Kind::ANSWER_ACCEPTED);
  }
}

namespace {

/// Runs one EXECUTE of the given command through the challenge and a right, timely answer.
IoBlindEmulator execute_and_answer(IoBlindEmulator::Config c, const IoFrame &request) {
  IoBlindEmulator blind(c);
  const auto challenge = blind.hear(heard(request, REQUEST_END_US, 510));
  static_cast<void>(blind.hear(answer(challenge[0], request, 10000)));
  return blind;
}

}  // namespace

TEST(IoBlindEmulator, AStopFreezesATravellingBlindWhereItIs) {
  IoBlindEmulator::Config c = config();
  c.position_pct = 0.0;
  IoBlindEmulator blind(c);
  blind.set_off(0xC800, 0);
  IoFrame stop{};
  ASSERT_TRUE(create_execute_command(stop, test::OWN_ID, test::DST_ID, true, CoverCommand::STOP));
  const uint64_t at_us = 10 * 1000000ull;
  const auto challenge = blind.hear(heard(stop, at_us, 8));  // travelling: the short train is heard
  ASSERT_EQ(challenge.size(), 1u);
  const uint16_t where = blind.current_raw(at_us + 20000);
  static_cast<void>(blind.hear(answer(challenge[0], stop, 10000)));
  EXPECT_EQ(blind.events().back().kind, EmulatorEvent::Kind::ANSWER_ACCEPTED);
  EXPECT_FALSE(blind.moving(at_us + 30 * 1000000ull));
  EXPECT_NEAR(blind.current_raw(at_us + 30 * 1000000ull), where, 1000) << "it stays about where the stop reached it";
  EXPECT_LT(blind.current_raw(at_us + 30 * 1000000ull), 0xC800);
}

TEST(IoBlindEmulator, AMainParameterItDoesNotUnderstandMovesNothingAndIsNotConfirmed) {
  IoFrame favourite{};
  ASSERT_TRUE(create_execute_command(favourite, test::OWN_ID, test::DST_ID, true, CoverCommand::FAVORITE));
  IoBlindEmulator::Config c = config();
  c.position_pct = 30.0;
  const IoBlindEmulator blind = execute_and_answer(c, favourite);
  EXPECT_EQ(blind.events().back().kind, EmulatorEvent::Kind::UNEXPECTED);
  EXPECT_EQ(blind.accepted_executes(), 0u);
  EXPECT_FALSE(blind.moving(REQUEST_END_US + 1000000));
  EXPECT_EQ(blind.current_raw(REQUEST_END_US + 1000000), 30 * 512);
}

// === The model against the real replies ===

namespace {

/// The data of the 0x04 in the corpus capture `id` (its last rx frame).
std::vector<uint8_t> captured_reply_data(const char *id) {
  const corpus::CorpusCapture *capture = corpus_test::capture_by_id(id);
  EXPECT_NE(capture, nullptr) << id;
  std::vector<uint8_t> data;
  if (capture == nullptr)
    return data;
  for (uint8_t i = 0; i < capture->frame_count; i++) {
    if (capture->frames[i].tx)
      continue;
    const IoFrame f = corpus_test::parse_capture_frame(capture->frames[i]);
    data.assign(f.data, f.data + f.data_len);
  }
  return data;
}

}  // namespace

TEST(IoBlindEmulator, ARestingStatusReplyIsTheCapturedOneUpToTheFewRawUnitsTheCaptureDiffersBy) {
  const std::vector<uint8_t> captured = captured_reply_data("velux_skylight_blind_statuspoll_hub_request_resting_2g4");
  ASSERT_EQ(captured.size(), 14u);
  IoBlindEmulator::Config c = config();
  c.position_pct = 47.0;
  const std::vector<uint8_t> model = IoBlindEmulator(c).status_data(0);
  ASSERT_EQ(model.size(), captured.size());
  for (const size_t i : {0u, 1u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u})
    EXPECT_EQ(model[i], captured[i]) << "byte " << i;
  EXPECT_NEAR(u16(model, 4), u16(captured, 4), STATUS_POS_TOLERANCE_RAW)
      << "the real blind stops 31 raw units past its target";
  EXPECT_NEAR(u16(model, 2), u16(captured, 2), STATUS_POS_TOLERANCE_RAW);
}

TEST(IoBlindEmulator, ASettingOffStatusReplyIsTheCapturedOneAtTheSameMomentOfTheTravel) {
  const std::vector<uint8_t> captured =
      captured_reply_data("velux_skylight_blind_statuspoll_hub_request_setting_off_2g4");
  ASSERT_EQ(captured.size(), 14u);
  IoBlindEmulator::Config c = config();
  c.position_pct = 0.0;
  IoBlindEmulator blind(c);
  blind.set_off(0xC800, 0);
  const std::vector<uint8_t> model = blind.status_data(2180000);  // 2.18 s into a 72 s travel: 3 % closed
  ASSERT_EQ(model.size(), captured.size());
  for (const size_t i : {0u, 1u, 2u, 3u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u})
    EXPECT_EQ(model[i], captured[i]) << "byte " << i;
  EXPECT_NEAR(u16(model, 4), u16(captured, 4), STATUS_POS_TOLERANCE_RAW);
}
