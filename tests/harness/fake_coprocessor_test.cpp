/// @file
/// @brief FakeCoprocessor: it answers commands the way the spec says, so tests built on it can trust it.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "fake_coprocessor.h"

using namespace esphome::home_io_control::bridle;
using esphome::test_clock::ManualClock;

namespace {

/// A host talking to the fake through the stream, with everything it hears kept for inspection.
struct Rig {
  ManualClock clock;
  FakeByteStream stream;
  FakeCoprocessor cp;
  Deframer deframer;
  std::vector<std::vector<uint8_t>> heard;
  uint8_t next_seq{1};

  explicit Rig(FakeCoprocessor::Config config = {}) : cp(stream, std::move(config)) {}

  /// Sends a command with the next seq and returns that seq.
  uint8_t send(const Body &body) {
    uint8_t wire[MAX_WIRE_SIZE];
    uint8_t const seq = next_seq++;
    size_t const n = encode_frame(seq, body, wire, sizeof(wire));
    EXPECT_GT(n, 0u);
    stream.write(wire, n);
    return seq;
  }

  /// Polls for `us` microseconds of test time, collecting every message.
  void listen(uint64_t us) {
    uint64_t const end = esphome::test_clock::state().now_us + us;
    uint8_t byte;
    while (esphome::test_clock::state().now_us < end) {
      while (stream.read(&byte, 1) == 1) {
        if (deframer.feed(byte) == Deframer::Event::MESSAGE)
          heard.emplace_back(deframer.message(), deframer.message() + deframer.message_size());
      }
    }
  }

  Message decode(size_t index) {
    Message m;
    EXPECT_EQ(decode_message(heard.at(index).data(), heard.at(index).size(), m), DecodeStatus::OK);
    return m;
  }
  template<class T> T get(size_t index) {
    Message m = decode(index);
    const T *body = std::get_if<T>(&m.body);
    EXPECT_NE(body, nullptr) << "message " << index << " has type 0x" << std::hex << int(heard.at(index)[0]);
    return body ? *body : T{};
  }
  static uint8_t type_of(const std::vector<uint8_t> &m) { return m[0]; }

  /// Configures radio 0 for the io profile and enables events.
  void configure(uint8_t event_mask = 0x0F) {
    uint8_t buf[16];
    TlvWriter dev(buf, sizeof(buf));
    dev.add_u8(static_cast<uint8_t>(ParamId::EVENT_MASK), event_mask);
    SetParams sp;
    sp.radio = DEVICE_SCOPE;
    sp.tlvs = dev.span();
    send(sp);
    uint8_t buf2[16];
    TlvWriter radio(buf2, sizeof(buf2));
    radio.add_u8(static_cast<uint8_t>(ParamId::PHY_PROFILE), static_cast<uint8_t>(PhyProfile::IEEE802154_OQPSK_250K))
        .add_u8(static_cast<uint8_t>(ParamId::SFD), 0x56)
        .add_u8(static_cast<uint8_t>(ParamId::TX_POWER_DBM), 8);
    SetParams sr;
    sr.radio = 0;
    sr.tlvs = radio.span();
    send(sr);
    listen(1000);
    heard.clear();
  }

  /// An `rx_on` on io channel 2 (or `freq`).
  static RxOn rx_on(uint8_t radio = 0, uint32_t freq = 2450000000u) {
    RxOn on;
    on.radio = radio;
    on.freq_hz = freq;
    return on;
  }

  static Tx tx_of(const std::vector<uint8_t> &payload, uint32_t freq = 2450000000u) {
    Tx tx;
    tx.radio = 0;
    tx.freq_hz = freq;
    tx.then_rx = true;
    tx.payload = {payload.data(), payload.size()};
    return tx;
  }
};

const std::vector<uint8_t> PAYLOAD = {0x01, 0x69, 0x6F, 0x0A, 0x00, 0xFE, 0xDC,
                                      0xBA, 0x12, 0x34, 0x56, 0x00, 0x01, 0x04};

}  // namespace

// === Handshake ===

TEST(FakeCoprocessor, BootSendsAReadyWithCounterZeroAndEveryRequiredTlv) {
  Rig rig;
  rig.cp.boot();
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 1u);
  EXPECT_EQ(rig.heard[0][1], 0) << "the event counter starts at 0 with the boot ready";
  Ready ready = rig.get<Ready>(0);
  EXPECT_EQ(ready.cmd_seq, 0);
  EXPECT_EQ(ready.proto_minor, PROTO_MINOR);
  ReadyInfo info;
  EXPECT_EQ(parse_ready_info(ready.info, info), ReadyInfoStatus::OK);
  EXPECT_EQ(info.radio_count, 1);
  EXPECT_EQ(info.radios[0].strobe_jitter_us, 2);
}

