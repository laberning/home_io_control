#pragma once

/// @file proto_frame.h
/// @brief IO-Homecontrol 2W frame container: control bytes, IoFrame and (de)serialization.
/// @ingroup hioc_protocol
///
/// IO-Homecontrol is a proprietary wireless protocol used by Somfy, Velux, and other
/// manufacturers for controlling shutters, awnings, blinds, and similar devices.
/// "2W" means two-way: the controller sends commands and receives status feedback.
///
/// The protocol uses FSK modulation at 868 MHz with frequency hopping across 3 channels.
/// Communication is encrypted with AES-128 and authenticated with a 6-byte HMAC.
/// Each installation has a unique 16-byte "system key" shared between controller and devices.
///
/// This header owns only the frame container itself. The rest of the protocol model lives in
/// cohesive headers (proto_sizes/proto_timing/proto_constants/proto_device_model/proto_codecs);
/// they are re-exported below for transition so existing includers keep compiling unchanged.
/// New code should include the specific header it needs.

#include "proto_sizes.h"

// Umbrella includes for transition; new code should include the specific header.
#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_device_model.h"
#include "proto_timing.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace esphome {
namespace home_io_control {

// ============================================================================
// Control Bytes
// ============================================================================

/// Control byte 0 (CTRL0) bit definitions.
/// CTRL0 encodes frame flags and the total frame length.
/// Bits [4:0] = frame_length - 1 (so 0x08 means 9 bytes total).
/// - START (bit 6): first frame in an exchange; uses long preamble (1024 bytes).
/// - END (bit 7): last frame in an exchange; set on responses and command completions.
/// - 1W (bit 5): 1=OneWay protocol (no response expected), 0=TwoWay (response expected).
/// For 2W operation, the controller sets START on initial command and device replies with END; subsequent frames in an
/// authenticated exchange also carry END.
static constexpr uint8_t CTRL0_END = 0x80;          ///< Bit 7: last frame in exchange
static constexpr uint8_t CTRL0_START = 0x40;        ///< Bit 6: first frame in exchange (uses long preamble)
static constexpr uint8_t CTRL0_PROTOCOL_1W = 0x20;  ///< Bit 5: 1=OneWay protocol, 0=TwoWay protocol
static constexpr uint8_t CTRL0_LENGTH_MASK = 0x1F;  ///< Bits [4:0]: frame length - 1

/// Control byte 1 (CTRL1) bit definitions.
/// CTRL1 carries protocol metadata flags that describe the frame's routing,
/// power mode, and priority characteristics.
/// - VERSION (bits [1:0]): protocol version number (usually 0 for current devices).
/// - PRIORITY (bit 2): marks a high-priority frame (e.g., discovery, security commands).
/// - ACK (bit 4): sender can handle 2W responses (set on all outbound 2W frames).
/// - LOW_POWER (bit 5): device is battery/solar powered; may sleep and requires long preamble to wake.
/// - ROUTED (bit 6): frame was relayed through a repeater node rather than direct.
/// - BEACON (bit 7): beacon announcement frame (device presence advertisement).
static constexpr uint8_t CTRL1_VERSION_MASK = 0x03;  ///< Bits [1:0]: protocol version (usually 0).
static constexpr uint8_t CTRL1_PRIORITY = 0x04;      ///< Bit 2: high-priority frame.
static constexpr uint8_t CTRL1_ACK = 0x10;           ///< Bit 4: sender can handle 2W responses (ACK-capable).
static constexpr uint8_t CTRL1_LOW_POWER = 0x20;     ///< Bit 5: low-power device (e.g., solar-powered).
static constexpr uint8_t CTRL1_ROUTED = 0x40;        ///< Bit 6: frame was relayed through a repeater.
static constexpr uint8_t CTRL1_BEACON = 0x80;        ///< Bit 7: beacon announcement frame.

// ============================================================================
// Frame Structure
// ============================================================================

/// @brief Parsed IO‑Homecontrol frame (CTRL0/1 + addresses + command + data).
/// @ingroup hioc_protocol
///
/// Over the air layout: [CTRL0][CTRL1][DST 3B][SRC 3B][CMD][DATA 0-23B][CRC 2B].
/// The on-air CRC is the radio driver's responsibility (hardware or software,
/// depending on the chip); it is not included in this struct.
struct IoFrame {
  uint8_t ctrl0;                      ///< Control byte 0: flags + length.
  uint8_t ctrl1;                      ///< Control byte 1: low power, beacon, etc.
  uint8_t dst[NODE_ID_SIZE];          ///< Destination node ID (3 bytes).
  uint8_t src[NODE_ID_SIZE];          ///< Source node ID (3 bytes).
  uint8_t cmd;                        ///< Command ID.
  uint8_t data[FRAME_MAX_DATA_SIZE];  ///< Command parameters (0–23 bytes).
  uint8_t data_len;                   ///< Actual length of data.
};

// --- Frame construction and parsing ---
/// Initialize an IoFrame header (ctrl0/ctrl1) with flags.
///
/// Note: CTRL1_ACK is NOT automatically set on outbound frames. Some real-world
/// devices reject frames with unexpected CTRL1 bits, causing total communication
/// failure. The ACK constant is retained for inbound frame parsing and logging only.
/// @param f Frame to initialize.
/// @param is_2w True for 2‑way (default), false for 1‑way.
/// @param start Set START flag (first frame in exchange).
/// @param end Set END flag (final frame in exchange).
/// @param low_power Set LOW_POWER flag.
void init_frame(IoFrame &f, bool is_2w = true, bool start = false, bool end = false, bool low_power = false);
/// Set destination node ID.
/// @param f Frame to modify.
/// @param id 3‑byte destination address.
void set_dst(IoFrame &f, const uint8_t id[NODE_ID_SIZE]);
/// Set source node ID.
/// @param f Frame to modify.
/// @param id 3‑byte source address.
void set_src(IoFrame &f, const uint8_t id[NODE_ID_SIZE]);
/// Set command and payload.
/// @param f Frame to modify.
/// @param cmd Command ID.
/// @param params Pointer to payload bytes (may be nullptr for zero‑length).
/// @param params_len Payload length (0–23).
/// @return true if frame fits within size limits; false otherwise.
bool set_cmd(IoFrame &f, uint8_t cmd, const uint8_t *params = nullptr, uint8_t params_len = 0);
/// Get total frame length from ctrl0.
/// @param f Parsed frame.
/// @return Length in bytes.
uint8_t frame_length(const IoFrame &f);
/// Check START flag.
/// @param f Parsed frame.
/// @return true if START flag is set.
bool is_start(const IoFrame &f);
/// Check END flag.
/// @param f Parsed frame.
/// @return true if END flag is set.
bool is_end(const IoFrame &f);
/// Serialize a parsed frame into a wire buffer (without CRC).
/// @param f Parsed frame.
/// @param buf Output buffer (must be at least frame_length(f) bytes).
/// @param buf_size Size of buf.
/// @return Number of bytes written, or 0 on failure.
uint8_t serialize(const IoFrame &f, uint8_t *buf, uint8_t buf_size);
/// Parse a wire buffer into a parsed IoFrame (validates length and CTRL0).
/// @param buf Raw byte buffer.
/// @param buf_len Number of bytes in buf.
/// @param f Output parsed frame.
/// @return true if parse succeeded; false otherwise.
bool parse(const uint8_t *buf, uint8_t buf_len, IoFrame &f);

// ============================================================================
// Node ID Helpers
// ============================================================================

/// @brief Convert a hex string (e.g., "123ABC") to a byte array.
/// @param hex Hex string (must be exactly len*2 characters).
/// @param out Output buffer (at least len bytes).
/// @param len Number of bytes to produce.
/// @return true on success; false if hex length mismatch or non‑hex characters.
bool hex_to_bytes(const std::string &hex, uint8_t *out, uint8_t len);
/// @brief Format a 3‑byte node ID as a 6‑character uppercase hex string.
/// @param id 3‑byte node ID.
/// @return Hex string (e.g., "123ABC").
std::string node_id_to_string(const uint8_t id[NODE_ID_SIZE]);

// ============================================================================
// CRC
// ============================================================================

/// @brief Compute CRC‑CCITT (poly 0x1021, init 0x0000) over a buffer.
/// Used by radio drivers without hardware IO-Homecontrol CRC support and by
/// frame validation in tests.
/// @param data Pointer to data bytes.
/// @param len Number of bytes.
/// @return 16‑bit CRC value.
uint16_t crc_ccitt(const uint8_t *data, uint8_t len);

}  // namespace home_io_control
}  // namespace esphome
