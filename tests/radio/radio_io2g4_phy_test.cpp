/// @file
/// @brief Io2g4Phy: framing, channel map, wake-up shapes, airtime and the band defaults.

#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "fake_coprocessor.h"
#include "radio_io2g4_phy.h"

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::bridle;

namespace {

/// A 12-byte frame of a plausible shape: enough to pass the minimum-length check.
std::vector<uint8_t> frame_of(size_t len) { return std::vector<uint8_t>(len, 0x11); }

/// A received PSDU: header, the frame, two FCS bytes.
std::vector<uint8_t> psdu_of(const std::vector<uint8_t> &frame) {
  std::vector<uint8_t> psdu(IO2G4_PSDU_HEADER, IO2G4_PSDU_HEADER + 3);
  psdu.insert(psdu.end(), frame.begin(), frame.end());
  psdu.push_back(0xAA);
  psdu.push_back(0xBB);
  return psdu;
}

}  // namespace

// === Wrapping ===

TEST(RadioIo2g4Phy, WrapPrependsTheHeaderAndKeepsTheFrame) {
  const auto frame = frame_of(12);
  uint8_t out[64];
  size_t n = io2g4_phy().wrap(frame.data(), 12, out, sizeof(out));
  ASSERT_EQ(n, 15u);
  EXPECT_EQ(0, std::memcmp(out, IO2G4_PSDU_HEADER, 3));
  EXPECT_EQ(0, std::memcmp(out + 3, frame.data(), 12));
}

TEST(RadioIo2g4Phy, WrapRefusesATooSmallBuffer) {
  const auto frame = frame_of(12);
  uint8_t out[14];
  EXPECT_EQ(io2g4_phy().wrap(frame.data(), 12, out, sizeof(out)), 0u);
}

TEST(RadioIo2g4Phy, WrappedSizeIsWhatWrapProduces) {
  const auto frame = frame_of(17);
  uint8_t out[64];
  EXPECT_EQ(io2g4_phy().wrapped_size(17), io2g4_phy().wrap(frame.data(), 17, out, sizeof(out)));
}

TEST(RadioIo2g4Phy, UnwrapRecoversWhatWrapSent) {
  const auto frame = frame_of(20);
  auto psdu = psdu_of(frame);
  uint8_t out[64];
  UnwrapResult r = io2g4_phy().unwrap(psdu.data(), psdu.size(), CrcState::OK, out, sizeof(out));
  ASSERT_EQ(r.status, UnwrapResult::Status::OK);
  ASSERT_EQ(r.frame_len, 20);
  EXPECT_EQ(0, std::memcmp(out, frame.data(), 20));
}

TEST(RadioIo2g4Phy, UnwrapStripsTheFcsAndKeepsTheLastFrameByte) {
  std::vector<uint8_t> frame = frame_of(10);
  frame.back() = 0x77;
  auto psdu = psdu_of(frame);
  uint8_t out[64];
  UnwrapResult r = io2g4_phy().unwrap(psdu.data(), psdu.size(), CrcState::OK, out, sizeof(out));
  ASSERT_EQ(r.status, UnwrapResult::Status::OK);
  EXPECT_EQ(out[r.frame_len - 1], 0x77);
}

TEST(RadioIo2g4Phy, UnwrapRejectsAPayloadTooShortForAFrame) {
  uint8_t out[64];
  // 3 header + FRAME_MIN_SIZE (9) + 2 FCS = 14 bytes is the smallest.
  auto psdu = psdu_of(frame_of(9));
  EXPECT_EQ(io2g4_phy().unwrap(psdu.data(), psdu.size(), CrcState::OK, out, sizeof(out)).status,
            UnwrapResult::Status::OK);
  EXPECT_EQ(io2g4_phy().unwrap(psdu.data(), psdu.size() - 1, CrcState::OK, out, sizeof(out)).status,
            UnwrapResult::Status::TOO_SHORT);
  EXPECT_EQ(io2g4_phy().unwrap(psdu.data(), 0, CrcState::OK, out, sizeof(out)).status, UnwrapResult::Status::TOO_SHORT);
}

