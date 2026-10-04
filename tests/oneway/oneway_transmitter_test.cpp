#include "oneway_transmitter.h"

#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_timing.h"
#include "tuning_config.h"

#include "test_helpers.h"

#include <esphome/core/application.h>

#include <cstring>
#include <vector>

using namespace esphome::home_io_control;

// ============================================================================
// OneWayTransmitter burst-cadence tests
// ============================================================================
// Repetition is 1W's only reliability mechanism — nothing replies, so nothing can report a
// missed frame. That makes two properties load-bearing and both are pinned here: the burst
// happens at all, and every copy in it is byte-identical. Four copies carrying four sequences
// would be four commands to a device, which accepts one and rejects the rest as replays.

namespace {

const uint8_t CONTROLLER_SRC[NODE_ID_SIZE] = {0x9D, 0x60, 0x85};

/// One recorded transmit call.
struct SentFrame {
  std::vector<uint8_t> bytes;
  uint32_t freq;
  uint16_t preamble;
};

/// Records what the transmitter asked for, so the cadence can be asserted without a radio.
class TransmitRecorder {
 public:
  OneWayTransmitFn fn() {
    return [this](const IoFrame &frame, uint32_t freq, uint16_t preamble) {
      if (this->fail_from_ >= 0 && static_cast<int>(this->sent.size()) >= this->fail_from_)
        return false;
      uint8_t buf[FRAME_MAX_WIRE_SIZE] = {0};
      const uint8_t len = serialize(frame, buf, sizeof(buf));
      this->sent.push_back(SentFrame{std::vector<uint8_t>(buf, buf + len), freq, preamble});
      return true;
    };
  }

  std::vector<SentFrame> sent;
  /// Index from which the radio starts refusing; -1 means it never does.
  int fail_from_{-1};
};

IoFrame make_stop_frame() {
  IoFrame frame{};
  EXPECT_TRUE(create_1w_execute_command(frame, CONTROLLER_SRC, DeviceType::AWNING, CoverCommand::STOP, 0x1234,
                                        test::TEST_SYSTEM_KEY));
  return frame;
}

/// `bytes` with CTRL1 (index 1) zeroed -- the one byte a burst's copies may legitimately differ on
/// (ADR 0038), so two copies meant to be otherwise identical can be compared directly.
std::vector<uint8_t> bytes_without_ctrl1(const std::vector<uint8_t> &bytes) {
  std::vector<uint8_t> out = bytes;
  out[1] = 0;
  return out;
}

/// Shared fixture: one default-constructed TuningConfig, so every test below constructs an
/// OneWayTransmitter with `&this->tuning_` instead of forty inline copies.
class OneWayTransmitterTest : public ::testing::Test {
 protected:
  void SetUp() override { esphome::test_hal::reset_delays(); }

  TuningConfig tuning_{};
};

}  // namespace

TEST_F(OneWayTransmitterTest, SendsExactlyFourCopiesOnChannelTwo) {
  TransmitRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn(), &tuning_);

  EXPECT_TRUE(transmitter.send_burst(make_stop_frame(), OneWayPowerClass::LEGACY_LONG));

  ASSERT_EQ(recorder.sent.size(), ONEWAY_BURST_REPEATS) << "a 1W command is a fixed-size burst, not a single frame";
  for (const auto &sent : recorder.sent)
    EXPECT_EQ(sent.freq, FREQ_CH2) << "1W uses only channel 2";
}

TEST_F(OneWayTransmitterTest, EveryCopyIsByteIdenticalExceptCtrl1) {
  // The regression test for the one-sequence-per-command rule, for every power class -- if
  // anything in the burst path ever rebuilds the frame, the sequence and MAC change and this
  // fails. CTRL1 (byte index 1) is the one byte the transmitter is allowed to vary per copy
  // (ADR 0038), so it is masked out via bytes_without_ctrl1() before comparing.
  for (const OneWayPowerClass power_class :
       {OneWayPowerClass::LEGACY_LONG, OneWayPowerClass::ALWAYS_ALIVE, OneWayPowerClass::LOW_POWER}) {
    TransmitRecorder recorder;
    OneWayTransmitter transmitter(recorder.fn(), &tuning_);

    transmitter.send_burst(make_stop_frame(), power_class);

    ASSERT_EQ(recorder.sent.size(), ONEWAY_BURST_REPEATS);
    const std::vector<uint8_t> first = bytes_without_ctrl1(recorder.sent[0].bytes);
    for (size_t i = 1; i < recorder.sent.size(); i++) {
      EXPECT_EQ(bytes_without_ctrl1(recorder.sent[i].bytes), first)
          << "power class " << static_cast<int>(power_class) << ", copy " << i
          << " differs outside CTRL1; a device would read them as separate commands";
    }
  }
}

