/// @file proto_frame.cpp
/// @brief IO-Homecontrol 2W frame container implementation.
/// @ingroup hioc_protocol

#include "proto_frame.h"

#include "proto_constants.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace home_io_control {

namespace {

constexpr int HEX_ALPHA_OFFSET = 10;

}  // namespace

static int hex_nibble(char ch) {
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  if (ch >= 'A' && ch <= 'F')
    return HEX_ALPHA_OFFSET + (ch - 'A');
  return -1;
}

bool hex_to_bytes(const std::string &hex, uint8_t *out, uint8_t len) {
  if (out == nullptr)
    return false;

  memset(out, 0, len);
  if (hex.length() != static_cast<size_t>(len) * 2)
    return false;

  for (uint8_t i = 0; i < len; i++) {
    const int high = hex_nibble(hex[i * 2]);
    const int low = hex_nibble(hex[(i * 2) + 1]);
    if (high < 0 || low < 0)
      return false;
    out[i] = static_cast<uint8_t>((high << 4) | low);
  }

  return true;
}

std::string node_id_to_string(const uint8_t id[NODE_ID_SIZE]) {
  char buf[NODE_ID_STRING_SIZE];
  snprintf(buf, sizeof(buf), "%02X%02X%02X", id[0], id[1], id[2]);
  return std::string(buf);
}

/// CRC-CCITT used by the IO-Homecontrol protocol for frame validation.
/// Polynomial: 0x1021 (reversed 0x8408), initial value: 0x0000.
/// Radio chips with native IO-Homecontrol framing compute this in hardware;
/// drivers for other chips call this helper instead.
uint16_t crc_ccitt(const uint8_t *data, uint8_t len) {
  uint16_t crc = 0x0000;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < BITS_PER_BYTE; j++)
      crc = ((crc & CRC_LSB_MASK) != 0) ? (crc >> 1) ^ CRC_POLYNOMIAL_REVERSED : crc >> 1;
  }
  return crc;
}

void init_frame(IoFrame &f, bool is_2w, bool start, bool end, bool low_power) {
  memset(&f, 0, sizeof(IoFrame));
  if (end)
    f.ctrl0 |= CTRL0_END;
  if (start)
    f.ctrl0 |= CTRL0_START;
  if (!is_2w)
    f.ctrl0 |= CTRL0_PROTOCOL_1W;
  if (low_power)
    f.ctrl1 |= CTRL1_LOW_POWER;
}

void set_dst(IoFrame &f, const uint8_t id[NODE_ID_SIZE]) { memcpy(f.dst, id, NODE_ID_SIZE); }
void set_src(IoFrame &f, const uint8_t id[NODE_ID_SIZE]) { memcpy(f.src, id, NODE_ID_SIZE); }

bool set_cmd(IoFrame &f, uint8_t cmd, const uint8_t *params, uint8_t params_len) {
  if (params_len > FRAME_MAX_DATA_SIZE)
    return false;
  f.cmd = cmd;
  f.data_len = params_len;
  if (params != nullptr && params_len > 0)
    memcpy(f.data, params, params_len);
  uint8_t const total = FRAME_MIN_SIZE + f.data_len;
  // Refuse to encode inconsistent frame metadata here so malformed commands never make it onto
  // the radio path and later confuse the serializer or on-air retries. This is a declared-length
  // guard: CTRL0's 5-bit length field cannot describe more than FRAME_MAX_DECLARED_SIZE bytes,
  // regardless of any out-of-length trailer a frame might separately carry (see IoFrame::has_mac).
  if (total > FRAME_MAX_DECLARED_SIZE)
    return false;
  f.ctrl0 = (f.ctrl0 & ~CTRL0_LENGTH_MASK) | ((total - 1) & CTRL0_LENGTH_MASK);
  return true;
}

uint8_t frame_length(const IoFrame &f) { return (f.ctrl0 & CTRL0_LENGTH_MASK) + 1; }
bool is_start(const IoFrame &f) { return (f.ctrl0 & CTRL0_START) != 0; }
bool is_end(const IoFrame &f) { return (f.ctrl0 & CTRL0_END) != 0; }

bool frame_carries_mac_trailer(uint8_t cmd) {
  // Only CMD_ONEWAY_ADD_CONTROLLER's declared payload plus its 6-byte MAC overflows CTRL0's
  // 5-bit length field (see that constant's Doxygen in proto_constants.h) — every other command
  // this project models keeps its authenticator, if any, inside the declared length.
  return cmd == CMD_ONEWAY_ADD_CONTROLLER;
}

