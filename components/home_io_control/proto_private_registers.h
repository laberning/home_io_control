#pragma once

/// @file proto_private_registers.h
/// @brief Transport of the private register protocol: CMD_WRITE_PRIVATE (0x20) requests and their echoing replies.
/// @ingroup hioc_protocol
///
/// Heaters (proto_heating.h) and domestic hot water heaters (proto_dhw.h) read and write 16-bit
/// registers with the same four-byte header, `0C <direction> <register high> <register low>`, and
/// the device answers with CMD_WRITE_PRIVATE_ACK (0x21) echoing that header before any value. This
/// module owns that header, the read request and the echo check; which registers exist and how
/// their values are encoded stays with each device family. It is pure: it touches no device record
/// and no radio; proto_commands.h's create_write_private() frames a payload.

#include "proto_frame.h"

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace home_io_control {

/// @brief Largest request payload: the header plus a 16-bit value.
constexpr size_t PRIVATE_REGISTER_PAYLOAD_MAX_SIZE = 6;

/// @brief Length of the request header: prefix, direction, register high byte, register low byte.
constexpr size_t PRIVATE_REGISTER_HEADER_SIZE = 4;

/// @brief Leading payload byte, common to every request. Its meaning is unconfirmed; do not read a
/// semantic (for example "device class") into it.
constexpr uint8_t PRIVATE_REGISTER_PREFIX = 0x0C;

/// @brief Payload byte at index 1 of a read ("get") request.
constexpr uint8_t PRIVATE_REGISTER_DIRECTION_GET = 0x60;

/// @brief Payload byte at index 1 of a write ("set") request.
constexpr uint8_t PRIVATE_REGISTER_DIRECTION_SET = 0x61;

/// @brief Offset of the first value byte in a register reply: the four header bytes it echoes.
constexpr size_t PRIVATE_REGISTER_VALUE_OFFSET = PRIVATE_REGISTER_HEADER_SIZE;

/// @brief Write the four-byte request header.
/// @param direction PRIVATE_REGISTER_DIRECTION_GET or PRIVATE_REGISTER_DIRECTION_SET.
/// @param reg_hi High byte of the 16-bit register number.
/// @param reg_lo Low byte of the 16-bit register number.
/// @param out Output buffer of at least PRIVATE_REGISTER_HEADER_SIZE bytes.
void write_private_register_header(uint8_t direction, uint8_t reg_hi, uint8_t reg_lo, uint8_t *out);

/// @brief Encode a read ("get") of one register into a CMD_WRITE_PRIVATE (0x20) payload.
///
/// The request is always `0C 60 <reg_hi> <reg_lo>`: the direction byte is
/// PRIVATE_REGISTER_DIRECTION_GET and this function has no parameter that could change it, so the
/// write direction cannot be produced from here for any input. The diagnostic register probes use
/// it, so it is where "read-only" is enforced.
/// \todo Register family 0x00xx is untested on hardware for heaters. Family 0x01xx is the one
///       encode_heating_read() already sends to Atlantic radiators.
/// @param reg_hi High byte of the 16-bit register number.
/// @param reg_lo Low byte of the 16-bit register number.
/// @param out Output buffer of PRIVATE_REGISTER_PAYLOAD_MAX_SIZE bytes.
/// @return Always PRIVATE_REGISTER_HEADER_SIZE.
size_t encode_private_register_read(uint8_t reg_hi, uint8_t reg_lo, uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE]);

/// @brief Is this frame a register reply that echoes a request for this register?
/// @details A CMD_WRITE_PRIVATE_ACK whose data starts with `0C 60 <reg_hi> <reg_lo>` (a read) or
///          `0C 61 <reg_hi> <reg_lo>` (a write). The value bytes, if any, follow at
///          PRIVATE_REGISTER_VALUE_OFFSET; a register the device does not hold is answered with none.
/// @param reply The device's reply frame.
/// @param reg_hi High byte of the register the request addressed.
/// @param reg_lo Low byte of the register the request addressed.
/// @return true when the reply is the register's echo.
bool private_reply_echoes_register(const IoFrame &reply, uint8_t reg_hi, uint8_t reg_lo);

}  // namespace home_io_control
}  // namespace esphome