TEST(RadioIo2g4Phy, UnwrapReturnsTheThreeBytesOfAHeaderMismatch) {
  auto psdu = psdu_of(frame_of(12));
  psdu[0] = 0x02;
  psdu[1] = 0x99;
  psdu[2] = 0x98;
  uint8_t out[64];
  UnwrapResult r = io2g4_phy().unwrap(psdu.data(), psdu.size(), CrcState::OK, out, sizeof(out));
  ASSERT_EQ(r.status, UnwrapResult::Status::HEADER_MISMATCH);
  EXPECT_EQ(r.header[0], 0x02);
  EXPECT_EQ(r.header[1], 0x99);
  EXPECT_EQ(r.header[2], 0x98);
}

TEST(RadioIo2g4Phy, UnwrapReportsABadFcsBeforeLookingAtTheHeader) {
  auto psdu = psdu_of(frame_of(12));
  psdu[0] = 0x02;
  uint8_t out[64];
  EXPECT_EQ(io2g4_phy().unwrap(psdu.data(), psdu.size(), CrcState::BAD, out, sizeof(out)).status,
            UnwrapResult::Status::BAD_CRC);
}

TEST(RadioIo2g4Phy, UnwrapRefusesAFrameThatDoesNotFitTheOutput) {
  auto psdu = psdu_of(frame_of(30));
  uint8_t out[16];
  EXPECT_EQ(io2g4_phy().unwrap(psdu.data(), psdu.size(), CrcState::OK, out, sizeof(out)).status,
            UnwrapResult::Status::TOO_LONG);
}

// === Channels ===

TEST(RadioIo2g4Phy, EachHubChannelMapsToItsAirFrequencyAndBack) {
  const Io2g4Phy &phy = io2g4_phy();
  EXPECT_EQ(phy.air_freq_hz(FREQ_CH1), 2425000000u);
  EXPECT_EQ(phy.air_freq_hz(FREQ_CH2), 2450000000u);
  EXPECT_EQ(phy.air_freq_hz(FREQ_CH3), 2475000000u);
  for (uint32_t channel : {FREQ_CH1, FREQ_CH2, FREQ_CH3})
    EXPECT_EQ(phy.channel_hz(phy.air_freq_hz(channel)), channel);
}

TEST(RadioIo2g4Phy, AnUnknownChannelLandsOnTheTxChannel) {
  EXPECT_EQ(io2g4_phy().air_freq_hz(123456789), 2450000000u);
  EXPECT_EQ(io2g4_phy().channel_hz(2412000000), 0u) << "an air frequency that is none of the three";
}

// === Wake-up shapes ===

TEST(RadioIo2g4Phy, NoWakeMeansNoStrobeAndTheStandardPreamble) {
  TxShape s = io2g4_phy().tx_shape(TxWake::NONE, 1024);
  EXPECT_FALSE(s.has_strobe);
  EXPECT_EQ(s.preamble_bytes, 0) << "802.15.4 has one preamble; the hub's byte count plays no part";
}

TEST(RadioIo2g4Phy, ShortWakeIsAnEightFrameStrobe) {
  TxShape s = io2g4_phy().tx_shape(TxWake::SHORT, SHORT_PREAMBLE);
  ASSERT_TRUE(s.has_strobe);
  EXPECT_EQ(s.strobe.count, 8);
  EXPECT_EQ(s.preamble_bytes, 0);
}

TEST(RadioIo2g4Phy, LongWakeIsA510FrameStrobe) {
  TxShape s = io2g4_phy().tx_shape(TxWake::LONG, LONG_PREAMBLE);
  ASSERT_TRUE(s.has_strobe);
  EXPECT_EQ(s.strobe.count, 510);
}

