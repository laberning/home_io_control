#include "oneway_transmitter.h"

#include "proto_commands.h"
#include "proto_timing.h"

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

class OneWayTransmitterTest : public ::testing::Test {
 protected:
  void SetUp() override { esphome::test_hal::reset_delays(); }
};

}  // namespace

TEST_F(OneWayTransmitterTest, SendsExactlyFourCopiesOnChannelTwo) {
  TransmitRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());

  EXPECT_TRUE(transmitter.send_burst(make_stop_frame()));

  ASSERT_EQ(recorder.sent.size(), ONEWAY_BURST_REPEATS) << "a 1W command is a fixed-size burst, not a single frame";
  for (const auto &sent : recorder.sent) {
    EXPECT_EQ(sent.freq, FREQ_CH2) << "1W uses only channel 2";
    EXPECT_EQ(sent.preamble, LONG_PREAMBLE)
        << "every copy needs the long preamble: a battery device may be asleep for any of them";
  }
}

TEST_F(OneWayTransmitterTest, EveryCopyIsByteIdentical) {
  // The regression test for the one-sequence-per-command rule. If anything in the burst path ever
  // rebuilds the frame, the sequence and MAC change and this fails.
  TransmitRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());

  transmitter.send_burst(make_stop_frame());

  ASSERT_EQ(recorder.sent.size(), ONEWAY_BURST_REPEATS);
  for (size_t i = 1; i < recorder.sent.size(); i++) {
    EXPECT_EQ(recorder.sent[i].bytes, recorder.sent[0].bytes)
        << "copy " << i << " differs from the first; a device would read them as separate commands";
  }
}

TEST_F(OneWayTransmitterTest, SpacesCopiesByTheProtocolInterval) {
  TransmitRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());

  transmitter.send_burst(make_stop_frame());

  const auto &delays = esphome::test_hal::recorded_delays();
  ASSERT_EQ(delays.size(), static_cast<size_t>(ONEWAY_BURST_REPEATS - 1))
      << "gaps go between copies, so there is one fewer of them than there are frames — a "
         "trailing delay would hold the loop for nothing";
  for (const uint32_t gap : delays)
    EXPECT_EQ(gap, ONEWAY_BURST_INTERVAL_MS) << "the inter-copy gap is a protocol value, not a tunable";
}

TEST_F(OneWayTransmitterTest, FeedsTheWatchdogDuringTheGaps) {
  // The burst blocks the ESPHome loop for ~120 ms by design (ADR 0013). That is well inside the
  // watchdog window, but the feed is what keeps it that way if the interval is ever raised.
  TransmitRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());

  // The counter is global and monotonic across the suite, so assert on the delta.
  const uint32_t feeds_before = esphome::App.feed_wdt_calls;
  transmitter.send_burst(make_stop_frame());

  EXPECT_GE(esphome::App.feed_wdt_calls - feeds_before, static_cast<uint32_t>(ONEWAY_BURST_REPEATS - 1))
      << "the watchdog must be fed at least once per blocking gap";
}

TEST_F(OneWayTransmitterTest, ReportsSuccessWhenOnlySomeCopiesGetOut) {
  // A device needs one copy. Reporting failure because the radio refused the last two would tell
  // the caller something false about a command that very likely landed.
  TransmitRecorder recorder;
  recorder.fail_from_ = 2;
  OneWayTransmitter transmitter(recorder.fn());

  EXPECT_TRUE(transmitter.send_burst(make_stop_frame())) << "a partial burst is still a transmitted command";
  EXPECT_EQ(recorder.sent.size(), 2u) << "the burst must keep trying the remaining copies, not abort on first refusal";
}

TEST_F(OneWayTransmitterTest, ReportsFailureWhenNothingGetsOut) {
  TransmitRecorder recorder;
  recorder.fail_from_ = 0;
  OneWayTransmitter transmitter(recorder.fn());

  EXPECT_FALSE(transmitter.send_burst(make_stop_frame()))
      << "with no copy on air there is nothing to claim; 1W gives no other evidence";
  EXPECT_TRUE(recorder.sent.empty());
}

TEST_F(OneWayTransmitterTest, StillAttemptsTheFullBurstWhenTheRadioRefusesFirst) {
  // Every attempt is counted even when the radio refuses, so a radio that recovers mid-burst
  // still gets the remaining copies out.
  TransmitRecorder recorder;
  OneWayTransmitter transmitter(recorder.fn());
  recorder.fail_from_ = 0;

  transmitter.send_burst(make_stop_frame());

  EXPECT_EQ(esphome::test_hal::recorded_delays().size(), static_cast<size_t>(ONEWAY_BURST_REPEATS - 1))
      << "the cadence must not collapse when transmits fail";
}
