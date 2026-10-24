/// @file fuzz_bridle_codec.cpp
/// @brief libFuzzer target for the Bridle codec (framing, message decode, TLV and `ready` parsing).
///
/// Every input takes two routes. As a byte stream it goes through the deframer, the way UART
/// bytes do; as a bare message (no COBS, no CRC to guess) it goes straight to the decoder, so
/// the fuzzer reaches every body layout without having to forge a checksum first. Beyond crashes
/// and sanitizer findings the target traps on a broken contract: a decoded message must encode
/// and decode again to a message that encodes to the same bytes, a TLV walk must stay inside its
/// list, and COBS must round-trip. Not part of `make check`; run via `make fuzz-bridle-codec`.

#include "radio_bridle_codec.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

using namespace esphome::home_io_control::bridle;

namespace {

/// Aborts the run when a stated contract does not hold, so it is reported like a crash.
inline void require(bool condition) {
  if (!condition)
    __builtin_trap();
}

/// A decoded message re-encodes, and that encoding is a fixed point of decode + encode.
void check_re_encoding(const uint8_t *msg, size_t n) {
  Message first;
  if (decode_message(msg, n, first) != DecodeStatus::OK)
    return;
  uint8_t once[MAX_MESSAGE_SIZE];
  size_t const once_len = encode_message(first.seq, first.body, once, sizeof(once));
  if (once_len == 0)
    return;  // a field the wire allows but the codec does not send (e.g. 17 scan frequencies)
  require(message_crc_ok(once, once_len));
  Message second;
  require(decode_message(once, once_len, second) == DecodeStatus::OK);
  uint8_t twice[MAX_MESSAGE_SIZE];
  size_t const twice_len = encode_message(second.seq, second.body, twice, sizeof(twice));
  require(twice_len == once_len && std::memcmp(once, twice, once_len) == 0);
}

/// Walks the TLV list a decoded message carries and the `ready` descriptor built from it.
void check_tlvs(const uint8_t *msg, size_t n) {
  Message m;
  if (decode_message(msg, n, m) != DecodeStatus::OK)
    return;
  ByteSpan list{};
  if (const auto *ready = std::get_if<Ready>(&m.body))
    list = ready->info;
  else if (const auto *set = std::get_if<SetParams>(&m.body))
    list = set->tlvs;
  else if (const auto *ack = std::get_if<ParamsAck>(&m.body))
    list = ack->applied;
  else
    return;
  require(tlv_list_valid(list));  // decode_message accepted it, so it must be walkable
  TlvReader reader(list);
  uint8_t id;
  ByteSpan value;
  size_t consumed = 0;
  while (reader.next(id, value)) {
    require(value.data >= list.data && value.data + value.size <= list.data + list.size);
    consumed += 2 + value.size;
  }
  require(consumed == list.size && !reader.malformed());
  ReadyInfo info;
  (void) parse_ready_info(list, info);
}

/// COBS encodes any short input without zeros and decodes it back.
void check_cobs_round_trip(const uint8_t *data, size_t n) {
  if (n > MAX_MESSAGE_SIZE)
    return;
  uint8_t enc[MAX_WIRE_SIZE];
  size_t const e = cobs_encode(data, n, enc, sizeof(enc));
  require(e > 0);
  for (size_t i = 0; i < e; i++)
    require(enc[i] != 0);
  uint8_t dec[MAX_WIRE_SIZE];
  int const d = cobs_decode(enc, e, dec, sizeof(dec));
  require(d == static_cast<int>(n) && (n == 0 || std::memcmp(dec, data, n) == 0));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  Deframer deframer;
  for (size_t i = 0; i < size; i++) {
    if (deframer.feed(data[i]) == Deframer::Event::MESSAGE) {
      require(message_crc_ok(deframer.message(), deframer.message_size()));
      check_re_encoding(deframer.message(), deframer.message_size());
      check_tlvs(deframer.message(), deframer.message_size());
    }
  }
  check_re_encoding(data, size);
  check_tlvs(data, size);
  check_cobs_round_trip(data, size);
  return 0;
}
