/// @file
/// @brief Bridle codec tests: every spec vector, COBS edges, layout rejections, TLV lists and the
///        `ready` descriptor.

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "radio_bridle_codec.h"
#include "bridle_vectors.h"

using namespace esphome::home_io_control::bridle;

namespace {

/// The vector called `name`; the test fails if the spec no longer has it.
const Vector &vector_named(const char *name) {
  for (const Vector &v : VECTORS) {
    if (std::strcmp(v.name, name) == 0)
      return v;
  }
  ADD_FAILURE() << "no vector named " << name;
  return VECTORS[0];
}

/// Feeds `wire` to a fresh deframer and returns the event of its last byte.
Deframer::Event deframe(Deframer &d, const uint8_t *wire, size_t n) {
  Deframer::Event last = Deframer::Event::NONE;
  for (size_t i = 0; i < n; i++)
    last = d.feed(wire[i]);
  return last;
}

std::vector<uint8_t> bytes_of(ByteSpan s) { return {s.data, s.data + s.size}; }

/// Decodes a valid vector's message, failing the test if it does not decode.
Message decode_vector(const Vector &v) {
  Message m;
  EXPECT_EQ(decode_message(v.decoded, v.decoded_len, m), DecodeStatus::OK) << v.name;
  return m;
}

template<class T> const T &as(const Message &m) {
  const T *body = std::get_if<T>(&m.body);
  EXPECT_NE(body, nullptr) << "wrong message type";
  static const T fallback{};
  return body != nullptr ? *body : fallback;
}

/// A message `type · seq · body · crc` with a correct CRC, for hand-built layouts.
std::vector<uint8_t> message_with(uint8_t type, const std::vector<uint8_t> &body) {
  uint8_t buf[MAX_MESSAGE_SIZE];
  Unknown raw{type, {body.data(), body.size()}};
  size_t n = encode_message(1, raw, buf, sizeof(buf));
  return {buf, buf + n};
}

DecodeStatus decode_hand_built(uint8_t type, const std::vector<uint8_t> &body) {
  std::vector<uint8_t> msg = message_with(type, body);
  Message m;
  return decode_message(msg.data(), msg.size(), m);
}

}  // namespace

// === Spec vectors ===

TEST(RadioBridleCodec, EveryValidVectorDeframesToItsMessage) {
  for (const Vector &v : VECTORS) {
    if (!v.valid)
      continue;
    Deframer d;
    ASSERT_EQ(deframe(d, v.wire, v.wire_len), Deframer::Event::MESSAGE) << v.name;
    ASSERT_EQ(d.message_size(), v.decoded_len) << v.name;
    EXPECT_EQ(0, std::memcmp(d.message(), v.decoded, v.decoded_len)) << v.name;
    EXPECT_EQ(d.counters().total(), 0u) << v.name;
  }
}

TEST(RadioBridleCodec, EveryValidVectorDecodesToATypedMessage) {
  for (const Vector &v : VECTORS) {
    if (!v.valid)
      continue;
    Message m;
    ASSERT_EQ(decode_message(v.decoded, v.decoded_len, m), DecodeStatus::OK) << v.name;
    EXPECT_EQ(m.seq, v.seq) << v.name;
    EXPECT_EQ(msg_type_of(m.body), v.msg_type) << v.name;
    EXPECT_FALSE(std::holds_alternative<Unknown>(m.body)) << v.name << " decoded as an unknown type";
    EXPECT_EQ((v.direction == VectorDirection::HOST_TO_COPROC), is_command(v.msg_type)) << v.name;
  }
}

TEST(RadioBridleCodec, EveryValidVectorReEncodesToItsBytes) {
  for (const Vector &v : VECTORS) {
    if (!v.valid)
      continue;
    Message m;
    ASSERT_EQ(decode_message(v.decoded, v.decoded_len, m), DecodeStatus::OK) << v.name;
    uint8_t msg[MAX_MESSAGE_SIZE];
    size_t n = encode_message(m.seq, m.body, msg, sizeof(msg));
    ASSERT_EQ(n, v.decoded_len) << v.name;
    EXPECT_EQ(0, std::memcmp(msg, v.decoded, n)) << v.name << ": message bytes";

    uint8_t wire[MAX_WIRE_SIZE];
    size_t w = encode_frame(m.seq, m.body, wire, sizeof(wire));
    ASSERT_EQ(w, v.wire_len) << v.name;
    EXPECT_EQ(0, std::memcmp(wire, v.wire, w)) << v.name << ": wire bytes";
  }
}

TEST(RadioBridleCodec, EveryInvalidVectorIsDroppedAndCountedByItsReason) {
  for (const Vector &v : VECTORS) {
    if (v.valid)
      continue;
    Deframer d;
    EXPECT_EQ(deframe(d, v.wire, v.wire_len), Deframer::Event::DROPPED) << v.name;
    ASSERT_EQ(d.counters().total(), 1u) << v.name;
    std::string const reason = v.reject_reason;
    if (reason == "bad crc")
      EXPECT_EQ(d.counters().bad_crc, 1u) << v.name;
    else if (reason == "truncated")
      EXPECT_EQ(d.counters().bad_cobs, 1u) << v.name;
    else if (reason == "bad length")
      EXPECT_EQ(d.counters().bad_length, 1u) << v.name;
    else
      ADD_FAILURE() << v.name << ": unmapped reject reason " << reason;
  }
}