TEST(FakeCoprocessor, HelloIsAnsweredWithTheCommandsSeqAndTheRetiredGetInfoWithUnknownType) {
  Rig rig;
  uint8_t const hello = rig.send(Hello{});
  uint8_t msg[MAX_MESSAGE_SIZE];
  uint8_t const retired = rig.next_seq++;
  size_t const n = encode_message(retired, Unknown{RETIRED_MSG_TYPES[0], {}}, msg, sizeof(msg));
  uint8_t wire[MAX_WIRE_SIZE];
  rig.stream.write(wire, frame_message(msg, n, wire, sizeof(wire)));
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 2u);
  EXPECT_EQ(rig.get<Ready>(0).cmd_seq, hello);
  Error err = rig.get<Error>(1);
  EXPECT_EQ(err.cmd_seq, retired);
  EXPECT_EQ(err.code, ErrorCode::UNKNOWN_TYPE);
}

TEST(FakeCoprocessor, TheDescriptorCanBeMadeToLackATlvOrAdvertiseAnotherVersion) {
  FakeCoprocessor::Config cfg;
  cfg.with_reset_cause = false;
  cfg.proto_minor = 9;
  Rig rig(cfg);
  rig.cp.boot();
  rig.listen(500);
  Ready ready = rig.get<Ready>(0);
  EXPECT_EQ(ready.proto_minor, 9);
  ReadyInfo info;
  EXPECT_EQ(parse_ready_info(ready.info, info), ReadyInfoStatus::MISSING_RESET_CAUSE);
}

TEST(FakeCoprocessor, TwoRadiosAreBothListed) {
  FakeCoprocessor::Config cfg;
  cfg.radios = {FakeCoprocessor::reference_802154_radio(0), FakeCoprocessor::reference_802154_radio(1)};
  Rig rig(cfg);
  rig.cp.boot();
  rig.listen(500);
  ReadyInfo info;
  Ready ready = rig.get<Ready>(0);
  ASSERT_EQ(parse_ready_info(ready.info, info), ReadyInfoStatus::OK);
  EXPECT_EQ(info.radio_count, 2);
}

// === Event counter ===

TEST(FakeCoprocessor, TheEventCounterCountsUpAndWraps) {
  Rig rig;
  rig.cp.boot();
  for (int i = 0; i < 300; i++)
    rig.cp.inject_rx_overflow(1);
  rig.listen(2000);
  ASSERT_EQ(rig.heard.size(), 301u);
  for (size_t i = 0; i < rig.heard.size(); i++)
    EXPECT_EQ(rig.heard[i][1], static_cast<uint8_t>(i)) << i;
}

TEST(FakeCoprocessor, ACounterGapCanBeScripted) {
  Rig rig;
  rig.cp.boot();
  rig.cp.inject_rx_overflow(1);
  rig.cp.skip_event_counter(3);
  rig.cp.inject_rx_overflow(1);
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 3u);
  EXPECT_EQ(rig.heard[2][1], static_cast<uint8_t>(rig.heard[1][1] + 4));
}

TEST(FakeCoprocessor, ACorruptFrameIsDroppedByTheHostsDeframer) {
  Rig rig;
  rig.cp.corrupt_next_frame();
  rig.cp.inject_rx_overflow(1);
  rig.cp.inject_rx_overflow(2);
  rig.listen(500);
  EXPECT_EQ(rig.heard.size(), 1u);
  EXPECT_EQ(rig.deframer.counters().bad_crc, 1u);
}

// === set_params ===

TEST(FakeCoprocessor, SetParamsEchoesAppliedValuesAndClampsTheTxPower) {
  Rig rig;
  uint8_t buf[8];
  TlvWriter w(buf, sizeof(buf));
  w.add_u8(static_cast<uint8_t>(ParamId::TX_POWER_DBM), 22);
  SetParams sp;
  sp.radio = 0;
  sp.tlvs = w.span();
  uint8_t const seq = rig.send(sp);
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 1u);
  ParamsAck ack = rig.get<ParamsAck>(0);
  EXPECT_EQ(ack.cmd_seq, seq);
  ByteSpan v;
  ASSERT_TRUE(find_tlv(ack.applied, static_cast<uint8_t>(ParamId::TX_POWER_DBM), v));
  EXPECT_EQ(static_cast<int8_t>(v.data[0]), 8) << "clamped to the radio's maximum and echoed";
  EXPECT_EQ(rig.cp.tx_power_dbm(), 8);
}

