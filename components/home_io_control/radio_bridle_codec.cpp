/// @file
/// @brief Bridle wire codec implementation: COBS, message encode/decode, deframer, TLV lists.
/// @ingroup hioc_radio

// IOHOME_BRIDLE is only visible after something pulls in esphome/core/defines.h (ESPHome codegen's
// cg.add_define() lands there, not as a compiler -D flag): this include must run before the #ifdef,
// or the file silently compiles to nothing in a real build although the host tests (which pass
// -DIOHOME_BRIDLE) are fine.
#include "esphome/core/hal.h"

#ifdef IOHOME_BRIDLE

#include "radio_bridle_codec.h"

#include <cstring>
#include <type_traits>
#include <utility>

#include "proto_frame.h"

namespace esphome::home_io_control::bridle {

// === Byte writer and reader ===

// The shifts and masks below are the wire encoding itself (little-endian fields, one-bit flags at
// the positions the .ksy fixes); naming each 8, 16 and 0xFF would hide the layout they spell out.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

namespace {

/// Appends little-endian fields to a caller-owned buffer; once a field does not fit, the writer
/// stays failed and ignores further writes.
class Writer {
 public:
  Writer(uint8_t *buf, size_t cap) : buf_(buf), cap_(cap) {}
  void u8(uint8_t v) { this->bytes(&v, 1); }
  void u16(uint16_t v) {
    this->u8(static_cast<uint8_t>(v));
    this->u8(static_cast<uint8_t>(v >> 8));
  }
  void u32(uint32_t v) {
    this->u16(static_cast<uint16_t>(v));
    this->u16(static_cast<uint16_t>(v >> 16));
  }
  void bytes(const uint8_t *p, size_t n) {
    if (!this->ok_ || this->size_ + n > this->cap_) {
      this->ok_ = false;
      return;
    }
    if (n > 0)
      std::memcpy(this->buf_ + this->size_, p, n);
    this->size_ += n;
  }
  void span(ByteSpan s) { this->bytes(s.data, s.size); }
  /// A length-prefixed run (`len u8 · bytes`); fails when the run is longer than 255 bytes.
  void prefixed(ByteSpan s) {
    if (s.size > UINT8_MAX)
      this->ok_ = false;
    this->u8(static_cast<uint8_t>(s.size));
    this->span(s);
  }
  void fail() { this->ok_ = false; }
  [[nodiscard]] bool ok() const { return this->ok_; }
  [[nodiscard]] size_t size() const { return this->size_; }

 private:
  uint8_t *buf_;
  size_t cap_;
  size_t size_{0};
  bool ok_{true};
};

/// Reads little-endian fields from a buffer; a read past the end fails the reader and returns 0.
class Reader {
 public:
  Reader(const uint8_t *p, size_t n) : p_(p), n_(n) {}
  uint8_t u8() {
    if (this->pos_ + 1 > this->n_) {
      this->ok_ = false;
      return 0;
    }
    return this->p_[this->pos_++];
  }
  uint16_t u16() {
    uint16_t const lo = this->u8();
    return static_cast<uint16_t>(lo | (this->u8() << 8));
  }
  uint32_t u32() {
    uint32_t const lo = this->u16();
    return lo | (static_cast<uint32_t>(this->u16()) << 16);
  }
  ByteSpan bytes(size_t n) {
    if (this->pos_ + n > this->n_) {
      this->ok_ = false;
      return {};
    }
    ByteSpan const s{this->p_ + this->pos_, n};
    this->pos_ += n;
    return s;
  }
  /// A length-prefixed run (`len u8 · bytes`).
  ByteSpan prefixed() { return this->bytes(this->u8()); }
  ByteSpan rest() { return this->bytes(this->n_ - this->pos_); }
  [[nodiscard]] size_t remaining() const { return this->n_ - this->pos_; }
  void fail() { this->ok_ = false; }
  /// Every field read fitted and nothing is left over. While the protocol is 0.y a receiver
  /// treats leftover bytes as a layout mismatch (spec §2.2).
  [[nodiscard]] bool done() const { return this->ok_ && this->pos_ == this->n_; }

