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

static constexpr uint8_t FRAME_MIN_SIZE = 9;  ///< Minimum frame: CTRL0+CTRL1+DST(3)+SRC(3)+CMD(1)

/// Largest frame length CTRL0's 5-bit length field (bits [4:0], `length - 1`) can express.
/// This is the wire-format bound on the *declared* portion of a frame — the part CTRL0's length
/// bits describe and `parse()`/`serialize()`/`set_cmd()` size against. It is not expressed here
/// as `CTRL0_LENGTH_MASK + 1` because `CTRL0_LENGTH_MASK` lives in `proto_frame.h`, which
/// includes this header — referencing it back here would be a circular include. `proto_frame.h`
/// carries a `static_assert` tying the two together right next to `CTRL0_LENGTH_MASK`'s
/// declaration, so this literal cannot silently drift from the mask that actually defines it.
static constexpr uint8_t FRAME_MAX_DECLARED_SIZE = 32;

/// Historical name for `FRAME_MAX_DECLARED_SIZE`, kept as an alias rather than a second
/// literal so the two names can never disagree — most call sites (`set_cmd`, `serialize`,
/// `parse`, every fixed-size frame buffer that only ever holds a declared-length frame) predate
/// the trailer/wire distinction and still read most naturally as "the max frame size".
static constexpr uint8_t FRAME_MAX_SIZE = FRAME_MAX_DECLARED_SIZE;

static constexpr uint8_t FRAME_MAX_DATA_SIZE = 23;  ///< Maximum data bytes after command ID (declared length - header)
static constexpr uint8_t FRAME_CMD_OFFSET = 8;      ///< Byte offset of the command ID in a raw wire buffer

/// Size of the on-air CRC-CCITT trailer appended after every frame (declared bytes, plus the
/// out-of-length MAC trailer when present).
static constexpr uint8_t FRAME_CRC_SIZE = 2;

/// Largest out-of-length authenticator a frame can carry. A 1W CMD 0x30 "add controller" payload
/// (enc_key[16] + man_id[1] + data[1] + sequence[2] = 20 bytes) plus its 9-byte header is 29
/// bytes — already representable in CTRL0's 5-bit field — but its 6-byte MAC does not fit inside
/// the same field's remaining headroom (29 declared + 6 MAC = 35, unrepresentable in 5 bits), so
/// the MAC rides after the declared length instead, still under the CRC. The iohomecontrol
/// reference implementation's `_p0x30` packet struct omits an `hmac` field entirely (unlike its
/// `_p0x2e`, whose MAC sits inside the declared length).
static constexpr uint8_t FRAME_MAX_TRAILER_SIZE = HMAC_SIZE;

/// Largest number of bytes a buffer must hold to receive or transmit any frame this project
/// knows about, trailer and CRC included: `FRAME_MAX_DECLARED_SIZE` (what CTRL0 can declare)
/// + @ref FRAME_MAX_TRAILER_SIZE (the out-of-length MAC, when present) + @ref FRAME_CRC_SIZE.
/// This is the *wire* bound — distinct from `FRAME_MAX_DECLARED_SIZE`, which is the
/// *declared* bound `set_cmd()` still enforces. A buffer sized to the declared bound alone
/// truncates a MAC-bearing frame; use this constant for any buffer that must survive one intact.
static constexpr uint8_t FRAME_MAX_WIRE_SIZE = FRAME_MAX_DECLARED_SIZE + FRAME_MAX_TRAILER_SIZE + FRAME_CRC_SIZE;

}  // namespace home_io_control
}  // namespace esphome
