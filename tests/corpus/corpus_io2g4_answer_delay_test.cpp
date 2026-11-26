/// @file corpus_io2g4_answer_delay_test.cpp
/// @brief How long after a challenge a 2.4 GHz blind was seen to accept an answer, from the captures.
///
/// A bench tool answered a real blind's 0x3C after a delay the co-processor timed. The blind accepted the answer
/// at 46 ms and ignored it at 47 ms. The captures stamp each frame at the end of its SFD, so the delay is the
/// 0x3D's start (its stamp less the preamble and SFD) minus the 0x3C's last bit. The result is one blind under
/// bench conditions, which is why the captures are named for the delay.
/// @ingroup hioc_hub

#include "corpus_generated.h"
#include "corpus_test_helpers.h"
#include "proto_constants.h"
#include "proto_frame.h"
#include "radio_io2g4_phy.h"
#include "timed_replay.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::bridle;

namespace {

/// Preamble (4 bytes) and SFD (1 byte) at the 32 µs a byte of 802.15.4: from a frame's start to its SFD stamp.
constexpr uint64_t PREAMBLE_AND_SFD_US = 5 * IO2G4_BYTE_US;

/// Microseconds from the last bit of the capture's 0x3C to the first bit of the 0x3D that follows it.
int64_t answer_delay_us(const corpus::CorpusCapture &capture) {
  const corpus::CorpusFrame *challenge = nullptr;
  for (uint8_t i = 0; i < capture.frame_count; i++) {
    const corpus::CorpusFrame &cf = capture.frames[i];
    const IoFrame frame = corpus_test::parse_capture_frame(cf);
    if (frame.cmd == CMD_CHALLENGE_REQ && !cf.tx) {
      challenge = &cf;
    } else if (frame.cmd == CMD_CHALLENGE_RESP && cf.tx && challenge != nullptr) {
      const uint64_t psdu_bytes = IO2G4_PSDU_HEADER_SIZE + corpus_test::wire_len(*challenge) + IO2G4_FCS_SIZE;
      const uint64_t challenge_end = corpus_test::stamp_us(*challenge) + (1 + psdu_bytes) * IO2G4_BYTE_US;
      return static_cast<int64_t>(corpus_test::stamp_us(cf) - PREAMBLE_AND_SFD_US) -
             static_cast<int64_t>(challenge_end);
    }
  }
  return -1;
}

}  // namespace

TEST(CorpusIo2g4AnswerDelay, TheBlindAcceptedAnAnswer46MsAfterItsChallenge) {
  const corpus::CorpusCapture *capture =
      corpus_test::capture_by_id("velux_skylight_blind_exchange_execute_answer_46ms_2g4");
  ASSERT_NE(capture, nullptr);
  EXPECT_EQ(answer_delay_us(*capture), 46000);
  EXPECT_EQ(capture->outcome, corpus::ExchangeOutcome::SUCCESS);
  EXPECT_EQ(corpus_test::parse_capture_frame(capture->frames[capture->frame_count - 1]).cmd, CMD_PRIVATE_RESP)
      << "the blind confirmed";
}

TEST(CorpusIo2g4AnswerDelay, TheBlindIgnoredAnAnswer47MsAfterItsChallenge) {
  const corpus::CorpusCapture *capture =
      corpus_test::capture_by_id("velux_skylight_blind_exchange_execute_answer_47ms_2g4");
  ASSERT_NE(capture, nullptr);
  EXPECT_EQ(answer_delay_us(*capture), 47000);
  EXPECT_EQ(capture->outcome, corpus::ExchangeOutcome::FAILURE);
  EXPECT_EQ(corpus_test::parse_capture_frame(capture->frames[capture->frame_count - 1]).cmd, CMD_CHALLENGE_RESP)
      << "nothing follows the answer";
}