 private:
  const uint8_t *p_;
  size_t n_;
  size_t pos_{0};
  bool ok_{true};
};

constexpr uint8_t flag(bool set, uint8_t mask) { return set ? mask : 0; }

/// A COBS block of this code carries 254 data bytes and is not followed by an implied zero.
constexpr uint8_t COBS_FULL_BLOCK = 0xFF;

void write_radio(Writer &w, const RadioDescriptor &d) {
  w.u8(d.radio);
  w.u16(d.chip);
  w.u16(d.profiles);
  w.u32(d.band_min_hz);
  w.u32(d.band_max_hz);
  w.u8(static_cast<uint8_t>(d.tx_power_min_dbm));
  w.u8(static_cast<uint8_t>(d.tx_power_max_dbm));
  w.u8(d.max_payload);
  w.u16(d.max_preamble_bytes);
  w.u16(d.strobe_jitter_us);
  w.u32(d.caps);
}

void read_radio(Reader &r, RadioDescriptor &d) {
  d.radio = r.u8();
  d.chip = r.u16();
  d.profiles = r.u16();
  d.band_min_hz = r.u32();
  d.band_max_hz = r.u32();
  d.tx_power_min_dbm = static_cast<int8_t>(r.u8());
  d.tx_power_max_dbm = static_cast<int8_t>(r.u8());
  d.max_payload = r.u8();
  d.max_preamble_bytes = r.u16();
  d.strobe_jitter_us = r.u16();
  d.caps = r.u32();
}

// === Body layouts: write() and read() per message type, in the order of the .ksy ===

void write(Writer & /*w*/, const Hello & /*m*/) {}
bool read(Reader &r, Hello & /*m*/) { return r.done(); }
void write(Writer & /*w*/, const SoftReset & /*m*/) {}
bool read(Reader &r, SoftReset & /*m*/) { return r.done(); }

void write(Writer &w, const SetParams &m) {
  w.u8(m.radio);
  w.span(m.tlvs);
}
bool read(Reader &r, SetParams &m) {
  m.radio = r.u8();
  m.tlvs = r.rest();
  return r.done() && tlv_list_valid(m.tlvs);
}

void write(Writer &w, const RxOn &m) {
  w.u8(m.radio);
  w.u32(m.freq_hz);
  w.u8(flag(m.force, rx_on_flags::FORCE));
}
bool read(Reader &r, RxOn &m) {
  m.radio = r.u8();
  m.freq_hz = r.u32();
  m.force = (r.u8() & rx_on_flags::FORCE) != 0;
  return r.done();
}
void write(Writer &w, const Idle &m) { w.u8(m.radio); }
bool read(Reader &r, Idle &m) {
  m.radio = r.u8();
  return r.done();
}
void write(Writer &w, const GetRssi &m) { w.u8(m.radio); }
bool read(Reader &r, GetRssi &m) {
  m.radio = r.u8();
  return r.done();
}

void write(Writer &w, const StrobeSpec &s) {
  if (s.len_template > MAX_STROBE_TEMPLATE)
    w.fail();
  w.u16(s.count);
  w.u32(s.period_ns);
  w.u32(s.main_gap_ns);
  w.u8(s.counter_offset);
  w.u8(s.counter_width);
  w.u32(s.counter_unit_ns);
  w.u16(static_cast<uint16_t>(s.counter_adjust));
  w.prefixed({s.template_bytes, s.len_template});
}
void read(Reader &r, StrobeSpec &s) {
  s.count = r.u16();
  s.period_ns = r.u32();
  s.main_gap_ns = r.u32();
  s.counter_offset = r.u8();
  s.counter_width = r.u8();
  s.counter_unit_ns = r.u32();
  s.counter_adjust = static_cast<int16_t>(r.u16());
  s.len_template = r.u8();
  ByteSpan const tmpl = r.bytes(s.len_template);
  // A template over the limit is a value error (param_range) for the receiver, not a layout
  // error, so the codec keeps what fits and leaves len_template to report it.
  size_t const kept = tmpl.size < MAX_STROBE_TEMPLATE ? tmpl.size : MAX_STROBE_TEMPLATE;
  if (kept > 0)
    std::memcpy(s.template_bytes, tmpl.data, kept);
}

void write(Writer &w, const Tx &m) {
  w.u8(m.radio);
  w.u32(m.freq_hz);
  w.u8(flag(m.then_rx, tx_flags::THEN_RX) | flag(m.has_strobe, tx_flags::HAS_STROBE) |
       flag(m.at_time, tx_flags::AT_TIME) | flag(m.cca, tx_flags::CCA));
  w.u16(m.preamble_bytes);
  if (m.at_time)
    w.u32(m.t_main_us);
  if (m.cca)
    w.u8(static_cast<uint8_t>(m.cca_threshold_dbm));
  if (m.has_strobe)
    write(w, m.strobe);
  w.prefixed(m.payload);
}
bool read(Reader &r, Tx &m) {
  m.radio = r.u8();
  m.freq_hz = r.u32();
  uint8_t const flags = r.u8();
  m.then_rx = (flags & tx_flags::THEN_RX) != 0;
  m.has_strobe = (flags & tx_flags::HAS_STROBE) != 0;
  m.at_time = (flags & tx_flags::AT_TIME) != 0;
  m.cca = (flags & tx_flags::CCA) != 0;
  m.preamble_bytes = r.u16();
  if (m.at_time)
    m.t_main_us = r.u32();
  if (m.cca)
    m.cca_threshold_dbm = static_cast<int8_t>(r.u8());
  if (m.has_strobe)
    read(r, m.strobe);
  m.payload = r.prefixed();
  return r.done();
}

void write(Writer &w, const GetStats &m) { w.u8(flag(m.reset_after_read, get_stats::RESET_AFTER_READ)); }
bool read(Reader &r, GetStats &m) {
  m.reset_after_read = (r.u8() & get_stats::RESET_AFTER_READ) != 0;
  return r.done();
}

void write(Writer &w, const EnterBootloader & /*m*/) {
  w.bytes(ENTER_BOOTLOADER_MAGIC, sizeof(ENTER_BOOTLOADER_MAGIC));
}
bool read(Reader &r, EnterBootloader & /*m*/) {
  ByteSpan const magic = r.bytes(sizeof(ENTER_BOOTLOADER_MAGIC));
  return r.done() && std::memcmp(magic.data, ENTER_BOOTLOADER_MAGIC, sizeof(ENTER_BOOTLOADER_MAGIC)) == 0;
}

void write(Writer &w, const RxScan &m) {
  if (m.num_freqs > MAX_SCAN_FREQS)
    w.fail();
  w.u8(m.radio);
  w.u16(m.dwell_us);
  w.u16(m.hold_us);
  w.u8(m.num_freqs);
  for (size_t i = 0; i < m.num_freqs && i < MAX_SCAN_FREQS; i++)
    w.u32(m.freqs_hz[i]);
}
bool read(Reader &r, RxScan &m) {
  m.radio = r.u8();
  m.dwell_us = r.u16();
  m.hold_us = r.u16();
  m.num_freqs = r.u8();
  // Frequencies past the limit are still consumed so the layout check sees the true length; the
  // receiver answers them with param_range.
  for (size_t i = 0; i < m.num_freqs; i++) {
    uint32_t const freq = r.u32();
    if (i < MAX_SCAN_FREQS)
      m.freqs_hz[i] = freq;
  }
  return r.done();
}

void write(Writer &w, const Ready &m) {
  w.u8(m.cmd_seq);
  w.u8(m.proto_major);
  w.u8(m.proto_minor);
  w.u16(m.chip_id);
  w.u32(m.caps);
  w.span(m.info);
}
bool read(Reader &r, Ready &m) {
  m.cmd_seq = r.u8();
  m.proto_major = r.u8();
  m.proto_minor = r.u8();
  m.chip_id = r.u16();
  m.caps = r.u32();
  m.info = r.rest();
  return r.done() && tlv_list_valid(m.info);
}

void write(Writer &w, const Ack &m) { w.u8(m.cmd_seq); }
bool read(Reader &r, Ack &m) {
  m.cmd_seq = r.u8();
  return r.done();
}

void write(Writer &w, const ParamsAck &m) {
  w.u8(m.cmd_seq);
  w.u8(m.radio);
  w.span(m.applied);
}
bool read(Reader &r, ParamsAck &m) {
  m.cmd_seq = r.u8();
  m.radio = r.u8();
  m.applied = r.rest();
  return r.done() && tlv_list_valid(m.applied);
}

void write(Writer &w, const FrameStart &m) {
  w.u8(m.radio);
  w.u32(m.t_us);
  w.u32(m.freq_hz);
  w.u8(static_cast<uint8_t>(m.stage));
}
bool read(Reader &r, FrameStart &m) {
  m.radio = r.u8();
  m.t_us = r.u32();
  m.freq_hz = r.u32();
  m.stage = static_cast<FrameStage>(r.u8());
  return r.done();
}

void write(Writer &w, const Rx &m) {
  w.u8(m.radio);
  w.u32(m.t_us);
  w.u32(m.freq_hz);
  w.u8(static_cast<uint8_t>(m.rssi_dbm));
  w.u8(m.lqi);
  w.u8(static_cast<uint8_t>(m.crc));
  w.prefixed(m.payload);
}
bool read(Reader &r, Rx &m) {
  m.radio = r.u8();
  m.t_us = r.u32();
  m.freq_hz = r.u32();
  m.rssi_dbm = static_cast<int8_t>(r.u8());
  m.lqi = r.u8();
  m.crc = static_cast<CrcState>(r.u8());
  m.payload = r.prefixed();
  return r.done();
}

void write(Writer &w, const RxAbort &m) {
  w.u8(m.radio);
  w.u32(m.t_us);
  w.u32(m.freq_hz);
  w.u8(static_cast<uint8_t>(m.reason));
}
bool read(Reader &r, RxAbort &m) {
  m.radio = r.u8();
  m.t_us = r.u32();
  m.freq_hz = r.u32();
  m.reason = static_cast<RxAbortReason>(r.u8());
  return r.done();
}

void write(Writer &w, const TxDone &m) {
  w.u8(m.cmd_seq);
  w.u8(m.radio);
  w.u32(m.t_main_start_us);
  w.u32(m.t_main_end_us);
  w.u16(m.strobe_sent);
  w.u16(m.strobe_late);
}
bool read(Reader &r, TxDone &m) {
  m.cmd_seq = r.u8();
  m.radio = r.u8();
  m.t_main_start_us = r.u32();
  m.t_main_end_us = r.u32();
  m.strobe_sent = r.u16();
  m.strobe_late = r.u16();
  return r.done();
}

void write(Writer &w, const Rssi &m) {
  w.u8(m.cmd_seq);
  w.u8(m.radio);
  w.u8(static_cast<uint8_t>(m.rssi_dbm));
}
bool read(Reader &r, Rssi &m) {
  m.cmd_seq = r.u8();
  m.radio = r.u8();
  m.rssi_dbm = static_cast<int8_t>(r.u8());
  return r.done();
}

void write(Writer &w, const Stats &m) {
  if (m.present > Stats::COUNT)
    w.fail();
  w.u8(m.cmd_seq);
  for (size_t i = 0; i < m.present && i < Stats::COUNT; i++)
    w.u32(m.counters[i]);
}
bool read(Reader &r, Stats &m) {
  m.cmd_seq = r.u8();
  // Append-only counters: take every whole counter the body holds and ignore ones this codec
  // does not name. A partial counter is a layout error.
  while (r.remaining() >= sizeof(uint32_t)) {
    uint32_t const value = r.u32();
    if (m.present < Stats::COUNT)
      m.counters[m.present++] = value;
  }
  return r.done();
}

void write(Writer &w, const Error &m) {
  w.u8(m.cmd_seq);
  w.u8(static_cast<uint8_t>(m.code));
  w.u16(m.detail);
}
bool read(Reader &r, Error &m) {
  m.cmd_seq = r.u8();
  m.code = static_cast<ErrorCode>(r.u8());
  m.detail = r.u16();
  return r.done();
}

void write(Writer &w, const Log &m) {
  w.u8(static_cast<uint8_t>(m.level));
  w.span(m.text);
}
bool read(Reader &r, Log &m) {
  m.level = static_cast<LogLevel>(r.u8());
  m.text = r.rest();
  return r.done();
}

void write(Writer &w, const RxOverflow &m) { w.u16(m.dropped); }
bool read(Reader &r, RxOverflow &m) {
  m.dropped = r.u16();
  return r.done();
}

void write(Writer &w, const Unknown &m) { w.span(m.body); }

/// Decodes the body as message type `T` into `body`.
template<class T> DecodeStatus decode_as(Reader &r, Body &body) {
  T value{};
  if (!read(r, value))
    return DecodeStatus::BAD_LENGTH;
  body = value;
  return DecodeStatus::OK;
}

}  // namespace
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

// === COBS ===

size_t cobs_encode(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
  if (cap == 0)
    return 0;
  size_t code_pos = 0;
  size_t o = 1;
  uint8_t code = 1;
  // A new block reserves its code byte at once, so the output always ends in a complete block.
  auto close_block = [&]() -> bool {
    out[code_pos] = code;
    code_pos = o;
    code = 1;
    if (o >= cap)
      return false;
    o++;
    return true;
  };
  for (size_t i = 0; i < n; i++) {
    if (in[i] == 0) {
      if (!close_block())
        return 0;
      continue;
    }
    if (o >= cap)
      return 0;
    out[o++] = in[i];
    if (++code == COBS_FULL_BLOCK && !close_block())
      return 0;
  }
  out[code_pos] = code;
  return o;
}

int cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
  size_t i = 0;
  size_t o = 0;
  while (i < n) {
    uint8_t const code = in[i++];
    if (code == 0 || i + (code - 1) > n)
      return COBS_MALFORMED;
    for (uint8_t k = 1; k < code; k++) {
      if (in[i] == 0)
        return COBS_MALFORMED;
      if (o >= cap)
        return COBS_TOO_LONG;
      out[o++] = in[i++];
    }
    // A block shorter than the maximum stands for a zero, unless the input ends with it.
    if (code != COBS_FULL_BLOCK && i < n) {
      if (o >= cap)
        return COBS_TOO_LONG;
      out[o++] = 0;
    }
  }
  return static_cast<int>(o);
}

