/// @file
/// @brief Io868GfskPhy: the modem params against the spec vector, framing against what the software PHY
///        writes to a chip, the UART probe at every bit alignment, airtime and the band defaults.

#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "fake_coprocessor.h"
#include "radio_io868_gfsk_phy.h"
#include "radio_sx1262.h"
#include "bridle_vectors.h"

#include "stubs/radio_test_common.h"
#include "stubs/scripted_spi.h"
#include "stubs/soft_phy_test_driver.h"
#include "test_helpers.h"

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::bridle;

namespace {

using TestableRadioSX1262 = test::TestableSoftPhy<RadioSX1262>;

/// Room for the params of one `set_params`.
constexpr size_t TLV_CAP = 128;
/// The SX1262 TCXO voltage code of 1.8 V.
constexpr uint8_t TCXO_CODE_1_8V = 0x02;

/// The vector called `name`; the test fails if the spec no longer has it.
const Vector &vector_named(const char *name) {
  for (const Vector &v : VECTORS) {
    if (std::strcmp(v.name, name) == 0)
      return v;
  }
  ADD_FAILURE() << "no vector named " << name;
  return VECTORS[0];
}

/// The body of a valid vector: its decoded bytes without msg_type, seq and CRC.
std::vector<uint8_t> body_of(const char *name) {
  const Vector &v = vector_named(name);
  return {v.decoded + 2, v.decoded + v.decoded_len - 2};
}

/// A real 15-byte RS100 challenge (0x3C): a frame of a command the hub knows, with its CRC valid once coded.
const std::vector<uint8_t> kFrame = {0x0E, 0x00, 0xD1, 0xD4, 0xFF, 0x8C, 0x08, 0x3C,
                                     0x3C, 0x45, 0x51, 0x6F, 0xFE, 0x59, 0x80};

/// Wrapped bytes of `frame`.
std::vector<uint8_t> wrapped(const std::vector<uint8_t> &frame) {
  uint8_t out[RADIO_PACKET_BUFFER_SIZE];
  const size_t n = io868_gfsk_phy().wrap(frame.data(), static_cast<uint8_t>(frame.size()), out, sizeof(out));
  return {out, out + n};
}

/// The coded bytes shifted `bits` bits into the stream (idle ones in front, as a line that was high), padded with
/// deterministic noise to `rx_len` bytes: what a radio hands over when the sync word ends mid-cell.
std::vector<uint8_t> raw_at_offset(const std::vector<uint8_t> &coded, unsigned bits) {
  std::vector<bool> stream(bits, true);
  for (uint8_t byte : coded) {
    for (int bit = 7; bit >= 0; bit--)
      stream.push_back(((byte >> bit) & 1U) != 0);
  }
  std::vector<uint8_t> raw(IO868_RX_LEN, 0);
  for (size_t i = 0; i < raw.size() * 8; i++) {
    const bool value = i < stream.size() ? stream[i] : ((i * 2654435761U) >> 7) & 1U;  // noise past the frame
    raw[i / 8] = static_cast<uint8_t>(raw[i / 8] | (value ? 0x80U >> (i % 8) : 0));
  }
  return raw;
}

}  // namespace

// === Modem params ===

TEST(RadioIo868GfskPhy, TheParamsAreTheSpecVectorByteForByteAtTheVectorsBandwidth) {
  TuningConfig tuning;
  tuning.sx1262_rx_bandwidth = SX1262RxBandwidth::BW_117_3_KHZ;
  uint8_t buf[TLV_CAP];
  TlvWriter tlvs(buf, sizeof(buf));
  tlvs.add_u8(static_cast<uint8_t>(ParamId::PHY_PROFILE), static_cast<uint8_t>(io868_gfsk_phy().profile()));
  io868_gfsk_phy().write_params(tlvs, tuning);
  tlvs.add_u8(static_cast<uint8_t>(ParamId::TX_POWER_DBM), 14);
  ASSERT_TRUE(tlvs.ok());
  std::vector<uint8_t> expected = body_of("set_params_io868");
  expected.erase(expected.begin());  // the radio index, which the command carries
  EXPECT_EQ(std::vector<uint8_t>(buf, buf + tlvs.size()), expected);
}

