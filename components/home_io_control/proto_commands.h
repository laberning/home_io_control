#pragma once

/// @file proto_commands.h
/// @brief Command builders for the IO‑Homecontrol protocol.
/// @ingroup hioc_protocol
///
/// This module provides builder functions that populate IoFrame structures for
/// the various commands used in discovery, pairing, control, and status operations.
/// All builders follow the same pattern: fill an IoFrame with CTRL0/CTRL1 flags,
/// addresses, command ID, and optional payload.
///
/// Position encoding:
///   - IO protocol position values: 0 = fully open, 100 = fully closed.
///   - Use create_execute_position() for numeric positions (0–100).
///   - Use create_execute_command() for named commands: CoverCommand::STOP,
///     CoverCommand::FAVORITE, CoverCommand::VENT, CoverCommand::FORCE_OPEN.
///   - The Home Assistant layer maps HA's 1.0=open/0.0=closed to the IO scale via
///     ha_position = 1.0 - (io_position / 100.0). Some devices (horizontal awnings)
///     have inverted mapping; see platform_cover.h.
///
/// Preamble handling:
///   - Commands to battery/solar‑powered devices must use LONG_PREAMBLE (1024 bytes)
///     so the sleeping receiver can detect the frame. Mains‑powered devices use
///     SHORT_PREAMBLE (8 bytes). See CTRL1_LOW_POWER flag and battery_powered_ field
///     in IoDevice.

#include "proto_frame.h"

