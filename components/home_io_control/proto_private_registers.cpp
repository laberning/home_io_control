/// @file proto_private_registers.cpp
/// @brief Header writer, read request and echo check of the private register protocol.
/// @ingroup hioc_protocol

#include "proto_private_registers.h"

#include "proto_constants.h"

namespace esphome {
namespace home_io_control {

void write_private_register_header(uint8_t direction, uint8_t reg_hi, uint8_t reg_lo, uint8_t *out) {
  out[0] = PRIVATE_REGISTER_PREFIX;
  out[1] = direction;
  out[2] = reg_hi;
  out[3] = reg_lo;
}

size_t encode_private_register_read(uint8_t reg_hi, uint8_t reg_lo, uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE]) {
  write_private_register_header(PRIVATE_REGISTER_DIRECTION_GET, reg_hi, reg_lo, out);
  return PRIVATE_REGISTER_HEADER_SIZE;
}

bool private_reply_echoes_register(const IoFrame &reply, uint8_t reg_hi, uint8_t reg_lo) {
  if (reply.cmd != CMD_WRITE_PRIVATE_ACK || reply.data_len < PRIVATE_REGISTER_VALUE_OFFSET)
    return false;
  const uint8_t *data = reply.data;
  if (data[0] != PRIVATE_REGISTER_PREFIX || data[2] != reg_hi || data[3] != reg_lo)
    return false;
  return data[1] == PRIVATE_REGISTER_DIRECTION_GET || data[1] == PRIVATE_REGISTER_DIRECTION_SET;
}

}  // namespace home_io_control
}  // namespace esphome