TEST(FakeCoprocessor, AnUnknownParamRefusesTheWholeSetParamsAndNothingApplies) {
  Rig rig;
  uint8_t buf[16];
  TlvWriter w(buf, sizeof(buf));
  w.add_u8(static_cast<uint8_t>(ParamId::PHY_PROFILE), static_cast<uint8_t>(PhyProfile::IEEE802154_OQPSK_250K))
      .add_u8(0x77, 1)
      .add_u8(static_cast<uint8_t>(ParamId::SFD), 0x56);
  SetParams sp;
  sp.radio = 0;
  sp.tlvs = w.span();
  uint8_t const seq = rig.send(sp);
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 1u) << "one error and no params_ack";
  Error err = rig.get<Error>(0);
  EXPECT_EQ(err.cmd_seq, seq);
  EXPECT_EQ(err.code, ErrorCode::UNSUPPORTED);
  EXPECT_EQ(err.detail, 0x77) << "the first TLV that fails is named";
  rig.send(Rig::rx_on());
  rig.listen(500);
  EXPECT_EQ(rig.get<Error>(1).code, ErrorCode::NOT_CONFIGURED) << "the profile before the bad TLV did not apply";
}

TEST(FakeCoprocessor, APhyProfileThatIsNotTheFirstTlvIsParamRangeAndNothingApplies) {
  Rig rig;
  uint8_t buf[16];
  TlvWriter w(buf, sizeof(buf));
  w.add_u8(static_cast<uint8_t>(ParamId::TX_POWER_DBM), 0)
      .add_u8(static_cast<uint8_t>(ParamId::PHY_PROFILE), static_cast<uint8_t>(PhyProfile::IEEE802154_OQPSK_250K));
  SetParams sp;
  sp.radio = 0;
  sp.tlvs = w.span();
  rig.send(sp);
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 1u);
  Error err = rig.get<Error>(0);
  EXPECT_EQ(err.code, ErrorCode::PARAM_RANGE);
  EXPECT_EQ(err.detail, static_cast<uint8_t>(ParamId::PHY_PROFILE));
  EXPECT_EQ(rig.cp.tx_power_dbm(), 8) << "the power TLV before it did not apply either";
}

TEST(FakeCoprocessor, TheEventMaskIsZeroAfterBootAndSetPerDevice) {
  Rig rig;
  rig.cp.boot();
  EXPECT_EQ(rig.cp.event_mask(), 0);
  rig.configure(0x05);
  EXPECT_EQ(rig.cp.event_mask(), 0x05);
  rig.cp.reboot(ResetCause::WATCHDOG);
  EXPECT_EQ(rig.cp.event_mask(), 0) << "a reboot forgets it";
}

TEST(FakeCoprocessor, ABadRadioIndexIsNoSuchRadioWithTheIndex) {
  Rig rig;
  rig.send(Rig::rx_on(7));
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 1u);
  Error err = rig.get<Error>(0);
  EXPECT_EQ(err.code, ErrorCode::NO_SUCH_RADIO);
  EXPECT_EQ(err.detail, 7);
}

// === Errors in the specified order ===

TEST(FakeCoprocessor, UnknownTypeThenBadLengthThenNotConfigured) {
  Rig rig;
  uint8_t msg[MAX_MESSAGE_SIZE];
  auto send_raw = [&](uint8_t type, const std::vector<uint8_t> &body) {
    size_t n = encode_message(rig.next_seq++, Unknown{type, {body.data(), body.size()}}, msg, sizeof(msg));
    uint8_t wire[MAX_WIRE_SIZE];
    size_t w = frame_message(msg, n, wire, sizeof(wire));
    rig.stream.write(wire, w);
  };
  send_raw(0x7F, {});
  send_raw(static_cast<uint8_t>(MsgType::RX_ON), {0, 1});
  rig.send(Rig::rx_on());
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 3u);
  EXPECT_EQ(rig.get<Error>(0).code, ErrorCode::UNKNOWN_TYPE);
  EXPECT_EQ(rig.get<Error>(1).code, ErrorCode::BAD_LENGTH);
  EXPECT_EQ(rig.get<Error>(2).code, ErrorCode::NOT_CONFIGURED);
}

