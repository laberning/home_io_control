/// @file corpus_rain_limitation_test.cpp
/// @brief The rain decision applied to real limitation replies from the corpus.
///
/// A VELUX INTEGRA window with a wired rain sensor answers the minimum-limitation read with 0 %
/// dry and 93 % under rain; a Somfy awning answers in a layout whose first data byte is not the
/// main parameter, so the same rule must call it unknown.

#include "corpus_generated.h"
#include "decisions_status.h"
#include "proto_codecs.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

#include <optional>

using namespace esphome::home_io_control;

namespace {

/// Decode a capture frame (no CRC on the wire in these captures) and apply the rain decision: true for rain, false
/// for dry, nullopt for no reading.
std::optional<bool> rain_of(const uint8_t *bytes, size_t len) {
  IoFrame frame;
  EXPECT_TRUE(parse(bytes, static_cast<uint8_t>(len), frame));
  EXPECT_EQ(frame.cmd, CMD_LIMITATION_STATUS_RESP);
  LimitationStatus status;
  EXPECT_TRUE(decode_limitation_status(frame, status));
  return decisions::rain_from_limitation_reply(status);
}

template<size_t N> std::optional<bool> rain_of(const uint8_t (&bytes)[N]) { return rain_of(bytes, N); }

}  // namespace

TEST(CorpusRainLimitation, WindowReadsDryThenRainOnTheMinimumLimit) {
  using namespace corpus;
  // Frames 0 and 2 answer the minimum request, frames 1 and 3 the maximum.
  EXPECT_EQ(rain_of(velux_window_probe_limitation_rain_sx1262_frame0_bytes), false);
  EXPECT_EQ(rain_of(velux_window_probe_limitation_rain_sx1262_frame2_bytes), true)
      << "the wet minimum limit is the 93 % ventilation position, with originator and time both 00";
}

TEST(CorpusRainLimitation, SomfyAwningReplyIsUnknownAtEveryPosition) {
  using namespace corpus;
  EXPECT_EQ(rain_of(somfy_awning_probe_limitation_sx1262_frame1_bytes), std::nullopt);
  EXPECT_EQ(rain_of(somfy_awning_probe_limitation_sx1262_frame5_bytes), std::nullopt);
  EXPECT_EQ(rain_of(somfy_awning_probe_limitation_sx1262_frame9_bytes), std::nullopt);
}
