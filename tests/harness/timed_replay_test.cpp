/// @file
/// @brief The timed-replay builder over both time bases (`t_ms` and `t_us`) and both stand-ins.

#include <gtest/gtest.h>

#include "corpus_test_helpers.h"
#include "timed_replay.h"

using namespace esphome::home_io_control;
using esphome::test_clock::ManualClock;

namespace {

/// A hand-built capture of tx, rx, tx, rx with the given stamps in the given base.
struct StampedCapture {
  corpus::CorpusFrame frames[4]{};
  uint8_t bytes[4][9]{};
  corpus::CorpusCapture capture{};

  StampedCapture(const uint32_t stamps[4], bool micros) {
    for (int i = 0; i < 4; i++) {
      frames[i].tx = i % 2 == 0;
      frames[i].bytes = bytes[i];
      frames[i].len = 9;
      bytes[i][0] = static_cast<uint8_t>(i);
      (micros ? frames[i].has_t_us : frames[i].has_t_ms) = true;
      (micros ? frames[i].t_us : frames[i].t_ms) = stamps[i];
    }
    capture.id = "hand_built";
    capture.source_origin = "own-hardware";
    capture.captured_with = "sx1276";
    capture.frames = frames;
    capture.frame_count = 4;
  }
};

const uint8_t FRAME[9] = {};

}  // namespace

TEST(TimedReplay, MillisecondStampsGiveTheGapBetweenTxAndRx) {
  const uint32_t stamps[4] = {1000, 1040, 2000, 2100};
  StampedCapture cap(stamps, false);
  ASSERT_TRUE(corpus_test::capture_is_timed(cap.capture));
  ManualClock clock;
  MockStandIn radio;
  ASSERT_EQ(corpus_test::queue_timed_rx(radio, cap.capture, corpus_test::ReplayAnchor::SEND_INDEX), "");
  uint64_t const start = esphome::test_clock::state().now_us;
  RadioRxPacket packet;
  radio.driver().send_packet(FRAME, sizeof(FRAME), {});
  ASSERT_TRUE(radio.driver().wait_for_packet(packet, 100));
  EXPECT_EQ(esphome::test_clock::state().now_us - start, 40000u) << "40 ms after the send's start";
}

TEST(TimedReplay, MicrosecondStampsGiveTheGapInMicroseconds) {
  const uint32_t stamps[4] = {1000000, 1002500, 1010000, 1012000};
  StampedCapture cap(stamps, true);
  ASSERT_TRUE(corpus_test::capture_is_timed(cap.capture));
  ManualClock clock;
  MockStandIn radio;
  ASSERT_EQ(corpus_test::queue_timed_rx(radio, cap.capture, corpus_test::ReplayAnchor::SEND_INDEX), "");
  uint64_t const start = esphome::test_clock::state().now_us;
  RadioRxPacket packet;
  radio.driver().send_packet(FRAME, sizeof(FRAME), {});
  EXPECT_FALSE(radio.driver().wait_for_packet(packet, 1)) << "2.5 ms is beyond a 1 ms listen";
  ASSERT_TRUE(radio.driver().wait_for_packet(packet, 100));
  EXPECT_EQ(esphome::test_clock::state().now_us - start, 2500u);
}

TEST(TimedReplay, ACaptureWithAFrameWithoutAStampIsNotTimed) {
  const uint32_t stamps[4] = {1, 2, 3, 4};
  StampedCapture cap(stamps, true);
  cap.frames[2].has_t_us = false;
  EXPECT_FALSE(corpus_test::capture_is_timed(cap.capture));
  ManualClock clock;
  MockStandIn radio;
  EXPECT_NE(corpus_test::queue_timed_rx(radio, cap.capture, corpus_test::ReplayAnchor::SEND_INDEX), "");
}

TEST(TimedReplay, AReplyStampedBeforeItsTxIsRefused) {
  const uint32_t stamps[4] = {1000, 900, 2000, 2100};
  StampedCapture cap(stamps, true);
  ManualClock clock;
  MockStandIn radio;
  EXPECT_NE(corpus_test::queue_timed_rx(radio, cap.capture, corpus_test::ReplayAnchor::SEND_INDEX), "");
}

TEST(TimedReplay, TheStandInIsChosenByTheCapturesPhy) {
  ManualClock clock;
  corpus::CorpusCapture c2g4{};
  c2g4.phy = corpus::Phy::IO_2G4;
  c2g4.captured_with = "other";
  auto bridge = corpus_test::make_stand_in(c2g4);
  EXPECT_STREQ(bridge->driver().chip_name(), "bridle");

  corpus::CorpusCapture c868{};
  c868.captured_with = "sx1262";
  EXPECT_STREQ(corpus_test::make_stand_in(c868)->driver().chip_name(), "sx1262");
  c868.captured_with = "sx1276";
  EXPECT_STREQ(corpus_test::make_stand_in(c868)->driver().chip_name(), "MockRadio");
}

TEST(TimedReplay, A2g4CapturesFrequencyIsTheHubsLogicalChannel) {
  corpus::CorpusCapture capture{};
  capture.phy = corpus::Phy::IO_2G4;
  corpus::CorpusFrame cf{};
  cf.freq_hz = 2450000000u;
  EXPECT_EQ(corpus_test::logical_freq(cf, capture), FREQ_CH2);
  cf.freq_hz = 0;
  EXPECT_EQ(corpus_test::logical_freq(cf, capture), 0u);
  capture.phy = corpus::Phy::IO_868;
  cf.freq_hz = FREQ_CH3;
  EXPECT_EQ(corpus_test::logical_freq(cf, capture), FREQ_CH3);
}
