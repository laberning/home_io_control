/// @file radio_soft_phy.cpp
/// @brief Software PHY implementation for radios without IoHomeOn hardware framing.
/// @ingroup hioc_radio

// Line-coding widths and recovery thresholds are written in the same shape as the on-air
// framing they reproduce.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

#include "radio_soft_phy.h"

#include "proto_constants.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace esphome {
namespace home_io_control {

/// Maximum bit offset to search for valid UART decode start position.
/// The UART frame is 10 bits (start + 8 data). If the sync word is not aligned,
/// we probe up to 10 bits offset to recover the correct framing.
static const uint8_t UART_PROBE_MAX_BIT_OFFSET = 10;

namespace {

/// Extract a single bit (MSB‑first) from a byte buffer.
/// Used by UART decoding to scan raw radio samples.
/// @param data Input byte buffer.
/// @param bit_pos Global bit index within buffer.
/// @return The bit value (0 or 1).
uint8_t get_bit_msb(const uint8_t *data, uint16_t bit_pos) { return (data[bit_pos / 8] >> (7 - (bit_pos % 8))) & 0x01; }

}  // namespace

/// Decode a raw UART‑encoded bitstream into bytes.
/// IO‑Homecontrol uses a UART‑like encoding over the air: each byte is represented
/// by a 10‑bit sequence (start bit 0, 8 data bits LSB‑first, stop bit 1). This
/// function slides a window across the raw bitstream and attempts to recover the
/// original bytes. It stops when the sync pattern (0 followed by 1) is not found.
/// @param raw Raw bytes from the radio buffer.
/// @param raw_len Number of raw bytes available.
/// @param bit_offset Initial bit position to start decoding (probe offset).
/// @param decoded Output buffer for decoded bytes.
/// @param decoded_max_len Capacity of decoded buffer.
/// @return Number of bytes successfully decoded.
uint8_t decode_uart_probe(const uint8_t *raw, uint8_t raw_len, uint8_t bit_offset, uint8_t *decoded,
                          uint8_t decoded_max_len) {
  // Bit numbering: we read MSB-first across byte boundaries. The UART frame structure
  // within the bitstream is: start(0), data0, data1, ..., data7, stop(1). Byte values
  // are LSB-first within the 8 data bits (bit 0 arrives first after start).
  // We verify the start bit is 0 and stop bit is 1; if not, the probe offset is wrong.
  uint16_t bit_pos = bit_offset;
  uint16_t const total_bits = raw_len * 8;
  uint8_t decoded_len = 0;

  while (bit_pos + 10 <= total_bits && decoded_len < decoded_max_len) {
    if (get_bit_msb(raw, bit_pos) != 0 || get_bit_msb(raw, bit_pos + 9) != 1)
      break;

    uint8_t value = 0;
    for (uint8_t index = 0; index < 8; index++)
      value |= get_bit_msb(raw, bit_pos + 1 + index) << index;

    decoded[decoded_len++] = value;
    bit_pos += 10;
  }

  return decoded_len;
}

/// This list missing CMD_GET_GENERAL_INFO3_RESP (0x59) is exactly what turned a real Q2 probe
/// reply into a false "no reply" timeout on real hardware (2026-08-16); the same audit also found
/// CMD_IDENTIFY and CMD_WRITE_PRIVATE/CMD_WRITE_PRIVATE_ACK missing, unrelated to that probe but
/// affecting the already-shipped identify_device() action (and climate writes) on SX1262/LR1121.
/// Add every new opcode this codebase sends a request for, or expects a reply to, here as well as
/// in proto_constants.h. Deliberately
/// excludes CMD_UNKNOWN4A_REQ (0x4A, see ADR 0024) — recognizing a *received* 0x4A would not
/// violate the "never transmitted" rule, but nothing in this codebase currently sends anything
/// that would draw one, so there is no exchange for it to unblock; CMD_UNKNOWN4A_RESP (0x4B) is
/// included because it is a plausible reply to CMD_GET_GENERAL_INFO3 (0x58), which this codebase
/// does send. Declared in radio_soft_phy.h so tests can enumerate every accepted command directly.
bool is_known_io_command(uint8_t cmd) {
  switch (cmd) {
    case CMD_EXECUTE:
    case CMD_PRIVATE:
    case CMD_PRIVATE_RESP:
    case CMD_PRIVATE2:
    case CMD_PRIVATE2_RESP:
    case CMD_IDENTIFY:
    case CMD_WRITE_PRIVATE:
    case CMD_WRITE_PRIVATE_ACK:
    case CMD_DISCOVER_REQ:
    case CMD_DISCOVER_RESP:
    case CMD_DISCOVER_SPE_REQ:
    case CMD_DISCOVER_SPE_RESP:
    case CMD_DISCOVER_CONFIRM:
    case CMD_DISCOVER_CONFIRM_ACK:
    case CMD_KEY_INIT:
    case CMD_KEY_TRANSFER:
    case CMD_KEY_CONFIRM:
    case CMD_CHALLENGE_REQ:
    case CMD_CHALLENGE_RESP:
    case CMD_UNKNOWN4A_RESP:
    case CMD_GET_NAME:
    case CMD_GET_NAME_RESP:
    case CMD_SET_NAME:
    case CMD_SET_NAME_RESP:
    case CMD_GET_INFO2:
    case CMD_GET_INFO2_RESP:
    case CMD_GET_GENERAL_INFO3:
    case CMD_GET_GENERAL_INFO3_RESP:
    case CMD_SET_CONFIG1:
    case CMD_SET_CONFIG1_RESP:
    case CMD_STATUS_UPDATE:
    case CMD_STATUS_UPDATE_RESP:
    case CMD_ERROR_RESP:
      return true;
    default:
      return false;
  }
}

namespace {

/// @brief Check if a UART-decoded frame is plausible as an IO-Homecontrol packet.
/// Accepts frames at or above FRAME_MIN_SIZE (9 bytes) that contain a known command
/// or have the 1W protocol bit set. This allows short frames like CMD_KEY_CONFIRM (9 bytes)
/// and CMD_ERROR_RESP (10 bytes) to pass through when CRC validates.
/// @param frame Parsed IoFrame candidate.
/// @param candidate_len Total decoded length of the candidate.
/// @return true if the frame looks like a real protocol packet.
bool is_plausible_uart_frame(const IoFrame &frame, uint8_t candidate_len) {
  if (candidate_len < FRAME_MIN_SIZE)
    return false;
  if (is_known_io_command(frame.cmd))
    return true;
  return (frame.ctrl0 & CTRL0_PROTOCOL_1W) != 0;
}

}  // namespace

/// @brief Try to find a CRC-valid IO-Homecontrol frame within a decoded UART byte stream.
/// @param decoded Decoded byte buffer from UART probe.
/// @param decoded_len Number of decoded bytes.
/// @return Frame start index and length if found, or {0, 0} if no valid frame.
static std::pair<uint8_t, uint8_t> find_crc_valid_frame(const uint8_t *decoded, uint8_t decoded_len) {
  for (uint8_t start = 0; start < decoded_len; start++) {
    const uint8_t max_candidate_len = std::min<uint8_t>(decoded_len - start, FRAME_MAX_SIZE);
    for (uint8_t candidate_len = max_candidate_len; candidate_len >= FRAME_MIN_SIZE; candidate_len--) {
      IoFrame frame;
      if (!parse(decoded + start, candidate_len, frame))
        continue;
      if (!is_plausible_uart_frame(frame, candidate_len))
        continue;
      if (start + candidate_len + 2 > decoded_len)
        continue;
      const uint16_t computed_crc = crc_ccitt(decoded + start, candidate_len);
      const uint16_t received_crc =
          (uint16_t) decoded[start + candidate_len] | ((uint16_t) decoded[start + candidate_len + 1] << 8);
      if (computed_crc != received_crc)
        continue;
      return {start, candidate_len};
    }
  }
  return {0, 0};
}

UartProbeResult find_uart_probe(const uint8_t *raw, uint8_t raw_len) {
  // The raw RX buffer contains the demodulated bits packed as bytes. Due to unknown bit
  // alignment, we probe up to UART_PROBE_MAX_BIT_OFFSET (10) different starting positions.
  // For each offset we attempt UART decoding; if decoding yields a plausible frame length
  // (>= minimum) and contains a known command ID or indicates a 1W frame, we keep it as a
  // candidate. CRC-CCITT validation is used as the primary selection criterion: a frame that
  // passes CRC is preferred over one that merely parses. This rejects frames corrupted by
  // demodulator bit errors after TX→RX transitions.
  UartProbeResult best{};

  for (uint8_t bit_offset = 0; bit_offset < UART_PROBE_MAX_BIT_OFFSET; bit_offset++) {
    uint8_t decoded[RADIO_PACKET_BUFFER_SIZE] = {0};
    uint8_t const decoded_len = decode_uart_probe(raw, raw_len, bit_offset, decoded, sizeof(decoded));
    if (decoded_len == 0)
      continue;

    if (decoded_len > best.decoded_len && !best.valid) {
      best.bit_offset = bit_offset;
      best.decoded_len = decoded_len;
      memcpy(best.decoded, decoded, decoded_len);
    }

    auto [frame_start, frame_len] = find_crc_valid_frame(decoded, decoded_len);
    if (frame_len > 0) {
      best.valid = true;
      best.bit_offset = bit_offset;
      best.decoded_len = decoded_len;
      best.frame_start = frame_start;
      best.frame_len = frame_len;
      memcpy(best.decoded, decoded, decoded_len);
      return best;
    }
  }

  return best;
}

// === Length-driven receive helpers ===

uint8_t soft_phy_raw_bytes_for_frame(uint8_t frame_len) {
  // The CRC is appended before UART packing (see SoftPhyDriverBase::send_packet), so it occupies
  // two cells of its own.
  const uint16_t cells = (uint16_t) frame_len + FRAME_CRC_SIZE;
  const uint16_t bits = cells * UART_CELL_BITS;
  return (uint8_t) ((bits + 7) / 8);
}

uint8_t soft_phy_peek_frame_length(const uint8_t *raw, uint8_t raw_len) {
  uint8_t best = 0;
  for (uint8_t bit_offset = 0; bit_offset < UART_PROBE_MAX_BIT_OFFSET; bit_offset++) {
    uint8_t ctrl0 = 0;
    if (decode_uart_probe(raw, raw_len, bit_offset, &ctrl0, 1) != 1)
      continue;  // start/stop bits don't frame here — wrong alignment
    const auto frame_len = (uint8_t) ((ctrl0 & CTRL0_LENGTH_MASK) + 1);
    if (frame_len < FRAME_MIN_SIZE || frame_len > FRAME_MAX_SIZE)
      continue;
    best = std::max(frame_len, best);
  }
  return best;
}

// === Software UART encode (TX) ===

uint8_t uart_encode_packet(const uint8_t *data, uint8_t len, uint8_t *encoded, uint8_t encoded_max_len) {
  if (len == 0 || encoded_max_len == 0)
    return 0;

  memset(encoded, 0, encoded_max_len);
  uint16_t bit_pos = 0;
  const uint16_t total_bits = len * 10;
  if (((total_bits + 7) / 8) > encoded_max_len)
    return 0;

  auto write_bit = [encoded](uint16_t pos, uint8_t bit) {
    if (bit != 0)
      encoded[pos / 8] |= 1U << (7 - (pos % 8));
  };

  for (uint8_t byte_index = 0; byte_index < len; byte_index++) {
    const uint8_t value = data[byte_index];

    write_bit(bit_pos++, 0);  // UART start bit
    for (uint8_t bit_index = 0; bit_index < 8; bit_index++)
      write_bit(bit_pos++, (value >> bit_index) & 0x01);
    write_bit(bit_pos++, 1);  // UART stop bit
  }

  // A 10-bit cell only lands on a byte boundary every fourth byte, so the last byte of the buffer
  // is usually part data, part leftover — and the chip transmits it whole either way. Those
  // leftover bits are line-idle time, and a UART line idles *high*: zero-filling them puts what
  // looks like a start bit on air immediately after the frame's last stop bit. The SX1276's
  // IoHomeOn coder, which this software PHY exists to reproduce, never emits that. Pad with ones.
  const uint8_t encoded_len = (total_bits + 7) / 8;
  for (uint16_t pad_pos = total_bits; pad_pos < (uint16_t) encoded_len * 8; pad_pos++)
    write_bit(pad_pos, 1);

  return encoded_len;
}

}  // namespace home_io_control
}  // namespace esphome

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