TEST(RadioBridleCodec, VectorFieldsMatchTheSpec) {
  {
    const Message m = decode_vector(vector_named("tx_io_wake_strobe"));
    const Tx &tx = as<Tx>(m);
    EXPECT_EQ(tx.freq_hz, 2450000000u);
    EXPECT_TRUE(tx.then_rx);
    EXPECT_FALSE(tx.cca);
    EXPECT_FALSE(tx.at_time);
    ASSERT_TRUE(tx.has_strobe);
    EXPECT_EQ(tx.strobe.count, 510);
    EXPECT_EQ(tx.strobe.period_ns, 1000000u);
    EXPECT_EQ(tx.strobe.main_gap_ns, 1036000u);
    EXPECT_EQ(tx.strobe.counter_offset, 1);
    EXPECT_EQ(tx.strobe.counter_width, 2);
    EXPECT_EQ(tx.strobe.counter_unit_ns, 1000000u);
    EXPECT_EQ(tx.strobe.counter_adjust, -1);
    ASSERT_EQ(tx.strobe.len_template, 3);
    EXPECT_EQ(tx.payload.size, 14u);
  }
  {
    const Message m = decode_vector(vector_named("tx_cca"));
    const Tx &tx = as<Tx>(m);
    EXPECT_TRUE(tx.cca);
    EXPECT_EQ(tx.cca_threshold_dbm, -75);
    EXPECT_FALSE(tx.has_strobe);
  }
  {
    const Message m = decode_vector(vector_named("tx_at_time"));
    const Tx &tx = as<Tx>(m);
    EXPECT_TRUE(tx.at_time);
    EXPECT_EQ(tx.t_main_us, 123456789u);
    EXPECT_EQ(tx.freq_hz, 2425000000u);
  }
  {
    const Message m = decode_vector(vector_named("rx_scan_io_idle"));
    const RxScan &scan = as<RxScan>(m);
    EXPECT_EQ(scan.dwell_us, 1500);
    EXPECT_EQ(scan.hold_us, 8000);
    ASSERT_EQ(scan.num_freqs, 3);
    EXPECT_EQ(scan.freqs_hz[0], 2425000000u);
    EXPECT_EQ(scan.freqs_hz[2], 2475000000u);
  }
  {
    const Message m = decode_vector(vector_named("rx_crc_ok"));
    const Rx &rx = as<Rx>(m);
    EXPECT_EQ(rx.t_us, 1001u);
    EXPECT_EQ(rx.rssi_dbm, -67);
    EXPECT_EQ(rx.lqi, 200);
    EXPECT_EQ(rx.crc, CrcState::OK);
    EXPECT_EQ(rx.payload.size, 16u);
  }
  {
    const Message m = decode_vector(vector_named("tx_done_strobe"));
    const TxDone &done = as<TxDone>(m);
    EXPECT_EQ(done.cmd_seq, 11);
    EXPECT_EQ(done.t_main_start_us, 600000u);
    EXPECT_EQ(done.t_main_end_us, 600640u);
    EXPECT_EQ(done.strobe_sent, 510);
    EXPECT_EQ(done.strobe_late, 0);
  }
  {
    const Message m = decode_vector(vector_named("error_channel_busy"));
    const Error &err = as<Error>(m);
    EXPECT_EQ(err.cmd_seq, 22);
    EXPECT_EQ(err.code, ErrorCode::CHANNEL_BUSY);
    EXPECT_EQ(err.detail_dbm(), -70);
  }
  {
    const Message m = decode_vector(vector_named("stats_full"));
    const Stats &stats = as<Stats>(m);
    ASSERT_EQ(stats.present, Stats::COUNT);
    EXPECT_EQ(stats.counters[Stats::RX_OK], 10u);
    EXPECT_EQ(stats.counters[Stats::UPTIME_MS], 3600000u);
  }
}

TEST(RadioBridleCodec, ReadyVectorCarriesTheDescriptorTheSpecShows) {
  const Message m = decode_vector(vector_named("ready_nrf52840_boot"));
  const Ready &ready = as<Ready>(m);
  EXPECT_EQ(ready.cmd_seq, 0);
  EXPECT_EQ(ready.proto_major, PROTO_MAJOR);
  EXPECT_EQ(ready.proto_minor, PROTO_MINOR);
  EXPECT_EQ(ready.chip_id, static_cast<uint16_t>(ChipId::NRF52840));
  EXPECT_NE(ready.caps & device_caps::BOOTLOADER, 0u);

  ReadyInfo info;
  ASSERT_EQ(parse_ready_info(ready.info, info), ReadyInfoStatus::OK);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(info.fw_version.data), info.fw_version.size), "0.1.0+g1a2b3c4");
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(info.board.data), info.board.size), "itsybitsy-nrf52840");
  ASSERT_EQ(info.radio_count, 1);
  const RadioDescriptor &radio = info.radios[0];
  EXPECT_EQ(radio.chip, static_cast<uint16_t>(ChipId::NRF52840));
  EXPECT_EQ(radio.profiles, phy_profiles::IEEE802154_OQPSK_250K);
  EXPECT_EQ(radio.band_min_hz, 2400000000u);
  EXPECT_EQ(radio.band_max_hz, 2483500000u);
  EXPECT_EQ(radio.tx_power_min_dbm, -40);
  EXPECT_EQ(radio.tx_power_max_dbm, 8);
  EXPECT_EQ(radio.max_payload, 125);
  EXPECT_NE(radio.caps & radio_caps::STROBE, 0u);
  EXPECT_NE(radio.caps & radio_caps::CCA, 0u);
  EXPECT_EQ(radio.caps & radio_caps::FEM_CONTROL, 0u);
  EXPECT_EQ(radio.strobe_jitter_us, 2);
  EXPECT_EQ(radio.max_preamble_bytes, 0);
  EXPECT_TRUE(info.has_rx_queue_depth);
  EXPECT_EQ(info.rx_queue_depth, 16);
  EXPECT_EQ(info.reset_cause, ResetCause::PIN);
}