TEST_F(OneWayTransmitterTest, SpacesCopiesByTheProtocolInterval) {
  TransmitRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn(), &tuning_);

  transmitter.send_burst(make_stop_frame(), OneWayPowerClass::LEGACY_LONG);

  const auto &delays = esphome::test_hal::recorded_delays();
  ASSERT_EQ(delays.size(), static_cast<size_t>(ONEWAY_BURST_REPEATS - 1))
      << "gaps go between copies, so there is one fewer of them than there are frames — a "
         "trailing delay would hold the loop for nothing";
  for (const uint32_t gap : delays)
    EXPECT_EQ(gap, ONEWAY_BURST_INTERVAL_MS) << "the inter-copy gap is a protocol value, not a tunable";
}

TEST_F(OneWayTransmitterTest, GapCountIsUnchangedAcrossEveryPowerClass) {
  // The power class changes preamble/CTRL1 per copy, never the cadence -- pin the gap count for
  // all three classes so a future change cannot collapse it for one of them.
  for (const OneWayPowerClass power_class :
       {OneWayPowerClass::LEGACY_LONG, OneWayPowerClass::ALWAYS_ALIVE, OneWayPowerClass::LOW_POWER}) {
    esphome::test_hal::reset_delays();
    TransmitRecorder recorder;
    OneWayTransmitter transmitter(recorder.fn(), &tuning_);

    transmitter.send_burst(make_stop_frame(), power_class);

    EXPECT_EQ(esphome::test_hal::recorded_delays().size(), static_cast<size_t>(ONEWAY_BURST_REPEATS - 1))
        << "power class " << static_cast<int>(power_class);
  }
}

TEST_F(OneWayTransmitterTest, FeedsTheWatchdogDuringTheGaps) {
  // The burst blocks the ESPHome loop by design (ADR 0013). That is well inside the watchdog
  // window, but the feed is what keeps it that way if the interval is ever raised.
  TransmitRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn(), &tuning_);

  // The counter is global and monotonic across the suite, so assert on the delta.
  const uint32_t feeds_before = esphome::App.feed_wdt_calls;
  transmitter.send_burst(make_stop_frame(), OneWayPowerClass::LEGACY_LONG);

  EXPECT_GE(esphome::App.feed_wdt_calls - feeds_before, static_cast<uint32_t>(ONEWAY_BURST_REPEATS - 1))
      << "the watchdog must be fed at least once per blocking gap";
}

TEST_F(OneWayTransmitterTest, ReportsSuccessWhenOnlySomeCopiesGetOut) {
  // A device needs one copy. Reporting failure because the radio refused the last two would tell
  // the caller something false about a command that very likely landed.
  TransmitRecorder recorder;
  recorder.fail_from_ = 2;
  OneWayTransmitter transmitter(recorder.fn(), &tuning_);

  EXPECT_TRUE(transmitter.send_burst(make_stop_frame(), OneWayPowerClass::LEGACY_LONG))
      << "a partial burst is still a transmitted command";
  EXPECT_EQ(recorder.sent.size(), 2u) << "the burst must keep trying the remaining copies, not abort on first refusal";
}

TEST_F(OneWayTransmitterTest, ReportsFailureWhenNothingGetsOut) {
  TransmitRecorder recorder;
  recorder.fail_from_ = 0;
  OneWayTransmitter transmitter(recorder.fn(), &tuning_);

  EXPECT_FALSE(transmitter.send_burst(make_stop_frame(), OneWayPowerClass::LEGACY_LONG))
      << "with no copy on air there is nothing to claim; 1W gives no other evidence";
  EXPECT_TRUE(recorder.sent.empty());
}

TEST_F(OneWayTransmitterTest, StillAttemptsTheFullBurstWhenTheRadioRefusesFirst) {
  // Every attempt is counted even when the radio refuses, so a radio that recovers mid-burst
  // still gets the remaining copies out.
  TransmitRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn(), &tuning_);
  recorder.fail_from_ = 0;

  transmitter.send_burst(make_stop_frame(), OneWayPowerClass::LEGACY_LONG);

  EXPECT_EQ(esphome::test_hal::recorded_delays().size(), static_cast<size_t>(ONEWAY_BURST_REPEATS - 1))
      << "the cadence must not collapse when transmits fail";
}

