/// @file
/// @brief RadioBridle over a scripted co-processor: handshake, transmit, receive, ordering, scans,
///        and the spec's own scenario event streams.

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bridle_stand_in.h"
#include "fake_coprocessor.h"
#include "log_frame.h"
#include "support/log_capture.h"
#include "testable_radio_bridle.h"

using namespace esphome::home_io_control;
using namespace esphome::home_io_control::bridle;
using esphome::test_clock::ManualClock;

namespace {

/// The default wake-up preamble (515 ms) in programmed bytes of a raw-stream radio, 8 bit times each.
constexpr uint16_t LONG_PREAMBLE = 2472;

/// Everything a driver test needs, wired: clock, stream, fake co-processor, reset line, driver.
struct Rig {
  ManualClock clock;
  FakeByteStream stream;
  FakeCoprocessor cp;
  FakeResetLine reset;
  TestableRadioBridle radio;

  explicit Rig(FakeCoprocessor::Config config = {}, std::optional<int8_t> tx_power = std::nullopt,
               const BridlePhy &phy = io2g4_phy())
      : cp(stream, std::move(config)), radio(&stream, phy, &reset, tx_power) {
    reset.on_release = [this] { cp.boot(); };
  }

  /// init() with everything working; the test fails if it does not.
  void start() { ASSERT_TRUE(radio.init()) << (radio.failure_reason() ? radio.failure_reason() : ""); }

  static uint64_t now() { return esphome::test_clock::state().now_us; }
  static void advance_to(uint64_t us) {
    if (us > now())
      esphome::test_clock::advance_us(us - now());
  }
  template<class T> T last_command() {
    Message m;
    const auto &cmd = cp.commands().back().message;
    EXPECT_EQ(decode_message(cmd.data(), cmd.size(), m), DecodeStatus::OK);
    T body{};
    if (const T *b = std::get_if<T>(&m.body))
      body = *b;
    else
      ADD_FAILURE() << "last command has another type";
    return body;
  }
  /// Decoded commands of one type, in order (bodies view the recorded bytes).
  template<class T> std::vector<std::pair<Message, T>> commands_of(MsgType type) {
    std::vector<std::pair<Message, T>> out;
    for (const FakeCoprocessor::RecordedCommand *c : cp.commands_of(type)) {
      Message m;
      EXPECT_EQ(decode_message(c->message.data(), c->message.size(), m), DecodeStatus::OK);
      if (const T *b = std::get_if<T>(&m.body))
        out.emplace_back(m, *b);
    }
    return out;
  }
};

/// A PSDU with the io header, `frame_len` frame bytes starting at `tag`, and two FCS bytes.
std::vector<uint8_t> psdu(uint8_t tag, size_t frame_len = 12) {
  std::vector<uint8_t> p = {0x01, 0x69, 0x6F};
  for (size_t i = 0; i < frame_len; i++)
    p.push_back(static_cast<uint8_t>(tag + i));
  p.push_back(0xAA);
  p.push_back(0xBB);
  return p;
}

std::vector<uint8_t> io_frame(uint8_t tag, size_t frame_len = 12) {
  std::vector<uint8_t> f;
  for (size_t i = 0; i < frame_len; i++)
    f.push_back(static_cast<uint8_t>(tag + i));
  return f;
}

/// Puts the radio in receive and lets the fake know, so injected frames are heard.
void listen_on(Rig &rig) { rig.radio.set_mode_rx(); }

}  // namespace

/// The driver as the firmware builds it, tracing through its default path (the frame log's hold) instead of a
/// test's own collector.
struct TracedRig {
  ManualClock clock;
  FakeByteStream stream;
  FakeCoprocessor cp;
  FakeResetLine reset;
  RadioBridle radio;

  TracedRig() : cp(stream), radio(&stream, io2g4_phy(), &reset, std::nullopt) {
    reset.on_release = [this] { cp.boot(); };
    radio.set_trace(true);
  }
};

/// The trace lines in @p log, in the order they were printed.
std::vector<std::string> trace_lines_of(const test::LogCapture &log) {
  std::vector<std::string> lines;
  for (const auto &line : log.lines()) {
    if (line.text.rfind(TRACE_MARKER, 0) == 0)
      lines.push_back(line.text);
  }
  return lines;
}

// === Handshake and configuration ===

TEST(RadioBridle, InitReachesRunningAfterTheHandshake) {
  Rig rig;
  rig.start();
  EXPECT_TRUE(rig.radio.running());
  EXPECT_FALSE(rig.radio.is_failed());
  EXPECT_EQ(rig.reset.pulse_starts_us.size(), 1u) << "one reset pulse, no retry";
}

TEST(RadioBridle, ConfigurationCommandsGoOutInTheSpecifiedOrder) {
  Rig rig;
  rig.start();
  auto sets = rig.commands_of<SetParams>(MsgType::SET_PARAMS);
  ASSERT_EQ(sets.size(), 2u);
  EXPECT_EQ(sets[0].second.radio, DEVICE_SCOPE) << "the event mask first";
  ByteSpan v;
  ASSERT_TRUE(find_tlv(sets[0].second.tlvs, static_cast<uint8_t>(ParamId::EVENT_MASK), v));
  EXPECT_EQ(v.data[0], event_mask::RX_START | event_mask::RX_FCS_BAD | event_mask::RX_ABORT | event_mask::LOG);

  EXPECT_EQ(sets[1].second.radio, 0);
  TlvReader r(sets[1].second.tlvs);
  uint8_t id;
  std::vector<uint8_t> ids;
  while (r.next(id, v))
    ids.push_back(id);
  ASSERT_EQ(ids.size(), 4u);
  EXPECT_EQ(ids[0], static_cast<uint8_t>(ParamId::PHY_PROFILE)) << "the profile first: it resets the profile params";
  EXPECT_EQ(ids[1], static_cast<uint8_t>(ParamId::SFD));
  EXPECT_EQ(ids[2], static_cast<uint8_t>(ParamId::RX_MIN_PSDU)) << "after the profile, which resets it";
  EXPECT_EQ(ids[3], static_cast<uint8_t>(ParamId::TX_POWER_DBM));
  EXPECT_EQ(rig.cp.event_mask(), 0x0F);
}

TEST(RadioBridle, TxPowerIsTheRadiosMaximumWhenNoneIsSetAndTheConfiguredValueOtherwise) {
  {
    Rig rig;
    rig.start();
    EXPECT_EQ(rig.radio.applied_tx_power_dbm(), 8);
    EXPECT_EQ(rig.cp.tx_power_dbm(), 8);
  }
  {
    Rig rig({}, 3);
    rig.start();
    EXPECT_EQ(rig.radio.applied_tx_power_dbm(), 3);
    EXPECT_EQ(rig.cp.tx_power_dbm(), 3);
  }
  {
    Rig rig({}, 20);  // more than the radio can: the co-processor clamps and the driver takes the echo
    rig.start();
    EXPECT_EQ(rig.radio.applied_tx_power_dbm(), 8);
  }
}

TEST(RadioBridle, ThisProtocolRefusesTheMinorBeforeAndTheMinorAfter) {
  for (int delta : {-1, 1}) {
    FakeCoprocessor::Config cfg;
    cfg.proto_minor = static_cast<uint8_t>(PROTO_MINOR + delta);
    Rig rig(cfg);
    EXPECT_FALSE(rig.radio.init()) << "minor " << int(cfg.proto_minor);
    EXPECT_STREQ(rig.radio.failure_reason(), "protocol version mismatch");
  }
}

TEST(RadioBridle, TheVersionOfTheCoprocessorIsKeptForTheGate) {
  Rig rig;
  rig.start();
  EXPECT_TRUE(rig.radio.coproc_supports(PROTO_MINOR));
  EXPECT_FALSE(rig.radio.coproc_supports(PROTO_MINOR + 1));
}

TEST(RadioBridle, AVersionMismatchFailsAndIsNotConfigured) {
  FakeCoprocessor::Config cfg;
  cfg.proto_minor = PROTO_MINOR + 1;
  Rig rig(cfg);
  EXPECT_FALSE(rig.radio.init());
  EXPECT_TRUE(rig.radio.is_failed());
  EXPECT_STREQ(rig.radio.failure_reason(), "protocol version mismatch");
  EXPECT_TRUE(rig.commands_of<SetParams>(MsgType::SET_PARAMS).empty());
}

TEST(RadioBridle, AReadyMissingARequiredTlvIsRefused) {
  FakeCoprocessor::Config cfg;
  cfg.with_reset_cause = false;
  Rig rig(cfg);
  EXPECT_FALSE(rig.radio.init());
  EXPECT_STREQ(rig.radio.failure_reason(), "ready lacks a required field");
}

TEST(RadioBridle, WithoutABootReadyItAsksWithHelloAndAcceptsTheAnswer) {
  Rig rig;
  rig.reset.on_release = nullptr;  // the reset produces no boot ready; a hello is still answered
  uint64_t const start = Rig::now();
  rig.start();
  EXPECT_EQ(rig.cp.commands_of(MsgType::HELLO).size(), 1u);
  EXPECT_GE(Rig::now() - start, 2000000u) << "the driver waited the 2 s the spec allows before asking";
  EXPECT_EQ(rig.reset.pulse_starts_us.size(), 1u) << "the hello worked, so no second reset";
}

TEST(RadioBridle, ACoprocessorThatRefusesHelloWithLinkInUseIsReportedAsBusyNotAsSilent) {
  Rig rig;
  rig.reset.on_release = nullptr;  // no boot ready: the driver asks with hello, after each reset
  rig.cp.reject_next(MsgType::HELLO, ErrorCode::LINK_IN_USE);
  rig.cp.reject_next(MsgType::HELLO, ErrorCode::LINK_IN_USE);
  EXPECT_TRUE(rig.radio.init()) << "busy is no failure: the link starts down and its retry asks again";
  EXPECT_FALSE(rig.radio.is_failed());
  EXPECT_EQ(rig.radio.link_state(), RadioBridle::Link::DOWN);
  EXPECT_EQ(rig.cp.commands_of(MsgType::HELLO).size(), 2u) << "it asked, not that it heard nothing";
}

TEST(RadioBridle, ASilentCoprocessorGetsHelloThenOneMoreResetMoreThanASecondLater) {
  Rig rig;
  rig.cp.set_silent(true);
  EXPECT_FALSE(rig.radio.init());
  EXPECT_STREQ(rig.radio.failure_reason(), "co-processor not answering");
  EXPECT_EQ(rig.cp.commands_of(MsgType::HELLO).size(), 2u) << "a hello after each reset";
  ASSERT_EQ(rig.reset.pulse_starts_us.size(), 2u);
  EXPECT_GT(rig.reset.pulse_starts_us[1] - rig.reset.pulse_starts_us[0], 1000000u)
      << "never two reset pulses within a second";
}

TEST(RadioBridle, ARadioIsPickedByProfileBandAndCapabilities) {
  // Radio 0 lacks strobe; radio 1 is fine: the second is used.
  FakeCoprocessor::Config cfg;
  auto weak = FakeCoprocessor::reference_802154_radio(0);
  weak.caps &= ~radio_caps::STROBE;
  cfg.radios = {weak, FakeCoprocessor::reference_802154_radio(1)};
  Rig rig(cfg);
  rig.start();
  EXPECT_EQ(rig.radio.selected_radio(), 1);
  auto sets = rig.commands_of<SetParams>(MsgType::SET_PARAMS);
  EXPECT_EQ(sets.back().second.radio, 1) << "the radio is configured under its own index";
}

TEST(RadioBridle, WhenNoRadioMatchesInitFailsAndSaysSo) {
  FakeCoprocessor::Config cfg;
  auto gfsk = FakeCoprocessor::reference_802154_radio(0);
  gfsk.profiles = phy_profiles::GFSK_RAW;
  gfsk.band_min_hz = 863000000u;
  gfsk.band_max_hz = 870000000u;
  cfg.radios = {gfsk};
  Rig rig(cfg);
  EXPECT_FALSE(rig.radio.init());
  EXPECT_STREQ(rig.radio.failure_reason(), "no suitable radio");
}

TEST(RadioBridle, ARadioWhoseBandMissesAnIoChannelIsNotUsed) {
  FakeCoprocessor::Config cfg;
  auto narrow = FakeCoprocessor::reference_802154_radio(0);
  narrow.band_min_hz = 2440000000u;  // channel 15 (2425 MHz) is out
  cfg.radios = {narrow};
  Rig rig(cfg);
  EXPECT_FALSE(rig.radio.init());
}