TEST(RadioBridleCodec, DualBandReadyListsBothRadios) {
  const Message m = decode_vector(vector_named("ready_dual_band"));
  ReadyInfo info;
  ASSERT_EQ(parse_ready_info(as<Ready>(m).info, info), ReadyInfoStatus::OK);
  ASSERT_EQ(info.radio_count, 2);
  EXPECT_EQ(info.radios[0].radio, 0);
  EXPECT_EQ(info.radios[1].radio, 1);
  EXPECT_NE(info.radios[1].profiles & phy_profiles::GFSK_RAW, 0);
  EXPECT_EQ(info.radios[1].chip, static_cast<uint16_t>(ChipId::SX1262));
  EXPECT_EQ(info.radios[1].max_preamble_bytes, 8191);
}

TEST(RadioBridleCodec, OlderPeersTruncatedStatsKeepTheCountersTheyHave) {
  const Message m = decode_vector(vector_named("stats_truncated_older_peer"));
  const Stats &stats = as<Stats>(m);
  EXPECT_EQ(stats.present, 2);
}

// === COBS ===

TEST(RadioBridleCodec, CobsRoundTripsZeroRunsAndBlockBoundaries) {
  for (size_t n : {0u, 1u, 2u, 253u, 254u, 255u}) {
    for (int pattern = 0; pattern < 3; pattern++) {
      std::vector<uint8_t> in(n);
      for (size_t i = 0; i < n; i++)
        in[i] = pattern == 0 ? 0 : pattern == 1 ? 0x41 : static_cast<uint8_t>(i % 3);  // zeros / no zeros / mixed
      uint8_t enc[MAX_WIRE_SIZE];
      size_t e = cobs_encode(in.data(), n, enc, sizeof(enc));
      ASSERT_GT(e, 0u) << n << "/" << pattern;
      for (size_t i = 0; i < e; i++)
        ASSERT_NE(enc[i], 0) << "COBS output must not contain the delimiter";
      uint8_t dec[MAX_WIRE_SIZE];
      int d = cobs_decode(enc, e, dec, sizeof(dec));
      ASSERT_EQ(d, static_cast<int>(n)) << n << "/" << pattern;
      EXPECT_EQ(0, n == 0 ? 0 : std::memcmp(dec, in.data(), n));
    }
  }
}

TEST(RadioBridleCodec, CobsEncodeReportsATooSmallOutput) {
  const uint8_t in[] = {1, 2, 3, 0, 4};
  uint8_t out[8];
  for (size_t cap = 0; cap < 6; cap++)
    EXPECT_EQ(cobs_encode(in, sizeof(in), out, cap), 0u) << "cap " << cap;
  EXPECT_EQ(cobs_encode(in, sizeof(in), out, 6), 6u);
}

TEST(RadioBridleCodec, CobsDecodeRejectsMalformedBlocks) {
  uint8_t out[16];
  const uint8_t truncated[] = {0x05, 0x01, 0x01};
  EXPECT_EQ(cobs_decode(truncated, sizeof(truncated), out, sizeof(out)), COBS_MALFORMED);
  const uint8_t zero_code[] = {0x00, 0x01};
  EXPECT_EQ(cobs_decode(zero_code, sizeof(zero_code), out, sizeof(out)), COBS_MALFORMED);
  const uint8_t zero_inside[] = {0x03, 0x01, 0x00};
  EXPECT_EQ(cobs_decode(zero_inside, sizeof(zero_inside), out, sizeof(out)), COBS_MALFORMED);
  const uint8_t fits[] = {0x03, 0x01, 0x01};
  EXPECT_EQ(cobs_decode(fits, sizeof(fits), out, 1), COBS_TOO_LONG);
}

TEST(RadioBridleCodec, MaximumMessageRoundTripsThroughAFrame) {
  std::vector<uint8_t> text(MAX_BODY_SIZE - 1, 'x');  // log: level byte + text
  Log log;
  log.level = LogLevel::INFO;
  log.text = {text.data(), text.size()};
  uint8_t wire[MAX_WIRE_SIZE];
  size_t w = encode_frame(9, log, wire, sizeof(wire));
  ASSERT_GT(w, 0u);
  EXPECT_LE(w, MAX_WIRE_SIZE);
  Deframer d;
  ASSERT_EQ(deframe(d, wire, w), Deframer::Event::MESSAGE);
  EXPECT_EQ(d.message_size(), MAX_MESSAGE_SIZE);
}