TEST(FakeCoprocessor, GetRssiIsNotReceivingWhileIdleAndBusyWhileTransmitting) {
  Rig rig;
  GetRssi probe;
  probe.radio = 0;
  rig.send(probe);  // not configured yet
  rig.listen(100);
  ASSERT_EQ(rig.heard.size(), 1u);
  EXPECT_EQ(rig.get<Error>(0).code, ErrorCode::NOT_CONFIGURED);
  rig.configure();
  rig.send(probe);
  uint8_t const rx_seq = rig.send(Rig::rx_on());
  rig.send(probe);
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 3u);
  EXPECT_EQ(rig.get<Error>(0).code, ErrorCode::NOT_RECEIVING);
  EXPECT_EQ(rig.get<Ack>(1).cmd_seq, rx_seq);
  EXPECT_EQ(rig.get<Rssi>(2).rssi_dbm, -60);
  std::vector<uint8_t> payload = PAYLOAD;
  rig.send(Rig::tx_of(payload));
  rig.send(probe);
  rig.listen(100);
  EXPECT_EQ(rig.get<Error>(rig.heard.size() - 1).code, ErrorCode::BUSY);
}

TEST(FakeCoprocessor, EveryCommandWithNothingToReportGetsOneAckNamingItsSeq) {
  Rig rig;
  rig.configure();
  uint8_t const on = rig.send(Rig::rx_on());
  RxScan scan;
  scan.dwell_us = 1500;
  scan.num_freqs = 1;
  scan.freqs_hz[0] = 2450000000u;
  uint8_t const sc = rig.send(scan);
  Idle idle;
  idle.radio = 0;
  uint8_t const id = rig.send(idle);
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 3u);
  EXPECT_EQ(rig.get<Ack>(0).cmd_seq, on);
  EXPECT_EQ(rig.get<Ack>(1).cmd_seq, sc);
  EXPECT_EQ(rig.get<Ack>(2).cmd_seq, id);
  EXPECT_FALSE(rig.cp.receiving());
}

TEST(FakeCoprocessor, ASoftResetAcksBeforeItRebootsAndTheBootReadyFollowsWithCounterZero) {
  Rig rig;
  rig.configure();
  uint8_t const seq = rig.send(SoftReset{});
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 2u);
  EXPECT_EQ(rig.get<Ack>(0).cmd_seq, seq);
  EXPECT_EQ(rig.get<Ready>(1).cmd_seq, 0);
  EXPECT_EQ(rig.heard[1][1], 0) << "the boot ready restarts the event counter";
  EXPECT_EQ(rig.cp.event_mask(), 0);
}

TEST(FakeCoprocessor, ADroppedAckIsLostAndTheNextOneIsSent) {
  Rig rig;
  rig.configure();
  rig.cp.drop_next_ack();
  rig.send(Rig::rx_on());
  uint8_t const second = rig.send(Rig::rx_on());
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 1u);
  EXPECT_EQ(rig.get<Ack>(0).cmd_seq, second);
}

TEST(FakeCoprocessor, ARefusedRxScanKeepsTheRadioWhereItWas) {
  Rig rig;
  rig.configure();
  rig.send(Rig::rx_on());
  rig.cp.refuse_next_rx_scan();
  RxScan scan;
  scan.dwell_us = 1500;
  scan.num_freqs = 1;
  scan.freqs_hz[0] = 2475000000u;
  rig.send(scan);
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 2u);
  Error err = rig.get<Error>(1);
  EXPECT_EQ(err.code, ErrorCode::PARAM_RANGE);
  EXPECT_EQ(err.detail, field::rx_scan::DWELL_US);
  EXPECT_FALSE(rig.cp.scanning());
  EXPECT_EQ(rig.cp.freq_hz(), 2450000000u);
}

TEST(FakeCoprocessor, AnInjectedRadioFaultIsAnErrorThatAnswersNoCommand) {
  Rig rig;
  rig.cp.inject_radio_fault(7);
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 1u);
  Error err = rig.get<Error>(0);
  EXPECT_EQ(err.cmd_seq, 0);
  EXPECT_EQ(err.code, ErrorCode::RADIO_FAULT);
  EXPECT_EQ(err.detail, 7);
}

TEST(FakeCoprocessor, RxOnOutsideTheBandNamesItsFrequencyFieldAndSetsNothing) {
  Rig rig;
  rig.configure();
  rig.send(Rig::rx_on(0, 868950000u));
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 1u);
  Error err = rig.get<Error>(0);
  EXPECT_EQ(err.code, ErrorCode::PARAM_RANGE);
  EXPECT_EQ(err.detail, field::rx_on::FREQ_HZ);
  EXPECT_FALSE(rig.cp.receiving());
}