TEST(RadioBridle, ARadioWithATooSmallPayloadLimitIsRefused) {
  FakeCoprocessor::Config cfg;
  auto tiny = FakeCoprocessor::reference_802154_radio(0);
  tiny.max_payload_bytes = 30;
  cfg.radios = {tiny};
  Rig rig(cfg);
  EXPECT_FALSE(rig.radio.init());
  EXPECT_STREQ(rig.radio.failure_reason(), "no suitable radio");
}

TEST(RadioBridle, ARefusedConfigurationFailsInit) {
  Rig rig;
  rig.cp.reject_next(MsgType::SET_PARAMS, ErrorCode::RADIO_FAULT, 1);
  // The rejection is armed for the first set_params; the boot happens inside init().
  EXPECT_FALSE(rig.radio.init());
  EXPECT_STREQ(rig.radio.failure_reason(), "event mask refused");
}

TEST(RadioBridle, ARefusedRadioConfigurationEndsTheHandshakeAtOnceAndNamesTheTlv) {
  Rig rig;
  rig.reset.on_release = [&rig] {
    rig.cp.boot();
    // The radio-scope set_params (radio 0) is refused naming the `sfd` TLV the 2.4 GHz profile sends; the device-scope
    // one (radio 0xFF) before it is accepted.
    rig.cp.reject_next(MsgType::SET_PARAMS, ErrorCode::OUT_OF_RANGE, static_cast<uint16_t>(ParamId::SFD), 0);
  };
  uint64_t const start = Rig::now();
  EXPECT_FALSE(rig.radio.init());
  EXPECT_LT(Rig::now() - start, uint64_t{RadioBridle::ACK_TIMEOUT_MS} * 1000)
      << "the refusal is read when it arrives, not after a timeout";
  EXPECT_STREQ(rig.radio.failure_reason(), "radio configuration refused");
  EXPECT_EQ(rig.radio.last_error_code(), ErrorCode::OUT_OF_RANGE);
  EXPECT_EQ(rig.radio.last_error_detail(), static_cast<int16_t>(ParamId::SFD)) << "the TLV the log names";
}

TEST(RadioBridle, WithoutAStreamInitFails) {
  FakeResetLine reset;
  TestableRadioBridle radio(nullptr, io2g4_phy(), &reset, std::nullopt);
  EXPECT_FALSE(radio.init());
  EXPECT_STREQ(radio.failure_reason(), "no byte stream");
}

// === Transmit ===

TEST(RadioBridle, TxFieldsFollowTheWakeLevel) {
  struct Case {
    TxWake wake;
    uint16_t strobe_count;
  };
  for (Case c : {Case{TxWake::NONE, 0}, Case{TxWake::SHORT, 8}, Case{TxWake::LONG, 510}}) {
    Rig rig;
    rig.start();
    RadioTxConfig cfg;
    cfg.freq_hz = FREQ_CH2;
    cfg.wake = c.wake;
    cfg.preamble_len = c.wake == TxWake::LONG ? LONG_PREAMBLE : SHORT_PREAMBLE;
    const auto frame = io_frame(0x40);
    ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), cfg).sent());
    Tx tx = rig.last_command<Tx>();
    EXPECT_EQ(tx.radio, 0);
    EXPECT_EQ(tx.freq_hz, 2450000000u);
    EXPECT_TRUE(tx.then_rx);
    EXPECT_EQ(tx.preamble_bytes, 0);
    EXPECT_EQ(tx.has_strobe, c.strobe_count != 0);
    EXPECT_EQ(tx.strobe.count, c.strobe_count);
    EXPECT_FALSE(tx.cca);
    EXPECT_FALSE(tx.at_time);
    ASSERT_EQ(tx.payload.size, 3u + frame.size());
    EXPECT_EQ(0, std::memcmp(tx.payload.data, IO2G4_PSDU_HEADER, 3));
    EXPECT_EQ(0, std::memcmp(tx.payload.data + 3, frame.data(), frame.size()));
    EXPECT_EQ(rig.radio.get_current_freq(), FREQ_CH2);
  }
}

TEST(RadioBridle, TheTrainFollowsTheWakeLevelAndNeverThePreambleTheHubChose) {
  // The wake-up duration is a tunable (`low_power_wake_ms`), and the hub turns it into programmed bytes; whatever it is
  // set to, a wake-up request carries the 510-frame train the VELUX controllers send (a strobe has no preamble to
  // lengthen). A start frame without a wake-up gets the 8-frame train, a continuation frame none.
  struct Case {
    TxWake wake;
    uint16_t strobe_count;
  };
  for (const Case c : {Case{TxWake::LONG, 510}, Case{TxWake::SHORT, 8}, Case{TxWake::NONE, 0}}) {
    for (const uint16_t preamble : {SHORT_PREAMBLE, uint16_t{48}, LONG_PREAMBLE, uint16_t{4800}}) {
      Rig rig;
      rig.start();
      RadioTxConfig cfg;
      cfg.wake = c.wake;
      cfg.preamble_len = preamble;
      const auto frame = io_frame(0x40);
      ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), cfg).sent());
      const Tx tx = rig.last_command<Tx>();
      EXPECT_EQ(tx.strobe.count, c.strobe_count) << "preamble " << preamble;
      EXPECT_EQ(tx.has_strobe, c.strobe_count != 0) << "preamble " << preamble;
      EXPECT_EQ(tx.preamble_bytes, 0) << "802.15.4 has one preamble: the hub's byte count plays no part";
    }
  }
}

TEST(RadioBridle, TheReceiverRunsAfterASentFrame) {
  Rig rig;
  rig.start();
  EXPECT_FALSE(rig.cp.receiving());
  const auto frame = io_frame(1);
  ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), {}).sent());
  EXPECT_TRUE(rig.cp.receiving()) << "the tx returns to receive by itself (then_rx)";
  EXPECT_TRUE(rig.last_command<Tx>().then_rx);
}

TEST(RadioBridle, TheReceiverRunsAfterAFailedSendToo) {
  // Issue #228: a send that failed must not leave the radio idle, or the hub listens to nothing. A refused `tx`
  // changes nothing (spec §2.2), so a radio that listened before listens after it, and the driver sends no `rx_on`
  // to make sure.
  {
    Rig rig;  // a transmission the co-processor refuses before it starts
    rig.start();
    listen_on(rig);
    rig.cp.reject_next(MsgType::TX, ErrorCode::PAYLOAD_TOO_LONG);
    const auto frame = io_frame(1);
    EXPECT_EQ(rig.radio.send_packet(frame.data(), frame.size(), {}).status, TxResult::Status::FAILED);
    EXPECT_TRUE(rig.cp.receiving()) << "a refused tx leaves the radio as it was: receiving";
    EXPECT_TRUE(rig.radio.rx_on_active());
    EXPECT_EQ(rig.commands_of<RxOn>(MsgType::RX_ON).size(), 1u) << "and the driver sends no rx_on to make sure";
  }
  {
    Rig rig;  // a refused tx leaves a running scan running, and the driver's own state says so
    rig.start();
    ASSERT_TRUE(rig.radio.run_idle_scan());
    rig.cp.reject_next(MsgType::TX, ErrorCode::RADIO_BUSY);
    const auto frame = io_frame(1);
    EXPECT_EQ(rig.radio.send_packet(frame.data(), frame.size(), {}).status, TxResult::Status::FAILED);
    EXPECT_TRUE(rig.radio.scanning()) << "the scan the co-processor still runs";
    EXPECT_TRUE(rig.cp.scanning());
    EXPECT_EQ(rig.commands_of<RxOn>(MsgType::RX_ON).size(), 0u);
  }
  {
    Rig rig;  // the clear-channel check found the channel busy: nothing was sent, the radio is in receive
    rig.start();
    rig.cp.set_channel_level_dbm(-60);
    RadioTxConfig cfg;
    cfg.cca_threshold_dbm = -75;
    const auto frame = io_frame(1);
    EXPECT_EQ(rig.radio.send_packet(frame.data(), frame.size(), cfg).status, TxResult::Status::CHANNEL_BUSY);
    EXPECT_TRUE(rig.cp.receiving());
  }
  {
    Rig rig;  // no tx_done: the link is resynced and the next wait restores the receiver
    rig.start();
    rig.cp.set_silent(true);
    const auto frame = io_frame(1);
    EXPECT_EQ(rig.radio.send_packet(frame.data(), frame.size(), {}).status, TxResult::Status::FAILED);
    rig.cp.set_silent(false);
    RadioRxPacket packet;
    for (int pass = 0; pass < 2000 && !(rig.radio.running() && rig.cp.receiving()); pass++) {
      esphome::test_clock::advance_ms(5);
      rig.radio.wait_for_packet(packet, 1);
    }
    EXPECT_TRUE(rig.radio.running());
    EXPECT_TRUE(rig.cp.receiving());
  }
}

TEST(RadioBridle, ARefusedTxDoesNotRestoreTheStateOfAnUnawaitedRxOnRefusedDuringItsWait) {
  // The co-processor refuses the unawaited `rx_on` (spec §2.2: nothing changes) while the `tx` is awaited, then the
  // `tx` itself. The driver's record must match the co-processor, which does not receive: the next wait sends an
  // `rx_on` again instead of believing the radio listens.
  Rig rig;
  rig.start();
  rig.cp.reject_next(MsgType::RX_ON, ErrorCode::RADIO_BUSY, 0, -1, 500);
  rig.cp.reject_next(MsgType::TX, ErrorCode::PAYLOAD_TOO_LONG, 0, -1, 20000);
  listen_on(rig);
  const auto frame = io_frame(1);
  EXPECT_EQ(rig.radio.send_packet(frame.data(), frame.size(), {}).status, TxResult::Status::FAILED);
  EXPECT_FALSE(rig.cp.receiving()) << "both refusals changed nothing: the radio does not receive";
  EXPECT_FALSE(rig.radio.rx_on_active()) << "and the driver's record says the same";
  EXPECT_EQ(rig.commands_of<RxOn>(MsgType::RX_ON).size(), 1u);
  esphome::test_clock::advance_ms(6000);  // past SCAN_RETRY_MS, the hold-off after a refused rx_on
  RadioRxPacket packet;
  rig.radio.wait_for_packet(packet, 1);
  EXPECT_TRUE(rig.cp.receiving()) << "the next wait tells the radio to receive";
}

TEST(RadioBridle, ARefusedTxDoesNotRestoreTheStateOfAnUnawaitedRxScanRefusedDuringItsWait) {
  // As above for the scan: the unawaited `rx_scan` is refused while the `tx` is awaited, then the `tx` itself. The
  // radio neither scans nor receives, and the driver must not believe it scans, or the idle pass starts nothing.
  Rig rig;
  rig.start();
  rig.cp.reject_next(MsgType::RX_SCAN, ErrorCode::RADIO_BUSY, 0, -1, 500);
  rig.cp.reject_next(MsgType::TX, ErrorCode::PAYLOAD_TOO_LONG, 0, -1, 20000);
  ASSERT_TRUE(rig.radio.run_idle_scan());
  const auto frame = io_frame(1);
  EXPECT_EQ(rig.radio.send_packet(frame.data(), frame.size(), {}).status, TxResult::Status::FAILED);
  EXPECT_FALSE(rig.cp.receiving()) << "both refusals changed nothing: the radio neither scans nor receives";
  EXPECT_FALSE(rig.radio.scanning()) << "and the driver's record says the same";
  EXPECT_FALSE(rig.radio.rx_on_active());
  // Within the hold-off after the refused scan the idle pass hops from the host, which needs the radio receiving.
  EXPECT_FALSE(rig.radio.run_idle_scan());
  EXPECT_TRUE(rig.cp.receiving()) << "the idle pass tells the radio to receive";
}

