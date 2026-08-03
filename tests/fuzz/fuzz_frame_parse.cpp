/// @file fuzz_frame_parse.cpp
/// @brief libFuzzer target for the protocol frame parser and its downstream decoders.
///
/// Feeds raw bytes through parse() (the entry point every radio driver's RX path hands
/// chip-derived bytes to) and, on a successful parse, exercises the pure decoders that consume
/// the resulting IoFrame: 1W classification/decode, packed device-type/subtype, and
/// position/tilt report decoding. All of these are ESPHome-free (parser + protocol model only),
/// so this binary links without any ESPHome stubs. Not part of `make check` — run via
/// `make fuzz-frame`, a time-boxed background check, not a per-change gate.

#include "proto_codecs.h"
#include "proto_device_model.h"
#include "proto_frame.h"

#include <cstddef>
#include <cstdint>

using namespace esphome::home_io_control;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size == 0 || size > 64)  // Wire frames are 9-32 bytes; leave headroom, cap cheap.
    return 0;

  IoFrame f{};
  if (!parse(data, static_cast<uint8_t>(size), f))
    return 0;

  // Address classification and 1W decode never depend on data_len (src/dst are fixed 3-byte
  // fields), so these are always safe to exercise on any parsed frame.
  (void) classify_address(f.dst);
  (void) broadcast_target_type(f.dst);
  (void) decode_1w_frame(f);

  // Packed device-type/subtype and position/tilt decoders take scalar bytes, not buffers, so
  // they can't overrun regardless of data_len — but gate on the same minimum lengths the real
  // RX path (hub_status.cpp) uses before touching these offsets, so the fuzzer explores the
  // same "plausible frame" shapes production actually decodes rather than decoding noise.
  if (f.data_len >= 2) {
    (void) decode_packed_device_type(f.data[0], f.data[1]);
    (void) decode_packed_device_subtype(f.data[1]);
    uint16_t const tilt_raw = (static_cast<uint16_t>(f.data[0]) << 8) | f.data[1];
    (void) decode_tilt_report(tilt_raw);
  }
  if (f.data_len >= 4) {
    uint16_t const target_raw = (static_cast<uint16_t>(f.data[0]) << 8) | f.data[1];
    uint16_t const current_raw = (static_cast<uint16_t>(f.data[2]) << 8) | f.data[3];
    float target = 0.0F;
    float position = 0.0F;
    decode_position_report(target_raw, current_raw, false, target, position);
  }

  // Re-serialize to exercise the buffer-bounds path on the parser's own output. Not asserting
  // round-trip equality here — that's the golden-frame corpus's job on real captures, not a
  // fuzz-target concern.
  uint8_t out[FRAME_MAX_SIZE];
  (void) serialize(f, out, sizeof(out));

  return 0;
}