TEST(FakeCoprocessor, AScriptedRejectionAnswersOnlyTheNextCommandOfThatType) {
  Rig rig;
  rig.configure();
  rig.cp.reject_next(MsgType::RX_ON, ErrorCode::RADIO_FAULT, 5);
  rig.send(Rig::rx_on());
  rig.send(Rig::rx_on());
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 2u) << "the refusal, then the second one's ack";
  Error err = rig.get<Error>(0);
  EXPECT_EQ(err.code, ErrorCode::RADIO_FAULT);
  EXPECT_EQ(err.detail, 5);
  EXPECT_TRUE(rig.cp.receiving()) << "the second one went through";
}

TEST(FakeCoprocessor, ARxScanOutsideTheLimitsIsParamRange) {
  Rig rig;
  rig.configure();
  RxScan scan;
  scan.dwell_us = 100;
  scan.num_freqs = 1;
  scan.freqs_hz[0] = 2450000000u;
  rig.send(scan);
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 1u);
  EXPECT_EQ(rig.get<Error>(0).code, ErrorCode::PARAM_RANGE);
  EXPECT_FALSE(rig.cp.scanning());
}

TEST(FakeCoprocessor, ASilentFakeRecordsButNeverAnswers) {
  Rig rig;
  rig.cp.set_silent(true);
  rig.send(Hello{});
  rig.listen(500);
  EXPECT_TRUE(rig.heard.empty());
  EXPECT_EQ(rig.cp.commands().size(), 1u);
}

// === tx ===

TEST(FakeCoprocessor, TxIsRefusedBeforeTheRadioIsConfiguredAndWhenTooLong) {
  Rig rig;
  rig.send(Rig::tx_of(PAYLOAD));
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 1u);
  EXPECT_EQ(rig.get<Error>(0).code, ErrorCode::NOT_CONFIGURED);
  rig.configure();
  std::vector<uint8_t> big(126, 0x11);
  rig.send(Rig::tx_of(big));
  rig.listen(500);
  ASSERT_EQ(rig.heard.size(), 1u);
  EXPECT_EQ(rig.get<Error>(0).code, ErrorCode::PAYLOAD_TOO_LONG);
}

TEST(FakeCoprocessor, TxDoneComesAfterTheModelledAirTime) {
  Rig rig;
  rig.configure();
  uint64_t const start = esphome::test_clock::state().now_us;
  uint8_t const seq = rig.send(Rig::tx_of(PAYLOAD));
  rig.listen(300);
  EXPECT_TRUE(rig.heard.empty()) << "a 14-byte frame takes (6+14+2)*32 = 704 µs";
  EXPECT_TRUE(rig.cp.transmitting());
  rig.listen(600);
  ASSERT_EQ(rig.heard.size(), 1u);
  TxDone done = rig.get<TxDone>(0);
  EXPECT_EQ(done.cmd_seq, seq);
  EXPECT_EQ(done.t_main_end_us - done.t_main_start_us, 704u);
  EXPECT_EQ(done.t_main_start_us, static_cast<uint32_t>(start));
  EXPECT_TRUE(rig.cp.receiving()) << "then_rx";
  EXPECT_EQ(rig.cp.freq_hz(), 2450000000u);
}

TEST(FakeCoprocessor, ALongStrobeTakesHalfASecondAndIsCountedAndAnchoredAtTheMainFrame) {
  Rig rig;
  rig.configure();
  Tx tx = Rig::tx_of(PAYLOAD);
  TxShape shape = io2g4_phy().tx_shape(esphome::home_io_control::TxWake::LONG, 0);
  tx.has_strobe = true;
  tx.strobe = shape.strobe;
  uint64_t const start = esphome::test_clock::state().now_us;
  rig.send(tx);
  rig.cp.set_next_strobe_late(2);
  rig.listen(600000);
  ASSERT_EQ(rig.heard.size(), 1u);
  TxDone done = rig.get<TxDone>(0);
  EXPECT_EQ(done.strobe_sent, 510);
  ASSERT_EQ(rig.cp.send_timeline().start_us().size(), 1u);
  EXPECT_EQ(rig.cp.send_timeline().start_us()[0], start + 510036u) << "the main frame, not the tx command";
  EXPECT_EQ(rig.cp.send_timeline().sent_data()[0], PAYLOAD);
}