TEST(RadioBridle, ARefusedUnawaitedCommandIsUndoneBeforeATxAndSupersededByASentOne) {
  const auto frame = io_frame(1);
  {
    Rig rig;  // the refusal is read before the `tx` is sent: the `tx`'s restore starts from the undone state
    rig.start();
    rig.cp.reject_next(MsgType::RX_ON, ErrorCode::RADIO_BUSY);
    rig.cp.reject_next(MsgType::TX, ErrorCode::PAYLOAD_TOO_LONG, 0, -1, 20000);
    listen_on(rig);
    RadioRxPacket packet;
    EXPECT_FALSE(rig.radio.check_for_packet(packet));
    EXPECT_FALSE(rig.radio.rx_on_active()) << "the refused rx_on is undone at once";
    EXPECT_EQ(rig.radio.send_packet(frame.data(), frame.size(), {}).status, TxResult::Status::FAILED);
    EXPECT_FALSE(rig.cp.receiving());
    EXPECT_FALSE(rig.radio.rx_on_active());
  }
  {
    Rig rig;  // the refusal arrives during the wait of a `tx` that is sent: the `tx` defines the state
    rig.start();
    rig.cp.reject_next(MsgType::RX_SCAN, ErrorCode::RADIO_BUSY, 0, -1, 500);
    ASSERT_TRUE(rig.radio.run_idle_scan());
    EXPECT_EQ(rig.radio.send_packet(frame.data(), frame.size(), {}).status, TxResult::Status::SENT);
    EXPECT_TRUE(rig.cp.receiving()) << "the tx returns to receive by itself (then_rx)";
    EXPECT_TRUE(rig.radio.rx_on_active()) << "and the refused scan before it does not undo that";
    EXPECT_FALSE(rig.radio.scanning());
  }
}

// === The trace goes through the held log ===

TEST(RadioBridleHeldTrace, AMessageIsTracedAtOnceWhenNothingIsHeld) {
  test::LogCapture log(true);
  TracedRig rig;
  ASSERT_TRUE(rig.radio.init());
  // The start-up conversation: `ready`, then the device and the radio configuration, each with its acknowledgement.
  const auto lines = trace_lines_of(log);
  ASSERT_EQ(lines.size(), 5u);
  const char *const directions[] = {"[bridle] <", "[bridle] >", "[bridle] <", "[bridle] >", "[bridle] <"};
  for (size_t i = 0; i < lines.size(); i++)
    EXPECT_EQ(lines[i].rfind(directions[i], 0), 0u) << lines[i];
}

TEST(RadioBridleHeldTrace, TheDriversTraceIsNotPrintedWhileTheFrameLogIsHeldAndFollowsInOrderOnRelease) {
  TracedRig rig;
  ASSERT_TRUE(rig.radio.init());
  test::LogCapture log(true);
  const auto frame = io_frame(1);

  frame_log_hold();
  ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), {}).sent());
  EXPECT_TRUE(log.lines().empty()) << "the hub is waiting for a reply it provoked: nothing may be printed";

  frame_log_release();
  const auto lines = trace_lines_of(log);
  ASSERT_EQ(lines.size(), 2u) << "the tx and its tx_done";
  EXPECT_EQ(lines[0].rfind("[bridle] > ", 0), 0u) << lines[0];
  EXPECT_EQ(lines[1].rfind("[bridle] < ", 0), 0u) << lines[1];
#ifdef IOHOME_FRAME_LOG
  EXPECT_EQ(log.lines().size(), 3u) << "the two trace lines and the TX line of the frame log";
#else
  EXPECT_EQ(log.lines().size(), 2u) << "the two trace lines, and no frame line without the frame log";
#endif
}

TEST(RadioBridleHeldTrace, OneAuthenticatedExchangeWritesEightTraceLinesAndTheHoldBufferTakesThemWithRoomToSpare) {
  TracedRig rig;
  ASSERT_TRUE(rig.radio.init());
  rig.radio.set_mode_rx();
  esphome::test_clock::advance_ms(5);
  RadioRxPacket packet;
  rig.radio.check_for_packet(packet);  // the ack of the rx_on
  test::LogCapture log(true);
  const auto request = io_frame(1);
  const auto answer = io_frame(2);

  frame_log_hold();
  ASSERT_TRUE(rig.radio.send_packet(request.data(), request.size(), {}).sent());  // tx, tx_done
  rig.cp.inject_rx_start(2450000000u, RxStage::SYNC, 2000);
  rig.cp.inject_rx_frame(2450000000u, psdu(0x20), FcsState::OK, 2500);  // the challenge
  ASSERT_TRUE(rig.radio.wait_for_packet(packet, 20));
  ASSERT_TRUE(rig.radio.send_packet(answer.data(), answer.size(), {}).sent());  // tx, tx_done
  rig.cp.inject_rx_start(2450000000u, RxStage::SYNC, 2000);
  rig.cp.inject_rx_frame(2450000000u, psdu(0x30), FcsState::OK, 2500);  // the final reply
  ASSERT_TRUE(rig.radio.wait_for_packet(packet, 20));
  EXPECT_TRUE(log.lines().empty());
  frame_log_release();

  EXPECT_EQ(trace_lines_of(log).size(), TRACE_EXCHANGE_LINES);
  // The hold buffer holds 36 lines per exchange for the trace (two channel checks with their retune and four readings
  // each, the co-processor's refusal of the first `tx`, these 8 and 4 spare), and the frame log's own 12 in a build
  // with the frame log: exactly, as radio_bridle_trace.cpp asserts at compile time and bridle_held_log_lines() in
  // hub_validators.py sets it.
  EXPECT_EQ(TRACE_CHANNEL_CHECK_LINES, 10u);
  EXPECT_EQ(TRACE_REFUSAL_LINES, 4u);
  EXPECT_EQ(TRACE_HELD_LINES, 36u);
#ifdef IOHOME_FRAME_LOG
  EXPECT_EQ(FRAME_LOG_HELD_LINES, 48u);
#else
  EXPECT_EQ(FRAME_LOG_HELD_LINES, 36u);
#endif
}

// === The frame log shows the frames of the bridge ===
// Only a build with the frame log writes these lines: `make unit-test` runs them in its frame-log variant.
#ifdef IOHOME_FRAME_LOG

namespace {

/// The `io_frame` lines in @p log, in the order they were printed.
std::vector<std::string> frame_lines_of(const test::LogCapture &log) {
  std::vector<std::string> lines;
  for (const auto &line : log.lines()) {
    if (line.tag == "io_frame")
      lines.push_back(line.text);
  }
  return lines;
}

}  // namespace

TEST(RadioBridleFrameLog, ATransmittedFrameIsLoggedWithTheAirFrequencyOfItsChannelAndIsHeldWhileTheHubWaits) {
  TracedRig rig;
  rig.radio.set_trace(false);
  ASSERT_TRUE(rig.radio.init());
  test::LogCapture log(true);
  RadioTxConfig cfg;
  cfg.freq_hz = FREQ_CH3;
  const auto frame = io_frame(1);

  frame_log_hold();
  ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), cfg).sent());
  EXPECT_TRUE(log.lines().empty()) << "the hub is waiting for the reply it provoked: nothing may be printed";
  frame_log_release();

  const auto lines = frame_lines_of(log);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].rfind("TX [12 bytes] freq=2475000000 ", 0), 0u) << lines[0];
}

TEST(RadioBridleFrameLog, AReceivedFrameIsLoggedWithTheAirFrequencyItCameInOnAfterTheHoldEnds) {
  TracedRig rig;
  rig.radio.set_trace(false);
  ASSERT_TRUE(rig.radio.init());
  rig.radio.set_mode_rx();
  test::LogCapture log(true);
  RadioRxPacket packet;

  frame_log_hold();
  rig.cp.inject_rx_frame(2425000000u, psdu(0x20, 12));
  ASSERT_TRUE(rig.radio.wait_for_packet(packet, 5));
  EXPECT_EQ(packet.freq_hz, FREQ_CH1);
  EXPECT_TRUE(log.lines().empty()) << "the hub is waiting for the reply: nothing may be printed";
  frame_log_release();

  const auto lines = frame_lines_of(log);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].rfind("RX [12 bytes] freq=2425000000 ", 0), 0u) << lines[0];
}

TEST(RadioBridleFrameLog, AFrameWithABadCrcIsNotLoggedAsReceived) {
  TracedRig rig;
  rig.radio.set_trace(false);
  ASSERT_TRUE(rig.radio.init());
  rig.radio.set_mode_rx();
  test::LogCapture log(true);
  RadioRxPacket packet;
  rig.cp.inject_rx_frame(2450000000u, psdu(0x20, 12), FcsState::BAD);
  EXPECT_FALSE(rig.radio.wait_for_packet(packet, 5));
  EXPECT_TRUE(frame_lines_of(log).empty());
}

TEST(RadioBridleFrameLog, AFrameTheCoprocessorRefusedForABusyChannelIsNotLoggedAsSent) {
  TracedRig rig;
  rig.radio.set_trace(false);
  ASSERT_TRUE(rig.radio.init());
  rig.radio.set_mode_rx();
  rig.cp.set_channel_level_dbm(-60);
  test::LogCapture log(true);
  RadioTxConfig cfg;
  cfg.cca_threshold_dbm = -75;
  const auto frame = io_frame(1);
  EXPECT_EQ(rig.radio.send_packet(frame.data(), frame.size(), cfg).status, TxResult::Status::CHANNEL_BUSY);
  EXPECT_TRUE(frame_lines_of(log).empty());
}
#endif  // IOHOME_FRAME_LOG

TEST(RadioBridle, AWarningTheReceivePathWritesIsHeldToo) {
  TracedRig rig;
  ASSERT_TRUE(rig.radio.init());
  rig.radio.set_trace(false);
  test::LogCapture log(true);
  frame_log_hold();
  rig.cp.inject_log(LogLevel::WARN, "radio hot");
  RadioRxPacket packet;
  rig.radio.check_for_packet(packet);
  EXPECT_TRUE(log.lines().empty()) << "the co-processor's own log line is a line of the receive path";
  frame_log_release();
  ASSERT_EQ(log.lines().size(), 1u);
  EXPECT_NE(log.lines()[0].text.find("radio hot"), std::string::npos);
  EXPECT_EQ(log.lines()[0].level, 'W');
}

TEST(RadioBridle, TheChannelIsMappedToItsAirFrequencyButKeptLogicalInTheDriver) {
  Rig rig;
  rig.start();
  RadioTxConfig cfg;
  cfg.freq_hz = FREQ_CH3;
  const auto frame = io_frame(1);
  ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), cfg).sent());
  EXPECT_EQ(rig.last_command<Tx>().freq_hz, 2475000000u);
  EXPECT_EQ(rig.radio.get_current_freq(), FREQ_CH3);
}

TEST(RadioBridle, LogLinesGetTheAirFrequencyOfAChannelThroughTheBand) {
  Rig rig;
  rig.start();
  EXPECT_EQ(rig.radio.band().air_hz_of(FREQ_CH1), 2425000000u);
  EXPECT_EQ(rig.radio.band().air_hz_of(FREQ_CH2), 2450000000u);
  EXPECT_EQ(rig.radio.band().air_hz_of(FREQ_CH3), 2475000000u);
  EXPECT_EQ(rig.radio.band().air_hz_of(2450000000u), 2450000000u)
      << "a value that is not a channel comes back unchanged";
}

TEST(RadioBridle, TheClearChannelCheckLogsItsRetuneWithTheAirFrequency) {
  Rig rig;
  rig.start();
  rig.radio.set_mode_rx();
  test::LogCapture log(true);
  rig.radio.measure_channel_level(FREQ_CH3);
  const auto retune = std::find_if(log.lines().begin(), log.lines().end(), [](const auto &line) {
    return line.text.rfind("Clear-channel check retuned to ", 0) == 0;
  });
  ASSERT_NE(retune, log.lines().end());
  EXPECT_NE(retune->text.find("to 2475000000 Hz"), std::string::npos) << retune->text;
}

TEST(RadioBridle, CcaIsRequestedOnlyWithAThresholdAndTheCapability) {
  {
    Rig rig;
    rig.start();
    RadioTxConfig cfg;
    cfg.cca_threshold_dbm = -75;
    const auto frame = io_frame(1);
    ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), cfg).sent());
    Tx tx = rig.last_command<Tx>();
    EXPECT_TRUE(tx.cca);
    EXPECT_EQ(tx.cca_threshold_dbm, -75);
  }
  {
    FakeCoprocessor::Config cfg;
    auto no_cca = FakeCoprocessor::reference_802154_radio(0);
    no_cca.caps &= ~radio_caps::CCA;
    cfg.radios = {no_cca};
    Rig rig(cfg);
    rig.start();
    RadioTxConfig tx_cfg;
    tx_cfg.cca_threshold_dbm = -75;
    const auto frame = io_frame(1);
    ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), tx_cfg).sent());
    EXPECT_FALSE(rig.last_command<Tx>().cca) << "without the capability the frame is sent unchecked";
  }
}