// === Encoding and decoding ===

uint8_t msg_type_of(const Body &body) {
  return std::visit(
      [](const auto &b) -> uint8_t {
        if constexpr (std::is_same_v<std::decay_t<decltype(b)>, Unknown>) {
          return b.msg_type;
        } else {
          return static_cast<uint8_t>(std::decay_t<decltype(b)>::TYPE);
        }
      },
      body);
}

size_t encode_message(uint8_t seq, const Body &body, uint8_t *out, size_t cap) {
  Writer w(out, cap);
  w.u8(msg_type_of(body));
  w.u8(seq);
  std::visit([&w](const auto &b) { write(w, b); }, body);
  // The CRC covers everything before it, and crc_ccitt() takes an 8-bit length: a message that
  // would exceed the protocol's 255 bytes is refused here, before it could overflow that.
  if (!w.ok() || w.size() + sizeof(uint16_t) > MAX_MESSAGE_SIZE)
    return 0;
  w.u16(crc_ccitt(out, static_cast<uint8_t>(w.size())));
  return w.ok() ? w.size() : 0;
}

size_t frame_message(const uint8_t *msg, size_t n, uint8_t *out, size_t cap) {
  size_t const encoded = cobs_encode(msg, n, out, cap);
  if (encoded == 0 || encoded >= cap)
    return 0;
  out[encoded] = 0;
  return encoded + 1;
}