TEST(RadioBridleCodec, MessageOverTheLimitIsNotEncoded) {
  std::vector<uint8_t> text(MAX_BODY_SIZE, 'x');
  Log log;
  log.text = {text.data(), text.size()};  // level byte + 251 = 252 body bytes
  uint8_t out[MAX_WIRE_SIZE + 8];
  EXPECT_EQ(encode_message(1, log, out, sizeof(out)), 0u);
  EXPECT_EQ(encode_frame(1, log, out, sizeof(out)), 0u);
}

TEST(RadioBridleCodec, EncodeRefusesATooSmallBuffer) {
  uint8_t out[3];
  EXPECT_EQ(encode_message(1, Hello{}, out, sizeof(out)), 0u);
  uint8_t msg[MAX_MESSAGE_SIZE];
  size_t n = encode_message(1, Hello{}, msg, sizeof(msg));
  ASSERT_EQ(n, MIN_MESSAGE_SIZE);
  uint8_t wire[4];
  EXPECT_EQ(frame_message(msg, n, wire, sizeof(wire)), 0u) << "the delimiter needs room too";
}

// === Deframer ===

TEST(RadioBridleCodec, DeframerResynchronisesAfterGarbage) {
  Deframer d;
  const uint8_t garbage[] = {0x13, 0x37, 0x00};
  EXPECT_EQ(deframe(d, garbage, sizeof(garbage)), Deframer::Event::DROPPED);
  const Vector &hello = vector_named("hello");
  EXPECT_EQ(deframe(d, hello.wire, hello.wire_len), Deframer::Event::MESSAGE);
}

TEST(RadioBridleCodec, DeframerIgnoresEmptyFrames) {
  Deframer d;
  EXPECT_EQ(d.feed(0x00), Deframer::Event::NONE);
  EXPECT_EQ(d.feed(0x00), Deframer::Event::NONE);
  EXPECT_EQ(d.counters().total(), 0u);
}

TEST(RadioBridleCodec, DeframerDeliversBackToBackFramesOneByOne) {
  Deframer d;
  const Vector &a = vector_named("hello");
  const Vector &b = vector_named("soft_reset");
  std::vector<uint8_t> stream(a.wire, a.wire + a.wire_len);
  stream.insert(stream.end(), b.wire, b.wire + b.wire_len);
  int messages = 0;
  for (uint8_t byte : stream) {
    if (d.feed(byte) == Deframer::Event::MESSAGE)
      messages++;
  }
  EXPECT_EQ(messages, 2);
}

TEST(RadioBridleCodec, DeframerSurvivesAnOversizedRun) {
  Deframer d;
  for (int i = 0; i < 1000; i++)
    d.feed(0x41);
  EXPECT_EQ(d.feed(0x00), Deframer::Event::DROPPED);
  EXPECT_EQ(d.counters().bad_length, 1u);
  const Vector &hello = vector_named("hello");
  EXPECT_EQ(deframe(d, hello.wire, hello.wire_len), Deframer::Event::MESSAGE);
}

// === Layout rejections (spec §2.2, error(bad_length)) ===

TEST(RadioBridleCodec, ShortBodiesAreBadLength) {
  const std::vector<std::pair<MsgType, size_t>> minimum = {
      {MsgType::SET_PARAMS, 1}, {MsgType::RX_ON, 6},        {MsgType::ACK, 1},     {MsgType::IDLE, 1},
      {MsgType::GET_RSSI, 1},   {MsgType::GET_STATS, 1},    {MsgType::RX_SCAN, 6}, {MsgType::READY, 9},
      {MsgType::PARAMS_ACK, 2}, {MsgType::FRAME_START, 10}, {MsgType::RX, 13},     {MsgType::RX_ABORT, 10},
      {MsgType::TX_DONE, 14},   {MsgType::RSSI, 3},         {MsgType::STATS, 1},   {MsgType::ERROR, 4},
      {MsgType::LOG, 1},        {MsgType::RX_OVERFLOW, 2},  {MsgType::TX, 9},      {MsgType::ENTER_BOOTLOADER, 4}};
  for (const auto &[type, size] : minimum) {
    std::vector<uint8_t> body(size, 0);
    if (type == MsgType::ENTER_BOOTLOADER)
      std::memcpy(body.data(), ENTER_BOOTLOADER_MAGIC, sizeof(ENTER_BOOTLOADER_MAGIC));
    EXPECT_EQ(decode_hand_built(static_cast<uint8_t>(type), body), DecodeStatus::OK)
        << "type 0x" << std::hex << static_cast<int>(type) << " at its minimum size";
    body.pop_back();
    EXPECT_EQ(decode_hand_built(static_cast<uint8_t>(type), body), DecodeStatus::BAD_LENGTH)
        << "type 0x" << std::hex << static_cast<int>(type) << " one byte short";
  }
}

TEST(RadioBridleCodec, RxOnTwoBytesShortIsBadLength) {
  // Conformance check C17's example.
  EXPECT_EQ(decode_hand_built(static_cast<uint8_t>(MsgType::RX_ON), {0x00, 0x01, 0x02, 0x03}),
            DecodeStatus::BAD_LENGTH);
}

