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
///   - IO protocol position values: 0 = fully open, 100 = fully closed, for a non-inverted
///     device — but this is per-device, not a universal wire constant: IoDevice::inverted
///     devices (e.g. horizontal awnings) have it backwards (0 = fully closed, 100 = fully
///     open). Never hardcode "0 = open" in caller code without checking inversion first.
///   - Use create_execute_position() for numeric positions (0–100).
///   - Use create_execute_command() for named commands: CoverCommand::STOP,
///     CoverCommand::FAVORITE, CoverCommand::VENT.
///   - Use create_force_open() for CoverCommand::FORCE_OPEN — it takes the target "fully open"
///     position explicitly rather than assuming 0
///   - The Home Assistant layer maps HA's 1.0=open/0.0=closed to the IO scale via
///     ha_position = 1.0 - (io_position / 100.0), or the inverted form for IoDevice::inverted
///     devices; see platform_cover.h.
///
/// Low‑power flag and preamble handling:
///   - Every frame addressed to a specific device sets CTRL1_LOW_POWER: the codebase does
///     not track per‑device power class, battery/solar devices need the flag on every frame
///     sent to them, and the golden‑frame corpus shows real devices accepting it.
///   - The flag does not select the TX preamble. The exchange engine picks the preamble from
///     frame position: start frames use LONG_PREAMBLE (1024 bytes) so a sleeping receiver
///     can wake, follow‑up frames use the driver's response_preamble() (exchange_engine.cpp).

#include "proto_codecs.h"
#include "proto_device_model.h"
#include "proto_frame.h"