TEST(RadioBridle, ABusyChannelIsReportedWithItsLevelAndKeepsQueuedFrames) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.cp.inject_rx_frame(2450000000u, psdu(0x10));  // arrives before the send
  rig.cp.set_channel_level_dbm(-60);
  RadioTxConfig cfg;
  cfg.cca_threshold_dbm = -75;
  const auto frame = io_frame(1);
  TxResult result = rig.radio.send_packet(frame.data(), frame.size(), cfg);
  EXPECT_EQ(result.status, TxResult::Status::CHANNEL_BUSY);
  EXPECT_EQ(result.cca_level_dbm, -60);
  EXPECT_TRUE(rig.cp.send_timeline().sent_data().empty()) << "nothing was sent";
  RadioRxPacket packet;
  EXPECT_TRUE(rig.radio.check_for_packet(packet)) << "nothing was transmitted, so the queued frame is still ours";
}

TEST(RadioBridle, ALateStrobeFrameStillCountsAsSent) {
  Rig rig;
  rig.start();
  rig.cp.set_next_strobe_late(2);
  RadioTxConfig cfg;
  cfg.wake = TxWake::SHORT;
  const auto frame = io_frame(1);
  EXPECT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), cfg).sent());
  EXPECT_EQ(rig.radio.strobe_late_total(), 2u);
}

TEST(RadioBridle, ARefusedTxFails) {
  Rig rig;
  rig.start();
  rig.cp.reject_next(MsgType::TX, ErrorCode::RADIO_FAULT, 9);
  const auto frame = io_frame(1);
  EXPECT_EQ(rig.radio.send_packet(frame.data(), frame.size(), {}).status, TxResult::Status::FAILED);
}

TEST(RadioBridle, WithoutATxDoneSendPacketGivesUpAfterTheTransmissionPlusAMargin) {
  Rig rig;
  rig.start();
  rig.cp.set_silent(true);
  RadioTxConfig cfg;
  const auto frame = io_frame(1);
  uint64_t const start = Rig::now();
  EXPECT_EQ(rig.radio.send_packet(frame.data(), frame.size(), cfg).status, TxResult::Status::FAILED);
  uint64_t const waited = Rig::now() - start;
  EXPECT_GE(waited, 100000u) << "at least the margin";
  EXPECT_LT(waited, 110000u);
}

TEST(RadioBridle, OversizeOrEmptyFramesAreNotSent) {
  Rig rig;
  rig.start();
  uint8_t big[200] = {};
  EXPECT_FALSE(rig.radio.send_packet(big, 0, {}).sent());
  EXPECT_FALSE(rig.radio.send_packet(big, 200, {}).sent());
  EXPECT_TRUE(rig.cp.commands_of(MsgType::TX).empty());
}

TEST(RadioBridle, NoSetParamsGoesOutBetweenATxAndItsTxDone) {
  Rig rig;
  rig.start();
  const auto frame = io_frame(1);
  for (int i = 0; i < 3; i++) {
    ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), {}).sent());
    rig.radio.change_frequency(FREQ_CH1);
    rig.radio.set_mode_rx();
  }
  EXPECT_EQ(rig.cp.set_params_during_tx(), 0u);
}

TEST(RadioBridle, AirTimeEstimateCoversTheStrobeAndTheCommandOnTheWire) {
  Rig rig;
  rig.start();
  RadioTxConfig none;
  RadioTxConfig lng;
  lng.wake = TxWake::LONG;
  uint32_t const plain = rig.radio.tx_air_time_us(12, none);
  uint32_t const strobe = rig.radio.tx_air_time_us(12, lng);
  EXPECT_GE(plain, 704u);  // (6 + 15 + 2) x 32 µs is 736; the exact number is the PHY's business
  EXPECT_LT(plain, 1500u);
  // The PHY's own test pins 510 676 µs for the train and a 12-byte main frame; the command's UART time adds
  // about a millisecond on top.
  EXPECT_GE(strobe, 510676u) << "the LONG train, ~511 ms";
  EXPECT_LT(strobe, 512700u);
  EXPECT_GT(strobe, plain + 500000u);
}

TEST(RadioBridle, ByteTimeFollowsTheBaudRateRoundedUp) {
  EXPECT_EQ(RadioBridle::byte_time_us(1000000), 10u);
  EXPECT_EQ(RadioBridle::byte_time_us(921600), 11u);  // 10.85 µs
  EXPECT_EQ(RadioBridle::byte_time_us(460800), 22u);  // 21.7 µs
  EXPECT_EQ(RadioBridle::byte_time_us(115200), 87u);  // 86.8 µs
}

TEST(RadioBridle, AirTimeEstimateGrowsWithTheCommandsUartTimeAtALowerRate) {
  FakeByteStream fast;
  FakeByteStream slow;
  slow.set_baud_rate(115200);
  FakeResetLine reset;
  TestableRadioBridle fast_radio(&fast, io2g4_phy(), &reset, std::nullopt);
  TestableRadioBridle slow_radio(&slow, io2g4_phy(), &reset, std::nullopt);
  RadioTxConfig none;
  // A 12-byte frame wraps to a command of 20 + wrapped bytes: each costs 77 µs more at 115200.
  uint32_t const extra = slow_radio.tx_air_time_us(12, none) - fast_radio.tx_air_time_us(12, none);
  EXPECT_GE(extra, 20u * 77u);
  EXPECT_LT(extra, 80u * 77u);
}

// === Receive ===

TEST(RadioBridle, AReceivedFrameArrivesWithItsLogicalChannelAndTheCaptureWithItsAirFrequency) {
  Rig rig;
  rig.start();
  listen_on(rig);
  auto p = psdu(0x20, 12);
  rig.cp.set_rssi_dbm(-71);
  rig.cp.inject_rx_frame(2475000000u, p);
  RadioRxPacket packet;
  ASSERT_TRUE(rig.radio.wait_for_packet(packet, 5));
  EXPECT_EQ(packet.freq_hz, FREQ_CH3);
  ASSERT_EQ(packet.len, 12);
  EXPECT_EQ(0, std::memcmp(packet.data, io_frame(0x20).data(), 12));
  const RadioCaptureInfo &cap = rig.radio.get_last_capture();
  EXPECT_TRUE(cap.valid);
  EXPECT_TRUE(cap.rx_done);
  EXPECT_FALSE(cap.crc_error);
  EXPECT_EQ(cap.rssi_dbm, -71);
  EXPECT_EQ(cap.freq_hz, 2475000000u);  // what the io_capture log line prints
  EXPECT_EQ(cap.frame_len, 12);
  EXPECT_EQ(cap.reported_len, p.size());
}

TEST(RadioBridle, WaitForPacketTimesOutAndLeavesTheRadioReceiving) {
  Rig rig;
  rig.start();
  RadioRxPacket packet;
  uint64_t const start = Rig::now();
  EXPECT_FALSE(rig.radio.wait_for_packet(packet, 10));
  EXPECT_GE(Rig::now() - start, 9000u) << "the timeout is counted in whole milliseconds";
  EXPECT_TRUE(rig.cp.receiving()) << "wait_for_packet ends with the radio in RX";
}

TEST(RadioBridle, CheckForPacketNeverWaits) {
  Rig rig;
  rig.start();
  listen_on(rig);
  RadioRxPacket packet;
  uint64_t const start = Rig::now();
  EXPECT_FALSE(rig.radio.check_for_packet(packet));
  EXPECT_LT(Rig::now() - start, 1000u);
}

TEST(RadioBridle, ABadCrcFrameIsNotDeliveredButShowsInTheCapture) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.cp.inject_rx_frame(2450000000u, psdu(0x30), FcsState::BAD);
  RadioRxPacket packet;
  EXPECT_FALSE(rig.radio.wait_for_packet(packet, 5));
  EXPECT_TRUE(rig.radio.get_last_capture().crc_error);
  EXPECT_EQ(rig.radio.bad_crc_frames(), 1u);
}

TEST(RadioBridle, AHeaderMismatchIsDroppedAndCounted) {
  Rig rig;
  rig.start();
  listen_on(rig);
  auto p = psdu(0x30);
  p[0] = 0x02;
  rig.cp.inject_rx_frame(2450000000u, p);
  RadioRxPacket packet;
  EXPECT_FALSE(rig.radio.wait_for_packet(packet, 5));
  EXPECT_EQ(rig.radio.header_mismatches(), 1u);
}

TEST(RadioBridle, AnOversizePayloadIsDroppedAndCounted) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.cp.inject_rx_frame(2450000000u, std::vector<uint8_t>(MAX_MESSAGE_SIZE - 20, 0x55));
  RadioRxPacket packet;
  EXPECT_FALSE(rig.radio.wait_for_packet(packet, 5));
  EXPECT_EQ(rig.radio.oversize_dropped(), 1u);
}

TEST(RadioBridle, TheCoprocessorIsToldToDropFramesTooShortForAnIoFrame) {
  Rig rig;
  rig.start();
  uint8_t value = 0;
  bool found = false;
  for (const auto &c : rig.commands_of<SetParams>(MsgType::SET_PARAMS)) {
    TlvReader in(c.second.tlvs);
    uint8_t id;
    ByteSpan v;
    while (in.next(id, v))
      if (id == static_cast<uint8_t>(ParamId::RX_MIN_PSDU) && v.size == 1) {
        found = true;
        value = v.data[0];
      }
  }
  ASSERT_TRUE(found);
  EXPECT_EQ(value, IO2G4_MIN_PSDU_SIZE);
}

TEST(RadioBridle, AStrobeOfShortFramesGivesTheHostNoEventAndTheFrameBehindItIsDelivered) {
  Rig rig;
  rig.start();
  listen_on(rig);
  for (int i = 0; i < 8; i++)
    rig.cp.inject_rx_frame(2450000000u, {0x00, 0x00, static_cast<uint8_t>(7 - i), 0xAA, 0xBB}, FcsState::OK, i * 100);
  rig.cp.inject_rx_frame(2450000000u, psdu(0x50), FcsState::OK, 1000);
  RadioRxPacket packet;
  ASSERT_TRUE(rig.radio.wait_for_packet(packet, 5));
  EXPECT_EQ(packet.data[0], 0x50);
  EXPECT_EQ(rig.radio.short_frames(), 0u) << "the co-processor dropped them; none reached the host";
  EXPECT_FALSE(rig.radio.check_for_packet(packet));
}

TEST(RadioBridle, TheRxShortCounterOfTheHeartbeatIsKeptForTheDump) {
  Rig rig;
  rig.start();
  listen_on(rig);
  for (int i = 0; i < 5; i++)
    rig.cp.inject_rx_frame(2450000000u, {0x00, 0x00, static_cast<uint8_t>(i), 0xAA, 0xBB});
  EXPECT_FALSE(rig.radio.have_rx_short());
  Rig::advance_to(Rig::now() + 30100 * 1000ull);
  RadioRxPacket packet;
  rig.radio.check_for_packet(packet);
  Rig::advance_to(Rig::now() + 50 * 1000ull);
  rig.radio.check_for_packet(packet);
  EXPECT_TRUE(rig.radio.have_rx_short());
  EXPECT_EQ(rig.radio.rx_short(), 5u);
  rig.radio.dump_debug();
}

TEST(RadioBridle, TheFifoDropsTheOldestWhenFull) {
  Rig rig;
  rig.start();
  listen_on(rig);
  for (uint8_t i = 0; i < 10; i++)
    rig.cp.inject_rx_frame(2450000000u, psdu(static_cast<uint8_t>(0x60 + i * 16)));
  RadioRxPacket packet;
  std::vector<uint8_t> tags;
  ASSERT_TRUE(rig.radio.wait_for_packet(packet, 5));
  tags.push_back(packet.data[0]);
  while (rig.radio.check_for_packet(packet))
    tags.push_back(packet.data[0]);
  ASSERT_EQ(tags.size(), 8u);
  EXPECT_EQ(tags.front(), 0x60 + 2 * 16) << "the two oldest are gone";
  EXPECT_EQ(tags.back(), 0x60 + 9 * 16);
  EXPECT_EQ(rig.radio.fifo_overflows(), 2u);
}