TEST(FakeCoprocessor, AStrobeLateCountIsReportedOnce) {
  Rig rig;
  rig.configure();
  rig.cp.set_next_strobe_late(4);
  rig.send(Rig::tx_of(PAYLOAD));
  rig.send(Rig::tx_of(PAYLOAD));  // refused as busy; the late count waits for the next real tx
  rig.listen(2000);
  rig.send(Rig::tx_of(PAYLOAD));
  rig.listen(2000);
  std::vector<uint16_t> late;
  for (size_t i = 0; i < rig.heard.size(); i++) {
    if (Rig::type_of(rig.heard[i]) == static_cast<uint8_t>(MsgType::TX_DONE))
      late.push_back(rig.get<TxDone>(i).strobe_late);
  }
  ASSERT_EQ(late.size(), 2u);
  EXPECT_EQ(late[0], 4);
  EXPECT_EQ(late[1], 0);
}

TEST(FakeCoprocessor, ASecondTxDuringATransmissionIsBusy) {
  Rig rig;
  rig.configure();
  rig.send(Rig::tx_of(PAYLOAD));
  rig.listen(100);
  rig.send(Rig::tx_of(PAYLOAD));
  rig.listen(100);
  ASSERT_EQ(rig.heard.size(), 1u);
  EXPECT_EQ(rig.get<Error>(0).code, ErrorCode::BUSY);
}

TEST(FakeCoprocessor, ARadioScopeSetParamsDuringATxIsBusyAndNotApplied) {
  Rig rig;
  rig.configure();
  rig.send(Rig::tx_of(PAYLOAD));
  rig.listen(100);
  uint8_t buf[8];
  TlvWriter w(buf, sizeof(buf));
  w.add_u8(static_cast<uint8_t>(ParamId::TX_POWER_DBM), 0);
  SetParams sp;
  sp.radio = 0;
  sp.tlvs = w.span();
  rig.send(sp);
  rig.listen(100);
  EXPECT_EQ(rig.cp.set_params_during_tx(), 1u);
  EXPECT_EQ(rig.cp.tx_power_dbm(), 8) << "not applied";
  rig.listen(2000);
  EXPECT_EQ(rig.get<Error>(0).code, ErrorCode::BUSY);
  EXPECT_EQ(Rig::type_of(rig.heard.back()), static_cast<uint8_t>(MsgType::TX_DONE)) << "the tx still finishes";
}

TEST(FakeCoprocessor, TxWithCcaOnABusyChannelAnswersChannelBusyWithTheLevelAndSendsNothing) {
  Rig rig;
  rig.configure();
  rig.cp.set_channel_level_dbm(-60);
  Tx tx = Rig::tx_of(PAYLOAD);
  tx.cca = true;
  tx.cca_threshold_dbm = -75;
  rig.send(tx);
  rig.listen(2000);
  ASSERT_EQ(rig.heard.size(), 1u);
  Error err = rig.get<Error>(0);
  EXPECT_EQ(err.code, ErrorCode::CHANNEL_BUSY);
  EXPECT_EQ(err.detail_dbm(), -60);
  EXPECT_TRUE(rig.cp.send_timeline().sent_data().empty());

  rig.heard.clear();
  rig.cp.set_channel_level_dbm(-90);
  rig.send(tx);
  rig.listen(2000);
  EXPECT_EQ(Rig::type_of(rig.heard.back()), static_cast<uint8_t>(MsgType::TX_DONE));
  EXPECT_EQ(rig.cp.send_timeline().sent_data().size(), 1u);
}

// === Reception ===

TEST(FakeCoprocessor, InjectedFramesReachTheHostOnlyWhileTheRadioReceivesAndTheMaskAllowsThem) {
  Rig rig;
  rig.configure(0x00);  // no events enabled beyond good rx
  std::vector<uint8_t> frame(16, 0x22);
  rig.cp.inject_rx(2450000000u, frame);
  rig.listen(100);
  EXPECT_TRUE(rig.heard.empty()) << "the radio is idle";

  rig.send(Rig::rx_on());
  rig.listen(100);
  rig.heard.clear();  // the ack
  rig.cp.inject_rx(2450000000u, frame);
  rig.cp.inject_rx(2450000000u, frame, CrcState::BAD);
  rig.cp.inject_frame_start(2450000000u, FrameStage::SYNC);
  rig.listen(100);
  ASSERT_EQ(rig.heard.size(), 1u) << "a bad-CRC rx and frame_start need their mask bits";
  EXPECT_EQ(rig.get<Rx>(0).crc, CrcState::OK);

  rig.configure(0x0F);
  rig.send(Rig::rx_on());
  rig.listen(100);
  rig.heard.clear();
  rig.cp.inject_rx(2450000000u, frame, CrcState::BAD);
  rig.cp.inject_frame_start(2450000000u, FrameStage::SYNC);
  rig.cp.inject_rx_abort(2450000000u, RxAbortReason::FORCED_RETUNE);
  rig.cp.inject_log(LogLevel::WARN, "hello");
  rig.listen(100);
  EXPECT_EQ(rig.heard.size(), 4u);
}