TEST(RadioIo868GfskPhy, TheReceiverBandwidthFollowsTheTuning) {
  TuningConfig tuning;  // the host's default: 58.6 kHz, which its field experience supports
  uint8_t buf[TLV_CAP];
  TlvWriter tlvs(buf, sizeof(buf));
  io868_gfsk_phy().write_params(tlvs, tuning);
  ByteSpan value;
  ASSERT_TRUE(find_tlv(tlvs.span(), static_cast<uint8_t>(ParamId::RX_BW_HZ), value));
  ASSERT_EQ(value.size, 4u);
  EXPECT_EQ(value.data[0] | (value.data[1] << 8) | (value.data[2] << 16), 58600);
  tuning.sx1262_rx_bandwidth = SX1262RxBandwidth::BW_156_2_KHZ;
  TlvWriter again(buf, sizeof(buf));
  io868_gfsk_phy().write_params(again, tuning);
  ASSERT_TRUE(find_tlv(again.span(), static_cast<uint8_t>(ParamId::RX_BW_HZ), value));
  EXPECT_EQ(value.data[0] | (value.data[1] << 8) | (value.data[2] << 16), 156200);
}

TEST(RadioIo868GfskPhy, ItsConstantsMatchTheSoftPhysProbeLengthAndLineRate) {
  EXPECT_EQ(IO868_RX_LEN, SOFT_PHY_RX_PROBE_PACKET_LEN);
  EXPECT_EQ(IO868_BITRATE_BPS, SOFT_PHY_LINE_RATE_BPS);
  EXPECT_LE(SOFT_PHY_MAX_WIRE_FRAME_RAW_BYTES, IO868_RX_LEN)
      << "the longest frame the hub has to recover (a 1W add-controller with its MAC) fits what the radio captures";
}

namespace {

/// The SX1262 behind a Kite, with `change` applied.
template<class F> RadioDescriptor gfsk_radio_with(F change) {
  RadioDescriptor d = FakeCoprocessor::reference_gfsk_radio(0);
  change(d);
  return d;
}

}  // namespace

TEST(RadioIo868GfskPhy, ARadioFitsWhenItOffersTheProfileTheScanAndTheLongestWakeUpPreamble) {
  const Io868GfskPhy &phy = io868_gfsk_phy();
  EXPECT_EQ(phy.profile(), PhyProfile::GFSK_RAW);
  EXPECT_TRUE(phy.radio_fits(FakeCoprocessor::reference_gfsk_radio(0)));
  EXPECT_FALSE(phy.radio_fits(gfsk_radio_with([](RadioDescriptor &d) { d.caps &= ~radio_caps::RX_SCAN; })))
      << "the idle scan is a host-side impossibility";
  EXPECT_FALSE(
      phy.radio_fits(gfsk_radio_with([](RadioDescriptor &d) { d.profiles = phy_profiles::IEEE802154_OQPSK_250K; })));
}

TEST(RadioIo868GfskPhy, ARadioWhosePreambleCapIsBelowTheLongestWakeUpIsNotChosen) {
  const Io868GfskPhy &phy = io868_gfsk_phy();
  EXPECT_TRUE(
      phy.radio_fits(gfsk_radio_with([](RadioDescriptor &d) { d.max_preamble_bytes = LOW_POWER_WAKE_PREAMBLE_MAX; })));
  EXPECT_FALSE(phy.radio_fits(
      gfsk_radio_with([](RadioDescriptor &d) { d.max_preamble_bytes = LOW_POWER_WAKE_PREAMBLE_MAX - 1; })));
}

TEST(RadioIo868GfskPhy, TheBandMustCoverAllThreeChannels) {
  const Io868GfskPhy &phy = io868_gfsk_phy();
  auto fits = [&](uint32_t lo, uint32_t hi) {
    return phy.radio_fits(gfsk_radio_with([&](RadioDescriptor &d) {
      d.band_min_hz = lo;
      d.band_max_hz = hi;
    }));
  };
  EXPECT_TRUE(fits(863000000, 870000000));
  EXPECT_TRUE(fits(FREQ_CH1, FREQ_CH3));
  EXPECT_FALSE(fits(868500000, 870000000));
  EXPECT_FALSE(fits(863000000, 869000000));
  EXPECT_FALSE(fits(2400000000U, 2483500000U));
}

TEST(RadioIo868GfskPhy, ARadioWhosePayloadCannotHoldTheLongestFrameIsNotChosen) {
  EXPECT_FALSE(io868_gfsk_phy().radio_fits(gfsk_radio_with([](RadioDescriptor &d) { d.max_payload = 34; })));
}

// === Frequencies ===

TEST(RadioIo868GfskPhy, TheChannelsAreTheFrequenciesOnAir) {
  for (uint32_t hz : {FREQ_CH1, FREQ_CH2, FREQ_CH3}) {
    EXPECT_EQ(io868_gfsk_phy().air_freq_hz(hz), hz);
    EXPECT_EQ(io868_gfsk_phy().channel_hz(hz), hz);
  }
  EXPECT_EQ(io868_gfsk_phy().air_freq_hz(12345), FREQ_CH2);
  EXPECT_EQ(io868_gfsk_phy().channel_hz(12345), 0u);
}