TEST(RadioIo2g4Phy, StrobeFramesCountDownToZeroFromATemplateOfThreeBytes) {
  // Spec §5 vector tx_io_wake_strobe: template 00 00 00, a 16-bit counter at offset 1, adjust -1,
  // period and counter unit 1 000 000 ns, main frame 1 036 000 ns after the last strobe start.
  const StrobeSpec &st = io2g4_phy().tx_shape(TxWake::LONG, 0).strobe;
  EXPECT_EQ(st.period_ns, 1000000u);
  EXPECT_EQ(st.main_gap_ns, 1036000u);
  EXPECT_EQ(st.counter_offset, 1);
  EXPECT_EQ(st.counter_width, 2);
  EXPECT_EQ(st.counter_unit_ns, 1000000u);
  EXPECT_EQ(st.counter_adjust, -1);
  ASSERT_EQ(st.len_template, 3);
  EXPECT_EQ(st.template_bytes[0], 0);
  EXPECT_EQ(st.template_bytes[1], 0);
  EXPECT_EQ(st.template_bytes[2], 0);
}

TEST(RadioIo2g4Phy, ShapeOfTheSpecVectorMatchesTheVectorsFields) {
  // The same numbers as vector tx_io_short_wake_strobe (count 8) apart from the count.
  EXPECT_EQ(io2g4_phy().tx_shape(TxWake::SHORT, 0).strobe.period_ns,
            io2g4_phy().tx_shape(TxWake::LONG, 0).strobe.period_ns);
}

// === Airtime, against numbers worked out by hand ===

TEST(RadioIo2g4Phy, AirTimeWithoutAStrobeIsTheMainFrameAlone) {
  // 12-byte payload: (4 preamble + 1 SFD + 1 PHR + 12 + 2 FCS) bytes = 20 bytes x 32 µs.
  EXPECT_EQ(io2g4_phy().tx_air_time_us(io2g4_phy().tx_shape(TxWake::NONE, 0), 12), 640u);
}

TEST(RadioIo2g4Phy, AirTimeOfAShortStrobeAddsSevenPeriodsAndTheGap) {
  // 7 x 1 000 000 ns + 1 036 000 ns = 8 036 000 ns; the main frame is 640 µs = 640 000 ns;
  // together 8 676 000 ns = 8676 µs.
  EXPECT_EQ(io2g4_phy().tx_air_time_us(io2g4_phy().tx_shape(TxWake::SHORT, 0), 12), 8676u);
}

TEST(RadioIo2g4Phy, AirTimeOfALongStrobeAdds509PeriodsAndTheGap) {
  // 509 x 1 000 000 ns + 1 036 000 ns = 510 036 000 ns; + 640 000 = 510 676 000 ns = 510 676 µs:
  // the ~511 ms the exchange budget has to make room for.
  EXPECT_EQ(io2g4_phy().tx_air_time_us(io2g4_phy().tx_shape(TxWake::LONG, 0), 12), 510676u);
}

TEST(RadioIo2g4Phy, AirTimeGrowsThirtyTwoMicrosecondsPerPayloadByte) {
  const TxShape none = io2g4_phy().tx_shape(TxWake::NONE, 0);
  EXPECT_EQ(io2g4_phy().tx_air_time_us(none, 13) - io2g4_phy().tx_air_time_us(none, 12), 32u);
}

// === Listening ===

TEST(RadioIo2g4Phy, IdleScanCyclesThreeChannelsAndHoldsForAnExchange) {
  ScanSpec scan = io2g4_phy().idle_scan();
  EXPECT_EQ(scan.dwell_us, 1500);
  EXPECT_EQ(scan.hold_us, 8000);
  ASSERT_EQ(scan.num_freqs, 3);
  EXPECT_EQ(scan.freqs_hz[0], 2425000000u);
  EXPECT_EQ(scan.freqs_hz[1], 2450000000u);
  EXPECT_EQ(scan.freqs_hz[2], 2475000000u);
}

TEST(RadioIo2g4Phy, AFrameIsArrivingFromTheSfd) { EXPECT_EQ(io2g4_phy().frame_arriving_stage(), FrameStage::SYNC); }

// === What the driver asks the co-processor for ===

