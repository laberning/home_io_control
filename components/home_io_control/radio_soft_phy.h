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

/// @brief Number of leading bit alignments the UART probe tries when locating a frame start.
///
/// The chip hands us raw bytes with no guaranteed cell alignment, so decode_uart_probe() sweeps
/// starting positions 0..UART_PROBE_MAX_BIT_OFFSET-1. A recovered frame can therefore begin up to
/// `UART_PROBE_MAX_BIT_OFFSET - 1` bits into the raw buffer — buffer-sizing asserts must budget
/// for that slack on top of the frame's own packed length.
static constexpr uint8_t UART_PROBE_MAX_BIT_OFFSET = 10;

/// @brief Raw on-air bytes needed to carry a whole frame: `frame_len` protocol bytes plus the
/// two trailing CRC bytes, each UART-packed into a 10-bit cell.
///
/// This is what makes a length-driven receive possible at all: a frame's own size is knowable
/// from its first decoded byte, so the raw byte count it will occupy is knowable too — no chip
/// needs to tell us where the frame ends.
/// @param frame_len Protocol frame length in bytes (CTRL0's own length field, +1).
/// @return Raw byte count, rounded up to whole bytes.
uint8_t soft_phy_raw_bytes_for_frame(uint8_t frame_len);

/// Protocol line rate. The same 38400 bps every driver programs into its own bitrate register.
static constexpr uint32_t SOFT_PHY_LINE_RATE_BPS = 38400;
/// Microseconds in a second, for the air-time arithmetic below.
static constexpr uint32_t SOFT_PHY_US_PER_SECOND = 1000000;

/// @brief On-air time in microseconds for `raw_bytes` bytes at the protocol's line rate.
///
/// One byte is 8 / 38400 s = 208.333 µs. Computed as an integer division rounded *up*, so the
/// result never falls short of a whole byte's air time and a caller that waits on it never reads
/// the chip's buffer early. The numerator is 64-bit: in 32 bits it would wrap above 536 bytes, and
/// a 1024-byte wake-up preamble is a real transmission length.
constexpr uint32_t soft_phy_air_time_us(uint32_t raw_bytes) {
  const uint64_t bit_periods = static_cast<uint64_t>(raw_bytes) * BITS_PER_BYTE * SOFT_PHY_US_PER_SECOND;
  return static_cast<uint32_t>((bit_periods + SOFT_PHY_LINE_RATE_BPS - 1) / SOFT_PHY_LINE_RATE_BPS);
}

/// Sync word bytes on air after the preamble (`55 FF 33` on the SX1276, the UART-coded equivalent
/// on the software PHY): three on every 868 MHz chip.
static constexpr uint16_t IO868_SYNC_WORD_BYTES = 3;

/// @brief Time on air of one 868 MHz transmission: preamble, sync word, and the frame plus its CRC,
/// each byte of the latter a 10-bit UART cell, at the protocol's line rate.
///
/// The same line coding applies to every 868 MHz chip (the SX1276's IoHomeOn coder and the
/// software PHY produce the same waveform), so every 868 driver's `tx_air_time_us()` is this.
/// @param preamble_len Preamble length in bytes.
/// @param frame_len    Frame length in bytes, without the CRC.
/// @return Air time in microseconds, rounded up.
uint32_t io868_tx_air_time_us(uint16_t preamble_len, uint8_t frame_len);

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