TEST(RadioBridle, ARxBeforeTheOwnTxDoneIsNotTheReply) {
  Rig rig;
  rig.start();
  listen_on(rig);
  RadioTxConfig cfg;
  const auto frame = io_frame(1);
  // One frame lands during the transmission (which lasts ~700 µs), one after it ends.
  rig.cp.inject_rx_frame(2450000000u, psdu(0x70), FcsState::OK, 300);
  rig.cp.inject_rx_frame(2450000000u, psdu(0x80), FcsState::OK, 3000);
  ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), cfg).sent());
  EXPECT_EQ(rig.radio.dropped_before_tx_done(), 1u);
  RadioRxPacket packet;
  ASSERT_TRUE(rig.radio.wait_for_packet(packet, 10));
  EXPECT_EQ(packet.data[0], 0x80);
  EXPECT_FALSE(rig.radio.check_for_packet(packet));
}

TEST(RadioBridle, AFrameReceivedBeforeTheSendIsAlsoStale) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.cp.inject_rx_frame(2450000000u, psdu(0x90));
  RadioTxConfig cfg;
  const auto frame = io_frame(1);
  ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), cfg).sent());
  RadioRxPacket packet;
  EXPECT_FALSE(rig.radio.wait_for_packet(packet, 5)) << "a late reply to an earlier try is never the current one";
}

// === Reception state ===

TEST(RadioBridle, TheGettersDrainBeforeTheyAnswer) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.cp.inject_rx_start(2450000000u, RxStage::SYNC);
  EXPECT_TRUE(rig.radio.is_sync_detected());
  EXPECT_FALSE(rig.radio.is_preamble_detected());
  EXPECT_TRUE(rig.radio.reception_in_progress());
  rig.cp.inject_rx_frame(2450000000u, psdu(0x10));
  EXPECT_FALSE(rig.radio.is_sync_detected()) << "the frame ended";
  EXPECT_FALSE(rig.radio.reception_in_progress());
}

TEST(RadioBridle, IdleDropsAFrameInProgressWithoutAnAbort) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.cp.inject_rx_start(2450000000u, RxStage::SYNC);
  ASSERT_TRUE(rig.radio.reception_in_progress());
  rig.radio.set_mode_standby();
  EXPECT_FALSE(rig.radio.reception_in_progress());
  EXPECT_FALSE(rig.radio.is_sync_detected());
}

TEST(RadioBridle, AnAbortEndsTheReceptionState) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.cp.inject_rx_start(2450000000u, RxStage::SYNC);
  ASSERT_TRUE(rig.radio.is_sync_detected());
  rig.cp.inject_rx_abort(2450000000u, RxAbortReason::NO_END_AFTER_SYNC);
  EXPECT_FALSE(rig.radio.is_sync_detected());
  EXPECT_FALSE(rig.radio.reception_in_progress());
}

TEST(RadioBridle, ThePreambleStageIsReportedAsAPreamble) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.cp.inject_rx_start(2450000000u, RxStage::PREAMBLE);
  EXPECT_TRUE(rig.radio.is_preamble_detected());
  EXPECT_FALSE(rig.radio.is_sync_detected());
}

// === Tuning, modes, scan, RSSI ===

TEST(RadioBridle, ChangeFrequencyOnAnIdleRadioOnlyRecordsTheFrequency) {
  Rig rig;
  rig.start();
  rig.radio.change_frequency(FREQ_CH3);
  EXPECT_EQ(rig.cp.commands_of(MsgType::RX_ON).size(), 0u) << "nothing is sent to a radio that is not receiving";
  EXPECT_EQ(rig.radio.get_current_freq(), FREQ_CH3);
  rig.radio.set_mode_rx();
  RxOn rx_on = rig.last_command<RxOn>();
  EXPECT_EQ(rx_on.freq_hz, 2475000000u) << "the next receive starts on the recorded frequency";
  EXPECT_FALSE(rx_on.force);
  EXPECT_EQ(rig.cp.freq_hz(), 2475000000u);
}

TEST(RadioBridle, ChangeFrequencyOnAReceivingRadioRetunesWithRxOnAndSkipsARepeat) {
  Rig rig;
  rig.start();
  rig.radio.set_mode_rx();
  size_t const before = rig.cp.commands_of(MsgType::RX_ON).size();
  rig.radio.change_frequency(FREQ_CH3);
  ASSERT_EQ(rig.cp.commands_of(MsgType::RX_ON).size(), before + 1);
  RxOn rx_on = rig.last_command<RxOn>();
  EXPECT_EQ(rx_on.freq_hz, 2475000000u);
  EXPECT_FALSE(rx_on.force);
  rig.radio.change_frequency(FREQ_CH3);
  EXPECT_EQ(rig.cp.commands_of(MsgType::RX_ON).size(), before + 1);
  EXPECT_EQ(rig.radio.get_current_freq(), FREQ_CH3);
}

TEST(RadioBridle, OnlyAChannelChecksRetuneIsForced) {
  Rig rig;
  rig.start();
  rig.radio.set_mode_rx();
  rig.radio.change_frequency(FREQ_CH3);
  EXPECT_FALSE(rig.last_command<RxOn>().force) << "an idle hop waits for the frame in progress";
  rig.radio.set_mode_rx();
  EXPECT_FALSE(rig.last_command<RxOn>().force);
  rig.radio.change_frequency(FREQ_CH1);
  EXPECT_FALSE(rig.last_command<RxOn>().force) << "so does a rotating listen";

  size_t const before = rig.cp.commands_of(MsgType::RX_ON).size();
  rig.cp.set_channel_rssi_dbm(-90);
  EXPECT_EQ(rig.radio.measure_channel_level(FREQ_CH2), -90);
  ASSERT_EQ(rig.cp.commands_of(MsgType::RX_ON).size(), before + 1);
  RxOn const check = rig.commands_of<RxOn>(MsgType::RX_ON).back().second;
  EXPECT_TRUE(check.force) << "the channel check retunes at once";
  EXPECT_EQ(check.freq_hz, rig.radio.band().air_hz_of(FREQ_CH2));

  EXPECT_EQ(rig.radio.measure_channel_level(FREQ_CH2), -90);
  EXPECT_EQ(rig.cp.commands_of(MsgType::RX_ON).size(), before + 1)
      << "the readings that follow in the same check need no retune of their own";

  // Anything sent in between (here an idle hop, which records the channel the next check asks for) starts the next
  // check with a forced retune again, even though the recorded frequency is the checked one.
  rig.radio.change_frequency(FREQ_CH3);
  EXPECT_EQ(rig.cp.commands_of(MsgType::RX_ON).size(), before + 2);
  EXPECT_EQ(rig.radio.measure_channel_level(FREQ_CH3), -90);
  ASSERT_EQ(rig.cp.commands_of(MsgType::RX_ON).size(), before + 3);
  RxOn const again = rig.commands_of<RxOn>(MsgType::RX_ON).back().second;
  EXPECT_TRUE(again.force) << "a check on the recorded channel sends its forced retune too";
  EXPECT_EQ(again.freq_hz, rig.radio.band().air_hz_of(FREQ_CH3));

  // So do a send, a scan and `idle` in between.
  auto forced_retune_before_next_check = [&rig](const char *what) {
    size_t const n = rig.cp.commands_of(MsgType::RX_ON).size();
    EXPECT_EQ(rig.radio.measure_channel_level(FREQ_CH3), -90) << what;
    ASSERT_EQ(rig.cp.commands_of(MsgType::RX_ON).size(), n + 1) << what;
    EXPECT_TRUE(rig.commands_of<RxOn>(MsgType::RX_ON).back().second.force) << what;
  };
  const auto frame = io_frame(1);
  RadioTxConfig tx_config{};
  tx_config.freq_hz = FREQ_CH3;
  ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), tx_config).sent());
  forced_retune_before_next_check("after a send");
  ASSERT_TRUE(rig.radio.run_idle_scan());
  forced_retune_before_next_check("after a scan");
  rig.radio.set_mode_standby();
  forced_retune_before_next_check("after idle");
}

// The race: an unforced retune (idle hop, rotating listen) went out before the driver saw the preamble that holds it,
// and the request's check follows on that very channel. The recorded frequency already is the checked one, so the
// forced retune must still go out: it replaces the waiting one and the reading is not `radio_busy`.
TEST(RadioBridle, AChannelCheckOnTheChannelAWaitingRetuneAsksForGivesAReadingNotBusy) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.cp.set_channel_rssi_dbm(-90);
  rig.cp.hold_preamble(1000000);
  rig.radio.change_frequency(FREQ_CH3);  // waits behind the preamble the driver has not read yet
  uint32_t const busy_before = rig.radio.rssi_busy();

  EXPECT_EQ(rig.radio.measure_channel_level(FREQ_CH3), -90) << "the forced retune drops the held preamble";
  EXPECT_EQ(rig.radio.rssi_busy(), busy_before);
  EXPECT_TRUE(rig.commands_of<RxOn>(MsgType::RX_ON).back().second.force);
}

// By specification §2.2 a refused command changes nothing: the forced retune to the recorded frequency leaves the
// co-processor receiving where it was, and the driver's record (frequency) still matches it after the refusal. A
// reading is only of the channel the retune it follows has put the radio on, and that retune was refused: the
// driver gives none, even though the radio happens to sit on the channel asked for (a reading is used only when the
// retune before it was accepted). The optimistic `receiving` flag is undone, as for any refused unawaited `rx_on`,
// so the next listen asks again on the same channel.
TEST(RadioBridle, ARefusedForcedRetuneToTheRecordedChannelLeavesTheRecordMatchingTheCoprocessor) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.radio.change_frequency(FREQ_CH3);
  uint32_t const cp_freq_before = rig.cp.freq_hz();
  rig.cp.reject_next(MsgType::RX_ON, ErrorCode::OUT_OF_RANGE, 1, 0);
  rig.cp.set_channel_rssi_dbm(-90);

  EXPECT_FALSE(rig.radio.measure_channel_level(FREQ_CH3).has_value()) << "the retune the reading follows was refused";
  EXPECT_EQ(rig.radio.unawaited_errors(), 1u);
  EXPECT_TRUE(rig.cp.receiving()) << "a refusal changed nothing";
  EXPECT_EQ(rig.cp.freq_hz(), cp_freq_before);
  EXPECT_EQ(rig.radio.get_current_freq(), FREQ_CH3) << "the recorded frequency is still the co-processor's";
  EXPECT_EQ(rig.radio.link_state(), TestableRadioBridle::Link::RUNNING);
  EXPECT_FALSE(rig.radio.rx_on_active());
  rig.radio.set_mode_rx();
  EXPECT_EQ(rig.last_command<RxOn>().freq_hz, rig.radio.band().air_hz_of(FREQ_CH3));
  EXPECT_TRUE(rig.radio.rx_on_active());
}

// A reading follows the retune right behind it, and the refusal of that retune arrives only with the reading's reply.
// The radio then stayed on the old channel and `get_rssi` reports that one's level: it is not the level of the channel
// asked for, so there is no reading, whether the retune was forced or not.
TEST(RadioBridle, ARefusedUnforcedRetuneLeavesTheReadingThatFollowsWithoutALevel) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.cp.set_channel_rssi_dbm(-90);
  ASSERT_EQ(rig.radio.read_rssi(), -90) << "the same reading without the refusal is a level";

  rig.cp.reject_next(MsgType::RX_ON, ErrorCode::OUT_OF_RANGE, 1, 0);
  rig.radio.change_frequency(FREQ_CH1);
  uint32_t const refused_before = rig.radio.rssi_refused();
  EXPECT_FALSE(rig.radio.read_rssi().has_value()) << "the old channel's level is not the new channel's";
  EXPECT_EQ(rig.radio.rssi_refused(), refused_before + 1);
  EXPECT_EQ(rig.radio.link_state(), TestableRadioBridle::Link::RUNNING);
}

TEST(RadioBridle, ARefusedForcedRetuneToAnotherChannelLeavesTheReadingWithoutALevel) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.radio.change_frequency(FREQ_CH3);
  rig.cp.set_channel_rssi_dbm(-90);
  rig.cp.reject_next(MsgType::RX_ON, ErrorCode::OUT_OF_RANGE, 1, 0);

  EXPECT_FALSE(rig.radio.measure_channel_level(FREQ_CH2).has_value());
  EXPECT_EQ(rig.cp.freq_hz(), rig.radio.band().air_hz_of(FREQ_CH3)) << "the radio stayed on the old channel";
}