TEST(RadioBridleCodec, TheRetiredGetInfoTypeIsAnUnknownMessage) {
  Message m;
  std::vector<uint8_t> msg = message_with(RETIRED_MSG_TYPES[0], {});
  ASSERT_EQ(decode_message(msg.data(), msg.size(), m), DecodeStatus::OK);
  EXPECT_TRUE(std::holds_alternative<Unknown>(m.body));
  EXPECT_FALSE(is_known_command(RETIRED_MSG_TYPES[0])) << "so a co-processor answers it error(unknown_type)";
}

TEST(RadioBridleCodec, MessageTypesCarryTheirDirectionAndRepliesNameTheirCommand) {
  for (const Vector &v : VECTORS) {
    if (!v.valid)
      continue;
    EXPECT_EQ(is_command(v.msg_type), v.direction == VectorDirection::HOST_TO_COPROC) << v.name;
    if (is_reply(v.msg_type)) {
      // A reply's body starts with cmd_seq: byte 2 of the message.
      EXPECT_GE(v.decoded_len, 5u) << v.name;
    }
    EXPECT_FALSE(is_reply(v.msg_type) && is_event(v.msg_type)) << v.name;
  }
  for (MsgType command :
       {MsgType::HELLO, MsgType::SET_PARAMS, MsgType::RX_ON, MsgType::RX_SCAN, MsgType::IDLE, MsgType::TX,
        MsgType::GET_RSSI, MsgType::GET_STATS, MsgType::SOFT_RESET, MsgType::ENTER_BOOTLOADER}) {
    uint8_t const reply = reply_type_of(static_cast<uint8_t>(command));
    EXPECT_TRUE(is_known_command(static_cast<uint8_t>(command)));
    EXPECT_TRUE(is_reply(reply)) << "command 0x" << std::hex << static_cast<int>(command);
  }
  EXPECT_EQ(reply_type_of(static_cast<uint8_t>(MsgType::RX_SCAN)), static_cast<uint8_t>(MsgType::ACK));
}

TEST(RadioBridleCodec, ExtraBytesAreBadLengthWhileTheProtocolIsInDevelopment) {
  EXPECT_EQ(decode_hand_built(static_cast<uint8_t>(MsgType::HELLO), {0x00}), DecodeStatus::BAD_LENGTH);
  EXPECT_EQ(decode_hand_built(static_cast<uint8_t>(MsgType::RX_ON), {0, 0, 0, 0, 0, 0, 0}), DecodeStatus::BAD_LENGTH);
  EXPECT_EQ(decode_hand_built(static_cast<uint8_t>(MsgType::RX_OVERFLOW), {0, 0, 0}), DecodeStatus::BAD_LENGTH);
}

TEST(RadioBridleCodec, WrongBootloaderMagicIsBadLength) {
  std::vector<uint8_t> body(ENTER_BOOTLOADER_MAGIC, ENTER_BOOTLOADER_MAGIC + sizeof(ENTER_BOOTLOADER_MAGIC));
  EXPECT_EQ(decode_hand_built(static_cast<uint8_t>(MsgType::ENTER_BOOTLOADER), body), DecodeStatus::OK);
  body[3] ^= 0x01;
  EXPECT_EQ(decode_hand_built(static_cast<uint8_t>(MsgType::ENTER_BOOTLOADER), body), DecodeStatus::BAD_LENGTH);
}

TEST(RadioBridleCodec, TlvRunningPastTheBodyIsBadLength) {
  // set_params: radio 0, then a TLV that promises 5 bytes and has 1.
  EXPECT_EQ(decode_hand_built(static_cast<uint8_t>(MsgType::SET_PARAMS), {0x00, 0x03, 0x05, 0x01}),
            DecodeStatus::BAD_LENGTH);
  // params_ack: the same defect.
  EXPECT_EQ(decode_hand_built(static_cast<uint8_t>(MsgType::PARAMS_ACK), {0x01, 0x00, 0x03, 0x05, 0x01}),
            DecodeStatus::BAD_LENGTH);
}

TEST(RadioBridleCodec, ATxWhosePayloadRunsPastTheBodyIsBadLength) {
  // radio, freq, flags, preamble, len_payload = 5 but only 1 byte follows.
  EXPECT_EQ(decode_hand_built(static_cast<uint8_t>(MsgType::TX), {0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 1}),
            DecodeStatus::BAD_LENGTH);
}

TEST(RadioBridleCodec, RxScanWithMoreFrequenciesThanTheLimitKeepsTheTrueCount) {
  std::vector<uint8_t> body = {0, 0xE8, 0x03, 0xC4, 0x09, 17};  // 17 frequencies
  body.insert(body.end(), 17 * 4, 0x11);
  Message m;
  std::vector<uint8_t> msg = message_with(static_cast<uint8_t>(MsgType::RX_SCAN), body);
  ASSERT_EQ(decode_message(msg.data(), msg.size(), m), DecodeStatus::OK);
  EXPECT_EQ(as<RxScan>(m).num_freqs, 17) << "the receiver answers param_range, not bad_length";
  uint8_t out[MAX_MESSAGE_SIZE];
  EXPECT_EQ(encode_message(1, as<RxScan>(m), out, sizeof(out)), 0u) << "the codec never sends more than the limit";
}