size_t encode_frame(uint8_t seq, const Body &body, uint8_t *out, size_t cap) {
  uint8_t msg[MAX_MESSAGE_SIZE];
  size_t const n = encode_message(seq, body, msg, sizeof(msg));
  return n == 0 ? 0 : frame_message(msg, n, out, cap);
}

DecodeStatus decode_message(const uint8_t *msg, size_t n, Message &out) {
  if (n < MIN_MESSAGE_SIZE)
    return DecodeStatus::BAD_LENGTH;
  uint8_t const type = msg[0];
  out.seq = msg[1];
  Reader r(msg + 2, n - MIN_MESSAGE_SIZE);
  switch (static_cast<MsgType>(type)) {
    case MsgType::HELLO:
      return decode_as<Hello>(r, out.body);
    case MsgType::SET_PARAMS:
      return decode_as<SetParams>(r, out.body);
    case MsgType::RX_ON:
      return decode_as<RxOn>(r, out.body);
    case MsgType::RX_SCAN:
      return decode_as<RxScan>(r, out.body);
    case MsgType::IDLE:
      return decode_as<Idle>(r, out.body);
    case MsgType::TX:
      return decode_as<Tx>(r, out.body);
    case MsgType::GET_RSSI:
      return decode_as<GetRssi>(r, out.body);
    case MsgType::GET_STATS:
      return decode_as<GetStats>(r, out.body);
    case MsgType::SOFT_RESET:
      return decode_as<SoftReset>(r, out.body);
    case MsgType::ENTER_BOOTLOADER:
      return decode_as<EnterBootloader>(r, out.body);
    case MsgType::READY:
      return decode_as<Ready>(r, out.body);
    case MsgType::PARAMS_ACK:
      return decode_as<ParamsAck>(r, out.body);
    case MsgType::ACK:
      return decode_as<Ack>(r, out.body);
    case MsgType::FRAME_START:
      return decode_as<FrameStart>(r, out.body);
    case MsgType::RX:
      return decode_as<Rx>(r, out.body);
    case MsgType::RX_ABORT:
      return decode_as<RxAbort>(r, out.body);
    case MsgType::TX_DONE:
      return decode_as<TxDone>(r, out.body);
    case MsgType::RSSI:
      return decode_as<Rssi>(r, out.body);
    case MsgType::STATS:
      return decode_as<Stats>(r, out.body);
    case MsgType::ERROR:
      return decode_as<Error>(r, out.body);
    case MsgType::LOG:
      return decode_as<Log>(r, out.body);
    case MsgType::RX_OVERFLOW:
      return decode_as<RxOverflow>(r, out.body);
  }
  // An unknown command is answered with error(unknown_type), an unknown event is ignored: both
  // are the caller's decision, so the body is kept raw.
  out.body = Unknown{type, r.rest()};
  return DecodeStatus::OK;
}