TEST(FakeCoprocessor, TimedRepliesAreAnchoredAtTheMainFrameOfTheirSend) {
  Rig rig;
  rig.configure();
  rig.send(Rig::rx_on());
  rig.cp.inject_rx_after_send(0, 3000, 2450000000u, std::vector<uint8_t>(16, 0x33));
  uint64_t const start = esphome::test_clock::state().now_us;
  rig.send(Rig::tx_of(PAYLOAD));
  rig.listen(5000);
  size_t rx_index = 0;
  for (size_t i = 0; i < rig.heard.size(); i++) {
    if (Rig::type_of(rig.heard[i]) == static_cast<uint8_t>(MsgType::RX))
      rx_index = i;
  }
  Rx rx = rig.get<Rx>(rx_index);
  EXPECT_EQ(rx.t_us, static_cast<uint32_t>(start + 3000));
  EXPECT_GT(rx_index, 0u) << "after tx_done, as the ordering rule needs";
}

// === Scenarios ===

TEST(FakeCoprocessor, ScenarioRepliesCarryTheDriversSeqAndTheFakesCounter) {
  const Scenario *scenario = nullptr;
  for (size_t i = 0; i < SCENARIO_COUNT; i++) {
    if (std::string(SCENARIOS[i].name) == "ordering_around_tx")
      scenario = &SCENARIOS[i];
  }
  ASSERT_NE(scenario, nullptr);
  Rig rig;
  rig.cp.boot();
  rig.listen(100);
  rig.heard.clear();
  rig.next_seq = 77;
  uint64_t const t0 = esphome::test_clock::state().now_us;
  rig.cp.play_scenario(*scenario, t0);
  rig.listen(1000);  // the host has not sent its tx yet: only the first rx (due at 2000) is not yet due either
  EXPECT_TRUE(rig.heard.empty());
  uint8_t const seq = rig.send(Rig::tx_of(PAYLOAD));
  rig.listen(7000);
  ASSERT_EQ(rig.heard.size(), 3u);
  EXPECT_EQ(Rig::type_of(rig.heard[0]), static_cast<uint8_t>(MsgType::RX));
  EXPECT_EQ(Rig::type_of(rig.heard[1]), static_cast<uint8_t>(MsgType::TX_DONE));
  EXPECT_EQ(rig.get<TxDone>(1).cmd_seq, seq) << "rewritten to the driver's command";
  EXPECT_EQ(Rig::type_of(rig.heard[2]), static_cast<uint8_t>(MsgType::RX));
  EXPECT_EQ(rig.heard[1][1], static_cast<uint8_t>(rig.heard[0][1] + 1)) << "the fake's own counter";
}

TEST(FakeCoprocessor, AScenarioReplyWaitsForTheCommandItAnswers) {
  const Scenario *scenario = nullptr;
  for (size_t i = 0; i < SCENARIO_COUNT; i++) {
    if (std::string(SCENARIOS[i].name) == "tx_cca_busy_then_clear")
      scenario = &SCENARIOS[i];
  }
  ASSERT_NE(scenario, nullptr);
  Rig rig;
  rig.cp.play_scenario(*scenario, esphome::test_clock::state().now_us);
  rig.listen(30000);
  EXPECT_TRUE(rig.heard.empty()) << "no tx was sent, so neither the error nor the tx_done is released";
}

// === Playback ===

namespace {

/// A hand-built session: the host sends two commands, the co-processor answers each 5 ms later.
struct HandBuiltSession {
  std::vector<uint8_t> hello = message(MsgType::HELLO, 1, {});
  std::vector<uint8_t> ready = message(MsgType::READY, 0, {0, PROTO_MAJOR, PROTO_MINOR, 1, 0, 0, 0, 0, 0});
  std::vector<uint8_t> idle = message(MsgType::IDLE, 2, {0});
  std::vector<uint8_t> stats = message(MsgType::STATS, 1, {2});  // answers the command with seq 2: no counters
  SessionMessage rows[4];
  Session session;