namespace esphome {
namespace home_io_control {

/// Build an execute command (0x00) to control a device (set position or special).
///
/// @deprecated Prefer create_execute_position() for numeric positions and
///             create_execute_command() for named commands (STOP, FAVORITE, VENT).
///             This function is retained for backward compatibility but new code
///             should use the typed alternatives.
/// @param f IoFrame to populate.
/// @param own Controller's 3‑byte node ID (source address).
/// @param dst Target device's 3‑byte node ID (destination address).
/// @param low_power True if target is battery/solar‑powered (uses long preamble).
/// @param position Desired position: 0–100 (open→closed), or POS_STOP/POS_FAVORITE.
/// @return true on success; false if position exceeds limits.
bool create_execute(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t position);

/// @brief Build a position execute command (0x00) to move a device to a numeric position.
///
/// Encodes a 0–100 position value into the standard 8-byte execute payload with
/// originator, ACEI, and functional parameter fields. The wire encoding doubles
/// the position value (0→0x00, 100→0xC8).
/// @param f IoFrame to populate.
/// @param own Controller's 3‑byte node ID (source address).
/// @param dst Target device's 3‑byte node ID (destination address).
/// @param low_power True if target is battery/solar‑powered (uses long preamble).
/// @param position Desired position 0–100 (0=fully open, 100=fully closed).
/// @return true on success; false if position > 100.
bool create_execute_position(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t position);

/// @brief Build a named-command execute frame (0x00) for STOP, FAVORITE, VENT, or FORCE_OPEN.
///
/// Each named command maps to a specific wire encoding in the CMD_EXECUTE payload:
///   - STOP:       main=0xD2, modifier=0x00 (6-byte special payload)
///   - FAVORITE:   main=0xD8, modifier=0x00 (6-byte special payload)
///   - VENT:       main=0xD8, modifier=0x03 (6-byte special payload)
///   - FORCE_OPEN: main=0x64, modifier=0x00 (6-byte special payload)
///
/// This cleanly separates "move to position X" from "execute named action"
/// without overloading a single numeric parameter.
/// @param f IoFrame to populate.
/// @param own Controller's 3‑byte node ID (source address).
/// @param dst Target device's 3‑byte node ID (destination address).
/// @param low_power True if target is battery/solar‑powered (uses long preamble).
/// @param cmd Named command to execute.
/// @return true on success; false for invalid command.
bool create_execute_command(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, CoverCommand cmd);

/// Build a get‑status request (0x03). The device responds with its current position.
/// @param f IoFrame to populate.
/// @param own Controller's 3‑byte node ID.
/// @param dst Target device's 3‑byte node ID.
/// @return true on success.
bool create_get_status(IoFrame &f, const uint8_t *own, const uint8_t *dst);

/// Build a get-name request (0x50). The device responds with its stored display name.
/// @param f IoFrame to populate.
/// @param own Controller's 3-byte node ID.
/// @param dst Target device's 3-byte node ID.
/// @param low_power True if target is battery/solar-powered (uses long preamble).
/// @return true on success.
bool create_get_name(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power);

/// Build an authenticated set-name request (0x52) using a fixed zero-padded Latin-1 payload.
/// @param f IoFrame to populate.
/// @param own Controller's 3-byte node ID.
/// @param dst Target device's 3-byte node ID.
/// @param payload Pre-validated fixed payload produced by encode_device_name_payload().
/// @return true on success.
bool create_set_name(IoFrame &f, const uint8_t *own, const uint8_t *dst,
                     const uint8_t payload[DEVICE_NAME_WRITE_PAYLOAD_SIZE]);

/// Build an execute‑tilt command (0x00) for slat angle control.
/// @param f IoFrame to populate.
/// @param own Controller node ID.
/// @param dst Target device node ID.
/// @param low_power True for long preamble (battery/solar devices).
/// @param tilt_percent 0 = fully closed, 100 = fully open.
/// @note This uses the same command (0x00) as position control but with a different
///       payload format indicating a tilt operation. The receiver infers tilt from
///       the payload structure. Only devices that advertise tilt support (see
///       device_supports_tilt in proto_frame.h) will honor this.
/// @return true on success.
bool create_execute_tilt(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t tilt_percent);

/// Build a combined position‑and‑tilt execute command (0x00).
/// Sets both the cover position and the slat angle atomically in one frame,
/// corresponding to the protocol's setClosureAndOrientation use case.
/// @param f IoFrame to populate.
/// @param own Controller node ID.
/// @param dst Target device node ID.
/// @param low_power True for long preamble (battery/solar devices).
/// @param position Desired position 0–100 (open→closed).
/// @param tilt_percent 0 = fully closed, 100 = fully open.
/// @return true on success; false if position exceeds limits.
bool create_execute_position_and_tilt(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power,
                                      uint8_t position, uint8_t tilt_percent);

/// Build a tilt‑aware get‑status request (0x03 with extended payload) that returns
/// the 16‑byte tilt block in the response.
/// @param f IoFrame to populate.
/// @param own Controller node ID.
/// @param dst Target device node ID.
/// @return true on success.
bool create_get_status_tilt(IoFrame &f, const uint8_t *own, const uint8_t *dst);

/// Build a discovery broadcast (0x28). Sent to the broadcast address; only devices
/// in pairing mode (PROG button pressed) will respond.
/// @param f IoFrame to populate.
/// @param own Controller node ID.
/// @note Destination is BROADCAST_DISCOVER (0x00003B). The device responds with
///       CMD_DISCOVER_RESP (0x29) containing its node ID and type/subtype. The
///       controller then switches to point‑to‑point communication for phases 2 and 3.
/// @return true on success.
bool create_discover(IoFrame &f, const uint8_t *own);

/// Build a key‑init request (0x31) to start pairing key exchange with a discovered device.
/// @param f IoFrame to populate.
/// @param own Controller node ID.
/// @param dst Discovered device node ID.
/// @return true on success.
bool create_key_init(IoFrame &f, const uint8_t *own, const uint8_t *dst);

/// Build a key‑transfer frame (0x32) containing the system key encrypted with the transfer key.
/// @param f IoFrame to populate.
/// @param old_frame The key‑init frame (used to derive the encryption IV).
/// @param dst Target device node ID.
/// @param src Controller node ID.
/// @param key The 16‑byte system key to transfer.
/// @param challenge 6‑byte challenge received from device in its 0x3C response.
/// @note The system key is obfuscated via the XOR‑AES construction in crypt_key().
///       The transfer key (hardcoded in proto_frame.h) is the same for all IO‑Homecontrol
///       devices worldwide; its purpose is to protect the system key in transit during
///       initial pairing. Once transferred, the device uses the system key for all
///       subsequent authenticated exchanges.
/// @return true on success.
bool create_key_transfer(IoFrame &f, IoFrame &old_frame, const uint8_t *dst, const uint8_t *src,
                         const uint8_t key[AES_KEY_SIZE], const uint8_t challenge[HMAC_SIZE]);

/// Build a challenge request (0x3C) containing 6 random bytes. Used when we need to
/// authenticate an incoming request from a device.
/// @param f IoFrame to populate.
/// @param dst Target device node ID (device we're challenging).
/// @param src Controller node ID.
/// @return true on success.
bool create_challenge_req(IoFrame &f, const uint8_t *dst, const uint8_t *src);

/// Build a challenge response (0x3D) proving we know the system key.
/// HMAC is computed over [original_command_id + original_data] using the challenge.
/// @param f IoFrame to populate.
/// @param dst Target device node ID.
/// @param src Controller node ID.
/// @param challenge 6‑byte challenge from the device.
/// @param origin Original request frame that triggered the challenge.
/// @param key System key (16 bytes).
/// @note The HMAC derivation uses the challenge as IV salt; see create_hmac() in
///       proto_crypto.h for the exact construction. This frame authenticates the
///       controller to the device for the current exchange.
/// @return true on success.
bool create_challenge_resp(IoFrame &f, const uint8_t *dst, const uint8_t *src, const uint8_t challenge[HMAC_SIZE],
                           const IoFrame &origin, const uint8_t *key);

/// Build a status‑update acknowledgment (0x72). Sent after authenticating a device's
/// status update; broadcast on all 3 channels for reliability.
/// @param f IoFrame to populate.
/// @param own Controller node ID.
/// @param dst Device node ID that sent the update.
/// @return true on success.
bool create_status_update_resp(IoFrame &f, const uint8_t *own, const uint8_t *dst);

/// Build a set‑config command (0x6F) telling the device to automatically send
/// status updates when controlled by any remote.
/// @param f IoFrame to populate.
/// @param own Controller node ID.
/// @param dst Target device node ID.
/// @note This configures the device to emit CMD_STATUS_UPDATE (0x71) frames whenever
///       it is controlled by any remote (including the paired controller). This enables
///       HA to receive unsolicited position updates. The controller must still
///       authenticate the status update using the inbound auth flow (hub_exchange.h).
/// @todo Confirm on real hardware which device families actually honor this SetConfig1
///       payload and emit unsolicited status updates after pairing.
/// @return true on success.
bool create_set_config1(IoFrame &f, const uint8_t *own, const uint8_t *dst);

}  // namespace home_io_control
}  // namespace esphome