bool message_crc_ok(const uint8_t *msg, size_t n) {
  if (n < MIN_MESSAGE_SIZE || n > MAX_MESSAGE_SIZE)
    return false;
  auto const wire_crc = static_cast<uint16_t>(msg[n - 2] | (msg[n - 1] << 8));
  return crc_ccitt(msg, static_cast<uint8_t>(n - 2)) == wire_crc;
}

// === Deframer ===

Deframer::Event Deframer::feed(uint8_t byte) {
  if (byte != 0) {
    // One byte short of MAX_WIRE_SIZE: the delimiter is the last of the 258.
    if (this->wire_len_ < MAX_WIRE_SIZE - 1) {
      this->wire_[this->wire_len_++] = byte;
    } else {
      this->overflow_ = true;
    }
    return Event::NONE;
  }
  Event const event = this->finish_frame_();
  this->wire_len_ = 0;
  this->overflow_ = false;
  return event;
}

Deframer::Event Deframer::finish_frame_() {
  if (this->overflow_) {
    this->counters_.bad_length++;
    return Event::DROPPED;
  }
  if (this->wire_len_ == 0)
    return Event::NONE;  // back-to-back delimiters carry nothing
  int const n = cobs_decode(this->wire_, this->wire_len_, this->msg_, sizeof(this->msg_));
  if (n == COBS_MALFORMED) {
    this->counters_.bad_cobs++;
    return Event::DROPPED;
  }
  if (n == COBS_TOO_LONG || std::cmp_less(n, MIN_MESSAGE_SIZE)) {
    this->counters_.bad_length++;
    return Event::DROPPED;
  }
  if (!message_crc_ok(this->msg_, static_cast<size_t>(n))) {
    this->counters_.bad_crc++;
    return Event::DROPPED;
  }
  this->msg_len_ = static_cast<size_t>(n);
  return Event::MESSAGE;
}

