#pragma once

/// @file proto_sizes.h
/// @brief Fundamental IO-Homecontrol frame and crypto size constants.
/// @ingroup hioc_protocol
///
/// These sizes are the lowest layer of the protocol model: node-ID widths,
/// AES/HMAC sizes and frame bounds. They live in their own header so the other
/// protocol headers can depend on them without pulling in the full frame API.

#include <cstdint>

namespace esphome {
namespace home_io_control {

// ============================================================================
// Frame and Crypto Sizes
// ============================================================================

static constexpr uint8_t NODE_ID_SIZE = 3;  ///< Device/node addresses are 3 bytes (e.g., "123ABC")
static constexpr uint8_t NODE_ID_STRING_SIZE = (NODE_ID_SIZE * 2) + 1;  ///< Uppercase hex node ID plus null terminator
static constexpr uint8_t HMAC_SIZE = 6;        ///< Authentication HMAC is 6 bytes (truncated AES output)
static constexpr uint8_t AES_KEY_SIZE = 16;    ///< AES-128 key size
static constexpr uint8_t AES_BLOCK_SIZE = 16;  ///< AES block size
static constexpr uint8_t IV_SIZE = 16;         ///< Initialization vector size for AES
static constexpr uint8_t IV_PADDING = 0x55;    ///< Padding byte used in IV construction
static constexpr uint8_t BITS_PER_BYTE = 8;    ///< Number of bits in one protocol byte

static constexpr uint8_t FRAME_MIN_SIZE = 9;        ///< Minimum frame: CTRL0+CTRL1+DST(3)+SRC(3)+CMD(1)
static constexpr uint8_t FRAME_MAX_SIZE = 32;       ///< Maximum frame size (9 header + 23 data)
static constexpr uint8_t FRAME_MAX_DATA_SIZE = 23;  ///< Maximum data bytes after command ID
static constexpr uint8_t FRAME_CMD_OFFSET = 8;      ///< Byte offset of the command ID in a raw wire buffer
static constexpr uint8_t FRAME_CRC_SIZE = 2;        ///< CRC-CCITT trailer appended after the frame body

}  // namespace home_io_control
}  // namespace esphome