TEST(RadioBridle, ALaterAcceptedRetuneGivesReadingsAgain) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.cp.set_channel_rssi_dbm(-90);
  rig.cp.reject_next(MsgType::RX_ON, ErrorCode::OUT_OF_RANGE, 1, 0);
  EXPECT_FALSE(rig.radio.measure_channel_level(FREQ_CH2).has_value());

  rig.radio.set_mode_rx();  // a new command: the refusal no longer describes the radio
  EXPECT_EQ(rig.radio.read_rssi(), -90);
}

// A check on an idle radio starts the receiver with its forced retune: one `rx_on` for all of the check's readings,
// within the trace budget of a check (TRACE_CHANNEL_CHECK_LINES). While a refused `rx_on` is held off, none is sent
// and there is no reading.
TEST(RadioBridle, AChannelCheckOnAnIdleRadioStartsTheReceiverWithItsForcedRetune) {
  Rig rig;
  rig.start();
  rig.radio.set_mode_standby();
  rig.cp.set_channel_rssi_dbm(-90);
  size_t const before = rig.cp.commands_of(MsgType::RX_ON).size();
  for (int reading = 0; reading < 4; reading++)
    EXPECT_EQ(rig.radio.measure_channel_level(FREQ_CH2), -90);
  ASSERT_EQ(rig.cp.commands_of(MsgType::RX_ON).size(), before + 1);
  RxOn const start = rig.commands_of<RxOn>(MsgType::RX_ON).back().second;
  EXPECT_TRUE(start.force);
  EXPECT_EQ(start.freq_hz, rig.radio.band().air_hz_of(FREQ_CH2));

  rig.radio.set_mode_standby();
  rig.cp.reject_next(MsgType::RX_ON, ErrorCode::OUT_OF_RANGE, 1, 0);
  EXPECT_FALSE(rig.radio.measure_channel_level(FREQ_CH3).has_value()) << "the refusal reaches the reading's wait";
  size_t const refused = rig.cp.commands_of(MsgType::RX_ON).size();
  rig.radio.set_mode_standby();
  EXPECT_FALSE(rig.radio.measure_channel_level(FREQ_CH3).has_value());
  EXPECT_EQ(rig.cp.commands_of(MsgType::RX_ON).size(), refused) << "a refused rx_on is not repeated at once";
}

// A check on the channel a frame is arriving on: the forced retune to the frequency the radio is on drops nothing
// (spec §3.1), so the frame stays in progress and the reading measures it.
TEST(RadioBridle, AChannelCheckOnTheChannelAFrameArrivesOnKeepsTheFrame) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.radio.change_frequency(FREQ_CH3);
  rig.cp.set_channel_rssi_dbm(-60);
  rig.cp.hold_preamble(1000000);  // on CH3, where the radio receives
  RadioRxPacket none;
  EXPECT_FALSE(rig.radio.check_for_packet(none));  // takes the rx_start in
  ASSERT_TRUE(rig.radio.is_preamble_detected());

  EXPECT_EQ(rig.radio.measure_channel_level(FREQ_CH3), -60);
  EXPECT_TRUE(rig.commands_of<RxOn>(MsgType::RX_ON).back().second.force);
  EXPECT_FALSE(rig.radio.check_for_packet(none));
  EXPECT_TRUE(rig.radio.is_preamble_detected()) << "no rx_abort: the frame is still arriving";
}

// The forced send ranks the channels one reading after another with nothing sent in between: each channel gets its
// own forced retune, so no reading describes the channel before it.
TEST(RadioBridle, AReadingOfAnotherChannelRightAfterACheckRetunesToIt) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.cp.set_channel_rssi_dbm(-90);
  ASSERT_EQ(rig.radio.measure_channel_level(FREQ_CH2), -90);
  size_t const before = rig.cp.commands_of(MsgType::RX_ON).size();
  EXPECT_EQ(rig.radio.measure_channel_level(FREQ_CH3), -90);
  ASSERT_EQ(rig.cp.commands_of(MsgType::RX_ON).size(), before + 1);
  RxOn const next = rig.commands_of<RxOn>(MsgType::RX_ON).back().second;
  EXPECT_TRUE(next.force);
  EXPECT_EQ(next.freq_hz, rig.radio.band().air_hz_of(FREQ_CH3));
  EXPECT_EQ(rig.cp.freq_hz(), rig.radio.band().air_hz_of(FREQ_CH3));
}

// Command ids wrap after 255 commands. A later command that happens to get the id of an old check's forced retune is
// not that retune: here the unforced `rx_on` that ends a scan, which may wait behind a frame on the scanned channel.
TEST(RadioBridle, ACommandThatReusesTheIdOfAnOldChecksRetuneDoesNotStandInForIt) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.cp.set_channel_rssi_dbm(-90);
  ASSERT_EQ(rig.radio.measure_channel_level(FREQ_CH2), -90);
  auto const check = rig.commands_of<RxOn>(MsgType::RX_ON).back();
  ASSERT_TRUE(check.second.force);
  uint8_t const check_seq = check.first.seq;
  auto next_seq = [&rig] {
    Message m;
    const auto &last = rig.cp.commands().back().message;
    EXPECT_EQ(decode_message(last.data(), last.size(), m), DecodeStatus::OK);
    return static_cast<uint8_t>(m.seq == UINT8_MAX ? 1 : m.seq + 1);
  };
  uint8_t const scan_seq = check_seq == 1 ? UINT8_MAX : static_cast<uint8_t>(check_seq - 1);
  for (int i = 0; i < 300 && next_seq() != scan_seq; i++)
    ASSERT_TRUE(rig.radio.read_rssi().has_value());  // each reading is a command and takes an id
  ASSERT_EQ(next_seq(), scan_seq);
  ASSERT_TRUE(rig.radio.run_idle_scan());
  rig.radio.set_mode_rx();
  ASSERT_EQ(rig.commands_of<RxOn>(MsgType::RX_ON).back().first.seq, check_seq) << "the id came round again";
  ASSERT_EQ(rig.radio.get_current_freq(), FREQ_CH2);

  size_t const before = rig.cp.commands_of(MsgType::RX_ON).size();
  EXPECT_EQ(rig.radio.measure_channel_level(FREQ_CH2), -90);
  ASSERT_EQ(rig.cp.commands_of(MsgType::RX_ON).size(), before + 1) << "the new check sends its own forced retune";
  EXPECT_TRUE(rig.commands_of<RxOn>(MsgType::RX_ON).back().second.force);
}

// Command ids wrap after 255 commands. A refused retune keeps its id only until a newer receive command goes out, an
// `rx_on` or a `tx`; one that comes round with the same id later was accepted and its reading is a level.
TEST(RadioBridle, ACommandThatReusesTheIdOfARefusedRetuneGivesReadings) {
  enum class Next : uint8_t { RX_ON, TX };
  for (Next const next : {Next::RX_ON, Next::TX}) {
    SCOPED_TRACE(next == Next::RX_ON ? "the id comes round on an rx_on" : "the id comes round on a tx");
    Rig rig;
    rig.start();
    listen_on(rig);
    rig.cp.set_channel_rssi_dbm(-90);
    rig.cp.reject_next(MsgType::RX_ON, ErrorCode::OUT_OF_RANGE, 1, 0);
    rig.radio.set_mode_rx();
    uint8_t const refused_seq = rig.commands_of<RxOn>(MsgType::RX_ON).back().first.seq;
    ASSERT_FALSE(rig.radio.read_rssi().has_value()) << "the refusal is taken in";
    auto next_seq = [&rig] {
      Message m;
      const auto &last = rig.cp.commands().back().message;
      EXPECT_EQ(decode_message(last.data(), last.size(), m), DecodeStatus::OK);
      return static_cast<uint8_t>(m.seq == UINT8_MAX ? 1 : m.seq + 1);
    };
    const auto frame = io_frame(1);
    auto send_next = [&] {
      if (next == Next::RX_ON)
        rig.radio.set_mode_rx();
      else
        ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), {}).sent());
    };
    send_next();  // accepted: the radio receives again, and the refusal no longer describes it
    for (int i = 0; i < 300 && next_seq() != refused_seq; i++)
      ASSERT_TRUE(rig.radio.read_rssi().has_value());  // each reading is a command and takes an id
    ASSERT_EQ(next_seq(), refused_seq);
    send_next();

    EXPECT_EQ(rig.radio.read_rssi(), -90) << "the command with the refused one's id was accepted";
  }
}

TEST(RadioBridle, AChannelCheckDuringAHeldPreambleGivesAReadingAndTheAbortIsNoFault) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.cp.set_channel_rssi_dbm(-90);
  rig.cp.hold_preamble(1000000);  // a preamble without a sync word, held for the profile's sync_timeout_us
  RadioRxPacket none;
  EXPECT_FALSE(rig.radio.check_for_packet(none));  // takes the rx_start in
  ASSERT_TRUE(rig.radio.is_preamble_detected());
  uint32_t const busy_before = rig.radio.rssi_busy();

  EXPECT_EQ(rig.radio.measure_channel_level(FREQ_CH3), -90) << "the retune does not wait for the preamble";
  EXPECT_FALSE(rig.radio.check_for_packet(none));  // takes the rx_abort(forced_retune) in
  EXPECT_FALSE(rig.radio.is_preamble_detected());
  EXPECT_FALSE(rig.radio.reception_in_progress()) << "the dropped frame is not one the hub waits for";
  EXPECT_EQ(rig.radio.rssi_busy(), busy_before);
  EXPECT_EQ(rig.radio.resyncs(), 0u);
  EXPECT_EQ(rig.radio.unawaited_errors(), 0u);
  EXPECT_EQ(rig.radio.link_state(), TestableRadioBridle::Link::RUNNING);

  // The reply listening that follows is not affected: a frame on the new channel arrives.
  rig.cp.inject_rx_frame(rig.cp.freq_hz(), psdu(0x30), FcsState::OK, 100);
  RadioRxPacket packet;
  ASSERT_TRUE(rig.radio.wait_for_packet(packet, 5));
  EXPECT_EQ(packet.freq_hz, FREQ_CH3);
}

TEST(RadioBridle, AnUnforcedRetuneBehindAHeldPreambleLeavesTheReadingBusy) {
  Rig rig;
  rig.start();
  listen_on(rig);
  rig.cp.set_channel_rssi_dbm(-90);
  rig.cp.hold_preamble(1000000);
  rig.radio.change_frequency(FREQ_CH3);  // not a channel check: it waits for the frame in progress
  EXPECT_FALSE(rig.radio.read_rssi().has_value()) << "busy: the retune is still waiting";
  EXPECT_EQ(rig.radio.rssi_busy(), 1u);
}

TEST(RadioBridle, ModesMapToRxOnAndIdle) {
  Rig rig;
  rig.start();
  rig.radio.set_mode_rx();
  EXPECT_TRUE(rig.cp.receiving());
  rig.radio.set_mode_standby();
  EXPECT_FALSE(rig.cp.receiving());
}

TEST(RadioBridle, TheIdleScanIsStartedOnceAndRestartedAfterARetune) {
  Rig rig;
  rig.start();
  EXPECT_TRUE(rig.radio.run_idle_scan());
  EXPECT_TRUE(rig.radio.run_idle_scan());
  ASSERT_EQ(rig.cp.commands_of(MsgType::RX_SCAN).size(), 1u);
  RxScan scan = rig.last_command<RxScan>();
  EXPECT_EQ(scan.dwell_us, IO2G4_SCAN_DWELL_US);  // what the phy asks for (its own test pins the values)
  EXPECT_EQ(scan.hold_us, IO2G4_SCAN_HOLD_US);
  ASSERT_EQ(scan.num_freqs, 3);
  EXPECT_EQ(scan.freqs_hz[1], 2450000000u);
  EXPECT_TRUE(rig.cp.scanning());

  rig.radio.change_frequency(FREQ_CH1);
  EXPECT_FALSE(rig.radio.scanning());
  EXPECT_TRUE(rig.radio.run_idle_scan());
  EXPECT_EQ(rig.cp.commands_of(MsgType::RX_SCAN).size(), 2u);
}