// ============================================================================
// Per-copy preamble/CTRL1 shape (ADR 0038)
// ============================================================================

TEST_F(OneWayTransmitterTest, LegacyLongBurstIsUnchanged) {
  TransmitRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn(), &tuning_);

  ASSERT_TRUE(transmitter.send_burst(make_stop_frame(), OneWayPowerClass::LEGACY_LONG));

  ASSERT_EQ(recorder.sent.size(), ONEWAY_BURST_REPEATS);
  for (const auto &sent : recorder.sent) {
    EXPECT_EQ(sent.preamble, LONG_PREAMBLE);
    EXPECT_EQ(sent.bytes[1] & CTRL1_LOW_POWER, 0) << "unset low_power never sets CTRL1_LOW_POWER";
  }
  for (size_t i = 1; i < recorder.sent.size(); i++)
    EXPECT_EQ(recorder.sent[i].bytes, recorder.sent[0].bytes) << "every byte, including CTRL1, is identical";
}

TEST_F(OneWayTransmitterTest, AlwaysAliveBurstUsesNormalStartPreambleOnEveryCopy) {
  // Full byte-identity (outside CTRL1) is pinned once, for every class, by
  // EveryCopyIsByteIdenticalExceptCtrl1 above -- this test only needs to check the shape specific
  // to ALWAYS_ALIVE: preamble and flag.
  tuning_.normal_start_preamble = 32;
  TransmitRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn(), &tuning_);

  ASSERT_TRUE(transmitter.send_burst(make_stop_frame(), OneWayPowerClass::ALWAYS_ALIVE));

  ASSERT_EQ(recorder.sent.size(), ONEWAY_BURST_REPEATS);
  for (const auto &sent : recorder.sent) {
    EXPECT_EQ(sent.preamble, 32);
    EXPECT_EQ(sent.bytes[1] & CTRL1_LOW_POWER, 0);
  }
}

TEST_F(OneWayTransmitterTest, LowPowerBurstWakesOnFirstCopyOnly) {
  // Full byte-identity (outside CTRL1) is pinned once, for every class, by
  // EveryCopyIsByteIdenticalExceptCtrl1 above -- this test only needs to check the shape specific
  // to LOW_POWER: which copy wakes and which don't.
  tuning_.normal_start_preamble = 32;
  TransmitRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn(), &tuning_);

  ASSERT_TRUE(transmitter.send_burst(make_stop_frame(), OneWayPowerClass::LOW_POWER));

  ASSERT_EQ(recorder.sent.size(), ONEWAY_BURST_REPEATS);
  EXPECT_EQ(recorder.sent[0].preamble, LONG_PREAMBLE) << "copy 1 wakes the receiver";
  EXPECT_NE(recorder.sent[0].bytes[1] & CTRL1_LOW_POWER, 0) << "copy 1 sets CTRL1_LOW_POWER";
  for (size_t i = 1; i < recorder.sent.size(); i++) {
    EXPECT_EQ(recorder.sent[i].preamble, 32) << "copy " << i << " is a normal repeat";
    EXPECT_EQ(recorder.sent[i].bytes[1] & CTRL1_LOW_POWER, 0) << "copy " << i << " clears the flag";
  }
}

TEST_F(OneWayTransmitterTest, NormalPreambleIsReadLiveFromTuning) {
  TransmitRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn(), &tuning_);

  tuning_.normal_start_preamble = 8;
  ASSERT_TRUE(transmitter.send_burst(make_stop_frame(), OneWayPowerClass::ALWAYS_ALIVE));
  EXPECT_EQ(recorder.sent[0].preamble, 8);

  tuning_.normal_start_preamble = 64;
  ASSERT_TRUE(transmitter.send_burst(make_stop_frame(), OneWayPowerClass::ALWAYS_ALIVE));
  EXPECT_EQ(recorder.sent[ONEWAY_BURST_REPEATS].preamble, 64)
      << "the tuning value is read per burst, never cached at construction";
}