uint8_t serialize(const IoFrame &f, uint8_t *buf, uint8_t buf_size) {
  if (buf == nullptr)
    return 0;
  uint8_t const len = frame_length(f);
  if (len < FRAME_MIN_SIZE || len > FRAME_MAX_DECLARED_SIZE)
    return 0;
  if (f.data_len > FRAME_MAX_DATA_SIZE)
    return 0;
  // Keep the wire length derived from ctrl0 and the explicit payload length in lockstep. This
  // catches partially initialized frames before they are transmitted.
  if ((uint8_t) (FRAME_MIN_SIZE + f.data_len) != len)
    return 0;
  // Keep serialize() and parse() agreeing on which commands may carry a trailer at all, so this
  // function can never emit a frame parse() would then reject. Without it a caller could set
  // has_mac on any command and produce 6 trailing bytes no receiver would read back.
  if (f.has_mac && !frame_carries_mac_trailer(f.cmd))
    return 0;
  // The MAC trailer (when present) rides after the declared length and is counted in the
  // returned length, so a caller's CRC — computed over serialize()'s return value, not a
  // separately recomputed frame_length() — covers it too.
  uint8_t const total_len = f.has_mac ? (uint8_t) (len + HMAC_SIZE) : len;
  if (buf_size < total_len)
    return 0;
  uint8_t offset = 0;
  buf[offset++] = f.ctrl0;
  buf[offset++] = f.ctrl1;
  memcpy(&buf[offset], f.dst, NODE_ID_SIZE);
  offset += NODE_ID_SIZE;
  memcpy(&buf[offset], f.src, NODE_ID_SIZE);
  offset += NODE_ID_SIZE;
  buf[offset++] = f.cmd;
  memcpy(&buf[offset], f.data, f.data_len);
  offset += f.data_len;
  if (f.has_mac) {
    memcpy(&buf[offset], f.mac, HMAC_SIZE);
    offset += HMAC_SIZE;
  }
  return offset;
}

bool parse(const uint8_t *buf, uint8_t buf_len, IoFrame &f) {
  if (buf == nullptr)
    return false;
  if (buf_len < FRAME_MIN_SIZE)
    return false;
  memset(&f, 0, sizeof(IoFrame));
  uint8_t offset = 0;
  f.ctrl0 = buf[offset++];
  f.ctrl1 = buf[offset++];
  uint8_t const len = frame_length(f);
  if (len < FRAME_MIN_SIZE || len > FRAME_MAX_DECLARED_SIZE)
    return false;
  // buf_len must be either exactly the CTRL0-declared length (the common case, no trailer), or
  // that length plus the out-of-length MAC trailer (see IoFrame::has_mac) — any other length
  // means buf isn't this frame at all, not a frame with an unusual trailer size. The wider shape
  // is additionally gated on the command byte (frame_carries_mac_trailer()): without that gate,
  // any ordinary frame arriving with 6 extra trailing bytes for whatever reason would also match
  // this shape and be accepted as a fabricated MAC trailer, with only a ~1-in-65536 CRC check
  // downstream to catch it. Reading buf[FRAME_CMD_OFFSET] here is safe — buf_len is already
  // known to be >= FRAME_MIN_SIZE (== FRAME_CMD_OFFSET + 1) from the guard above.
  if (buf_len == len) {
    f.has_mac = false;
  } else if (buf_len == (uint8_t) (len + HMAC_SIZE) && frame_carries_mac_trailer(buf[FRAME_CMD_OFFSET])) {
    f.has_mac = true;
  } else {
    return false;
  }
  if (offset + NODE_ID_SIZE > buf_len)
    return false;
  memcpy(f.dst, &buf[offset], NODE_ID_SIZE);
  offset += NODE_ID_SIZE;
  if (offset + NODE_ID_SIZE > buf_len)
    return false;
  memcpy(f.src, &buf[offset], NODE_ID_SIZE);
  offset += NODE_ID_SIZE;
  if (offset >= buf_len)
    return false;
  f.cmd = buf[offset++];
  // data_len is always derived from the declared length alone — the trailer is never part of
  // data[], so FRAME_MIN_SIZE + data_len == frame_length(f) holds whether or not has_mac is set.
  f.data_len = len - FRAME_MIN_SIZE;
  if (f.data_len > FRAME_MAX_DATA_SIZE)
    return false;
  if (offset + f.data_len > buf_len)
    return false;
  memcpy(f.data, &buf[offset], f.data_len);
  offset += f.data_len;
  if (f.has_mac)
    memcpy(f.mac, &buf[offset], HMAC_SIZE);
  return true;
}

}  // namespace home_io_control
}  // namespace esphome