TEST(RadioBridleCodec, StatsWithAPartialCounterIsBadLength) {
  EXPECT_EQ(decode_hand_built(static_cast<uint8_t>(MsgType::STATS), {1, 0, 0, 0, 0, 9}), DecodeStatus::BAD_LENGTH);
  EXPECT_EQ(decode_hand_built(static_cast<uint8_t>(MsgType::STATS), {1}), DecodeStatus::OK);
}

TEST(RadioBridleCodec, StatsBeyondTheKnownCountersAreIgnored) {
  std::vector<uint8_t> body = {1};
  body.insert(body.end(), 11 * 4, 0x01);  // two counters more than this codec names
  Message m;
  std::vector<uint8_t> msg = message_with(static_cast<uint8_t>(MsgType::STATS), body);
  ASSERT_EQ(decode_message(msg.data(), msg.size(), m), DecodeStatus::OK);
  EXPECT_EQ(as<Stats>(m).present, Stats::COUNT);
}

TEST(RadioBridleCodec, MessageShorterThanHeaderAndCrcIsBadLength) {
  Message m;
  const uint8_t tiny[] = {0x01, 0x01, 0x00};
  EXPECT_EQ(decode_message(tiny, sizeof(tiny), m), DecodeStatus::BAD_LENGTH);
}

// === Unknown types ===

TEST(RadioBridleCodec, UnknownTypesDecodeAsUnknownWithTheirRawBody) {
  for (uint8_t type : {uint8_t{0x7F}, uint8_t{0xFE}}) {
    std::vector<uint8_t> msg = message_with(type, {0xDE, 0xAD});
    Message m;
    ASSERT_EQ(decode_message(msg.data(), msg.size(), m), DecodeStatus::OK);
    const Unknown &u = as<Unknown>(m);
    EXPECT_EQ(u.msg_type, type);
    EXPECT_EQ(bytes_of(u.body), (std::vector<uint8_t>{0xDE, 0xAD}));
    EXPECT_EQ(msg_type_of(m.body), type);
  }
}

// === TLV lists ===

TEST(RadioBridleCodec, TlvWriterAndReaderRoundTrip) {
  uint8_t buf[32];
  TlvWriter w(buf, sizeof(buf));
  w.add_u8(static_cast<uint8_t>(ParamId::SFD), 0x56).add_u16(0x80, 0x0102);
  w.add_u32(static_cast<uint8_t>(ParamId::BITRATE_BPS), 38400);
  ASSERT_TRUE(w.ok());
  EXPECT_EQ(w.size(), 3u + 4u + 6u);

  TlvReader r(w.span());
  uint8_t id;
  ByteSpan value;
  ASSERT_TRUE(r.next(id, value));
  EXPECT_EQ(id, static_cast<uint8_t>(ParamId::SFD));
  EXPECT_EQ(bytes_of(value), (std::vector<uint8_t>{0x56}));
  ASSERT_TRUE(r.next(id, value));
  EXPECT_EQ(bytes_of(value), (std::vector<uint8_t>{0x02, 0x01}));
  ASSERT_TRUE(r.next(id, value));
  EXPECT_EQ(bytes_of(value), (std::vector<uint8_t>{0x00, 0x96, 0x00, 0x00}));
  EXPECT_FALSE(r.next(id, value));
  EXPECT_FALSE(r.malformed());
}

TEST(RadioBridleCodec, TlvWriterFailsStickyWhenAnEntryDoesNotFit) {
  uint8_t buf[5];
  TlvWriter w(buf, sizeof(buf));
  w.add_u8(1, 1);
  ASSERT_TRUE(w.ok());
  w.add_u32(2, 2);  // needs 6 bytes, 3 are left
  EXPECT_FALSE(w.ok());
  w.add_u8(3, 3);  // would fit, but the list is already incomplete
  EXPECT_FALSE(w.ok());
  EXPECT_EQ(w.size(), 3u);
}

TEST(RadioBridleCodec, TlvReaderFlagsAnEntryRunningPastTheList) {
  const uint8_t list[] = {0x01, 0x01, 0xAA, 0x02, 0x09, 0xBB};
  TlvReader r({list, sizeof(list)});
  uint8_t id;
  ByteSpan value;
  EXPECT_TRUE(r.next(id, value));
  EXPECT_FALSE(r.next(id, value));
  EXPECT_TRUE(r.malformed());
  EXPECT_FALSE(tlv_list_valid({list, sizeof(list)}));
  const uint8_t lone_id[] = {0x01};
  EXPECT_FALSE(tlv_list_valid({lone_id, 1}));
  EXPECT_TRUE(tlv_list_valid({}));
}

TEST(RadioBridleCodec, FindTlvReturnsTheFirstEntryWithThatId) {
  const uint8_t list[] = {0x01, 0x01, 0xAA, 0x02, 0x01, 0xBB, 0x02, 0x01, 0xCC};
  ByteSpan value;
  ASSERT_TRUE(find_tlv({list, sizeof(list)}, 0x02, value));
  EXPECT_EQ(value.data[0], 0xBB);
  EXPECT_FALSE(find_tlv({list, sizeof(list)}, 0x09, value));
}

TEST(RadioBridleCodec, UnknownTlvIdsSurviveARoundTripUntouched) {
  // Vendor params (0x80…0xFF) are opaque to the codec.
  const Message m = decode_vector(vector_named("set_params_vendor_tlv"));
  ByteSpan value;
  ASSERT_TRUE(find_tlv(as<SetParams>(m).tlvs, 0x80, value));
  EXPECT_EQ(value.size, 2u);
}