// === Framing ===

TEST(RadioIo868GfskPhy, WrapIsWhatTheSoftPhyWritesToTheChipForTheSameFrame) {
  ScriptedSpi spi;
  MockPin rst, dio1, busy(false);
  TestableRadioSX1262 radio(&spi, &rst, &dio1, &busy, 0, TCXO_CODE_1_8V);
  ASSERT_TRUE(radio.init());
  RadioTxConfig cfg{};
  cfg.freq_hz = FREQ_CH2;
  cfg.preamble_len = SHORT_PREAMBLE;
  radio.send_packet(kFrame.data(), static_cast<uint8_t>(kFrame.size()), cfg);
  std::vector<uint8_t> written;
  for (const auto &tx : spi.transactions()) {
    if (!tx.empty() && tx[0] == SX1262_WRITE_BUFFER)
      written.assign(tx.begin() + 2, tx.end());  // opcode and offset, then the data
  }
  ASSERT_FALSE(written.empty());
  EXPECT_EQ(wrapped(kFrame), written) << "the bridge's payload is the software PHY's, byte for byte";
}

TEST(RadioIo868GfskPhy, WrappedSizeIsWhatWrapProduces) {
  for (size_t len : {5u, 12u, 15u, 30u, 35u}) {
    const std::vector<uint8_t> frame(len, 0x11);
    EXPECT_EQ(wrapped(frame).size(), io868_gfsk_phy().wrapped_size(static_cast<uint8_t>(len))) << len;
  }
}

TEST(RadioIo868GfskPhy, WrapRefusesAnEmptyFrameAndATooSmallBuffer) {
  uint8_t out[8];
  EXPECT_EQ(io868_gfsk_phy().wrap(kFrame.data(), 0, out, sizeof(out)), 0u);
  EXPECT_EQ(io868_gfsk_phy().wrap(kFrame.data(), static_cast<uint8_t>(kFrame.size()), out, sizeof(out)), 0u);
}

TEST(RadioIo868GfskPhy, UnwrapRecoversTheFrameAtEveryBitAlignmentWithNoiseBehindIt) {
  const auto coded = wrapped(kFrame);
  for (unsigned offset = 0; offset < UART_PROBE_MAX_BIT_OFFSET; offset++) {
    const auto raw = raw_at_offset(coded, offset);
    uint8_t out[RADIO_PACKET_BUFFER_SIZE];
    const UnwrapResult r = io868_gfsk_phy().unwrap(raw.data(), raw.size(), CrcState::NOT_CHECKED, out, sizeof(out));
    ASSERT_EQ(r.status, UnwrapResult::Status::OK) << "offset " << offset;
    ASSERT_EQ(r.frame_len, kFrame.size()) << "offset " << offset;
    EXPECT_EQ(std::vector<uint8_t>(out, out + r.frame_len), kFrame) << "offset " << offset;
  }
}

TEST(RadioIo868GfskPhy, NoiseIsNotAFrame) {
  std::vector<uint8_t> noise(IO868_RX_LEN);
  for (size_t i = 0; i < noise.size(); i++)
    noise[i] = static_cast<uint8_t>(0x5A + i * 37);
  uint8_t out[RADIO_PACKET_BUFFER_SIZE];
  EXPECT_EQ(io868_gfsk_phy().unwrap(noise.data(), noise.size(), CrcState::NOT_CHECKED, out, sizeof(out)).status,
            UnwrapResult::Status::BAD_CRC);
  EXPECT_EQ(io868_gfsk_phy().unwrap(noise.data(), 0, CrcState::NOT_CHECKED, out, sizeof(out)).status,
            UnwrapResult::Status::TOO_SHORT);
}

TEST(RadioIo868GfskPhy, AFrameWithAWrongCrcIsNotAFrame) {
  auto coded = wrapped(kFrame);
  coded[coded.size() - 2] = static_cast<uint8_t>(coded[coded.size() - 2] ^ 0x18);  // a bit of a CRC cell
  const auto raw = raw_at_offset(coded, 0);
  uint8_t out[RADIO_PACKET_BUFFER_SIZE];
  EXPECT_EQ(io868_gfsk_phy().unwrap(raw.data(), raw.size(), CrcState::NOT_CHECKED, out, sizeof(out)).status,
            UnwrapResult::Status::BAD_CRC);
}