// === TLV lists ===

bool TlvReader::next(uint8_t &id, ByteSpan &value) {
  if (this->malformed_ || this->pos_ >= this->list_.size)
    return false;
  if (this->pos_ + 2 > this->list_.size) {
    this->malformed_ = true;
    return false;
  }
  id = this->list_.data[this->pos_];
  size_t const len = this->list_.data[this->pos_ + 1];
  if (this->pos_ + 2 + len > this->list_.size) {
    this->malformed_ = true;
    return false;
  }
  value = {this->list_.data + this->pos_ + 2, len};
  this->pos_ += 2 + len;
  return true;
}

bool tlv_list_valid(ByteSpan list) {
  TlvReader reader(list);
  uint8_t id;
  ByteSpan value;
  while (reader.next(id, value)) {
  }
  return !reader.malformed();
}

bool find_tlv(ByteSpan list, uint8_t id, ByteSpan &value) {
  TlvReader reader(list);
  uint8_t found;
  while (reader.next(found, value)) {
    if (found == id)
      return true;
  }
  return false;
}

TlvWriter &TlvWriter::add(uint8_t id, const uint8_t *value, size_t n) {
  if (!this->ok_ || n > UINT8_MAX || this->size_ + 2 + n > this->cap_) {
    this->ok_ = false;
    return *this;
  }
  this->buf_[this->size_++] = id;
  this->buf_[this->size_++] = static_cast<uint8_t>(n);
  if (n > 0)
    std::memcpy(this->buf_ + this->size_, value, n);
  this->size_ += n;
  return *this;
}