// === The ready descriptor ===

namespace {

struct ReadyBuilder {
  uint8_t buf[320];
  TlvWriter w{buf, sizeof(buf)};
  RadioDescriptor radio{};
  ReadyBuilder() {
    radio.radio = 0;
    radio.chip = static_cast<uint16_t>(ChipId::NRF52840);
    radio.profiles = phy_profiles::IEEE802154_OQPSK_250K;
    radio.band_min_hz = 2400000000u;
    radio.band_max_hz = 2483500000u;
    radio.tx_power_min_dbm = -40;
    radio.tx_power_max_dbm = 8;
    radio.max_payload = 125;
    radio.caps = radio_caps::RX_SCAN;
  }
  ReadyInfoStatus parse(ReadyInfo &info) { return parse_ready_info(w.span(), info); }
};

const uint8_t FW[] = {'1', '.', '0'};

}  // namespace

TEST(RadioBridleCodec, ReadyInfoNeedsEveryRequiredTlv) {
  ReadyInfo info;
  {
    ReadyBuilder b;  // no fw_version
    b.w.add_radio(b.radio).add_u8(static_cast<uint8_t>(InfoId::RESET_CAUSE), 0);
    EXPECT_EQ(b.parse(info), ReadyInfoStatus::MISSING_FW_VERSION);
  }
  {
    ReadyBuilder b;  // no radio
    b.w.add(static_cast<uint8_t>(InfoId::FW_VERSION), FW, sizeof(FW))
        .add_u8(static_cast<uint8_t>(InfoId::RESET_CAUSE), 0);
    EXPECT_EQ(b.parse(info), ReadyInfoStatus::MISSING_RADIO);
  }
  {
    ReadyBuilder b;  // no reset_cause
    b.w.add(static_cast<uint8_t>(InfoId::FW_VERSION), FW, sizeof(FW)).add_radio(b.radio);
    EXPECT_EQ(b.parse(info), ReadyInfoStatus::MISSING_RESET_CAUSE);
  }
}

TEST(RadioBridleCodec, ReadyInfoCarriesTheRadiosOwnJitterAndPreambleLimitAndSkipsUnknownIds) {
  ReadyBuilder b;
  b.w.add(static_cast<uint8_t>(InfoId::FW_VERSION), FW, sizeof(FW))
      .add_radio(b.radio)
      .add_u8(0x77, 1)  // a TLV id from the future
      .add_u8(static_cast<uint8_t>(InfoId::RESET_CAUSE), static_cast<uint8_t>(ResetCause::WATCHDOG));
  ReadyInfo info;
  ASSERT_EQ(b.parse(info), ReadyInfoStatus::OK);
  EXPECT_EQ(info.radios[0].chip, static_cast<uint16_t>(ChipId::NRF52840));
  EXPECT_EQ(info.reset_cause, ResetCause::WATCHDOG);
}

TEST(RadioBridleCodec, ReadyInfoRejectsATlvOfTheWrongLength) {
  ReadyInfo info;
  {
    ReadyBuilder b;
    const uint8_t short_radio[] = {0, 1, 0};
    b.w.add(static_cast<uint8_t>(InfoId::RADIO), short_radio, sizeof(short_radio));
    EXPECT_EQ(b.parse(info), ReadyInfoStatus::BAD_TLV);
  }
  {
    ReadyBuilder b;
    uint8_t const too_long[INFO_FW_VERSION_MAX_BYTES + 1] = {};  // fw_version is at most 32 bytes
    b.w.add(static_cast<uint8_t>(InfoId::FW_VERSION), too_long, sizeof(too_long));
    EXPECT_EQ(b.parse(info), ReadyInfoStatus::BAD_TLV);
  }
  {
    ReadyBuilder b;
    uint8_t const too_long[INFO_BOARD_MAX_BYTES + 1] = {};  // board is at most 32 bytes
    b.w.add(static_cast<uint8_t>(InfoId::BOARD), too_long, sizeof(too_long));
    EXPECT_EQ(b.parse(info), ReadyInfoStatus::BAD_TLV);
  }
  {
    ReadyBuilder b;
    b.w.add_u16(static_cast<uint8_t>(InfoId::RESET_CAUSE), 1);  // wants one byte
    EXPECT_EQ(b.parse(info), ReadyInfoStatus::BAD_TLV);
  }
  {
    const uint8_t overrun[] = {0x01, 0x09, 0x41};
    EXPECT_EQ(parse_ready_info({overrun, sizeof(overrun)}, info), ReadyInfoStatus::BAD_TLV);
  }
}

TEST(RadioBridleCodec, ReadyInfoKeepsTheFirstRadiosOnly) {
  ReadyBuilder b;
  b.w.add(static_cast<uint8_t>(InfoId::FW_VERSION), FW, sizeof(FW));
  for (uint8_t i = 0; i < MAX_RADIOS + 2; i++) {
    b.radio.radio = i;
    b.w.add_radio(b.radio);
  }
  b.w.add_u8(static_cast<uint8_t>(InfoId::RESET_CAUSE), 0);
  ReadyInfo info;
  ASSERT_EQ(b.parse(info), ReadyInfoStatus::OK);
  EXPECT_EQ(info.radio_count, MAX_RADIOS);
  EXPECT_EQ(info.radios[MAX_RADIOS - 1].radio, MAX_RADIOS - 1);
}