TEST_F(OneWayTransmitterTest, TransmitterOwnsTheLowPowerBit) {
  // A frame built with CTRL1_LOW_POWER already set (e.g. by a future builder change) must still go
  // out with the bit clear on a NORMAL-shaped copy -- the transmitter owns this bit unconditionally.
  IoFrame frame = make_stop_frame();
  frame.ctrl1 |= CTRL1_LOW_POWER;
  TransmitRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn(), &tuning_);

  ASSERT_TRUE(transmitter.send_burst(frame, OneWayPowerClass::ALWAYS_ALIVE));

  for (const auto &sent : recorder.sent)
    EXPECT_EQ(sent.bytes[1] & CTRL1_LOW_POWER, 0) << "ALWAYS_ALIVE clears the bit regardless of the builder's output";
}

TEST_F(OneWayTransmitterTest, FlaggedCopyStillCarriesAValidMac) {
  // Proves CTRL1 sits outside the 1W add-controller MAC span, on the real decode path rather than
  // a hand-rolled recomputation: the flagged copy 1 of a LOW_POWER burst must decode_1w_add_
  // controller() to a VERIFIED MAC and the correct recovered key, exactly like an unflagged repeat.
  IoFrame frame{};
  ASSERT_TRUE(create_1w_add_controller(frame, CONTROLLER_SRC, DeviceType::AWNING, MANUFACTURER_SOMFY, 0x1234,
                                       test::TEST_SYSTEM_KEY, /*with_mac=*/true));
  TransmitRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn(), &tuning_);

  ASSERT_TRUE(transmitter.send_burst(frame, OneWayPowerClass::LOW_POWER));
  ASSERT_EQ(recorder.sent.size(), ONEWAY_BURST_REPEATS);

  IoFrame flagged{};
  IoFrame unflagged{};
  ASSERT_TRUE(parse(recorder.sent[0].bytes.data(), static_cast<uint8_t>(recorder.sent[0].bytes.size()), flagged));
  ASSERT_TRUE(parse(recorder.sent[1].bytes.data(), static_cast<uint8_t>(recorder.sent[1].bytes.size()), unflagged));
  ASSERT_NE(flagged.ctrl1 & CTRL1_LOW_POWER, 0) << "sanity: copy 1 really is the flagged one";
  ASSERT_EQ(unflagged.ctrl1 & CTRL1_LOW_POWER, 0) << "sanity: copy 2 really is unflagged";

  OneWayAdoptedKey flagged_key{};
  OneWayAdoptedKey unflagged_key{};
  ASSERT_EQ(decode_1w_add_controller(flagged, flagged_key), OneWayAddControllerDecodeError::NONE);
  ASSERT_EQ(decode_1w_add_controller(unflagged, unflagged_key), OneWayAddControllerDecodeError::NONE);

  EXPECT_EQ(flagged_key.mac_status, OneWayMacStatus::VERIFIED)
      << "the flagged copy's MAC verifies -- CTRL1 is outside its span";
  EXPECT_EQ(unflagged_key.mac_status, OneWayMacStatus::VERIFIED);
  EXPECT_EQ(0, memcmp(flagged_key.system_key, test::TEST_SYSTEM_KEY, AES_KEY_SIZE))
      << "the recovered key matches the one the frame actually wraps";
  EXPECT_EQ(0, memcmp(unflagged_key.system_key, test::TEST_SYSTEM_KEY, AES_KEY_SIZE));
}

// ============================================================================
// format_oneway_preamble_list() -- the pure formatter behind the TX log line
// ============================================================================

TEST(FormatOnewayPreambleList, LegacyLongIsFourLongCopies) {
  EXPECT_EQ(format_oneway_preamble_list(OneWayPowerClass::LEGACY_LONG, 32), "1024/1024/1024/1024");
}

TEST(FormatOnewayPreambleList, AlwaysAliveIsFourNormalCopies) {
  EXPECT_EQ(format_oneway_preamble_list(OneWayPowerClass::ALWAYS_ALIVE, 32), "32/32/32/32");
}

TEST(FormatOnewayPreambleList, LowPowerIsOneLongThenNormal) {
  EXPECT_EQ(format_oneway_preamble_list(OneWayPowerClass::LOW_POWER, 32), "1024/32/32/32");
}

TEST(FormatOnewayPreambleList, ReflectsTheLiveNormalStartPreambleValue) {
  EXPECT_EQ(format_oneway_preamble_list(OneWayPowerClass::ALWAYS_ALIVE, 8), "8/8/8/8");
}
