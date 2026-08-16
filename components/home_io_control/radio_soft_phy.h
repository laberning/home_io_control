#pragma once

/// @file radio_soft_phy.h
/// @brief Software PHY for radios without IoHomeOn hardware framing.
/// @ingroup hioc_radio
///
/// Chips such as the SX1262 and LR1121 have no hardware mode equivalent to the SX1276's
/// IoHomeOn: they provide generic GFSK framing only. This header
/// declares the chip-agnostic pieces such a driver needs to reproduce IO-Homecontrol framing
/// in software — UART bit-encoding for TX, and a UART-decode probe (with CRC validation) to
/// recover frame boundaries from an unaligned raw RX bitstream.

#include "radio_interface.h"

#include <cstdint>

namespace esphome {
namespace home_io_control {

/// @brief Result of the UART probe: best candidate frame within a raw capture.
struct UartProbeResult {
  bool valid{false};                            ///< A plausible frame was found.
  uint8_t bit_offset{0};                        ///< Bit offset where the best decode started.
  uint8_t decoded_len{0};                       ///< Total number of bytes decoded at that offset.
  uint8_t frame_start{0};                       ///< Index into decoded buffer where the frame begins.
  uint8_t frame_len{0};                         ///< Length of the candidate IoFrame (decoded bytes).
  uint8_t decoded[RADIO_PACKET_BUFFER_SIZE]{};  ///< Full decoded UART stream at the chosen offset.
};

/// @brief Decode a UART-encoded bitstream from the given bit offset.
uint8_t decode_uart_probe(const uint8_t *raw, uint8_t raw_len, uint8_t bit_offset, uint8_t *decoded,
                          uint8_t decoded_max_len);

/// @brief Search raw RX buffer for the best CRC-validated IO-Homecontrol frame.
UartProbeResult find_uart_probe(const uint8_t *raw, uint8_t raw_len);

/// @brief Check if a command ID is one of the known IO-Homecontrol commands.
///
/// This is a gate, not a directory: find_uart_probe() only accepts a CRC-valid candidate whose
/// cmd passes this check (or whose CTRL0_PROTOCOL_1W bit is set), so a command missing here makes
/// every SX1262/LR1121 reception of that opcode silently unrecoverable on this software PHY —
/// the frame is on air, its CRC matches, and it still never reaches the parser. Exposed (out of
/// radio_soft_phy.cpp's anonymous namespace) so tests can iterate every accepted command directly
/// instead of hand-maintaining a parallel list that can drift out of sync with this one.
/// @param cmd Command byte.
/// @return true if cmd matches a known command constant.
bool is_known_io_command(uint8_t cmd);

/// @brief Bits an on-air UART cell spends per protocol byte: start(1) + data(8) + stop(1).
static constexpr uint8_t UART_CELL_BITS = 10;

/// @brief Raw on-air bytes needed to carry a whole frame: `frame_len` protocol bytes plus the
/// two trailing CRC bytes, each UART-packed into a 10-bit cell.
///
/// This is what makes a length-driven receive possible at all: a frame's own size is knowable
/// from its first decoded byte, so the raw byte count it will occupy is knowable too — no chip
/// needs to tell us where the frame ends.
/// @param frame_len Protocol frame length in bytes (CTRL0's own length field, +1).
/// @return Raw byte count, rounded up to whole bytes.
uint8_t soft_phy_raw_bytes_for_frame(uint8_t frame_len);

/// @brief Recover a frame's total length from the very first UART cell of a reception.
///
/// CTRL0 bits [4:0] hold `frame_length - 1` (see proto_frame.h), and CTRL0 is the first byte
/// after the sync word — so ten bits of air time are enough to learn how long the whole frame
/// will be. Alignment is not yet known at that point, so every probe offset is tried and the
/// largest plausible answer wins: over-waiting by a few bytes costs a little latency, whereas
/// under-waiting would truncate the frame.
/// @param raw Raw bytes read from the chip's data buffer, starting at the reception's own offset.
/// @param raw_len Number of raw bytes available (three is enough at any alignment).
/// @return Plausible frame length in bytes, or 0 when no offset yields one.
uint8_t soft_phy_peek_frame_length(const uint8_t *raw, uint8_t raw_len);

/// @brief UART-encode a buffer of bytes (start bit 0, 8 data bits LSB-first, stop bit 1).
/// @return Number of encoded bytes, or 0 if the output buffer is too small.
uint8_t uart_encode_packet(const uint8_t *data, uint8_t len, uint8_t *encoded, uint8_t encoded_max_len);

}  // namespace home_io_control
}  // namespace esphome
