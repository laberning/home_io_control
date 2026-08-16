/// @file proto_commands.cpp
/// @brief Command builders for the IO-Homecontrol protocol.
/// @ingroup hioc_protocol

#include "proto_commands.h"

#include "proto_constants.h"
#include "proto_crypto.h"

#include <array>
#include <cstring>

namespace esphome {
namespace home_io_control {

namespace {

// === Command payload templates ===

/// The protocol uses 0-100 for percentage-style position inputs before encoding them on wire.
constexpr uint8_t POSITION_PERCENT_MAX = 100;
/// Byte 0 in execute-family payloads identifies a user-originated remote action.
/// Uses the public ORIGINATOR_USER_REMOTE constant from proto_frame.h.
constexpr uint8_t EXECUTE_ORIGINATOR = ORIGINATOR_USER_REMOTE;
/// ACEI byte for execute commands — composed from priority and validity bits.
///
/// Level=3 (user_default), matching what a real 2W hub puts on the air: a third-party capture of a
/// Velux KIG300 commanding a Somfy RS100 shows its EXECUTE payload as `01 63 C8 00 80 32 00 00` —
/// ACEI 0x63, level 3. The 0x43 (user_high) alternative comes from a 1W remote reference vector
/// (tests/corpus/captures/reference_1w_vectors/oneway_execute_iv_vector.yaml), not a 2W hub, and a
/// handheld remote claiming a higher priority than a home-automation hub is unsurprising.
///
/// A device locked at level 3 rejects this with RESULT_PRIORITY_LOCKED_NON_EXEC (0x38) rather than
/// silence. Watch command results for 0x38 after touching this value; if it appears, level=2
/// (0x43) is the fallback and the priority/reliability tradeoff is real.
///
/// Composition: (ACEI_LEVEL_USER_DEFAULT << 5) | (0 << 3) | (1 << 1) | 1 = 0x63.
constexpr uint8_t EXECUTE_ACEI =
    (ACEI_LEVEL_USER_DEFAULT << ACEI_LEVEL_SHIFT) | (1 << ACEI_EXTENDED_SHIFT) | ACEI_VALID_BIT;
/// @brief ACEI byte for the force-open command — same bit layout as EXECUTE_ACEI but at the
/// highest priority level instead of user_high.
///
/// The protocol's only documented override mechanism for an environmental soft lock (e.g. a
/// wind/rain sensor holding a device at its secured position) is ACEI priority elevation: a node
/// locked at some priority level rejects any command at that level or below (result codes
/// RESULT_PRIORITY_LEVEL_LOCKED / RESULT_PRIORITY_LOCKED_NON_EXEC) and only a strictly
/// higher-priority command gets through. Real captures of this repo's own wind/rain sensor
/// traffic show it locks at ACEI_LEVEL_PROTECTION_SENSOR (1), so force-open uses level 0
/// (protection_human, the highest level)
/// Composition: (ACEI_LEVEL_PROTECTION_HUMAN << 5) | (1 << 1) | 1 = 0x03.
/// @note Real-hardware testing confirmed this correctly moves a device to fully open (see
///       create_force_open()'s position-inversion note), but elevation to level 0 has not yet
///       been confirmed to actually override an *active* lock — only that the device accepts
///       the frame when nothing is locking it.
constexpr uint8_t EXECUTE_ACEI_FORCE_OPEN =
    (ACEI_LEVEL_PROTECTION_HUMAN << ACEI_LEVEL_SHIFT) | (1 << ACEI_EXTENDED_SHIFT) | ACEI_VALID_BIT;
/// Standard payload length for full execute-family commands.
constexpr size_t EXECUTE_PAYLOAD_SIZE = 8;
/// Bit flag that marks the standard position payload layout after the encoded position byte.
constexpr uint8_t EXECUTE_POSITION_LAYOUT_FLAG = 0x80;
/// Travel-profile byte for a normal-speed move — the last field of the extended execute block.
constexpr uint8_t EXECUTE_POSITION_PROFILE = 0x06;
/// Travel-profile byte for a silent (slow) move — same field, selecting reduced motor speed.
///
/// A Somfy hub commanding one RS100, same command and direction, with only the app's "silent
/// operation" toggle flipped, differs by exactly this one byte (`... D8 06 00` normal vs.
/// `... D8 05 00` silent); this hub follows Somfy's encoding rather than Velux's (which uses a
/// different byte, `80 32 00 00`, and omits the extended block entirely for a normal move) because
/// this hub's own hardware matches Somfy byte for byte.
constexpr uint8_t EXECUTE_PROFILE_SILENT = 0x05;
/// Short payload length for special execute commands such as stop/favorite.
constexpr size_t EXECUTE_SPECIAL_PAYLOAD_SIZE = 6;
/// Private sub-command for position status requests.
constexpr uint8_t PRIVATE_GET_POSITION_STATUS = 0x03;
/// Status-update acknowledgement payload matched from controller traffic.
constexpr uint8_t STATUS_UPDATE_ACK_PAYLOAD[] = {0x05, 0x00};
/// Set-config payload that enables automatic status updates from the device.
constexpr uint8_t SET_CONFIG1_STATUS_BROADCAST_PAYLOAD[] = {0xE0, 0x10, 0x0A, 0x08, 0x00};
/// Identify-request parameter byte (data[1] of the CMD_IDENTIFY payload).
constexpr uint8_t IDENTIFY_PARAMETER = 0xFF;

/// @brief Build the standard 8-byte position payload shared by create_execute_position() and
/// create_force_open() — identical except for the ACEI byte.
inline std::array<uint8_t, EXECUTE_PAYLOAD_SIZE> make_position_payload(uint8_t acei, uint8_t position,
                                                                       bool silent = false) {
  // Only the profile byte changes; the non-silent form is untouched and is byte-identical to what
  // a Somfy hub sends for a normal-speed move.
  return {EXECUTE_ORIGINATOR,
          acei,
          static_cast<uint8_t>(2 * position),
          0x00,
          EXECUTE_POSITION_LAYOUT_FLAG,
          POS_FAVORITE,
          silent ? EXECUTE_PROFILE_SILENT : EXECUTE_POSITION_PROFILE,
          0x00};
}

}  // namespace

/// Build a position execute command (0x00) to move a device to a numeric position.
bool create_execute_position(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t position,
                             bool silent) {
  if (position > POSITION_PERCENT_MAX)
    return false;
  init_frame(f, true, true, false, low_power);
  set_dst(f, dst);
  set_src(f, own);
  const auto payload = make_position_payload(EXECUTE_ACEI, position, silent);
  return set_cmd(f, CMD_EXECUTE, payload.data(), payload.size());
}

/// Build a named-command execute frame (0x00) for STOP, FAVORITE, or VENT.
///
/// FORCE_OPEN is deliberately not handled here — unlike these three, it needs to know the
/// device's wire-scale "fully open" position (0 or 100 depending on IoDevice::inverted, e.g.
/// horizontal awnings), which this builder has no way to know. Use create_force_open() instead;
/// see its comments for why.
bool create_execute_command(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, CoverCommand cmd,
                            bool silent) {
  uint8_t main_byte = 0;
  uint8_t modifier_byte = 0;
  switch (cmd) {
    case CoverCommand::STOP:
      main_byte = POS_STOP;
      modifier_byte = 0x00;
      break;
    case CoverCommand::FAVORITE:
      main_byte = POS_FAVORITE;
      modifier_byte = 0x00;
      break;
    case CoverCommand::VENT:
      main_byte = POS_FAVORITE;
      modifier_byte = POS_VENT_MODIFIER;
      break;
    default:
      return false;
  }
  init_frame(f, true, true, false, low_power);
  set_dst(f, dst);
  set_src(f, own);
  // FAVORITE is the one command here the capture covers: a Somfy hub sends the plain 6-byte form
  // for a normal "My" press and the extended 8-byte form with the silent profile when the toggle
  // is on, which is exactly the pair below. STOP is excluded because stopping has no travel speed,
  // and VENT because nothing has been captured for it — every extended frame observed so far has
  // byte 3 clear, whereas VENT puts its modifier there, so extending it would be a guess.
  if (silent && cmd == CoverCommand::FAVORITE) {
    const uint8_t extended[EXECUTE_PAYLOAD_SIZE] = {
        EXECUTE_ORIGINATOR, EXECUTE_ACEI,           main_byte, modifier_byte, EXECUTE_POSITION_LAYOUT_FLAG,
        POS_FAVORITE,       EXECUTE_PROFILE_SILENT, 0x00};
    return set_cmd(f, CMD_EXECUTE, extended, sizeof(extended));
  }
  const uint8_t payload[EXECUTE_SPECIAL_PAYLOAD_SIZE] = {EXECUTE_ORIGINATOR, EXECUTE_ACEI, main_byte,
                                                         modifier_byte,      0x00,         0x00};
  return set_cmd(f, CMD_EXECUTE, payload, sizeof(payload));
}

/// Build a force-open execute frame (0x00): an ordinary position command to the device's
/// wire-scale "fully open" value, sent at elevated ACEI priority (see EXECUTE_ACEI_FORCE_OPEN).
///
/// Takes the target position explicitly rather than assuming 0, because "fully open" is not
/// always wire-position 0: IoDevice::inverted devices (e.g. horizontal awnings) have open/close
/// swapped, so their fully-open wire position is 100. An earlier version of this builder
/// hardcoded 0; on a real inverted awning that targeted its already-*closed* resting position, a
/// real-hardware-confirmed no-op rather than a lock bypass. The caller (execute_device_command_()
/// in hub_operations.cpp) is responsible for resolving the correct value from the target IoDevice.
bool create_force_open(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t open_position) {
  init_frame(f, true, true, false, low_power);
  set_dst(f, dst);
  set_src(f, own);
  const auto payload = make_position_payload(EXECUTE_ACEI_FORCE_OPEN, open_position);
  return set_cmd(f, CMD_EXECUTE, payload.data(), payload.size());
}

/// Build a get-status request (0x03). The device responds with its current position.
bool create_get_status(IoFrame &f, const uint8_t *own, const uint8_t *dst) {
  // low_power=true for solar devices.
  init_frame(f, true, true, false, true);
  set_dst(f, dst);
  set_src(f, own);
  // Private sub-command = get position status.
  uint8_t d[3] = {PRIVATE_GET_POSITION_STATUS, 0x00, 0x00};
  return set_cmd(f, CMD_PRIVATE, d, sizeof(d));
}

bool create_get_name(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power) {
  init_frame(f, true, true, false, low_power);
  set_dst(f, dst);
  set_src(f, own);
  return set_cmd(f, CMD_GET_NAME);
}

bool create_set_name(IoFrame &f, const uint8_t *own, const uint8_t *dst,
                     const uint8_t payload[DEVICE_NAME_WRITE_PAYLOAD_SIZE]) {
  // low_power=true: matches every other frame addressed to a specific device (see
  // create_get_status/create_key_init) — solar/battery devices need CTRL1_LOW_POWER set on
  // every frame sent to them, not only the initiating request.
  init_frame(f, true, true, false, true);
  set_dst(f, dst);
  set_src(f, own);
  return set_cmd(f, CMD_SET_NAME, payload, DEVICE_NAME_WRITE_PAYLOAD_SIZE);
}

/// Build an authenticated device-identify request (0x1E).
bool create_identify(IoFrame &f, const uint8_t *own, const uint8_t *dst) {
  // low_power=true (see create_set_name above).
  init_frame(f, true, true, false, true);
  set_dst(f, dst);
  set_src(f, own);
  const uint8_t payload[2] = {ORIGINATOR_USER_REMOTE, IDENTIFY_PARAMETER};
  return set_cmd(f, CMD_IDENTIFY, payload, sizeof(payload));
}

/// Build a tilt execute command (0x00) for devices that support slat angle control.
bool create_execute_tilt(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t tilt_percent) {
  init_frame(f, true, true, false, low_power);
  set_dst(f, dst);
  set_src(f, own);

  auto const tilt_value =
      static_cast<uint16_t>((POSITION_PERCENT_MAX - tilt_percent) * STATUS_POS_MAX / POSITION_PERCENT_MAX);
  uint8_t d[EXECUTE_PAYLOAD_SIZE] = {EXECUTE_ORIGINATOR,
                                     EXECUTE_ACEI,
                                     POS_UNKNOWN,
                                     0x00,
                                     STATUS_TILT_SELECTOR,
                                     static_cast<uint8_t>(tilt_value >> BITS_PER_BYTE),
                                     static_cast<uint8_t>(tilt_value),
                                     0x00};
  return set_cmd(f, CMD_EXECUTE, d, sizeof(d));
}

/// Build a combined position-and-tilt execute command (0x00) — setClosureAndOrientation.
bool create_execute_position_and_tilt(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power,
                                      uint8_t position, uint8_t tilt_percent) {
  if (position > POSITION_PERCENT_MAX)
    return false;
  init_frame(f, true, true, false, low_power);
  set_dst(f, dst);
  set_src(f, own);

  auto const tilt_value =
      static_cast<uint16_t>((POSITION_PERCENT_MAX - tilt_percent) * STATUS_POS_MAX / POSITION_PERCENT_MAX);
  uint8_t d[EXECUTE_PAYLOAD_SIZE] = {EXECUTE_ORIGINATOR,
                                     EXECUTE_ACEI,
                                     static_cast<uint8_t>(2 * position),
                                     0x00,
                                     STATUS_TILT_SELECTOR,
                                     static_cast<uint8_t>(tilt_value >> BITS_PER_BYTE),
                                     static_cast<uint8_t>(tilt_value),
                                     0x00};
  return set_cmd(f, CMD_EXECUTE, d, sizeof(d));
}

/// Build a tilt-aware get-status request (0x03) that returns the extended 16-byte tilt payload.
bool create_get_status_tilt(IoFrame &f, const uint8_t *own, const uint8_t *dst) {
  init_frame(f, true, true, false, true);
  set_dst(f, dst);
  set_src(f, own);
  // The selector byte switches the private status response to the extended tilt layout.
  uint8_t d[4] = {PRIVATE_GET_POSITION_STATUS, STATUS_TILT_SELECTOR, 0x01, 0x00};
  return set_cmd(f, CMD_PRIVATE, d, sizeof(d));
}

/// Build a discovery broadcast (0x28). Sent to the broadcast address 0x00003B.
/// Only devices in pairing mode (PROG button pressed) will respond.
bool create_discover(IoFrame &f, const uint8_t *own) {
  // start+end: single broadcast frame.
  init_frame(f, true, true, true, false);
  set_dst(f, BROADCAST_DISCOVER);
  set_src(f, own);
  return set_cmd(f, CMD_DISCOVER_REQ);
}

/// Build a configurable discovery request command (0x28, 0x2A, or 0x2E).
///
/// For 0x2A (Discover SPE), the payload is a 6-byte random nonce followed by a 6-byte
/// HMAC over the command byte alone, using that nonce as the challenge and the supplied
/// system key — one frame carrying a whole challenge-response, which is what lets a
/// broadcast be authenticated. This requires a valid system key; it will not work for a
/// motor that has never been paired with this controller's key.
bool create_discovery_request(IoFrame &f, const uint8_t *own, uint8_t command, const uint8_t *dst, bool low_power,
                              bool payload_enabled, uint8_t payload, const uint8_t *system_key) {
  init_frame(f, true, true, true, low_power);
  set_dst(f, dst);
  set_src(f, own);

  switch (command) {
    case CMD_DISCOVER_REQ:
      return set_cmd(f, CMD_DISCOVER_REQ);

    case CMD_DISCOVER_SPE_REQ: {
      if (system_key == nullptr)
        return false;
      uint8_t nonce[HMAC_SIZE];
      crypto::generate_challenge(nonce);
      // The HMAC covers the command byte *alone*, with the nonce as the challenge — not the
      // nonce as transcript data, which is what this built until real bytes settled it (a Velux
      // KLR200's own 0x2A, tests/corpus/captures/velux_kux100/pairing_full.yaml, recomputed
      // under that installation's key). The old [cmd, nonce] transcript produced an HMAC no
      // device could verify, so every 0x2A we emitted was silently unanswerable.
      uint8_t hmac[HMAC_SIZE];
      if (!crypto::create_hmac(&command, 1, nonce, system_key, hmac))
        return false;
      uint8_t payload_buf[HMAC_SIZE * 2];
      memcpy(payload_buf, nonce, HMAC_SIZE);
      memcpy(payload_buf + HMAC_SIZE, hmac, HMAC_SIZE);
      return set_cmd(f, CMD_DISCOVER_SPE_REQ, payload_buf, sizeof(payload_buf));
    }

    case CMD_DISCOVER_ALT_REQ: {
      // 0x2E may carry an optional single-byte payload (e.g., 0x00) or be sent with no payload.
      if (payload_enabled)
        return set_cmd(f, CMD_DISCOVER_ALT_REQ, &payload, 1);
      return set_cmd(f, CMD_DISCOVER_ALT_REQ);
    }

    default:
      return false;
  }
}

/// Build a discovery response (0x29) — device side, used only by the key-extraction responder.
/// See proto_commands.h for the full contract and the real-capture cross-check.
bool create_discover_resp(IoFrame &f, const uint8_t *own, const uint8_t *dst, DeviceType type, uint8_t subtype,
                          uint8_t manufacturer_id) {
  init_frame(f, true, true, true, false);
  set_dst(f, dst);
  set_src(f, own);

  uint8_t payload[DISCOVERY_RESP_FULL_SIZE] = {0};
  encode_packed_device_type(type, subtype, payload[0], payload[1]);
  // Backbone address: the captured real Somfy 0x29 (see proto_commands.h doxygen) reports the
  // device's own node ID here, so we mirror that rather than inventing a separate address.
  memcpy(&payload[DISCOVERY_RESP_BACKBONE_OFFSET], own, NODE_ID_SIZE);
  payload[DISCOVERY_RESP_MANUFACTURER_OFFSET] = manufacturer_id;
  // Flags: turnaround class ATT_CLASS_5S (0) and POWER_SAVE_ALWAYS_ALIVE (0) both encode to 0 —
  // an honest description of this responder (it never sleeps while armed). Best-effort; see
  // proto_commands.h @note.
  // TODO(hardware-verify): confirm a real hub accepts these flags/timestamp values, or whether
  // it requires specific ATT/power-save/timestamp semantics before completing pairing.
  payload[DISCOVERY_RESP_FLAGS_OFFSET] = 0x00;
  payload[DISCOVERY_RESP_TIMESTAMP_OFFSET] = 0x00;
  payload[DISCOVERY_RESP_TIMESTAMP_OFFSET + 1] = 0x00;
  return set_cmd(f, CMD_DISCOVER_RESP, payload, sizeof(payload));
}

/// Build a bare device→hub terminal acknowledgement: no payload, END set, START and LOW_POWER
/// clear. Shared by create_key_confirm() and create_discover_confirm_ack(), which are the same
/// frame shape and differ only in command byte — real captures of both
/// (tests/corpus/captures/somfy_dimmer/pairing_full.yaml's 0x33 `88 00 …`,
/// velux_kux100/pairing_full.yaml's 0x2D `88 08 …`, and this project's own key-extraction
/// responder against a real hub in
/// tests/corpus/captures/issues/issue_45_velux_kig300_key_extraction_success.yaml, both 0x2D and
/// 0x33 as `88 00 …`) show a device closing its half of a two-frame handshake this way. LOW_POWER
/// stays clear because that bit describes the *target* of a controller-originated frame (see the
/// header's convention note); a device does not flag a frame it sends *to* the hub as low-power.
static bool create_device_terminal_ack(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t cmd) {
  init_frame(f, true, false, true, false);
  set_dst(f, dst);
  set_src(f, own);
  return set_cmd(f, cmd);
}

/// Build a key-confirm frame (0x33) — device side, used only by the key-extraction responder.
/// See proto_commands.h for the full contract and the real-capture cross-check.
bool create_key_confirm(IoFrame &f, const uint8_t *own, const uint8_t *dst) {
  return create_device_terminal_ack(f, own, dst, CMD_KEY_CONFIRM);
}

/// Build a discovery-confirm acknowledgement (0x2D) — device side, used only by the
/// key-extraction responder. See proto_commands.h for the full contract.
bool create_discover_confirm_ack(IoFrame &f, const uint8_t *own, const uint8_t *dst) {
  return create_device_terminal_ack(f, own, dst, CMD_DISCOVER_CONFIRM_ACK);
}

/// Recover the system key from a CMD_KEY_TRANSFER payload. See proto_commands.h for the full
/// contract; this is the single place the IV-`data` convention (`{CMD_KEY_INIT}, len 1`) lives
/// for the decode direction, mirroring create_key_transfer()'s encode side below.
bool recover_system_key_from_transfer(const uint8_t transfer_payload[AES_KEY_SIZE], const uint8_t challenge[HMAC_SIZE],
                                      uint8_t out_key[AES_KEY_SIZE]) {
  const uint8_t key_init_cmd = CMD_KEY_INIT;
  return crypto::crypt_key(&key_init_cmd, 1, challenge, transfer_payload, out_key);
}

/// Build a key-init request (0x31) to start the pairing key exchange with a discovered device.
bool create_key_init(IoFrame &f, const uint8_t *own, const uint8_t *dst) {
  init_frame(f, true, true, false, true);
  set_dst(f, dst);
  set_src(f, own);
  return set_cmd(f, CMD_KEY_INIT);
}

/// Build a key-transfer frame (0x32) containing the system key encrypted with the transfer key.
bool create_key_transfer(IoFrame &f, IoFrame &old_frame, const uint8_t *dst, const uint8_t *src,
                         const uint8_t key[AES_KEY_SIZE], const uint8_t challenge[HMAC_SIZE]) {
  // low_power=true: see create_set_name above.
  init_frame(f, true, false, false, true);
  set_dst(f, dst);
  set_src(f, src);
  // The pairing capture we matched derives the IV from the previous command byte only. Treating
  // the key-init frame that narrowly keeps our key transfer aligned with real controllers.
  uint8_t enc_key[AES_KEY_SIZE];
  if (!crypto::crypt_key(&old_frame.cmd, 1, challenge, key, enc_key))
    return false;
  return set_cmd(f, CMD_KEY_TRANSFER, enc_key, AES_KEY_SIZE);
}

/// Build a challenge request (0x3C) with caller-chosen framing bits. Shared by the
/// controller-role and device-role builders below, which differ only in those bits.
static bool create_challenge_req_framed(IoFrame &f, const uint8_t *dst, const uint8_t *src,
                                        const uint8_t challenge[HMAC_SIZE], bool start, bool low_power) {
  init_frame(f, true, start, false, low_power);
  set_dst(f, dst);
  set_src(f, src);
  return set_cmd(f, CMD_CHALLENGE_REQ, challenge, HMAC_SIZE);
}

/// Build a challenge request (0x3C) using a caller-supplied challenge. See proto_commands.h.
///
/// Framed exactly like the device-role builder below (`0E 00`, no START, no LOW_POWER) — matches
/// every 0x3C observed on air across multiple actuators and vendors. Kept as a separate entry
/// point from create_challenge_req_device_role() because the call sites and rationale differ
/// (inbound authentication vs the key-extraction responder). If devices ever stop answering our
/// challenges, START/LOW_POWER framing is the first thing to try restoring here.
bool create_challenge_req(IoFrame &f, const uint8_t *dst, const uint8_t *src, const uint8_t challenge[HMAC_SIZE]) {
  return create_challenge_req_framed(f, dst, src, challenge, /*start=*/false, /*low_power=*/false);
}

/// Build a challenge request (0x3C) containing 6 random bytes.
/// Used when WE need to authenticate an incoming request from a device.
bool create_challenge_req(IoFrame &f, const uint8_t *dst, const uint8_t *src) {
  uint8_t challenge[HMAC_SIZE];
  crypto::generate_challenge(challenge);
  return create_challenge_req(f, dst, src, challenge);
}

/// Build a device-role challenge request (0x3C) — device side, used only by the key-extraction
/// responder. See proto_commands.h for why the framing bits differ from the controller-role
/// builders above.
bool create_challenge_req_device_role(IoFrame &f, const uint8_t *dst, const uint8_t *src,
                                      const uint8_t challenge[HMAC_SIZE]) {
  return create_challenge_req_framed(f, dst, src, challenge, /*start=*/false, /*low_power=*/false);
}

/// Build a challenge response (0x3D) proving we know the system key.
/// The HMAC is computed over [original_command_id + original_data] using the challenge.
bool create_challenge_resp(IoFrame &f, const uint8_t *dst, const uint8_t *src, const uint8_t challenge[HMAC_SIZE],
                           const IoFrame &origin, const uint8_t *key) {
  // low_power=true (see create_set_name above)
  init_frame(f, true, false, false, true);
  set_dst(f, dst);
  set_src(f, src);
  // The authenticated transcript covers the original request, not the 0x3D wrapper. Using the
  // origin command byte and payload here was one of the key interoperability findings.
  uint8_t frame_data[FRAME_MAX_SIZE];
  frame_data[0] = origin.cmd;
  memcpy(frame_data + 1, origin.data, origin.data_len);
  uint8_t hmac[HMAC_SIZE];
  if (!crypto::create_hmac(frame_data, origin.data_len + 1, challenge, key, hmac))
    return false;
  return set_cmd(f, CMD_CHALLENGE_RESP, hmac, HMAC_SIZE);
}

/// Build a status-update acknowledgment (0x72). Sent after authenticating a device's status update.
/// The response is sent on all 3 channels to ensure the device receives it.
bool create_status_update_resp(IoFrame &f, const uint8_t *own, const uint8_t *dst) {
  // end=true: final frame. low_power=true (see create_set_name above).
  init_frame(f, true, false, true, true);
  set_dst(f, dst);
  set_src(f, own);
  // Status update acknowledgment payload matched from working controller captures.
  return set_cmd(f, CMD_STATUS_UPDATE_RESP, STATUS_UPDATE_ACK_PAYLOAD, sizeof(STATUS_UPDATE_ACK_PAYLOAD));
}

/// Build a set-config command (0x6F) to tell the device to automatically send status updates
/// when controlled by any remote (not just us). Not all devices support this.
bool create_set_config1(IoFrame &f, const uint8_t *own, const uint8_t *dst) {
  // low_power=true (see create_set_name above).
  init_frame(f, true, true, false, true);
  set_dst(f, dst);
  set_src(f, own);
  // Set-config payload matched from working controller captures.
  return set_cmd(f, CMD_SET_CONFIG1, SET_CONFIG1_STATUS_BROADCAST_PAYLOAD,
                 sizeof(SET_CONFIG1_STATUS_BROADCAST_PAYLOAD));
}

}  // namespace home_io_control
}  // namespace esphome