namespace esphome {
namespace home_io_control {

/// @brief Build a position execute command (0x00) to move a device to a numeric position.
///
/// Encodes a 0–100 position value into the standard 8-byte execute payload with
/// originator, ACEI, and functional parameter fields. The wire encoding doubles
/// the position value (0→0x00, 100→0xC8).
/// @param f IoFrame to populate.
/// @param own Controller's 3‑byte node ID (source address).
/// @param dst Target device's 3‑byte node ID (destination address).
/// @param low_power True if target is battery/solar‑powered (sets CTRL1_LOW_POWER).
/// @param position Desired position 0–100 (0=fully open, 100=fully closed).
/// @return true on success; false if position > 100.
/// @param silent Send the reference hub's "silent operation" extended block, which makes the motor
///        travel more slowly. Applies to position moves only — STOP has no speed, and no capture
///        yet shows what the toggle does to FAVORITE/VENT or tilt, so those are left alone rather
///        than guessed at.
bool create_execute_position(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t position,
                             bool silent = false);

/// @brief Build a named-command execute frame (0x00) for STOP, FAVORITE, or VENT.
///
/// Each maps to a specific wire encoding in the 6-byte special CMD_EXECUTE payload:
///   - STOP:       main=0xD2, modifier=0x00
///   - FAVORITE:   main=0xD8, modifier=0x00
///   - VENT:       main=0xD8, modifier=0x03
///
/// This cleanly separates "move to position X" from "execute named action"
/// without overloading a single numeric parameter.
///
/// FORCE_OPEN is NOT handled here — see create_force_open() instead. Unlike these three, it
/// needs a device-specific "fully open" wire position (0 or 100 depending on inversion), which
/// this generic dispatch has no way to supply; passing CoverCommand::FORCE_OPEN returns false.
/// @param f IoFrame to populate.
/// @param own Controller's 3‑byte node ID (source address).
/// @param dst Target device's 3‑byte node ID (destination address).
/// @param low_power True if target is battery/solar‑powered (sets CTRL1_LOW_POWER).
/// @param cmd Named command to execute (STOP, FAVORITE, or VENT).
/// @return true on success; false for invalid/unsupported command.
/// @param silent Use the silent (slower) travel profile. Honoured for FAVORITE only — STOP has no
///        travel speed, and nothing has been captured for VENT.
bool create_execute_command(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, CoverCommand cmd,
                            bool silent = false);

/// @brief Build a force-open execute frame (0x00): move to the device's wire-scale "fully open"
/// position at elevated ACEI priority (level 0, protection_human) instead of the usual
/// user_high level — see EXECUTE_ACEI_FORCE_OPEN in proto_commands.cpp for why priority
/// elevation, not a special position byte, is the protocol's real mechanism for getting past an
/// environmental soft lock.
///
/// @param f IoFrame to populate.
/// @param own Controller's 3‑byte node ID (source address).
/// @param dst Target device's 3‑byte node ID (destination address).
/// @param low_power True if target is battery/solar‑powered (sets CTRL1_LOW_POWER).
/// @param open_position The device's wire-scale position value that means "fully open": 0 for
///        ordinary devices, 100 for IoDevice::inverted ones (e.g. horizontal awnings) — the
///        caller must resolve this from the target device, this builder does not have access
///        to device state. Getting this wrong sends an ordinary, harmless-looking position
///        command to the device's already-resting position instead of moving it anywhere.
/// @return true on success.
/// @note The elevated-priority override has not yet been confirmed against a real *active*
///       environmental lock.
bool create_force_open(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t open_position);

/// @brief Build a CMD_PRIVATE (0x03) request for an arbitrary function ID.
///
/// function_id = 0x06 (battery-status) or 0x09 (battery-state) is reported elsewhere to read the
/// CMD_PRIVATE_RESP reply as data[1]==0x60 => battery/solar powered, value = data[2]<<8|data[3].
/// Unverified here -- both devices this project has probed are mains-powered and answered
/// data[1]==0x00 (tests/corpus/captures/somfy_{awning,dimmer}/private_fn_probe_lr1121.yaml).
/// create_get_status() below is this builder frozen at function_id =
/// PRIVATE_GET_POSITION_STATUS (0x03), the only function ID this codebase has ever captured on
/// its own wire.
/// @param f IoFrame to populate.
/// @param own Controller's 3-byte node ID.
/// @param dst Target device's 3-byte node ID.
/// @param function_id Private function ID (data[0] of the CMD_PRIVATE payload).
/// @return true on success.
bool create_private_function(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t function_id);

// ============================================================================
// 1W Execute (fire-and-forget class broadcast)
// ============================================================================
//
// 1W has no reply and no challenge: a controller transmits once and the frame either lands or it
// doesn't — there is no ACK to retry on and no 0x3C/0x3D to authenticate through, so the MAC
// inside the payload (create_1w_hmac(), sequence-keyed rather than challenge-keyed) is the only
// authentication a receiving device gets. The destination is a device *class*
// (encode_broadcast_address(), proto_codecs.h), never an individual node — 1W has no addressed
// unicast form at all. Both builders below are pure: they take `sequence` and `controller_key` as
// parameters and neither increment, persist, nor look either up. The sequence store and the
// identity registry that own those concerns (OneWayControllerRegistry, oneway_controller.h)
// arrive in a later step; until then callers are responsible for supplying both.

/// @brief Build a 1W position execute frame (CMD 0x00) targeting a device class.
///
/// Encodes a 0–100 position into the 2-byte main field (wire value is `2 * position`, matching
/// the 2W numeric encoding) inside 1W's 6-byte "special" payload form — 1W uses this short form
/// even for numeric positions, not the 8-byte layout create_execute_position() (2W) uses. The MAC
/// span is command-specific (see create_1w_execute_command()'s doxygen); there is no default, so
/// this builder assembles it itself via the shared internal helper.
///
/// @note Position 50 encodes as `2 * 50 = 0x64` on the wire, the same byte
///       decode_1w_main_intent() labels POS_FORCE_OPEN when reading overheard traffic. That label
///       is a decode-side diagnostic choice only (see POS_FORCE_OPEN in proto_constants.h) — it
///       does not change what this builder sends or what a device does with it. Position 50 is an
///       ordinary position command.
/// @param f IoFrame to populate.
/// @param src Our 3-byte controller node address (the 1W controller identity's `node_id`).
/// @param target_type Device class to address; encoded via encode_broadcast_address().
/// @param position Desired position 0–100 (0=fully open, 100=fully closed).
/// @param sequence 2-byte rolling sequence for this transmission (big-endian on wire); the
///        caller's identity/sequence store owns incrementing and persisting this, not this
///        builder — see the section note above.
/// @param controller_key 16-byte key held by the transmitting controller identity: the hub's own
///        `system_key` for its own network, or a foreign key adopted via CMD 0x30 (Phase 3A "key
///        adoption") when transmitting as an adopted identity.
/// @return true on success; false if `position > 100` or if crypto::create_1w_hmac() fails — no
///         partially-populated frame is left behind on failure.
bool create_1w_execute_position(IoFrame &f, const uint8_t src[NODE_ID_SIZE], DeviceType target_type, uint8_t position,
                                uint16_t sequence, const uint8_t controller_key[AES_KEY_SIZE]);

/// @brief Build a 1W named-command execute frame (CMD 0x00) targeting a device class.
///
/// Covers three of CoverCommand's values — STOP, FAVORITE, VENT. FORCE_OPEN is not handled here:
/// the only wire code this project has for that label, POS_FORCE_OPEN (main=0x64), was
/// hardware-tested as an outbound CMD_EXECUTE command and found to move a real device to 50%
/// open, not bypass anything (see POS_FORCE_OPEN in proto_constants.h). There is no known 1W
/// force-open encoding, so passing CoverCommand::FORCE_OPEN here falls to `default` and returns
/// false, the same way create_execute_command()'s 2W dispatch rejects it.
///   - STOP:       main=POS_STOP (0xD2),      modifier=0x00
///   - FAVORITE:   main=POS_FAVORITE (0xD8),   modifier=0x00
///   - VENT:       main=POS_FAVORITE (0xD8),   modifier=POS_VENT_MODIFIER (0x03)
///
/// @warning **These three do not rest on the same evidence, even though they read as a uniform
/// list.** STOP is the only one a real frame pins: the published IV vector
/// (tests/corpus/captures/reference_1w_vectors/oneway_execute_iv_vector.yaml) is a documented
/// worked example carrying main=0xD2. VENT is not captured anywhere in this project, but it does
/// match the reference implementation's own 1W remote byte-for-byte — `RemoteButton::Vent` in
/// reference/iohomecontrol/src/iohcRemote1W.cpp emits exactly main=0xD8/mod=0x03. FAVORITE has
/// neither kind of support: that same reference remote has no distinct favorite/My button at all
/// (its RemoteButton set is Open/Close/Stop/Vent/ForceOpen/Position/Absolute/Pair/Add/Remove/
/// Mode1-4 — no Favorite), so main=0xD8/mod=0x00 here is extrapolated purely by analogy with the
/// 2W builder's FAVORITE encoding, with no source behind it at all. Worse, the one real capture
/// this project has of an actual My/favorite button press —
/// tests/corpus/captures/somfy_awning/oneway_remote_favorite_sx1276.yaml, pinned by
/// `OneWayCommands.FavoriteButtonCaptureIsWritePrivateNotExecute` in tests/oneway_commands_test.cpp
/// — contradicts it directly: that remote's My button is CMD_WRITE_PRIVATE (0x20) with a 16-byte
/// payload, not CMD_EXECUTE with main=0xD8.
///
/// A real enrolled device does act on main=0xD8/mod=0x00, though: it changed the brightness of a
/// Somfy Izymo dimmer, so the byte is accepted, not ignored. What position it targets — a stored
/// "My" position vs. some fixed value — is unconfirmed; a 2W status-poll readback would settle
/// it. Treat FAVORITE as "does something, target unverified", not "plausibly wrong".
///
/// FORCE_OPEN is deliberately absent rather than merely untested. The reference remote's
/// `RemoteButton::ForceOpen` emits main=0x64/mod=0x00 — the same bytes this project labels
/// POS_FORCE_OPEN — so it would "match the reference implementation byte-for-byte" the same way
/// VENT does above. But matching the reference is not evidence it works: this project
/// hardware-tested that exact main byte as an outbound CMD_EXECUTE and confirmed it moves a real
/// device to 50% open (see POS_FORCE_OPEN in proto_constants.h), not past any lock. There is no
/// known 1W encoding for a real force-open, so it is unimplemented rather than shipped on a
/// mislabeled byte.
///
/// The MAC span is the 7 bytes `cmd, origin, acei, main0, main1, fp1, fp2` — the command byte
/// through fp2, stopping before the sequence, which is not frame data and never enters the MAC.
/// Pinned by the published IV vector at
/// tests/corpus/captures/reference_1w_vectors/oneway_execute_iv_vector.yaml and by the reference
/// implementation's own span (`toAdd = 6 + 1`, `reference/iohomecontrol/src/iohcRemote1W.cpp`).
/// This span is command-specific and does not generalise: CMD 0x30's span is `cmd + enc_key`
/// only — see crypto::create_1w_hmac()'s `@warning`.
/// @param f IoFrame to populate.
/// @param src Our 3-byte controller node address (the 1W controller identity's `node_id`).
/// @param target_type Device class to address; encoded via encode_broadcast_address().
/// @param cmd Named command to execute (STOP, FAVORITE, or VENT). CoverCommand::FORCE_OPEN
///        returns false — see the @warning above.
/// @param sequence 2-byte rolling sequence for this transmission (big-endian on wire); the
///        caller's identity/sequence store owns incrementing and persisting this, not this
///        builder — see the section note above.
/// @param controller_key 16-byte key held by the transmitting controller identity: the hub's own
///        `system_key` for its own network, or a foreign key adopted via CMD 0x30 (Phase 3A "key
///        adoption") when transmitting as an adopted identity.
/// @return true on success; false if `cmd` is CoverCommand::FORCE_OPEN (no 1W encoding exists) or
///         if crypto::create_1w_hmac() fails.
bool create_1w_execute_command(IoFrame &f, const uint8_t src[NODE_ID_SIZE], DeviceType target_type, CoverCommand cmd,
                               uint16_t sequence, const uint8_t controller_key[AES_KEY_SIZE]);

// ============================================================================
// 1W Enrollment (CMD 0x30 add-controller / CMD 0x39 remove-controller)
// ============================================================================
//
// Enrollment is the precondition the two builders above assume away: a device obeys a 1W frame
// only from a *registered* controller, so a correctly-keyed, correctly-signed execute from an
// unknown source node is silently ignored (see ADR 0026). These two builders are how a
// controller identity registers itself (0x30) or un-registers (0x39). Like the execute builders,
// both are pure — `sequence` and `controller_key` are parameters, never looked up or persisted —
// and both broadcast to the typed "all" address the same way create_1w_execute_command() does:
// there is no addressed/unicast form, and for a virgin 1W-only device there is usually no address
// to unicast to anyway.

/// @brief Build a 1W add-controller frame (CMD 0x30) that registers this identity as a controller
/// on every device of `target_type` currently in association mode — a physical 2 s PROG hold on
/// the receiver, which only the device's owner can trigger (see ADR 0026).
///
/// The payload wraps `controller_key` for transmission rather than sending it in the clear:
/// `enc_key = crypto::crypt_1w_key(src, controller_key)`, the same self-inverse primitive
/// decode_1w_add_controller() uses to unwrap an overheard 0x30 (crypt_1w_key() is its own
/// inverse, so this call and that one are literally the same function). The wrap key is the
/// public TRANSFER_KEY
/// (proto_constants.h) and the wrap IV derives only from `src` — see crypt_1w_key()'s doxygen for
/// why that is not a weakness: the receiver decrypts `enc_key` first, then checks this frame's MAC
/// under the key it just recovered, which is what makes the MAC meaningful (it proves the sender
/// holds the key it just sent), not the wrap's secrecy.
///
/// Declared payload (20 bytes): `enc_key[16] + manufacturer[1] + data[1]=0x01 + sequence[2]`.
/// **When `with_mac` is true, the MAC is an out-of-length trailer, not part of the declared
/// payload** — CTRL0's 5-bit length field cannot express 29 declared bytes plus a 6-byte MAC
/// together (see `IoFrame::has_mac`, `frame_carries_mac_trailer()` in proto_frame.h), so this
/// builder sets `f.has_mac = true` and fills `f.mac[]` directly; `serialize()` appends it after
/// the declared length and folds it into the length it returns, so a caller's CRC (computed over
/// that return value) covers it automatically. **When present, this is the opposite placement
/// from create_1w_remove_controller() below, whose MAC sits inside the declared payload** —
/// copying one builder's shape to make the other is the most likely bug a future edit introduces
/// here.
///
/// @warning **Real hardware and the published documentation vector disagree on whether the MAC
/// trailer exists at all — this is why `with_mac` is a parameter, not a fixed choice.** The
/// published `linklayer.md` vector (`with_mac=true`, 35 bytes) is what `with_mac` defaults to,
/// for compatibility with that vector's own pinning tests. Most real hardware captures instead
/// show 29 bytes with **no trailer at all** — see
/// `tests/corpus/captures/somfy_awning/oneway_add_and_remove_controller_sx1276.yaml` — matching
/// the reference `_p0x30` struct (`iohcPacket.h`, no `hmac` field). **The enroll button calls
/// this with `with_mac=false`** to match that shape, but this is a preference rather than a
/// hardware requirement: a real Somfy Izymo dimmer has separately been shown to accept the
/// `with_mac=true` form as well, enrolling successfully and remaining controllable afterward —
/// both shapes are safe to send.
///
/// @warning Reference divergences, deliberately not followed:
/// `reference/iown-homecontrol`'s `create_key_transfer_1w` derives the key-wrap
/// IV from the *destination* node, which contradicts the published frame this builder is pinned
/// against (a broadcast destination carries no identity, so `src` is the only value that
/// reproduces it); and its comment claims "key transfer carries no MAC", which the same published
/// frame also contradicts (its MAC is genuine and verifies) — that claim is a coincidental match
/// for what real hardware turned out to do, not evidence the comment's reasoning was right.
/// @param f IoFrame to populate.
/// @param src Our 3-byte controller node address (the 1W controller identity's `node_id`) — also
///        the key-wrap IV's only input, so this must be the identity actually being registered.
/// @param target_type Device class to address; encoded via encode_broadcast_address(). Only
///        devices of this class currently in association mode react.
/// @param manufacturer Manufacturer ID byte to advertise (`man_id`); echoed back verbatim by
///        anything that later decodes this frame with decode_1w_add_controller().
/// @param sequence 2-byte rolling sequence for this transmission (big-endian on wire); the caller's
///        identity/sequence store owns incrementing and persisting this, not this builder.
/// @param controller_key 16-byte key this identity registers itself with — normally the hub's own
///        `system_key`, wrapped here for transmission, never sent in the clear.
/// @param with_mac True to append the out-of-length MAC trailer (the published vector's shape,
///        and this parameter's default for backward compatibility); false to omit it entirely
///        (the shape most real hardware captures this project holds actually use, and what the
///        enroll button passes). Real hardware has been shown to accept both — see the
///        `@warning` above.
/// @return true on success; false if crypto::crypt_1w_key() fails, or (when `with_mac` is true)
///         crypto::create_1w_hmac() fails — no partially-populated frame is left behind on failure.
bool create_1w_add_controller(IoFrame &f, const uint8_t src[NODE_ID_SIZE], DeviceType target_type, uint8_t manufacturer,
                              uint16_t sequence, const uint8_t controller_key[AES_KEY_SIZE], bool with_mac = true);

/// @brief Build a 1W remove-controller frame (CMD 0x39) that un-registers this identity from every
/// device of `target_type` currently in association mode — the un-enroll counterpart to
/// create_1w_add_controller(), and (per the documented `0x39` → `0x30` flow, `linklayer.md:396`)
/// also the prelude OneWayTransmitter::send_enrollment() fires immediately before it.
///
/// Declared payload (9 bytes), shape matching the reference `_p0x2e` struct: `data[1]=0x00 +
/// sequence[2] + mac[6]`. **The MAC sits inside the declared payload here** — the opposite
/// placement from create_1w_add_controller() above, whose MAC is an out-of-length trailer because
/// its longer payload has no room left in CTRL0's 5-bit length field. `f.has_mac` stays false.
///
/// The MAC span is `cmd + data` (2 bytes) — the same "everything before the sequence" shape CMD
/// 0x00's span follows, though spans are command-specific and do not generalise on their own (see
/// create_1w_hmac()'s `@warning`; CMD 0x30's span is `cmd + enc_key`, a different rule). This one
/// is verified against real captures, not merely plausible: every `0x39` frame in
/// `tests/corpus/captures/somfy_awning/oneway_add_and_remove_controller_sx1276.yaml` verifies
/// under this exact span, and `scripts/corpus/validate.py` re-checks that on every corpus
/// validation run — a regression here would fail `make corpus-validate`, not just look wrong.
/// @param f IoFrame to populate.
/// @param src Our 3-byte controller node address (the 1W controller identity's `node_id`).
/// @param target_type Device class to address; encoded via encode_broadcast_address().
/// @param sequence 2-byte rolling sequence for this transmission (big-endian on wire); the caller's
///        identity/sequence store owns incrementing and persisting this, not this builder.
/// @param controller_key 16-byte key held by the identity being removed — the same key
///        create_1w_execute_command()/create_1w_add_controller() would use for this identity.
/// @return true on success; false if crypto::create_1w_hmac() fails — no partially-populated frame
///         is left behind on failure.
bool create_1w_remove_controller(IoFrame &f, const uint8_t src[NODE_ID_SIZE], DeviceType target_type, uint16_t sequence,
                                 const uint8_t controller_key[AES_KEY_SIZE]);

/// Build a get‑status request (0x03). The device responds with its current position.
/// @param f IoFrame to populate.
/// @param own Controller's 3‑byte node ID.
/// @param dst Target device's 3‑byte node ID.
/// @return true on success.
bool create_get_status(IoFrame &f, const uint8_t *own, const uint8_t *dst);

/// @brief Build a CMD_GET_GENERAL_INFO3 (0x58) request. No payload.
///
/// Byte-for-byte like create_get_name() above.
/// @param f IoFrame to populate.
/// @param own Controller's 3-byte node ID.
/// @param dst Target device's 3-byte node ID.
/// @param low_power True if target is battery/solar-powered (sets CTRL1_LOW_POWER).
/// @return true on success.
bool create_general_info3(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power);

/// Build a get-name request (0x50). The device responds with its stored display name.
/// @param f IoFrame to populate.
/// @param own Controller's 3-byte node ID.
/// @param dst Target device's 3-byte node ID.
/// @param low_power True if target is battery/solar-powered (sets CTRL1_LOW_POWER).
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

/// @brief Build an authenticated device-identify request (0x1E) that makes a device
/// physically identify itself (brief jog / flash).
///
/// @param f IoFrame to populate.
/// @param own Controller's 3-byte node ID (source address).
/// @param dst Target device's 3-byte node ID (destination address).
/// @note The device may reply with CMD_ERROR_RESP instead of a dedicated identify response;
///       callers should treat that reply as an expected, non-fatal outcome rather than a failure.
/// @return true on success.
bool create_identify(IoFrame &f, const uint8_t *own, const uint8_t *dst);

/// Build an execute‑tilt command (0x00) for slat angle control.
/// @param f IoFrame to populate.
/// @param own Controller node ID.
/// @param dst Target device node ID.
/// @param low_power True if target is battery/solar‑powered (sets CTRL1_LOW_POWER).
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
/// @param low_power True if target is battery/solar‑powered (sets CTRL1_LOW_POWER).
/// @param position Desired position 0–100 (open→closed).
/// @param tilt_percent 0 = fully closed, 100 = fully open.
/// @return true on success; false if position exceeds limits.
bool create_execute_position_and_tilt(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power,
                                      uint8_t position, uint8_t tilt_percent);

/// @brief Build an extended CMD_PRIVATE (0x03) request with a selector/block pair.
///
/// The shape real hubs use for both the tilt block (selector STATUS_TILT_SELECTOR) and the
/// field-observed selector 0x80 (tests/corpus/captures/issues/
/// issue_45_extended_private_both_selectors.yaml), which this codebase has never decoded.
/// create_get_status_tilt() below is this builder frozen at selector = STATUS_TILT_SELECTOR,
/// block = 0x01.
/// @param f IoFrame to populate.
/// @param own Controller's 3-byte node ID.
/// @param dst Target device's 3-byte node ID.
/// @param selector Extended-status selector byte (data[1]).
/// @param block Selector-specific block/index byte (data[2]) — the field-observed name for
///        this byte is "N" for selector 0x80, where the corpus has only ever observed 0x00/0x01.
/// @return true on success.
bool create_get_status_extended(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t selector, uint8_t block);

/// Build a tilt‑aware get‑status request (0x03 with extended payload) that returns
/// the 16‑byte tilt block in the response.
/// @param f IoFrame to populate.
/// @param own Controller node ID.
/// @param dst Target device node ID.
/// @return true on success.
bool create_get_status_tilt(IoFrame &f, const uint8_t *own, const uint8_t *dst);

/// @brief Build a CMD_PRIVATE2 (0x0C) request in either of the two field-observed shapes.
///
/// The payload is CMD_EXECUTE's POS_FAVORITE/POS_VENT_MODIFIER stored-position selector with
/// the execution prefix stripped. `low_power` is an explicit parameter like every other
/// device-addressed builder in this file, not derived from `long_form`: the two captures this
/// builder is pinned against happen to carry CTRL1_LOW_POWER set on the long-form request and
/// clear on the short-form one, but that tracks each capture's *target device's* power class
/// (solar shutter vs. mains switch), not the payload shape — see proto_commands.cpp for the
/// full reasoning.
/// @param f IoFrame to populate.
/// @param own Controller's 3-byte node ID.
/// @param dst Target device's 3-byte node ID.
/// @param modifier The POS_FAVORITE/POS_VENT_MODIFIER-family selector byte to read back.
/// @param long_form True for the 6-byte long form (data = POS_UNKNOWN, 0x00, 0x80,
///        POS_FAVORITE, modifier, 0x00); false for the 4-byte short form (data = POS_FAVORITE,
///        modifier, 0x00, 0x00).
/// @param low_power True if target is battery/solar-powered (sets CTRL1_LOW_POWER).
/// @return true on success.
bool create_private2_read(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t modifier, bool long_form,
                          bool low_power);

/// @brief Build a discovery request with configurable command, destination, and payload.
///
/// Supports the command codes 0x28 (DISCOVER_REQ), 0x2A (DISCOVER_SPE_REQ), and
/// 0x2E (DISCOVER_ALT_REQ, alternate discovery). For 0x2A the payload is a 6-byte random nonce
/// followed by a 6-byte HMAC computed over [cmd + nonce] using the supplied system key.
///
/// @param f IoFrame to populate.
/// @param own Controller's 3-byte node ID.
/// @param command Discovery command code (0x28, 0x2A, or 0x2E).
/// @param dst Destination node ID (broadcast or explicit).
/// @param low_power True to set the LOW_POWER flag in CTRL1.
/// @param payload_enabled True when the optional payload byte is enabled.
/// @param payload Optional payload byte (only used when command requires a payload).
/// @param system_key 16-byte system key; only used for 0x2A HMAC computation.
/// @return true on success; false for unsupported command or missing key for 0x2A.
bool create_discovery_request(IoFrame &f, const uint8_t *own, uint8_t command, const uint8_t *dst, bool low_power,
                              bool payload_enabled, uint8_t payload, const uint8_t *system_key);

/// Build a discovery broadcast (0x28). Sent to the broadcast address; only devices
/// in pairing mode (PROG button pressed) will respond.
/// @param f IoFrame to populate.
/// @param own Controller node ID.
/// @note Destination is BROADCAST_DISCOVER (0x00003B). The device responds with
///       CMD_DISCOVER_RESP (0x29) containing its node ID and type/subtype. The
///       controller then switches to point‑to‑point communication for phases 2 and 3.
/// @return true on success.
bool create_discover(IoFrame &f, const uint8_t *own);

/// @brief Build a discovery response (0x29) — the device side of discovery, used by the
/// key-extraction responder (see pairing_responder.h) to emulate an unpaired device.
///
/// Almost every builder in this file speaks the *controller* side of the protocol; this one,
/// create_key_confirm(), create_discover_confirm_ack(), and create_challenge_req_device_role()
/// below speak the *device* side, needed only for that one reverse-role feature. Device-side
/// frames never set CTRL1_LOW_POWER: that bit describes the *target* of a controller-originated
/// frame, and a device's replies are addressed to a mains-powered hub.
/// Payload layout matches the full 9-byte discovery response format documented at
/// DISCOVERY_RESP_BACKBONE_OFFSET/_MANUFACTURER_OFFSET/_FLAGS_OFFSET/_TIMESTAMP_OFFSET
/// (proto_constants.h), cross-checked against a real Somfy actuator's captured 0x29
/// (tests/corpus/captures/somfy_awning/pairing_lab_discovery_response.yaml): backbone address
/// equals the device's own node ID, start+end set, low_power clear.
/// @param f IoFrame to populate.
/// @param own Our advertised (throwaway) node ID — used as both src and the backbone address.
/// @param dst Destination node ID (the discovering hub's real node ID, from its 0x28's src).
/// @param type Device type to advertise.
/// @param subtype Device subtype to advertise.
/// @param manufacturer_id Manufacturer ID to advertise (see MANUFACTURER_* in proto_constants.h).
/// @note The flags byte (turnaround class, power-save) and timestamp are best-effort placeholder
///       values (0x00) — a real hub may require different values; unverified without hardware.
/// @return true on success.
bool create_discover_resp(IoFrame &f, const uint8_t *own, const uint8_t *dst, DeviceType type, uint8_t subtype,
                          uint8_t manufacturer_id);

/// @brief Build a key-confirm frame (0x33) — the device's acknowledgement that it received and
/// installed the system key, sent after decrypting a CMD_KEY_TRANSFER (0x32).
///
/// Device-side counterpart to create_key_transfer(); used only by the key-extraction responder
/// (see create_discover_resp() above for why this direction exists at all). No payload and END
/// set, matching real devices' 0x33 in
/// tests/corpus/captures/somfy_dimmer/pairing_full.yaml, velux_kux100/pairing_full.yaml, and (this
/// project's own key-extraction responder against a real hub)
/// tests/corpus/captures/issues/issue_45_velux_kig300_key_extraction_success.yaml —
/// 0x33 closes the key-exchange sequence that CMD_KEY_INIT (0x31) opened with START.
/// @param f IoFrame to populate.
/// @param own Our advertised (throwaway) node ID.
/// @param dst Destination node ID (the hub that sent the key transfer).
/// @return true on success.
bool create_key_confirm(IoFrame &f, const uint8_t *own, const uint8_t *dst);

/// @brief Build a discovery-confirm acknowledgement (0x2D) — the device's answer to a hub's
/// CMD_DISCOVER_CONFIRM (0x2C), which a hub sends directly to a freshly-discovered device before
/// it will proceed to the key exchange.
///
/// Device-side only, like create_discover_resp()/create_key_confirm() above; this project's own
/// controller role never sends 0x2C, so there is no counterpart builder for the other direction.
/// No payload and END set, matching real devices' 0x2D in
/// tests/corpus/captures/velux_kux100/pairing_full.yaml; for a second independent hub,
/// tests/corpus/captures/issues/issue_45_somfy_connectivity_kit_key_extraction_stall.yaml, where
/// an already-paired device answers the same hub the key-extraction responder was talking to; and
/// for this exact builder exercised against a real hub,
/// tests/corpus/captures/issues/issue_45_velux_kig300_key_extraction_success.yaml.
/// @param f IoFrame to populate.
/// @param own Our advertised (throwaway) node ID.
/// @param dst Destination node ID (the hub that sent the discovery confirm).
/// @return true on success.
bool create_discover_confirm_ack(IoFrame &f, const uint8_t *own, const uint8_t *dst);

/// @brief Recover the system key from an inbound CMD_KEY_TRANSFER (0x32) payload — the decode
/// counterpart to create_key_transfer()'s encode.
///
/// Centralizes the IV-`data` convention in one place: create_key_transfer() derives its IV from
/// the *preceding* CMD_KEY_INIT (0x31) command byte only (see its own doxygen), so decoding must
/// use that same single-byte `{CMD_KEY_INIT}` — not the 0x32 frame's own command byte, and not
/// the discovery frame. crypt_key() is symmetric, so this is the same primitive in reverse.
/// @param transfer_payload 16-byte CMD_KEY_TRANSFER payload (frame.data).
/// @param challenge The 6-byte challenge *we* generated and sent in our own CMD_CHALLENGE_REQ
///        (0x3C) — the far side mixes this into its IV, so decoding requires the exact same bytes.
/// @param out_key Output: recovered 16-byte system key.
/// @return true on success (crypt_key() AES failure is the only false case).
bool recover_system_key_from_transfer(const uint8_t transfer_payload[AES_KEY_SIZE], const uint8_t challenge[HMAC_SIZE],
                                      uint8_t out_key[AES_KEY_SIZE]);

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

/// @brief Build a challenge request (0x3C) using a caller-supplied challenge instead of
/// generating a fresh one internally.
///
/// The no-challenge overload above generates its own random bytes and does not expose them,
/// which is fine for the normal inbound-auth path (the challenge is only ever needed once, to
/// build this same frame). A caller that needs the *exact* bytes again later — the key-extraction
/// responder decrypting the corresponding CMD_KEY_TRANSFER (0x32) — generates the challenge itself
/// and passes it in here, keeping the transmitted 0x3C and the later decrypt on one source of
/// truth. Note that responder uses create_challenge_req_device_role() below, not this overload.
/// @param f IoFrame to populate.
/// @param dst Target device node ID (device we're challenging).
/// @param src Our own node ID.
/// @param challenge Caller-supplied 6-byte challenge (e.g. from crypto::generate_challenge()).
/// @return true on success.
bool create_challenge_req(IoFrame &f, const uint8_t *dst, const uint8_t *src, const uint8_t challenge[HMAC_SIZE]);

/// @brief Build a challenge request (0x3C) in the *device* direction — used only by the
/// key-extraction responder to challenge a foreign hub that sent us CMD_KEY_INIT (0x31).
///
/// Same command and payload as the controller-role builders above, but framed the way a real
/// device frames it: START clear and LOW_POWER clear. Both controller-role overloads set both
/// bits, which is correct for their direction — LOW_POWER describes the *target* of a
/// controller-originated frame (a device that may be battery/solar powered, see this header's
/// convention note), and the controller's 0x3C opens its own inbound-auth exchange. Neither holds
/// for a device answering a hub's key-init: real devices' pairing 0x3C frames in
/// tests/corpus/captures/somfy_dimmer/pairing_full.yaml (`0E 00 …`) and
/// velux_kux100/pairing_full.yaml carry neither bit, because the frame is a continuation of the
/// hub's already-open exchange and is addressed to a mains-powered hub — and this exact builder,
/// exercised against a real hub, produces the identical `0E 00` shape in
/// tests/corpus/captures/issues/issue_45_velux_kig300_key_extraction_success.yaml.
/// @param f IoFrame to populate.
/// @param dst The foreign hub's node ID (from the inbound 0x31's src).
/// @param src Our advertised (throwaway) node ID.
/// @param challenge Caller-supplied 6-byte challenge, retained for the later 0x32 decrypt.
/// @return true on success.
bool create_challenge_req_device_role(IoFrame &f, const uint8_t *dst, const uint8_t *src,
                                      const uint8_t challenge[HMAC_SIZE]);

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

/// @brief Build an address response (0x37) — device side, answering a hub's CMD_ADDRESS_REQ
/// (0x36).
///
/// Payload is our own advertised node ID — the only identity this emulated device has to offer —
/// the same value create_discover_resp() reports at DISCOVERY_RESP_BACKBONE_OFFSET.
/// CorpusDeviceRoleBuilders.AddressRespPayloadMatchesOwnDiscoverRespBackboneAddress
/// (tests/corpus_device_role_builder_test.cpp) pins that these two builders agree with *each
/// other*, not that this matches a real device's own backbone value: the one real capture of this
/// exchange (tests/corpus/captures/velux_kux100/pairing_full.yaml) shows a genuine device whose 0x37
/// payload — a persistent identity the io-homecontrol wire format tracks separately from a device's
/// node/session address — does NOT equal that device's own node ID. Our emulated device only ever
/// generates one identity per arm cycle, so it structurally cannot reproduce a real device's
/// separate backbone value; whether any real hub requires the two to differ, or even inspects this
/// field at all rather than treating it as informational, is unconfirmed. See docs/home_io_control.md's
/// Key Extraction "Known limitations" for the field-facing version of this note.
/// Also unverified: the full CTRL1 framing. This builder's ctrl1 is 0x00 (see init_frame() call in
/// the .cpp), but the KLR200 capture's 0x37 carries CTRL1_PRIORITY set. See the
/// TODO(hardware-verify) on the .cpp definition for why that bit is not mirrored here.
/// No controller-role counterpart exists to share with: this codebase has never sent 0x36, so there
/// is no analogous controller-role code that receives a 0x37 to keep in sync with.
/// @param f IoFrame to populate.
/// @param own Our advertised (throwaway) node ID — used as both src and the payload.
/// @param dst Destination node ID (the hub that sent the address request, from its 0x36's src).
/// @return true on success.
bool create_address_resp_device_role(IoFrame &f, const uint8_t *own, const uint8_t *dst);

/// @brief Build a challenge response (0x3D) in the *device* direction — used only by the
/// key-extraction responder to answer a hub-issued CMD_CHALLENGE_REQ (0x3C) challenging our own
/// CMD_ADDRESS_RESP (0x37).
///
/// Same transcript rule as create_challenge_resp() above (the challenged party HMACs its own
/// preceding frame's cmd+data), but END is set: this 0x3D closes the address-verification round the
/// hub opened with 0x36 (tests/corpus/captures/velux_kux100/pairing_full.yaml's `8E 08 …`, END
/// set). LOW_POWER is clear because this hub is mains-powered and never sleeps, the same
/// self-description create_discover_resp() already advertises via POWER_SAVE_ALWAYS_ALIVE. Neither
/// bit is a blanket "device role" convention — real devices also send mid-exchange 0x3D frames with
/// END clear and LOW_POWER set (somfy_rs100_oximo/exchange_oximo40_sx1262.yaml) — so this framing is
/// specific to the terminal shape this feature needs, not a general device-role rule.
/// @param f IoFrame to populate.
/// @param dst The hub's node ID (from the inbound 0x3C's src).
/// @param src Our advertised (throwaway) node ID.
/// @param challenge 6-byte challenge from the hub's 0x3C.
/// @param origin Our own preceding CMD_ADDRESS_RESP (0x37) frame — its cmd+data is the transcript.
/// @param key System key (16 bytes).
/// @return true on success.
bool create_challenge_resp_device_role(IoFrame &f, const uint8_t *dst, const uint8_t *src,
                                       const uint8_t challenge[HMAC_SIZE], const IoFrame &origin, const uint8_t *key);

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
