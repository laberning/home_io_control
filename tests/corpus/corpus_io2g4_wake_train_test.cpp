/// @file corpus_io2g4_wake_train_test.cpp
/// @brief The 2.4 GHz PHY's wake-up trains against the trains the controllers actually sent.
///
/// Every START frame in a `phy: io_2g4` capture carries the wake train captured ahead of it. The
/// level the controller used follows the same rule the hub applies (claim: START plus LOW_POWER
/// gets the long train, START alone the short one), so Io2g4Phy::tx_shape() for that level must
/// reproduce what the sniffer saw: a train that counts down to 0, no longer than the shape's,
/// and, where the capturing tool stamped the frames, the shape's period and gap within the stamp
/// resolution the capture declares (`t_us_stamp`) plus the spread real controllers show. This is the
/// independent check on the timing model the driver's air-time estimate is built from.

#include "corpus_generated.h"
#include "corpus_test_helpers.h"
#include "proto_frame.h"
#include "radio_io2g4_phy.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::bridle;

namespace {

/// How far a controller's main gap may sit from the shape's, on top of the stamp resolution: the
/// corpus has 1025 µs (a KLR 200) and 1036 to 1038 µs (a KLF 200) against the shape's 1036 µs.
constexpr int64_t MAIN_GAP_SPREAD_US = 25;

/// How far a train's span may differ from `intervals × period`, per mille, on top of the stamp
/// resolution: a controller's first interval and a late frame here and there shift the span by up to
/// 0.6 percent over 509 intervals of the captures, and a wrong period (2.4 percent) stays far outside.
constexpr int64_t SPAN_SPREAD_PER_MILLE = 6;

TxWake wake_level_of(const IoFrame &frame) {
  if (!is_start(frame))
    return TxWake::NONE;
  return (frame.ctrl1 & CTRL1_LOW_POWER) != 0 ? TxWake::LONG : TxWake::SHORT;
}

std::vector<const corpus::CorpusCapture *> io2g4_captures() {
  return corpus_test::captures_where([](const corpus::CorpusCapture *c) { return c->phy == corpus::Phy::IO_2G4; });
}

}  // namespace

class CorpusIo2g4WakeTrain : public ::testing::TestWithParam<const corpus::CorpusCapture *> {};

TEST_P(CorpusIo2g4WakeTrain, TheShapeReproducesTheCapturedTrain) {
  const corpus::CorpusCapture *capture = GetParam();
  for (uint8_t i = 0; i < capture->frame_count; i++) {
    const corpus::CorpusFrame &cf = capture->frames[i];
    if (!cf.has_wake_train)
      continue;
    SCOPED_TRACE(::testing::Message() << "capture=" << capture->id << " frame=" << static_cast<int>(i));
    const IoFrame frame = corpus_test::parse_capture_frame(cf);
    ASSERT_TRUE(is_start(frame)) << "a wake train precedes a START frame";

    const TxWake level = wake_level_of(frame);
    const TxShape shape = io2g4_phy().tx_shape(level, 0);
    ASSERT_TRUE(shape.has_strobe);
    const uint32_t count = shape.strobe.count;

    // The train counts down to 0, and the sniffer cannot have seen more frames than the shape
    // has, nor a counter above its first.
    EXPECT_EQ(cf.wake_last_counter, 0u);
    EXPECT_LE(cf.wake_first_counter, count - 1) << "the captured train starts above the shape's first counter";
    EXPECT_LE(cf.wake_frames_captured, cf.wake_first_counter + 1u);
    if (level == TxWake::SHORT && cf.wake_frames_captured == cf.wake_first_counter + 1u &&
        cf.wake_frames_captured == count) {
      EXPECT_EQ(cf.wake_first_counter, count - 1) << "a fully captured short train starts at count - 1";
    }

    if (!cf.has_wake_times || cf.wake_frames_captured < 2 || !cf.has_t_us)
      continue;
    // Period: the stamps of the first and last captured frame are (first_counter - last_counter)
    // periods apart when the capture missed no frame in between.
    if (cf.wake_frames_captured == cf.wake_first_counter + 1u) {
      const int64_t measured_us = static_cast<int64_t>(cf.wake_last_t_us) - cf.wake_first_t_us;
      const int64_t predicted_us = static_cast<int64_t>(cf.wake_first_counter) * shape.strobe.period_ns / 1000;
      const int64_t tolerance_us = capture->stamp_resolution_us + predicted_us * SPAN_SPREAD_PER_MILLE / 1000;
      EXPECT_NEAR(static_cast<double>(measured_us), static_cast<double>(predicted_us),
                  static_cast<double>(tolerance_us))
          << "period over " << cf.wake_first_counter << " intervals";
    }
    // Gap: with stamps at the SFD the START frame lags the last wake frame by the start-to-start gap
    // (both SFDs sit the same distance into their frames). With stamps at the last bit it lags by that
    // gap plus the difference of the two frames' airtimes.
    const uint32_t main_air_us =
        (IO2G4_PHY_OVERHEAD_BYTES + IO2G4_PSDU_HEADER_SIZE + corpus_test::wire_len(cf) + IO2G4_FCS_SIZE) *
        IO2G4_BYTE_US;
    const uint32_t wake_air_us =
        (IO2G4_PHY_OVERHEAD_BYTES + shape.strobe.len_template + IO2G4_FCS_SIZE) * IO2G4_BYTE_US;
    const int64_t measured_gap_us = static_cast<int64_t>(cf.t_us) - cf.wake_last_t_us;
    const int64_t airtime_difference_us =
        capture->stamp_at_sfd ? 0 : static_cast<int64_t>(main_air_us) - static_cast<int64_t>(wake_air_us);
    const int64_t predicted_gap_us = static_cast<int64_t>(shape.strobe.main_gap_ns / 1000) + airtime_difference_us;
    EXPECT_NEAR(static_cast<double>(measured_gap_us), static_cast<double>(predicted_gap_us),
                static_cast<double>(2 * capture->stamp_resolution_us + MAIN_GAP_SPREAD_US))
        << "main frame " << measured_gap_us << " µs after the last wake frame's stamp";
  }
}

TEST(CorpusIo2g4WakeTrainInventory, TheCorpusHasTrainsOfBothLevels) {
  bool short_seen = false;
  bool long_seen = false;
  for (const corpus::CorpusCapture *capture : io2g4_captures()) {
    for (uint8_t i = 0; i < capture->frame_count; i++) {
      if (!capture->frames[i].has_wake_train)
        continue;
      const TxWake level = wake_level_of(corpus_test::parse_capture_frame(capture->frames[i]));
      short_seen = short_seen || level == TxWake::SHORT;
      long_seen = long_seen || level == TxWake::LONG;
    }
  }
  EXPECT_TRUE(short_seen) << "no captured short train left to check tx_shape(SHORT) against";
  EXPECT_TRUE(long_seen) << "no captured long train left to check tx_shape(LONG) against";
}

INSTANTIATE_TEST_SUITE_P(CorpusIo2g4WakeTrain, CorpusIo2g4WakeTrain, ::testing::ValuesIn(io2g4_captures()),
                         corpus_test::capture_name_generator);