TEST(RadioIo868GfskPhy, AnOutputBufferThatIsTooSmallIsTooLong) {
  const auto raw = raw_at_offset(wrapped(kFrame), 3);
  uint8_t out[4];
  EXPECT_EQ(io868_gfsk_phy().unwrap(raw.data(), raw.size(), CrcState::NOT_CHECKED, out, sizeof(out)).status,
            UnwrapResult::Status::TOO_LONG);
}

// === Transmission ===

TEST(RadioIo868GfskPhy, TheShapeIsTheHubsPreambleAndNeverAStrobe) {
  for (TxWake wake : {TxWake::NONE, TxWake::SHORT, TxWake::LONG}) {
    const TxShape shape = io868_gfsk_phy().tx_shape(wake, 1024);
    EXPECT_EQ(shape.preamble_bytes, 1024);
    EXPECT_FALSE(shape.has_strobe);
  }
  EXPECT_EQ(io868_gfsk_phy().tx_shape(TxWake::NONE, 0).preamble_bytes, SHORT_PREAMBLE)
      << "a preamble the hub left at 0 is sent as the short one, always explicit";
}

TEST(RadioIo868GfskPhy, AirTimeIsWhatTheSoftPhysModelGivesForEveryPreambleTheHubSends) {
  for (uint16_t preamble : {8, 12, 48, 1024, 4096}) {
    for (uint8_t len : {5, 12, 15, 30, 35}) {
      const TxShape shape = io868_gfsk_phy().tx_shape(TxWake::NONE, preamble);
      EXPECT_EQ(io868_gfsk_phy().tx_air_time_us(shape, io868_gfsk_phy().wrapped_size(len)),
                io868_tx_air_time_us(preamble, len))
          << preamble << " / " << int(len);
    }
  }
}

TEST(RadioIo868GfskPhy, ADefaultPreambleCountsAsTheRadiosEight) {
  const TxShape shape = io868_gfsk_phy().tx_shape(TxWake::NONE, 0);
  EXPECT_EQ(io868_gfsk_phy().tx_air_time_us(shape, io868_gfsk_phy().wrapped_size(12)),
            io868_tx_air_time_us(SHORT_PREAMBLE, 12));
}

// === Listening and band defaults ===

TEST(RadioIo868GfskPhy, TheIdleScanIsTheSpecsVector) {
  const Vector &v = vector_named("rx_scan_io868_idle");
  const std::vector<uint8_t> body(v.decoded + 2, v.decoded + v.decoded_len - 2);  // radio, dwell, hold, n, freqs
  const ScanSpec scan = io868_gfsk_phy().idle_scan();
  ASSERT_EQ(body[5], scan.num_freqs);
  EXPECT_EQ(body[1] | (body[2] << 8), scan.dwell_us);
  EXPECT_EQ(body[3] | (body[4] << 8), scan.hold_us);
  for (uint8_t i = 0; i < scan.num_freqs; i++) {
    const uint8_t *f = &body[6 + 4 * i];
    EXPECT_EQ(static_cast<uint32_t>(f[0] | (f[1] << 8) | (f[2] << 16) | (static_cast<uint32_t>(f[3]) << 24)),
              scan.freqs_hz[i]);
  }
}

TEST(RadioIo868GfskPhy, AFrameIsArrivingFromItsPreambleOn) {
  EXPECT_EQ(io868_gfsk_phy().frame_arriving_stage(), FrameStage::PREAMBLE);
}

TEST(RadioIo868GfskPhy, TheBandDefaultsComeFromTheSx1262TuningFields) {
  TuningConfig tuning;
  tuning.sx1262_discovery_hop_slice_ms = 77;
  tuning.lbt_rssi_threshold_dbm = -85;
  tuning.sx1262_response_preamble = 11;
  EXPECT_EQ(io868_gfsk_phy().hop_dwell_ms(tuning), 77);
  EXPECT_EQ(io868_gfsk_phy().cca_threshold_dbm(tuning), -85);
  EXPECT_EQ(io868_gfsk_phy().response_preamble(tuning), 11);
  EXPECT_EQ(io868_gfsk_phy().default_start_preamble(), SOFT_PHY_START_PREAMBLE);
}

TEST(RadioIo868GfskPhy, KeepsTheHubsExchangeDefaults) {
  const Io868GfskPhy &phy = io868_gfsk_phy();
  TuningConfig tuning;
  EXPECT_FALSE(phy.low_power_by_default());
  EXPECT_FALSE(phy.retries_rotate_channel());
  EXPECT_EQ(phy.start_response_wait_ms(tuning), tuning.exchange_start_response_wait_ms);
  EXPECT_EQ(phy.retry_gap_ms(tuning), static_cast<uint32_t>(EXCHANGE_RETRY_DELAY_MS));
}