TEST(RadioBridleCodec, ReadyInfoStatusNamesTheMissingTlv) {
  EXPECT_STREQ(ready_info_status_name(ReadyInfoStatus::MISSING_FW_VERSION), "fw_version");
  EXPECT_STREQ(ready_info_status_name(ReadyInfoStatus::MISSING_RADIO), "radio");
  EXPECT_STREQ(ready_info_status_name(ReadyInfoStatus::MISSING_RESET_CAUSE), "reset_cause");
}

// === Building commands ===

TEST(RadioBridleCodec, ACommandBuiltFromStructsMatchesTheSpecVectors) {
  uint8_t wire[MAX_WIRE_SIZE];
  {
    // vector set_params_io2g4: phy_profile, sfd, tx_power_dbm, in that order
    uint8_t tlvs[16];
    TlvWriter w(tlvs, sizeof(tlvs));
    w.add_u8(static_cast<uint8_t>(ParamId::PHY_PROFILE), static_cast<uint8_t>(PhyProfile::IEEE802154_OQPSK_250K))
        .add_u8(static_cast<uint8_t>(ParamId::SFD), 0x56)
        .add_u8(static_cast<uint8_t>(ParamId::TX_POWER_DBM), 8);
    SetParams cmd;
    cmd.radio = 0;
    cmd.tlvs = w.span();
    const Vector &v = vector_named("set_params_io2g4");
    size_t n = encode_frame(v.seq, cmd, wire, sizeof(wire));
    ASSERT_EQ(n, v.wire_len);
    EXPECT_EQ(0, std::memcmp(wire, v.wire, n));
  }
  {
    // vector rx_on_force
    RxOn rx_on;
    rx_on.radio = 1;
    rx_on.freq_hz = 868950000;
    rx_on.force = true;
    const Vector &v = vector_named("rx_on_force");
    size_t n = encode_frame(v.seq, rx_on, wire, sizeof(wire));
    ASSERT_EQ(n, v.wire_len);
    EXPECT_EQ(0, std::memcmp(wire, v.wire, n));
  }
}

TEST(RadioBridleCodec, AStrobeTemplateOverTheLimitDecodesButIsNeverSent) {
  // radio, freq, flags (has_strobe), preamble, then a strobe whose template promises 33 bytes.
  std::vector<uint8_t> body = {0, 0, 0, 0, 0, 0x02, 0, 0};
  body.insert(body.end(), {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});  // count..counter_adjust
  body.push_back(33);
  body.insert(body.end(), 33, 0xAB);
  body.push_back(0);  // empty payload
  Message m;
  std::vector<uint8_t> msg = message_with(static_cast<uint8_t>(MsgType::TX), body);
  ASSERT_EQ(decode_message(msg.data(), msg.size(), m), DecodeStatus::OK) << "a value error, not a layout error";
  EXPECT_EQ(as<Tx>(m).strobe.len_template, 33);
  uint8_t out[MAX_MESSAGE_SIZE];
  EXPECT_EQ(encode_message(1, as<Tx>(m), out, sizeof(out)), 0u);
}

TEST(RadioBridleCodec, StatsWithMoreCountersThanTheStructIsNotEncoded) {
  Stats stats;
  stats.present = Stats::COUNT + 1;
  uint8_t out[MAX_MESSAGE_SIZE];
  EXPECT_EQ(encode_message(1, stats, out, sizeof(out)), 0u);
}

// === The host's version rule (spec §10.2), versions passed in ===

TEST(RadioBridleVersionRule, WhileTheMajorIsZeroBothNumbersMustMatch) {
  EXPECT_TRUE(version_accepted(0, 6, 0, 6));
  EXPECT_FALSE(version_accepted(0, 6, 0, 5));
  EXPECT_FALSE(version_accepted(0, 6, 0, 7));
  EXPECT_FALSE(version_accepted(0, 6, 1, 6));
}

TEST(RadioBridleVersionRule, FromOneOnAnyMinorOfTheSameMajorIsAccepted) {
  EXPECT_TRUE(version_accepted(1, 2, 1, 0));
  EXPECT_TRUE(version_accepted(1, 2, 1, 2));
  EXPECT_TRUE(version_accepted(1, 2, 1, 3));
  EXPECT_FALSE(version_accepted(1, 2, 2, 0));
  EXPECT_FALSE(version_accepted(1, 2, 0, 2));
}

TEST(RadioBridleVersionRule, ThisBuildAcceptsItsOwnVersion) {
  EXPECT_TRUE(version_accepted(PROTO_MAJOR, PROTO_MINOR, PROTO_MAJOR, PROTO_MINOR));
}

TEST(RadioBridleVersionRule, ACoprocessorSupportsWhatItsMinorDefines) {
  EXPECT_TRUE(coproc_supports(1, 3, 1, 3));
  EXPECT_TRUE(coproc_supports(1, 4, 1, 3));
  EXPECT_FALSE(coproc_supports(1, 2, 1, 3));
  EXPECT_FALSE(coproc_supports(2, 9, 1, 3));
}
