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
bool create_execute_position(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t position);

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
bool create_execute_command(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, CoverCommand cmd);

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
///       environmental lock — see analysis/reference_combined_integration.md item 5.
bool create_force_open(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t open_position);

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
/// payload, not CMD_EXECUTE with main=0xD8. Treat FAVORITE as untested and plausibly wrong, not
/// merely unconfirmed, until some capture shows a device actually accepting 0xD8 as a 1W favorite.
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

/// Build a tilt‑aware get‑status request (0x03 with extended payload) that returns
/// the 16‑byte tilt block in the response.
/// @param f IoFrame to populate.
/// @param own Controller node ID.
/// @param dst Target device node ID.
/// @return true on success.
bool create_get_status_tilt(IoFrame &f, const uint8_t *own, const uint8_t *dst);

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
/// Every other builder in this file speaks the *controller* side of the protocol; this one and
/// create_key_confirm() below speak the *device* side, needed only for that one reverse-role
/// feature. Payload layout matches the full 9-byte discovery response format documented at
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
/// (see create_discover_resp() above for why this direction exists at all). No payload, matching
/// the reconstructed-but-high-confidence 0x33 in
/// tests/corpus/captures/issues/field_rs100_pairing_key_exchange_retry_success.yaml (no real 0x33
/// has been captured raw anywhere in this project).
/// @param f IoFrame to populate.
/// @param own Our advertised (throwaway) node ID.
/// @param dst Destination node ID (the hub that sent the key transfer).
/// @return true on success.
bool create_key_confirm(IoFrame &f, const uint8_t *own, const uint8_t *dst);

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
/// build this same frame). The key-extraction responder (pairing_responder.h) needs the *exact*
/// bytes again later to decrypt the corresponding CMD_KEY_TRANSFER (0x32), so it generates the
/// challenge itself and passes it in here — this overload is what keeps both call sites (the
/// transmitted 0x3C and the later decrypt) using the same source of truth.
/// @param f IoFrame to populate.
/// @param dst Target node ID (device or, for the key-extraction responder, the foreign hub).
/// @param src Our own node ID.
/// @param challenge Caller-supplied 6-byte challenge (e.g. from crypto::generate_challenge()).
/// @return true on success.
bool create_challenge_req(IoFrame &f, const uint8_t *dst, const uint8_t *src, const uint8_t challenge[HMAC_SIZE]);

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
