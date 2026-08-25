#include "radio_soft_phy.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>

using namespace esphome::home_io_control;

namespace {

// Extracts `out_bytes` bytes worth of bits from `buf`, MSB-first, starting at `bit_offset` —
// mirrors radio_soft_phy.cpp's private get_bit_msb() bit numbering, but as raw extraction with no
// UART start/stop framing assumed (unlike decode_uart_probe()). Test-only: scans a UART-encoded
// buffer at every one of the 10 possible bit offsets within a cell, to find which offset (if any)
// reproduces a target byte sequence.
void extract_bits(const uint8_t *buf, uint16_t bit_offset, uint8_t *out, uint8_t out_bytes) {
  for (uint8_t i = 0; i < out_bytes; i++) {
    uint8_t byte = 0;
    for (uint8_t b = 0; b < 8; b++) {
      uint16_t const pos = bit_offset + i * 8 + b;
      uint8_t const bit = (buf[pos / 8] >> (7 - (pos % 8))) & 0x01;
      byte = static_cast<uint8_t>((byte << 1) | bit);
    }
    out[i] = byte;
  }
}

}  // namespace

// Where did the SX1262/LR1121 sync-word register value 57 FD 99 come from? SX1276 (hardware
// IoHomeOn, known good) uses the raw bytes 55 FF 33. This test uses the codebase's own
// uart_encode_packet() — the same software UART encoder used for every soft-PHY TX frame — as the
// oracle: encode {0x55, 0xFF, 0x33} and check whether any bit offset of the resulting stream
// reproduces 57 FD 99.
//
// Finding: it does, at exactly bit offset 6, and nowhere else in the 10-bit cell. 57 FD 99 is not
// an undocumented guess — it is what a receiver bit-synchronized 6 bits into the first UART cell
// (rather than on the cell's start bit) would see as the 24 on-air bits following SX1276's own
// raw sync bytes. That is a real, reproducible alignment (the soft-PHY RX path already searches
// all 10 offsets for this exact reason — see find_uart_probe()), not an arbitrary constant.
TEST(SyncWordDerivation, MatchesSx1276SyncEncodedAtBitOffsetSix) {
  const uint8_t sx1276_sync[3] = {0x55, 0xFF, 0x33};
  const uint8_t sx1262_register[3] = {0x57, 0xFD, 0x99};

  uint8_t encoded[8] = {0};
  uint8_t const encoded_len = uart_encode_packet(sx1276_sync, 3, encoded, sizeof(encoded));
  ASSERT_GT(encoded_len, 0);

  int match_offset = -1;
  for (uint8_t offset = 0; offset < UART_CELL_BITS; offset++) {
    uint8_t window[3];
    extract_bits(encoded, offset, window, 3);
    if (memcmp(window, sx1262_register, 3) == 0) {
      EXPECT_EQ(match_offset, -1) << "unexpected second match at offset " << static_cast<int>(offset)
                                  << " (first match was at " << match_offset << ")";
      match_offset = offset;
    }
  }

  EXPECT_EQ(match_offset, 6);
}

// Companion check: decode_uart_probe() (the RX-side oracle, i.e. "does this bit pattern look like
// a UART cell") does not, at any offset, decode 57 FD 99 back into 55 FF 33. The match found above
// only exists in the encode direction, confirming 57 FD 99 is the *product* of UART-encoding
// 55 FF 33, not an independently UART-framed value in its own right that happens to decode back to
// it.
TEST(SyncWordDerivation, RegisterValueDoesNotItselfDecodeAsUart) {
  const uint8_t sx1262_register[8] = {0x57, 0xFD, 0x99, 0x00, 0x00, 0x00, 0x00, 0x00};
  uint8_t decoded[8];

  for (uint8_t offset = 0; offset < UART_CELL_BITS; offset++) {
    uint8_t const decoded_len =
        decode_uart_probe(sx1262_register, sizeof(sx1262_register), offset, decoded, sizeof(decoded));
    if (decoded_len == 0)
      continue;
    // A spurious short decode (1-2 bytes) is possible by chance; a clean {0x55, 0xFF, 0x33} is not.
    bool full_match = decoded_len >= 3 && memcmp(decoded, "\x55\xFF\x33", 3) == 0;
    EXPECT_FALSE(full_match) << "offset " << static_cast<int>(offset) << " decoded back to 55 FF 33";
  }
}