TlvWriter &TlvWriter::add_u8(uint8_t id, uint8_t value) { return this->add(id, &value, 1); }

TlvWriter &TlvWriter::add_u16(uint8_t id, uint16_t value) {
  const uint8_t le[] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
  return this->add(id, le, sizeof(le));
}

TlvWriter &TlvWriter::add_u32(uint8_t id, uint32_t value) {
  const uint8_t le[] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                        static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  return this->add(id, le, sizeof(le));
}

TlvWriter &TlvWriter::add_radio(const RadioDescriptor &radio) {
  uint8_t value[RADIO_DESCRIPTOR_BYTES];
  Writer w(value, sizeof(value));
  write_radio(w, radio);
  return this->add(static_cast<uint8_t>(InfoId::RADIO), value, w.size());
}

// === The ready descriptor ===

const char *ready_info_status_name(ReadyInfoStatus status) {
  switch (status) {
    case ReadyInfoStatus::OK:
      return "ok";
    case ReadyInfoStatus::BAD_TLV:
      return "a malformed info TLV";
    case ReadyInfoStatus::MISSING_FW_VERSION:
      return "fw_version";
    case ReadyInfoStatus::MISSING_RADIO:
      return "radio";
    case ReadyInfoStatus::MISSING_RESET_CAUSE:
      return "reset_cause";
  }
  return "?";
}

ReadyInfoStatus parse_ready_info(ByteSpan info, ReadyInfo &out) {
  out = ReadyInfo{};
  bool has_fw_version = false;
  bool has_reset_cause = false;
  TlvReader reader(info);
  uint8_t id;
  ByteSpan value;
  while (reader.next(id, value)) {
    Reader r(value.data, value.size);
    switch (static_cast<InfoId>(id)) {
      case InfoId::FW_VERSION:
        if (value.size > INFO_FW_VERSION_MAX_BYTES)
          return ReadyInfoStatus::BAD_TLV;
        out.fw_version = value;
        has_fw_version = true;
        break;
      case InfoId::BOARD:
        if (value.size > INFO_BOARD_MAX_BYTES)
          return ReadyInfoStatus::BAD_TLV;
        out.board = value;
        break;
      case InfoId::RADIO:
        if (value.size != RADIO_DESCRIPTOR_BYTES)
          return ReadyInfoStatus::BAD_TLV;
        if (out.radio_count < MAX_RADIOS)
          read_radio(r, out.radios[out.radio_count++]);
        break;
      case InfoId::RX_QUEUE_DEPTH:
        if (value.size != 1)
          return ReadyInfoStatus::BAD_TLV;
        out.rx_queue_depth = r.u8();
        out.has_rx_queue_depth = true;
        break;
      case InfoId::RESET_CAUSE:
        if (value.size != 1)
          return ReadyInfoStatus::BAD_TLV;
        out.reset_cause = static_cast<ResetCause>(r.u8());
        has_reset_cause = true;
        break;
      default:
        break;  // an info TLV this codec does not know is skipped
    }
  }
  if (reader.malformed())
    return ReadyInfoStatus::BAD_TLV;
  if (!has_fw_version)
    return ReadyInfoStatus::MISSING_FW_VERSION;
  if (out.radio_count == 0)
    return ReadyInfoStatus::MISSING_RADIO;
  if (!has_reset_cause)
    return ReadyInfoStatus::MISSING_RESET_CAUSE;
  return ReadyInfoStatus::OK;
}

bool is_link_takeover(const Ready &ready) {
  ReadyInfo info;
  return parse_ready_info(ready.info, info) == ReadyInfoStatus::OK && info.reset_cause == ResetCause::LINK_TAKEOVER;
}

}  // namespace esphome::home_io_control::bridle

#endif  // IOHOME_BRIDLE