TEST(RadioBridle, ATransmissionEndsTheScan) {
  Rig rig;
  rig.start();
  ASSERT_TRUE(rig.radio.run_idle_scan());
  const auto frame = io_frame(1);
  ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), {}).sent());
  EXPECT_FALSE(rig.radio.scanning());
  EXPECT_TRUE(rig.radio.run_idle_scan());
  EXPECT_EQ(rig.cp.commands_of(MsgType::RX_SCAN).size(), 2u);
}

TEST(RadioBridle, ReadRssiReportsTheLevelWhileReceivingAndNoValueOtherwise) {
  Rig rig;
  rig.start();
  rig.radio.set_mode_standby();
  rig.cp.set_rssi_dbm(-88);
  EXPECT_EQ(rig.radio.read_rssi(), -88) << "an idle radio is told to receive first, and the reading follows";
  EXPECT_TRUE(rig.cp.receiving());
  rig.radio.run_idle_scan();
  EXPECT_FALSE(rig.radio.read_rssi().has_value()) << "a reading during a scan belongs to no channel";
}

TEST(RadioBridle, WhatTheHubAsksOfTheDriverComesFromThePhy) {
  Rig rig;
  TuningConfig tuning;
  EXPECT_EQ(rig.radio.hop_dwell_ms(tuning), 2);
  EXPECT_EQ(rig.radio.cca_settle_us(), 0u) << "the co-processor holds the reply until it has read";
  EXPECT_EQ(rig.radio.preamble_bits_per_byte(), io2g4_phy().preamble_bits_per_byte());
  EXPECT_EQ(rig.radio.band().air_hz, IO_2G4_BAND.air_hz);
  EXPECT_EQ(rig.radio.band().late_answer_warn_ms, 30);
  EXPECT_TRUE(rig.radio.band().low_power_by_default);
  EXPECT_EQ(rig.radio.response_preamble(), rig.radio.preamble_bytes_for_ms(tuning.response_preamble_ms));
  EXPECT_TRUE(rig.radio.has_fast_tx_rx_turnaround());
  EXPECT_STREQ(rig.radio.chip_name(), "bridle");
}

TEST(RadioBridle, LogEventsAreShownAndNeverActedOn) {
  Rig rig;
  rig.start();
  rig.cp.inject_log(LogLevel::ERROR, "boom");
  rig.cp.inject_log(LogLevel::DEBUG, "detail");
  RadioRxPacket packet;
  EXPECT_FALSE(rig.radio.wait_for_packet(packet, 2));
  EXPECT_FALSE(rig.radio.is_failed());
  rig.radio.dump_debug();
}

// === The spec's scenario event streams ===

namespace {

const Scenario &scenario_named(const char *name) {
  for (size_t i = 0; i < SCENARIO_COUNT; i++) {
    if (std::string(SCENARIOS[i].name) == name)
      return SCENARIOS[i];
  }
  ADD_FAILURE() << "no scenario " << name;
  return SCENARIOS[0];
}

}  // namespace

TEST(RadioBridleScenario, OrderingAroundTxDeliversOnlyTheFrameAfterTxDone) {
  Rig rig;
  rig.start();
  listen_on(rig);
  uint64_t const t0 = Rig::now();
  rig.cp.play_scenario(scenario_named("ordering_around_tx"), t0);
  Rig::advance_to(t0 + 1000);
  const auto frame = io_frame(0x01);
  ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), {}).sent());
  RadioRxPacket packet;
  ASSERT_TRUE(rig.radio.wait_for_packet(packet, 10));
  EXPECT_EQ(packet.data[0], 0x0A);
  EXPECT_EQ(packet.data[2], 0x12) << "the frame the scenario delivers after tx_done, not the one before";
  EXPECT_FALSE(rig.radio.check_for_packet(packet));
  EXPECT_EQ(rig.radio.dropped_before_tx_done(), 1u);
}

TEST(RadioBridleScenario, ADeferredRetuneLosesNoFrame) {
  Rig rig;
  rig.start();
  listen_on(rig);
  uint64_t const t0 = Rig::now();
  rig.cp.play_scenario(scenario_named("retune_deferred_during_frame"), t0);
  Rig::advance_to(t0 + 300);
  rig.radio.change_frequency(FREQ_CH3);
  RadioRxPacket first;
  ASSERT_TRUE(rig.radio.wait_for_packet(first, 5));
  EXPECT_EQ(first.freq_hz, FREQ_CH2) << "the frame that was arriving finished on the old channel";
  RadioRxPacket second;
  ASSERT_TRUE(rig.radio.wait_for_packet(second, 10));
  EXPECT_EQ(second.freq_hz, FREQ_CH3);
  EXPECT_FALSE(rig.radio.check_for_packet(second));
}

TEST(RadioBridleScenario, AForcedRetuneAbortShowsNoFrameAndTheNextOneArrives) {
  Rig rig;
  rig.start();
  listen_on(rig);
  uint64_t const t0 = Rig::now();
  rig.cp.play_scenario(scenario_named("retune_forced_during_frame"), t0);
  Rig::advance_to(t0 + 300);
  rig.radio.change_frequency(FREQ_CH3);
  RadioRxPacket packet;
  ASSERT_TRUE(rig.radio.wait_for_packet(packet, 10));
  EXPECT_EQ(packet.freq_hz, FREQ_CH3);
  EXPECT_FALSE(rig.radio.check_for_packet(packet));
  EXPECT_FALSE(rig.radio.is_sync_detected());
}

TEST(RadioBridleScenario, AScanCatchingATrainDeliversTheFrameBehindIt) {
  Rig rig;
  rig.start();
  uint64_t const t0 = Rig::now();
  rig.cp.play_scenario(scenario_named("scan_catches_short_train_then_resumes"), t0);
  ASSERT_TRUE(rig.radio.run_idle_scan());
  RadioRxPacket packet;
  ASSERT_TRUE(rig.radio.wait_for_packet(packet, 20));
  EXPECT_EQ(packet.freq_hz, FREQ_CH2);
  EXPECT_EQ(packet.len, 11);
  Rig::advance_to(t0 + 26000);
  EXPECT_FALSE(rig.radio.wait_for_packet(packet, 5)) << "the second train is wake frames only";
  EXPECT_EQ(rig.radio.short_frames(), 8u + 12u) << "every wake frame is skipped, none delivered twice";
}

TEST(RadioBridleScenario, ACcaBusyThenClearSequenceGivesBusyThenSent) {
  Rig rig;
  rig.start();
  uint64_t const t0 = Rig::now();
  rig.cp.play_scenario(scenario_named("tx_cca_busy_then_clear"), t0);
  RadioTxConfig cfg;
  cfg.cca_threshold_dbm = -75;
  const auto frame = io_frame(0x01);
  Rig::advance_to(t0 + 1000);
  TxResult first = rig.radio.send_packet(frame.data(), frame.size(), cfg);
  EXPECT_EQ(first.status, TxResult::Status::CHANNEL_BUSY);
  EXPECT_EQ(first.cca_level_dbm, -75);
  Rig::advance_to(t0 + 5000);
  EXPECT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), cfg).sent());
}

// === The 868 MHz profile on a gfsk_raw radio ===

namespace {

/// A co-processor with one `gfsk_raw` radio, as the SX1262 behind a bridge.
FakeCoprocessor::Config gfsk_only() {
  FakeCoprocessor::Config c;
  c.radios = {FakeCoprocessor::reference_gfsk_radio(0)};
  return c;
}

/// A rig for the 868 MHz profile on `config`.
struct Rig868 : Rig {
  explicit Rig868(FakeCoprocessor::Config config = gfsk_only())
      : Rig(std::move(config), std::nullopt, io868_gfsk_phy()) {}
};

/// The ids of the TLVs of the radio configuration (the second `set_params`), in order.
std::vector<uint8_t> radio_param_ids(Rig &rig) {
  auto sets = rig.commands_of<SetParams>(MsgType::SET_PARAMS);
  std::vector<uint8_t> ids;
  if (sets.size() < 2)
    return ids;
  TlvReader r(sets[1].second.tlvs);
  uint8_t id;
  ByteSpan v;
  while (r.next(id, v))
    ids.push_back(id);
  return ids;
}

}  // namespace

TEST(RadioBridle868, TheConfigurationIsTheProfileThenTheModemParamsThenTheTxPower) {
  Rig868 rig;
  rig.start();
  const std::vector<uint8_t> expected = {
      static_cast<uint8_t>(ParamId::PHY_PROFILE),      static_cast<uint8_t>(ParamId::BITRATE_BPS),
      static_cast<uint8_t>(ParamId::FDEV_HZ),          static_cast<uint8_t>(ParamId::RX_BW_HZ),
      static_cast<uint8_t>(ParamId::PULSE_SHAPE),      static_cast<uint8_t>(ParamId::SYNC_WORD),
      static_cast<uint8_t>(ParamId::RX_CAPTURE_BYTES), static_cast<uint8_t>(ParamId::PREAMBLE_DETECT_BITS),
      static_cast<uint8_t>(ParamId::SYNC_TIMEOUT_US),  static_cast<uint8_t>(ParamId::TX_POWER_DBM)};
  EXPECT_EQ(radio_param_ids(rig), expected);
  EXPECT_EQ(rig.radio.applied_tx_power_dbm(), 22) << "no TX power asked: the radio's own maximum";
}

TEST(RadioBridle868, ADualBandCoprocessorGivesItsGfskRadioToThe868Profile) {
  FakeCoprocessor::Config both;
  both.radios = FakeCoprocessor::dual_band_radios();
  Rig868 rig(both);
  rig.start();
  EXPECT_EQ(rig.radio.selected_radio(), 1);
  EXPECT_EQ(rig.commands_of<SetParams>(MsgType::SET_PARAMS)[1].second.radio, 1);
}

// Source of the session fixture bridle_868_boot_configure_dual_band (tests/corpus/sessions/).
TEST(RadioBridle868, TheBootHandshakeWithADualBandCoprocessorConfiguresItsGfskRadio) {
  FakeCoprocessor::Config both;
  both.radios = FakeCoprocessor::dual_band_radios();
  Rig868 rig(both);
  rig.radio.trace_to_environment_dir();
  rig.start();
  EXPECT_EQ(rig.cp.tx_power_dbm(1), rig.radio.applied_tx_power_dbm()) << "radio 1 was configured, not radio 0";
}

TEST(RadioBridle868, ADualBandCoprocessorGivesItsFirstRadioToThe24GhzProfile) {
  FakeCoprocessor::Config both;
  both.radios = FakeCoprocessor::dual_band_radios();
  Rig rig(both);
  rig.start();
  EXPECT_EQ(rig.radio.selected_radio(), 0);
  EXPECT_EQ(rig.commands_of<SetParams>(MsgType::SET_PARAMS)[1].second.radio, 0);
}

TEST(RadioBridle868, AnOnly802154CoprocessorIsRefusedWithoutAnyRadioConfigured) {
  Rig868 rig(FakeCoprocessor::Config{});
  EXPECT_FALSE(rig.radio.init());
  EXPECT_TRUE(rig.radio.is_failed());
  EXPECT_NE(std::string(rig.radio.failure_reason()).find("no suitable radio"), std::string::npos);
  EXPECT_EQ(rig.commands_of<SetParams>(MsgType::SET_PARAMS).size(), 0u);
}

TEST(RadioBridle868, AGfskRadioThatCapsThePreambleBelowTheSpecsCapacityIsRefused) {
  FakeCoprocessor::Config c = gfsk_only();
  c.radios[0].max_preamble_bytes = IO868_MIN_PREAMBLE_CAPACITY_BYTES - 1;
  Rig868 rig(c);
  EXPECT_FALSE(rig.radio.init());
  EXPECT_NE(std::string(rig.radio.failure_reason()).find("no suitable radio"), std::string::npos);
}

// === The wake-up must fit the radio's preamble (setup and runtime) ===

namespace {

/// A gfsk co-processor whose radio sends preambles up to @p max_bytes.
FakeCoprocessor::Config gfsk_with_preamble_cap(uint16_t max_bytes) {
  FakeCoprocessor::Config c = gfsk_only();
  c.radios[0].max_preamble_bytes = max_bytes;
  return c;
}

/// 4096 bytes of 8 bit times are 853.3 ms: 800 ms fits, 900 ms does not.
TuningConfig wake_of(float ms) {
  TuningConfig tuning;
  tuning.low_power_wake_ms = ms;
  return tuning;
}

}  // namespace

