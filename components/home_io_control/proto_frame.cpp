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
  // the radio path and later confuse the serializer or on-air retries.
  if (total > FRAME_MAX_SIZE)
    return false;
  f.ctrl0 = (f.ctrl0 & ~CTRL0_LENGTH_MASK) | ((total - 1) & CTRL0_LENGTH_MASK);
  return true;
}

uint8_t frame_length(const IoFrame &f) { return (f.ctrl0 & CTRL0_LENGTH_MASK) + 1; }
bool is_start(const IoFrame &f) { return (f.ctrl0 & CTRL0_START) != 0; }
bool is_end(const IoFrame &f) { return (f.ctrl0 & CTRL0_END) != 0; }

uint8_t serialize(const IoFrame &f, uint8_t *buf, uint8_t buf_size) {
  if (buf == nullptr)
    return 0;
  uint8_t const len = frame_length(f);
  if (len < FRAME_MIN_SIZE || len > FRAME_MAX_SIZE)
    return 0;
  if (f.data_len > FRAME_MAX_DATA_SIZE)
    return 0;
  // Keep the wire length derived from ctrl0 and the explicit payload length in lockstep. This
  // catches partially initialized frames before they are transmitted.
  if ((uint8_t) (FRAME_MIN_SIZE + f.data_len) != len)
    return 0;
  if (buf_size < len)
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
  if (len < FRAME_MIN_SIZE || len > FRAME_MAX_SIZE)
    return false;
  if (buf_len != len)
    return false;
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
  f.data_len = len - FRAME_MIN_SIZE;
  if (f.data_len > FRAME_MAX_DATA_SIZE)
    return false;
  if (offset + f.data_len > buf_len)
    return false;
  memcpy(f.data, &buf[offset], f.data_len);
  return true;
}

}  // namespace home_io_control
}  // namespace esphome
