#pragma once

/// @file radio_soft_phy.h
/// @brief Software PHY for radios without IoHomeOn hardware framing.
/// @ingroup hioc_radio
///
/// Chips such as the SX1262 (and, in the future, the LR1121) have no hardware mode
/// equivalent to the SX1276's IoHomeOn: they provide generic GFSK framing only. This header
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

/// @brief UART-encode a buffer of bytes (start bit 0, 8 data bits LSB-first, stop bit 1).
/// @return Number of encoded bytes, or 0 if the output buffer is too small.
uint8_t uart_encode_packet(const uint8_t *data, uint8_t len, uint8_t *encoded, uint8_t encoded_max_len);

}  // namespace home_io_control
}  // namespace esphome