  static std::vector<uint8_t> message(MsgType type, uint8_t seq, std::vector<uint8_t> body) {
    std::vector<uint8_t> m = {static_cast<uint8_t>(type), seq};
    m.insert(m.end(), body.begin(), body.end());
    m.push_back(0);
    m.push_back(0);
    return m;
  }

  HandBuiltSession() {
    rows[0] = {true, 1000, hello.data(), hello.size()};
    rows[1] = {false, 1005, ready.data(), ready.size()};
    rows[2] = {true, 1010, idle.data(), idle.size()};
    rows[3] = {false, 1015, stats.data(), stats.size()};
    session = {"hand_built", "synthetic-bootstrap", "", "", "", rows, 4};
  }
};

}  // namespace

TEST(FakeCoprocessorPlayback, EventsAreReleasedAtTheirRecordedGapAfterTheCommandTheyFollowed) {
  HandBuiltSession recorded;
  Rig rig;
  rig.cp.play_session(recorded.session);
  uint64_t const t0 = esphome::test_clock::state().now_us;
  rig.listen(1000);
  EXPECT_TRUE(rig.heard.empty()) << "the ready follows the hello, which has not been sent";
  rig.send(Hello{});
  uint64_t const sent_at = esphome::test_clock::state().now_us;
  rig.listen(4000);
  EXPECT_TRUE(rig.heard.empty()) << "5 ms after the hello, not 4";
  rig.listen(1200);
  ASSERT_EQ(rig.heard.size(), 1u);
  EXPECT_GE(esphome::test_clock::state().now_us - sent_at, 5000u);
  EXPECT_EQ(rig.get<Ready>(0).proto_minor, PROTO_MINOR);
  EXPECT_GT(sent_at, t0);
}

TEST(FakeCoprocessorPlayback, ARepliesSeqIsRewrittenToTheDriversCommandAndTheCounterIsTheFakesOwn) {
  HandBuiltSession recorded;
  Rig rig;
  rig.cp.play_session(recorded.session);
  rig.next_seq = 40;  // the driver numbers its commands differently from the recording
  rig.send(Hello{});
  uint8_t const idle_seq = rig.send(Idle{0});
  rig.listen(20000);
  ASSERT_EQ(rig.heard.size(), 2u);
  EXPECT_EQ(rig.heard[0][1], 0) << "the fake's own event counter";
  EXPECT_EQ(rig.heard[1][1], 1);
  EXPECT_EQ(rig.get<Stats>(1).cmd_seq, idle_seq) << "the recording's cmd_seq 2 is the driver's second command";
  EXPECT_TRUE(rig.cp.playback_errors().empty());
  EXPECT_TRUE(rig.cp.playback_finished());
}

TEST(FakeCoprocessorPlayback, CommandsAreComparedByTypeAndBodyNotBySeq) {
  HandBuiltSession recorded;
  Rig rig;
  rig.cp.play_session(recorded.session);
  rig.next_seq = 200;
  rig.send(Hello{});
  rig.send(Idle{0});
  EXPECT_TRUE(rig.cp.playback_errors().empty()) << "seq and CRC differ, type and body do not";
}

TEST(FakeCoprocessorPlayback, ADifferentBodyAnExtraCommandAndAWrongTypeAreReported) {
  HandBuiltSession recorded;
  Rig rig;
  rig.cp.play_session(recorded.session);
  rig.send(Hello{});
  rig.send(Idle{3});  // the recording idles radio 0
  ASSERT_EQ(rig.cp.playback_errors().size(), 1u);
  EXPECT_NE(rig.cp.playback_errors()[0].find("differs"), std::string::npos);
  rig.send(Rig::rx_on());  // a third command; the recording has two
  ASSERT_EQ(rig.cp.playback_errors().size(), 2u);
  EXPECT_NE(rig.cp.playback_errors()[1].find("not in the recording"), std::string::npos);
}

TEST(FakeCoprocessorPlayback, AReplayIsUnfinishedUntilTheHostHasSentEveryRecordedCommand) {
  HandBuiltSession recorded;
  Rig rig;
  rig.cp.play_session(recorded.session);
  rig.send(Hello{});
  rig.listen(20000);
  EXPECT_FALSE(rig.cp.playback_finished());
  EXPECT_EQ(rig.cp.playback_events_pending(), 1u);
  rig.send(Idle{0});
  rig.listen(20000);
  EXPECT_TRUE(rig.cp.playback_finished());
}
