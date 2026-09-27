/// @file fuzz_soft_phy_rx.cpp
/// @brief libFuzzer target for the software-PHY RX decoder (SX1262/LR1121 receive path).
///
/// The input is a raw chip buffer: the bytes `SoftPhyDriverBase::read_rx_packet()` reads out of
/// the radio before any recovery, with unknown bit alignment and no frame boundaries. It is fed
/// through every pure stage that turns such a buffer into a frame: the length peek used for
/// length-driven receive, the per-offset UART decoder, and the CRC-validated probe whose result
/// the driver hands to the parser. Beyond crashes and sanitizer findings, the target checks each
/// stage's contract and traps on a violation, and it checks the PHY against itself: a frame the
/// probe recovered, re-encoded the way the TX path encodes it, must probe back to the same bytes.
/// Not part of `make check` — run via `make fuzz-soft-phy`, a time-boxed background check.

#include "proto_frame.h"
#include "proto_sizes.h"
#include "radio_soft_phy.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace esphome::home_io_control;

namespace {

/// Checks a contract the decoder states and aborts the run when it does not hold, so the fuzzer
/// reports a property violation the same way it reports a crash.
inline void require(bool condition) {
  if (!condition)
    __builtin_trap();
}

/// Re-encodes recovered frame bytes as the TX path does (SoftPhyDriverBase::send_packet():
/// append crc_ccitt() little-endian, then uart_encode_packet()) and probes the result again.
void check_round_trip(const uint8_t *frame, uint8_t frame_len) {
  uint8_t with_crc[FRAME_MAX_WIRE_SIZE] = {0};
  require(static_cast<size_t>(frame_len) + FRAME_CRC_SIZE <= sizeof(with_crc));
  std::memcpy(with_crc, frame, frame_len);
  const uint16_t crc = crc_ccitt(frame, frame_len);
  with_crc[frame_len] = crc & 0xFF;
  with_crc[frame_len + 1] = (crc >> 8) & 0xFF;

  uint8_t encoded[RADIO_PACKET_BUFFER_SIZE];
  const uint8_t encoded_len =
      uart_encode_packet(with_crc, static_cast<uint8_t>(frame_len + FRAME_CRC_SIZE), encoded, sizeof(encoded));
  require(encoded_len == soft_phy_raw_bytes_for_frame(frame_len));

  const UartProbeResult again = find_uart_probe(encoded, encoded_len);
  require(again.valid);
  require(again.bit_offset == 0 && again.frame_start == 0);
  require(again.frame_len == frame_len);
  require(std::memcmp(again.decoded, frame, frame_len) == 0);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  // read_rx_packet() never reads more than its RADIO_PACKET_BUFFER_SIZE buffer.
  if (size == 0 || size > RADIO_PACKET_BUFFER_SIZE)
    return 0;
  const auto raw_len = static_cast<uint8_t>(size);

  const uint8_t peeked = soft_phy_peek_frame_length(data, raw_len);
  require(peeked == 0 || (peeked >= FRAME_MIN_SIZE && peeked <= FRAME_MAX_SIZE));

  // Exact-size heap buffers, so ASan catches a write even one byte past decoded_max_len.
  for (uint8_t bit_offset = 0; bit_offset < UART_PROBE_MAX_BIT_OFFSET; bit_offset++) {
    for (const uint8_t max_len : {static_cast<uint8_t>(1), static_cast<uint8_t>(RADIO_PACKET_BUFFER_SIZE)}) {
      std::vector<uint8_t> decoded(max_len);
      const uint8_t decoded_len = decode_uart_probe(data, raw_len, bit_offset, decoded.data(), max_len);
      require(decoded_len <= max_len);
      // Every decoded byte costs a whole 10-bit cell from the input, counted from the offset.
      require(decoded_len == 0 ||
              static_cast<size_t>(bit_offset) + static_cast<size_t>(decoded_len) * UART_CELL_BITS <= size * 8);
    }
  }

  const UartProbeResult probe = find_uart_probe(data, raw_len);
  require(probe.decoded_len <= RADIO_PACKET_BUFFER_SIZE);
  if (!probe.valid)
    return 0;

  require(probe.bit_offset < UART_PROBE_MAX_BIT_OFFSET);
  require(probe.frame_len >= FRAME_MIN_SIZE && probe.frame_len <= FRAME_MAX_WIRE_SIZE);
  require(static_cast<size_t>(probe.frame_start) + probe.frame_len + FRAME_CRC_SIZE <= probe.decoded_len);

  const uint8_t *frame = probe.decoded + probe.frame_start;
  const uint16_t received_crc =
      static_cast<uint16_t>(frame[probe.frame_len]) | static_cast<uint16_t>(frame[probe.frame_len + 1] << 8);
  require(crc_ccitt(frame, probe.frame_len) == received_crc);

  // The probe only accepts a candidate that parses, so this is what the driver hands upward.
  IoFrame parsed{};
  require(parse(frame, probe.frame_len, parsed));

  check_round_trip(frame, probe.frame_len);
  return 0;
}