TEST(RadioIo2g4Phy, TheRadioNeedsTheProfileTheSfdAndTheBandOfAllThreeChannels) {
  const Io2g4Phy &phy = io2g4_phy();
  EXPECT_EQ(phy.profile(), PhyProfile::IEEE802154_OQPSK_250K);

  uint8_t buf[8];
  TlvWriter w(buf, sizeof(buf));
  phy.write_params(w, TuningConfig{});
  ASSERT_TRUE(w.ok());
  ByteSpan value;
  ASSERT_TRUE(find_tlv(w.span(), static_cast<uint8_t>(ParamId::SFD), value));
  ASSERT_EQ(value.size, 1u);
  EXPECT_EQ(value.data[0], 0x56);

  auto radio = [](uint32_t lo, uint32_t hi, uint32_t caps) {
    RadioDescriptor d = FakeCoprocessor::reference_802154_radio(0);
    d.band_min_hz = lo;
    d.band_max_hz = hi;
    d.caps = caps;
    return d;
  };
  constexpr uint32_t NEEDED = radio_caps::STROBE | radio_caps::RX_SCAN;
  EXPECT_TRUE(phy.radio_fits(radio(2400000000u, 2483500000u, NEEDED)));
  EXPECT_FALSE(phy.radio_fits(radio(2440000000u, 2483500000u, NEEDED))) << "channel 15 is out of band";
  EXPECT_FALSE(phy.radio_fits(radio(2400000000u, 2460000000u, NEEDED))) << "channel 25 is out of band";
  EXPECT_FALSE(phy.radio_fits(radio(2400000000u, 2483500000u, radio_caps::RX_SCAN)))
      << "the wake-up trains need a strobe";
  EXPECT_FALSE(phy.radio_fits(radio(2400000000u, 2483500000u, radio_caps::STROBE))) << "the idle scan is required";
  EXPECT_TRUE(phy.radio_fits(radio(2400000000u, 2483500000u, NEEDED | radio_caps::CCA)))
      << "a check is used when offered, never required";
  RadioDescriptor gfsk = FakeCoprocessor::reference_gfsk_radio(0);
  gfsk.caps = NEEDED;
  EXPECT_FALSE(phy.radio_fits(gfsk)) << "the radio must offer the 802.15.4 profile";
}

// === Band defaults ===

TEST(RadioIo2g4Phy, TheBandDefaultsComeFromTheTuningFields) {
  TuningConfig tuning;
  EXPECT_EQ(io2g4_phy().hop_dwell_ms(tuning), 2);
  EXPECT_EQ(io2g4_phy().cca_threshold_dbm(tuning), -75);
  tuning.io2g4_hop_dwell_ms = 9;
  tuning.io2g4_lbt_rssi_threshold_dbm = -60;
  EXPECT_EQ(io2g4_phy().hop_dwell_ms(tuning), 9);
  EXPECT_EQ(io2g4_phy().cca_threshold_dbm(tuning), -60);
}

TEST(RadioIo2g4Phy, PreamblesAreTheProtocolDefaults) {
  EXPECT_EQ(io2g4_phy().response_preamble(TuningConfig{}), SHORT_PREAMBLE);
  EXPECT_EQ(io2g4_phy().default_start_preamble(), NORMAL_START_PREAMBLE);
}

TEST(RadioIo2g4Phy, TheBandIsDutyCycledAndRetriesRotateWithTheShortWindowAndGap) {
  const Io2g4Phy &phy = io2g4_phy();
  TuningConfig tuning;
  EXPECT_TRUE(phy.low_power_by_default()) << "a receiver at rest ignores a train without the low-power flag";
  EXPECT_TRUE(phy.retries_rotate_channel());
  EXPECT_EQ(phy.start_response_wait_ms(tuning), tuning.io2g4_exchange_start_response_wait_ms);
  EXPECT_EQ(phy.retry_gap_ms(tuning), tuning.io2g4_exchange_retry_gap_ms);
  tuning.io2g4_exchange_start_response_wait_ms = 80;
  tuning.io2g4_exchange_retry_gap_ms = 10;
  EXPECT_EQ(phy.start_response_wait_ms(tuning), 80u);
  EXPECT_EQ(phy.retry_gap_ms(tuning), 10u);
}