TEST(RadioBridle868, AConfiguredWakeUpThatFitsTheRadioStartsTheLink) {
  Rig868 rig(gfsk_with_preamble_cap(IO868_MIN_PREAMBLE_CAPACITY_BYTES));
  rig.radio.apply_tuning(wake_of(800.0F));  // before init, as a hub that applies tuning first
  test::LogCapture log(true);
  EXPECT_TRUE(rig.radio.init());
  EXPECT_EQ(log.count('E'), 0u);
}

TEST(RadioBridle868, AConfiguredWakeUpLongerThanTheRadiosPreambleFailsSetupAndNamesBothNumbers) {
  Rig868 rig(gfsk_with_preamble_cap(IO868_MIN_PREAMBLE_CAPACITY_BYTES));
  rig.radio.apply_tuning(wake_of(900.0F));
  test::LogCapture log(true);
  EXPECT_FALSE(rig.radio.init());
  EXPECT_NE(std::string(rig.radio.failure_reason()).find("wake-up"), std::string::npos);
  bool named = false;
  for (const auto &line : log.lines())
    named = named || (line.text.find("4320") != std::string::npos && line.text.find("4096") != std::string::npos);
  EXPECT_TRUE(named) << "the line names the 4320 bytes the wake-up needs and the radio's 4096";
}

TEST(RadioBridle868, TheHubsTuningIsCheckedAfterInitToo) {
  Rig868 rig(gfsk_with_preamble_cap(IO868_MIN_PREAMBLE_CAPACITY_BYTES));
  rig.start();  // the default tuning: a 515 ms wake-up, 2472 bytes
  EXPECT_TRUE(rig.radio.accepts_tuning(TuningConfig{}));
  EXPECT_TRUE(rig.radio.accepts_tuning(wake_of(800.0F)));
  test::LogCapture log(true);
  EXPECT_FALSE(rig.radio.accepts_tuning(wake_of(900.0F))) << "a wake-up is never shortened to fit";
  ASSERT_EQ(log.count('W'), 1u);
  EXPECT_NE(log.lines()[0].text.find("4320"), std::string::npos);
  EXPECT_NE(log.lines()[0].text.find("4096"), std::string::npos);
}

TEST(RadioBridle868, ARadioWithRoomForTheLongestWakeUpTheHubAcceptsAcceptsIt) {
  Rig868 rig;  // the reference gfsk radio: 8191 bytes
  rig.start();
  EXPECT_TRUE(rig.radio.accepts_tuning(wake_of(1000.0F)));
}

TEST(RadioBridle, ABandWhoseWakeUpIsAStrobeAcceptsAnyWakeUpDuration) {
  Rig rig;
  rig.start();
  EXPECT_TRUE(rig.radio.accepts_tuning(wake_of(1000.0F)));
}

TEST(RadioBridle, BeforeTheRadioIsChosenThereIsNoLimitToCheckAgainst) {
  Rig868 rig;
  EXPECT_TRUE(rig.radio.accepts_tuning(wake_of(1000.0F)));
}

TEST(RadioBridle868, ASentFrameIsTheCodedFrameAndItsPreambleIsTheOneTheHubChose) {
  Rig868 rig;
  rig.start();
  const auto frame = io_frame(0x20, 12);
  RadioTxConfig cfg{};
  cfg.freq_hz = FREQ_CH2;
  cfg.preamble_len = LONG_PREAMBLE;
  ASSERT_TRUE(rig.radio.send_packet(frame.data(), frame.size(), cfg).sent());
  auto txs = rig.commands_of<Tx>(MsgType::TX);
  ASSERT_EQ(txs.size(), 1u);
  const Tx &tx = txs[0].second;
  EXPECT_EQ(tx.preamble_bytes, LONG_PREAMBLE);
  EXPECT_FALSE(tx.has_strobe);
  EXPECT_EQ(tx.freq_hz, FREQ_CH2);
  uint8_t coded[RADIO_PACKET_BUFFER_SIZE];
  const size_t n = io868_gfsk_phy().wrap(frame.data(), static_cast<uint8_t>(frame.size()), coded, sizeof(coded));
  EXPECT_EQ(std::vector<uint8_t>(tx.payload.data, tx.payload.data + tx.payload.size),
            std::vector<uint8_t>(coded, coded + n));
}

TEST(RadioBridle868, AFrameIsArrivingFromThePreambleOnAndTheReceptionEndsWithTheFrameOrItsAbort) {
  Rig868 rig;
  rig.start();
  listen_on(rig);
  rig.cp.inject_rx_start(FREQ_CH2, RxStage::PREAMBLE);
  EXPECT_TRUE(rig.radio.is_preamble_detected());
  EXPECT_TRUE(rig.radio.reception_in_progress());
  rig.cp.inject_rx_abort(FREQ_CH2, RxAbortReason::NO_SYNC_AFTER_PREAMBLE);
  EXPECT_FALSE(rig.radio.is_preamble_detected());
  EXPECT_FALSE(rig.radio.reception_in_progress());
}

TEST(RadioBridle868, AReceivedFrameIsFoundInItsRawBytes) {
  Rig868 rig;
  rig.start();
  listen_on(rig);
  const auto frame =
      std::vector<uint8_t>{0x0E, 0x00, 0xD1, 0xD4, 0xFF, 0x8C, 0x08, 0x3C, 0x3C, 0x45, 0x51, 0x6F, 0xFE, 0x59, 0x80};
  rig.cp.inject_rx_frame(FREQ_CH2, Bridge868::reply_payload(frame), FcsState::NOT_CHECKED);
  RadioRxPacket packet;
  ASSERT_TRUE(rig.radio.wait_for_packet(packet, 50));
  EXPECT_EQ(std::vector<uint8_t>(packet.data, packet.data + packet.len), frame);
  EXPECT_EQ(packet.freq_hz, FREQ_CH2);
}

TEST(RadioBridle868, NoiseThatIsNoFrameIsCountedAsABadCrcNotDelivered) {
  Rig868 rig;
  rig.start();
  listen_on(rig);
  std::vector<uint8_t> noise(IO868_RX_CAPTURE_BYTES);
  for (size_t i = 0; i < noise.size(); i++)
    noise[i] = static_cast<uint8_t>(0x5A + 37 * i);
  rig.cp.inject_rx_frame(FREQ_CH2, noise, FcsState::NOT_CHECKED);
  RadioRxPacket packet;
  EXPECT_FALSE(rig.radio.wait_for_packet(packet, 20));
  EXPECT_EQ(rig.radio.bad_crc_frames(), 1u);
}

TEST(RadioBridle868, ATxTheCoprocessorRefusesWithOutOfRangeIsAFailedSendAtOnce) {
  Rig868 rig;
  rig.start();
  rig.cp.reject_next(MsgType::TX, ErrorCode::OUT_OF_RANGE, 7);
  const auto frame = io_frame(0x20, 12);
  RadioTxConfig cfg{};
  cfg.freq_hz = FREQ_CH2;
  cfg.preamble_len = 9000;  // beyond what the chip sends
  const uint32_t before = esphome::test_clock::peek_ms();
  EXPECT_FALSE(rig.radio.send_packet(frame.data(), frame.size(), cfg).sent());
  EXPECT_LT(esphome::test_clock::peek_ms() - before, 100u) << "an error is an answer, not a wait for tx_done";
  EXPECT_TRUE(rig.radio.running());
}

TEST(RadioBridle868, ANewReceiverBandwidthIsSentToTheRadioAndARepeatIsNot) {
  Rig868 rig;
  rig.start();
  ASSERT_EQ(rig.commands_of<SetParams>(MsgType::SET_PARAMS).size(), 2u);
  TuningConfig tuning;
  tuning.sx1262_rx_bandwidth = SX1262RxBandwidth::BW_156_2_KHZ;
  rig.radio.apply_tuning(tuning);
  auto sets = rig.commands_of<SetParams>(MsgType::SET_PARAMS);
  ASSERT_EQ(sets.size(), 3u);
  EXPECT_EQ(sets[2].second.radio, 0);
  TlvReader r(sets[2].second.tlvs);
  uint8_t id;
  ByteSpan v;
  bool saw_bandwidth = false;
  while (r.next(id, v)) {
    EXPECT_NE(id, static_cast<uint8_t>(ParamId::PHY_PROFILE)) << "the profile would reset the params to their defaults";
    if (id == static_cast<uint8_t>(ParamId::RX_BW_HZ)) {
      saw_bandwidth = true;
      EXPECT_EQ(v.data[0] | (v.data[1] << 8) | (v.data[2] << 16), 156200);
    }
  }
  EXPECT_TRUE(saw_bandwidth);
  rig.radio.apply_tuning(tuning);
  EXPECT_EQ(rig.commands_of<SetParams>(MsgType::SET_PARAMS).size(), 3u) << "nothing changed: nothing is sent";
}

TEST(RadioBridle868, ATuningThatChangesNothingTheRadioIsTunedByIsNotSentAndTheFirstOneBeforeInitWaitsForIt) {
  Rig868 rig;
  TuningConfig tuning;
  tuning.sx1262_rx_bandwidth = SX1262RxBandwidth::BW_187_2_KHZ;
  rig.radio.apply_tuning(tuning);  // the link is not up: the configuration carries it
  rig.start();
  auto sets = rig.commands_of<SetParams>(MsgType::SET_PARAMS);
  ASSERT_EQ(sets.size(), 2u);
  TlvReader r(sets[1].second.tlvs);
  uint8_t id;
  ByteSpan v;
  while (r.next(id, v)) {
    if (id == static_cast<uint8_t>(ParamId::RX_BW_HZ)) {
      EXPECT_EQ(v.data[0] | (v.data[1] << 8) | (v.data[2] << 16), 187200);
    }
  }
  tuning.sx1262_discovery_hop_slice_ms = 99;  // a hub-side value: the radio is not tuned by it
  rig.radio.apply_tuning(tuning);
  EXPECT_EQ(rig.commands_of<SetParams>(MsgType::SET_PARAMS).size(), 2u);
}

TEST(RadioBridle868, A802154RadioNeverGetsAResendForATuning) {
  Rig rig;
  rig.start();
  TuningConfig tuning;
  tuning.sx1262_rx_bandwidth = SX1262RxBandwidth::BW_156_2_KHZ;
  rig.radio.apply_tuning(tuning);
  EXPECT_EQ(rig.commands_of<SetParams>(MsgType::SET_PARAMS).size(), 2u);
}

TEST(RadioBridle868, ARefusedResendKeepsTheOldParamsAndTheLinkUp) {
  Rig868 rig;
  rig.start();
  rig.cp.reject_next(MsgType::SET_PARAMS, ErrorCode::OUT_OF_RANGE, static_cast<uint8_t>(ParamId::RX_BW_HZ));
  TuningConfig tuning;
  tuning.sx1262_rx_bandwidth = SX1262RxBandwidth::BW_156_2_KHZ;
  rig.radio.apply_tuning(tuning);
  EXPECT_TRUE(rig.radio.running());
  rig.radio.apply_tuning(tuning);  // the radio still has the old bandwidth: the host tries again
  EXPECT_EQ(rig.commands_of<SetParams>(MsgType::SET_PARAMS).size(), 4u);
}

TEST(RadioBridle868, TheResponsePreambleFollowsTheResponsePreambleDurationInRawBytes) {
  Rig868 rig;
  rig.start();
  TuningConfig tuning;
  EXPECT_EQ(rig.radio.response_preamble(), 17) << "3.5 ms at 38400 bit/s is 134 bits, 17 bytes of 8 bit times";
  tuning.response_preamble_ms = 10.0F;
  rig.radio.apply_tuning(tuning);
  EXPECT_EQ(rig.radio.response_preamble(), 48);
}

TEST(RadioBridle868, ALongTryTakesItsAirTimePlusTheCommandOnTheUart) {
  Rig868 rig;
  rig.start();
  RadioTxConfig cfg{};
  cfg.preamble_len = LONG_PREAMBLE;
  const uint32_t air = io868_tx_air_time_us(LONG_PREAMBLE, 12, IO868_PREAMBLE_BITS_PER_BYTE);
  const uint32_t modelled = rig.radio.tx_air_time_us(12, cfg);
  EXPECT_GE(modelled, air);
  EXPECT_LE(modelled, air + 20 * 2000) << "a few bytes of UART time at 1 Mbaud, never milliseconds";
}
