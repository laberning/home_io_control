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
/// (tests/corpus/captures/oneway/reference_1w_oneway_execute_iv_vector.yaml), not a 2W hub, and a
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

/// Multi Information Byte for our device-role CMD_DISCOVER_RESP (0x29) — the turnaround-time
/// class and power-save mode this responder advertises to a foreign hub.
///
/// ATT_CLASS_5S | POWER_SAVE_ALWAYS_ALIVE (0x00) would be an honest-looking but false claim: it
/// tells a hub to expect a reply within 5s from a device that never sleeps, when this responder
/// actually hops across 3 channels on the ESPHome loop cadence and is briefly deaf while
/// transmitting its own replies. A real device with that profile signals ATT_CLASS_40S instead.
///
/// 0xDD = 1101_1101: bits [7:6] are ATT_CLASS_40S, bit [0] is POWER_SAVE_LOW_POWER, bit [3] is
/// DISCOVERY_FLAGS_RF_SUPPORT, and bits [4]/[2] have no named constant here yet. All of this byte
/// except the power-save bit mirrors the exact byte a real Somfy Izymo dimmer advertised in this
/// project's own corpus
/// (tests/corpus/captures/pairing/somfy_izymo_dimmer_pairing_full_sx1276.yaml: flags=0xDC, ATT_CLASS_40S |
/// POWER_SAVE_ALWAYS_ALIVE), on the theory that matching a real device's full byte is safer than
/// guessing at bits this codebase doesn't yet have a documented meaning for — the unnamed bits and
/// the RF-support bit are carried over unmodified for exactly that reason.
///
/// The power-save bit is the one deliberate departure from that real capture: POWER_SAVE_LOW_POWER
/// here, not the captured device's POWER_SAVE_ALWAYS_ALIVE. That is a reasoned choice, not a
/// guess — this responder's own true behavior (hopping across 3 channels, briefly deaf while
/// transmitting its own replies) is genuinely closer to a low-power device's profile than to an
/// always-listening one, so mirroring the capture's power-save bit would make the same false claim
/// ATT_CLASS_5S | POWER_SAVE_ALWAYS_ALIVE made above, just with the opposite polarity.
/// TODO(hardware-verify): unconfirmed against a real hub — see create_discover_resp()'s callers'
/// file-level @warning (key_extraction_responder.cpp) for the general caveat this falls under.
constexpr uint8_t KEY_EXTRACTION_DISCOVER_RESP_FLAGS = 0xDD;

/// Timestamp for our device-role CMD_DISCOVER_RESP (0x29), alongside the flags constant above. Set
/// to the same Somfy Izymo dimmer capture's value (0x000E) for the same reason: an unverified
/// field is safer matched to a real device's exact bytes than left at the one value no real
/// capture in this project's corpus ever shows (0x0000 — see KEY_EXTRACTION_DISCOVER_RESP_FLAGS
/// above for the flags half of the same reasoning).
/// TODO(hardware-verify): unconfirmed against a real hub — same caveat as the flags constant above.
constexpr uint16_t KEY_EXTRACTION_DISCOVER_RESP_TIMESTAMP = 0x000E;

/// Mask for the low byte of a 16-bit field split across 2 wire bytes — same big-endian split
/// proto_codecs.cpp's decode side uses (DISCOVERY_TIMESTAMP_MSB_SHIFT there).
///
/// This is one of several file-local 0xFF low-byte masks in this codebase (see also
/// RANDOM_LOW_BYTE_MASK in key_extraction_responder.cpp and the inline `& 0xFF` uses in
/// proto_crypto.cpp's construct_iv_1w_sequence()) — a recurring pattern with no shared constant.
/// That is consistent with how this codebase treats single-purpose local masks generally (kept
/// file-local rather than centralized), not an oversight specific to this one.
constexpr uint16_t LOW_BYTE_MASK = 0xFF;
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
/// Long-form CMD_PRIVATE2 (0x0C) payload length. Coincides numerically with
/// EXECUTE_SPECIAL_PAYLOAD_SIZE but is a distinct command's payload shape — do not merge the two
/// constants; a change to one must not silently change the other.
constexpr size_t PRIVATE2_LONG_PAYLOAD_SIZE = 6;
/// Extended-block flag at data[2] of the long-form CMD_PRIVATE2 payload — the same 0x80 byte
/// value the field-observed extended-CMD_PRIVATE selector uses (create_get_status_extended()),
/// but a distinct field on a distinct command. EXECUTE_POSITION_LAYOUT_FLAG above is CMD_EXECUTE's
/// unrelated data[4] flag and must not be reused here even though it is also 0x80.
constexpr uint8_t PRIVATE2_EXTENDED_BLOCK_FLAG = 0x80;
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
  return {EXECUTE_ORIGINATOR,           acei,         static_cast<uint8_t>(POSITION_WIRE_SCALE * position),       0x00,
          EXECUTE_POSITION_LAYOUT_FLAG, POS_FAVORITE, silent ? EXECUTE_PROFILE_SILENT : EXECUTE_POSITION_PROFILE, 0x00};
}

// === 1W execute (CMD 0x00) frame assembly ===

/// 1W execute command parameters: origin(1) + acei(1) + main[2] + fp1(1) + fp2(1). This is the
/// 6-byte "special" payload form, which 1W uses even for numeric positions (unlike 2W's 8-byte
/// create_execute_position() layout).
constexpr uint8_t ONEWAY_EXECUTE_PARAMS_SIZE = 6;
// ONEWAY_EXECUTE_ACEI (the Somfy-shaped default) and ONEWAY_EXECUTE_ACEI_VELUX live in
// proto_constants.h so oneway_controller.h's resolve_oneway_wire_profile() can name them without
// the protocol layer depending on the controller layer. build_1w_execute() takes the ACEI as a
// parameter now, defaulting to ONEWAY_EXECUTE_ACEI.
/// Rolling sequence width; big-endian on the wire, and never part of the signed span.
constexpr uint8_t ONEWAY_SEQUENCE_SIZE = 2;
/// 1W execute MAC span: the command byte followed by all ONEWAY_EXECUTE_PARAMS_SIZE parameter
/// bytes, stopping before the sequence. See create_1w_execute_command()'s doxygen
/// (proto_commands.h) for the published-vector and reference-implementation citations pinning
/// this span; it is command-specific and must not be reused for any other 1W command.
constexpr uint8_t ONEWAY_EXECUTE_MAC_SPAN_SIZE = 1 + ONEWAY_EXECUTE_PARAMS_SIZE;
/// Offsets of the sequence and MAC within the payload, derived from the field widths above so
/// the layout cannot be restated inconsistently.
constexpr uint8_t ONEWAY_EXECUTE_SEQUENCE_OFFSET = ONEWAY_EXECUTE_PARAMS_SIZE;
constexpr uint8_t ONEWAY_EXECUTE_MAC_OFFSET = ONEWAY_EXECUTE_SEQUENCE_OFFSET + ONEWAY_SEQUENCE_SIZE;
/// 1W execute declared payload: parameters + sequence + MAC = 14 bytes, matching the reference
/// `_p0x00_14` struct.
constexpr uint8_t ONEWAY_EXECUTE_PAYLOAD_SIZE = ONEWAY_EXECUTE_MAC_OFFSET + HMAC_SIZE;
/// 1W execute functional-parameter bytes; always zero for the frames this codebase builds.
constexpr uint8_t ONEWAY_EXECUTE_FP1 = 0x00;
constexpr uint8_t ONEWAY_EXECUTE_FP2 = 0x00;

/// @brief Shared assembly for both 1W execute builders: header, MAC span/HMAC, and the 14-byte
/// payload. create_1w_execute_position() and create_1w_execute_command() differ only in how they
/// derive `main0`/`main1`; every other byte on the wire is identical, so this is the one place
/// that wiring lives — a second copy would risk drifting from the published IV vector this
/// span is pinned against.
///
/// ctrl1 is deliberately left at 0 (LOW_POWER / CTRL1_LOW_POWER NOT set), unlike every 2W builder
/// in this file. The reference `forgePacket` sets it, but five independently captured real 1W
/// frames all disagree: this project's own Somfy awning remote
/// (tests/corpus/captures/oneway/somfy_smoove_oneway_{open,close,stop}_sx1276.yaml), an
/// unidentified 1W remote (tests/corpus/captures/oneway/unidentified_1w_remote_oneway_execute.yaml), and
/// the published vector (tests/corpus/captures/oneway/reference_1w_oneway_execute_iv_vector.yaml)
/// all carry ctrl1=0x00. Followed the captures over the reference source on this point.
/// Shared frame-header setup for every 1W broadcast builder (build_1w_execute(),
/// create_1w_add_controller(), create_1w_remove_controller()): all three address a device-class
/// broadcast rather than an individual node, and none set LOW_POWER.
void init_1w_broadcast_frame(IoFrame &f, const uint8_t src[NODE_ID_SIZE], DeviceType target_type) {
  init_frame(f, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);

  uint8_t dst[NODE_ID_SIZE];
  encode_broadcast_address(target_type, dst);
  set_dst(f, dst);
  set_src(f, src);
}

bool build_1w_execute(IoFrame &f, const uint8_t src[NODE_ID_SIZE], DeviceType target_type, uint8_t main0, uint8_t main1,
                      uint16_t sequence, const uint8_t controller_key[AES_KEY_SIZE], uint8_t acei, bool broadcast_all) {
  uint8_t payload[ONEWAY_EXECUTE_PAYLOAD_SIZE] = {EXECUTE_ORIGINATOR, acei, main0, main1, ONEWAY_EXECUTE_FP1,
                                                  ONEWAY_EXECUTE_FP2};

  // The span is the command byte followed by exactly the parameter bytes that go on air, so it
  // is copied out of `payload` rather than restated — a restatement could drift from the wire
  // and produce a signature that verifies against nothing.
  uint8_t mac_span[ONEWAY_EXECUTE_MAC_SPAN_SIZE];
  mac_span[0] = CMD_EXECUTE;
  memcpy(&mac_span[1], payload, ONEWAY_EXECUTE_PARAMS_SIZE);

  // Sign before touching `f`, so a failure leaves the caller's frame exactly as it found it.
  // 1W is fire-and-forget: a caller that ignored the return value and transmitted a half-built
  // frame would get no error back from anywhere — the failure would be silent and on air.
  if (!crypto::create_1w_hmac(mac_span, sizeof(mac_span), sequence, controller_key,
                              &payload[ONEWAY_EXECUTE_MAC_OFFSET]))
    return false;

  payload[ONEWAY_EXECUTE_SEQUENCE_OFFSET] = static_cast<uint8_t>(sequence >> BITS_PER_BYTE);
  payload[ONEWAY_EXECUTE_SEQUENCE_OFFSET + 1] = static_cast<uint8_t>(sequence);

  // A handheld cover remote of either vendor broadcasts open/close/stop to the all-devices
  // address; only a class-bound identity uses the typed one. encode_broadcast_address(UNKNOWN)
  // is already `00 00 3F` (only DeviceType 0 yields it) — do not add a second address path.
  init_1w_broadcast_frame(f, src, broadcast_all ? DeviceType::UNKNOWN : target_type);

  return set_cmd(f, CMD_EXECUTE, payload, sizeof(payload));
}

// === 1W Enrollment (CMD 0x30 add-controller / CMD 0x39 remove-controller) ===

/// CMD 0x30 declared-payload layout: enc_key[16] + man_id[1] + data[1] + sequence[2] = 20 bytes.
/// Offsets mirror decode_1w_add_controller()'s ONEWAY_ADD_CONTROLLER_* constants (proto_codecs.cpp)
/// exactly, since the two must agree on the wire shape by construction.
constexpr uint8_t ONEWAY_ADD_ENC_KEY_OFFSET = 0;
constexpr uint8_t ONEWAY_ADD_MANUFACTURER_OFFSET = AES_KEY_SIZE;  // 16
constexpr uint8_t ONEWAY_ADD_DATA_OFFSET = AES_KEY_SIZE + 1;      // 17
constexpr uint8_t ONEWAY_ADD_SEQUENCE_OFFSET = AES_KEY_SIZE + 2;  // 18
constexpr uint8_t ONEWAY_ADD_PAYLOAD_SIZE = AES_KEY_SIZE + 4;     // 20
/// `data` field of CMD 0x30's payload — every source (published vector, reference `Add` case)
/// shows 0x01 here; its meaning beyond "add" is undocumented.
constexpr uint8_t ONEWAY_ADD_DATA_VALUE = 0x01;
/// CMD 0x30's MAC span: cmd + enc_key only (17 bytes) — verified against the published vector
/// (create_1w_hmac()'s `@warning`), NOT the whole declared payload.
constexpr uint8_t ONEWAY_ADD_MAC_SPAN_SIZE = 1 + AES_KEY_SIZE;

/// CMD 0x39 declared-payload layout, matching the reference `_p0x2e` struct: data[1] + sequence[2]
/// + mac[6] = 9 bytes, MAC inside the declared length (unlike CMD 0x30's out-of-length trailer).
constexpr uint8_t ONEWAY_REMOVE_DATA_OFFSET = 0;
constexpr uint8_t ONEWAY_REMOVE_SEQUENCE_OFFSET = 1;
constexpr uint8_t ONEWAY_REMOVE_MAC_OFFSET = 3;
constexpr uint8_t ONEWAY_REMOVE_PAYLOAD_SIZE = 9;
/// `data` field observed in every source for CMD 0x39.
constexpr uint8_t ONEWAY_REMOVE_DATA_VALUE = 0x00;
/// CMD 0x39's MAC span. No known-answer vector pins this — see create_1w_remove_controller()'s
/// `@warning` (proto_commands.h) for why `cmd + data` was chosen over the alternatives.
constexpr uint8_t ONEWAY_REMOVE_MAC_SPAN_SIZE = 2;

/// Build a no-payload, addressed, authenticated request for `cmd` — the shape every
/// device-info read (CMD_GET_NAME, CMD_GET_INFO1/2, CMD_GET_GENERAL_INFO3) uses.
bool create_no_payload_request(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t cmd) {
  init_frame(f, true, true, false, low_power);
  set_dst(f, dst);
  set_src(f, own);
  return set_cmd(f, cmd);
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

/// Build a 1W position execute frame (CMD 0x00) targeting a device class. See proto_commands.h
/// for the full contract.
bool create_1w_execute_position(IoFrame &f, const uint8_t src[NODE_ID_SIZE], DeviceType target_type, uint8_t position,
                                uint16_t sequence, const uint8_t controller_key[AES_KEY_SIZE], uint8_t acei,
                                bool broadcast_all) {
  if (position > POSITION_PERCENT_MAX)
    return false;
  return build_1w_execute(f, src, target_type, static_cast<uint8_t>(POSITION_WIRE_SCALE * position), 0x00, sequence,
                          controller_key, acei, broadcast_all);
}

/// Build a 1W named-command execute frame (CMD 0x00) targeting a device class. See
/// proto_commands.h for the full contract, including why 1W has no FORCE_OPEN: the only known
/// wire encoding for the label (POS_FORCE_OPEN, main=0x64) was hardware-tested as an ordinary
/// move-to-50% command, not a lock bypass — see the POS_FORCE_OPEN doc comment in
/// proto_constants.h. Passing CoverCommand::FORCE_OPEN here falls through to `default` and
/// returns false, matching 2W's create_execute_command().
bool create_1w_execute_command(IoFrame &f, const uint8_t src[NODE_ID_SIZE], DeviceType target_type, CoverCommand cmd,
                               uint16_t sequence, const uint8_t controller_key[AES_KEY_SIZE], uint8_t acei,
                               bool broadcast_all) {
  uint8_t main0 = 0;
  uint8_t main1 = 0;
  switch (cmd) {
    case CoverCommand::STOP:
      main0 = POS_STOP;
      break;
    case CoverCommand::FAVORITE:
      main0 = POS_FAVORITE;
      break;
    case CoverCommand::VENT:
      main0 = POS_FAVORITE;
      main1 = POS_VENT_MODIFIER;
      break;
    default:
      return false;
  }
  return build_1w_execute(f, src, target_type, main0, main1, sequence, controller_key, acei, broadcast_all);
}

/// Build a 1W add-controller frame (CMD 0x30). See proto_commands.h for the full contract,
/// including why the MAC is an optional, out-of-length trailer here and not inside the declared
/// payload, and why `with_mac` exists at all (real hardware omits it; the published vector
/// doesn't).
bool create_1w_add_controller(IoFrame &f, const uint8_t src[NODE_ID_SIZE], DeviceType target_type, uint8_t manufacturer,
                              uint16_t sequence, const uint8_t controller_key[AES_KEY_SIZE], bool with_mac) {
  uint8_t payload[ONEWAY_ADD_PAYLOAD_SIZE] = {0};

  // Self-inverse wrap (crypto::crypt_1w_key()'s doxygen) -- the same call decode_1w_add_controller()
  // uses to unwrap an overheard 0x30 also wraps our own key for transmission here.
  if (!crypto::crypt_1w_key(src, controller_key, &payload[ONEWAY_ADD_ENC_KEY_OFFSET]))
    return false;

  payload[ONEWAY_ADD_MANUFACTURER_OFFSET] = manufacturer;
  payload[ONEWAY_ADD_DATA_OFFSET] = ONEWAY_ADD_DATA_VALUE;
  payload[ONEWAY_ADD_SEQUENCE_OFFSET] = static_cast<uint8_t>(sequence >> BITS_PER_BYTE);
  payload[ONEWAY_ADD_SEQUENCE_OFFSET + 1] = static_cast<uint8_t>(sequence);

  // The span is command + enc_key only, copied out of `payload` rather than restated -- see
  // build_1w_execute()'s comment for why a restatement risks drifting from the wire. Computed
  // even when with_mac is false so a caller flipping the flag later doesn't also have to
  // reconsider whether signing itself can fail -- crypto::create_1w_hmac() is still the "sign
  // before touching f" guard for the whole builder either way.
  uint8_t mac_span[ONEWAY_ADD_MAC_SPAN_SIZE];
  mac_span[0] = CMD_ONEWAY_ADD_CONTROLLER;
  memcpy(&mac_span[1], &payload[ONEWAY_ADD_ENC_KEY_OFFSET], AES_KEY_SIZE);

  uint8_t mac[HMAC_SIZE];
  // Sign before touching `f`, same rule as build_1w_execute(): 1W is fire-and-forget, so a
  // half-built frame a caller transmitted anyway would fail silently with nobody to report it.
  if (!crypto::create_1w_hmac(mac_span, sizeof(mac_span), sequence, controller_key, mac))
    return false;

  init_1w_broadcast_frame(f, src, target_type);

  if (!set_cmd(f, CMD_ONEWAY_ADD_CONTROLLER, payload, sizeof(payload)))
    return false;

  // The MAC is an out-of-length trailer for this command only (frame_carries_mac_trailer()) --
  // set after set_cmd() succeeds, since init_frame() above would otherwise reset it right back
  // off. Left false (real Somfy hardware's own shape) when with_mac is false.
  if (with_mac) {
    f.has_mac = true;
    memcpy(f.mac, mac, HMAC_SIZE);
  }
  return true;
}

/// Build a 1W remove-controller frame (CMD 0x39). See proto_commands.h for the full contract,
/// including the @warning that no vector pins this command's MAC span.
bool create_1w_remove_controller(IoFrame &f, const uint8_t src[NODE_ID_SIZE], DeviceType target_type, uint16_t sequence,
                                 const uint8_t controller_key[AES_KEY_SIZE]) {
  uint8_t payload[ONEWAY_REMOVE_PAYLOAD_SIZE] = {0};
  payload[ONEWAY_REMOVE_DATA_OFFSET] = ONEWAY_REMOVE_DATA_VALUE;
  payload[ONEWAY_REMOVE_SEQUENCE_OFFSET] = static_cast<uint8_t>(sequence >> BITS_PER_BYTE);
  payload[ONEWAY_REMOVE_SEQUENCE_OFFSET + 1] = static_cast<uint8_t>(sequence);

  // Unpinned span (see the @warning in proto_commands.h): cmd + data, the same "everything before
  // the sequence" shape CMD 0x00's span follows.
  uint8_t mac_span[ONEWAY_REMOVE_MAC_SPAN_SIZE] = {CMD_ONEWAY_REMOVE, payload[ONEWAY_REMOVE_DATA_OFFSET]};

  // Sign before touching `f` -- same rule as every other 1W builder in this file.
  if (!crypto::create_1w_hmac(mac_span, sizeof(mac_span), sequence, controller_key, &payload[ONEWAY_REMOVE_MAC_OFFSET]))
    return false;

  init_1w_broadcast_frame(f, src, target_type);

  return set_cmd(f, CMD_ONEWAY_REMOVE, payload, sizeof(payload));
}

/// Build a CMD_PRIVATE (0x03) request for an arbitrary function ID. function_id = 0x06/0x09
/// reads battery state; create_get_status() below is this builder frozen at function_id =
/// PRIVATE_GET_POSITION_STATUS (0x03), the only function ID this codebase has ever captured on
/// its own wire.
bool create_private_function(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t function_id,
                             uint8_t sub_index) {
  init_frame(f, true, true, false, low_power);
  set_dst(f, dst);
  set_src(f, own);
  uint8_t d[3] = {function_id, sub_index, 0x00};
  return set_cmd(f, CMD_PRIVATE, d, sizeof(d));
}

/// Build a get-status request (0x03). The device responds with its current position.
bool create_get_status(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power) {
  return create_private_function(f, own, dst, low_power, PRIVATE_GET_POSITION_STATUS);
}

bool create_get_name(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power) {
  return create_no_payload_request(f, own, dst, low_power, CMD_GET_NAME);
}

/// Build a CMD_GET_GENERAL_INFO3 (0x58) request. No payload — delegates to the shared
/// create_no_payload_request() helper, the shape every device-info read uses.
bool create_general_info3(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power) {
  return create_no_payload_request(f, own, dst, low_power, CMD_GET_GENERAL_INFO3);
}

/// Build a CMD_GET_INFO1 (0x54) request. No payload. See proto_commands.h for the evidence note.
bool create_get_info1(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power) {
  return create_no_payload_request(f, own, dst, low_power, CMD_GET_INFO1);
}

/// Build a CMD_GET_INFO2 (0x56) request. No payload. See proto_commands.h for the evidence note.
bool create_get_info2(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power) {
  return create_no_payload_request(f, own, dst, low_power, CMD_GET_INFO2);
}

bool create_set_name(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power,
                     const uint8_t payload[DEVICE_NAME_WRITE_PAYLOAD_SIZE]) {
  init_frame(f, true, true, false, low_power);
  set_dst(f, dst);
  set_src(f, own);
  return set_cmd(f, CMD_SET_NAME, payload, DEVICE_NAME_WRITE_PAYLOAD_SIZE);
}

/// Build an authenticated device-identify request (0x1E).
bool create_identify(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power) {
  init_frame(f, true, true, false, low_power);
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

/// Build an extended CMD_PRIVATE (0x03) request with a selector/block pair — the shape real
/// hubs use for both the tilt block (selector STATUS_TILT_SELECTOR) and the field-observed
/// selector 0x80 (tests/corpus/captures/probe/multi_somfy_probe_extended_private_both_selectors.yaml),
/// which this codebase has never decoded. `block` is the field-observed name for the byte that
/// varies (0x00/0x01) for selector 0x80; create_get_status_tilt() below is this builder frozen
/// at selector = STATUS_TILT_SELECTOR, block = 0x01. `function_id` defaults to
/// PRIVATE_GET_POSITION_STATUS (0x03) — the only value ever seen on air in this shape; other
/// values are diagnostic probes into an undecoded function ID.
bool create_get_status_extended(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t selector,
                                uint8_t block, uint8_t function_id) {
  init_frame(f, true, true, false, low_power);
  set_dst(f, dst);
  set_src(f, own);
  uint8_t d[4] = {function_id, selector, block, 0x00};
  return set_cmd(f, CMD_PRIVATE, d, sizeof(d));
}

/// Build a tilt-aware get-status request (0x03) that returns the extended 16-byte tilt payload.
bool create_get_status_tilt(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power) {
  return create_get_status_extended(f, own, dst, low_power, STATUS_TILT_SELECTOR, 0x01);
}

/// Build a CMD_PRIVATE2 (0x0C) request in either of the two field-observed shapes. The payload
/// is CMD_EXECUTE's POS_FAVORITE/POS_VENT_MODIFIER stored-position selector with the execution
/// prefix stripped — `modifier` is that same selector byte (e.g. POS_VENT_MODIFIER for vent).
///
/// `low_power` is a separate parameter, not derived from `long_form`: the two captured fixtures
/// (tests/corpus/captures/probe/multi_somfy_probe_private2_{long_form,short_form}.yaml)
/// do carry CTRL1_LOW_POWER set on the long-form request and clear on the short-form ones, but
/// that tracks the *target device's* power class in each capture (a solar shutter vs. a
/// mains-powered switch), not the payload shape — the same relationship every other
/// device-addressed builder in this file has to `low_power`. Deriving it from `long_form` would
/// silently clear the flag on a short-form probe sent to a solar device.
bool create_private2_read(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t modifier, bool long_form,
                          bool low_power) {
  init_frame(f, true, true, false, low_power);
  set_dst(f, dst);
  set_src(f, own);
  if (long_form) {
    uint8_t d[PRIVATE2_LONG_PAYLOAD_SIZE] = {POS_UNKNOWN,  0x00,     PRIVATE2_EXTENDED_BLOCK_FLAG,
                                             POS_FAVORITE, modifier, 0x00};
    return set_cmd(f, CMD_PRIVATE2, d, sizeof(d));
  }
  uint8_t d[4] = {POS_FAVORITE, modifier, 0x00, 0x00};
  return set_cmd(f, CMD_PRIVATE2, d, sizeof(d));
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
      // KLR200's own 0x2A, tests/corpus/captures/pairing/velux_kux100_pairing_full.yaml, recomputed
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
  // See KEY_EXTRACTION_DISCOVER_RESP_FLAGS/_TIMESTAMP above for the derivation of these two values.
  payload[DISCOVERY_RESP_FLAGS_OFFSET] = KEY_EXTRACTION_DISCOVER_RESP_FLAGS;
  payload[DISCOVERY_RESP_TIMESTAMP_OFFSET] =
      static_cast<uint8_t>(KEY_EXTRACTION_DISCOVER_RESP_TIMESTAMP >> BITS_PER_BYTE);
  payload[DISCOVERY_RESP_TIMESTAMP_OFFSET + 1] =
      static_cast<uint8_t>(KEY_EXTRACTION_DISCOVER_RESP_TIMESTAMP & LOW_BYTE_MASK);
  return set_cmd(f, CMD_DISCOVER_RESP, payload, sizeof(payload));
}

/// Build a bare device→hub terminal acknowledgement: no payload, END set, START and LOW_POWER
/// clear. Shared by create_key_confirm() and create_discover_confirm_ack(), which are the same
/// frame shape and differ only in command byte — real captures of both
/// (tests/corpus/captures/pairing/somfy_izymo_dimmer_pairing_full_sx1276.yaml's 0x33 `88 00 …`,
/// velux_kux100_pairing_full.yaml's 0x2D `88 08 …`, and this project's own key-extraction
/// responder against a real hub in
/// tests/corpus/captures/pairing/velux_kig300_pairing_key_extraction_success.yaml, both 0x2D and
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
  // low_power=true: the pairing key-transfer keeps a fixed frame shape, hardware-validated as-is.
  // It is a non-start continuation frame and stays outside the per-device low_power rule (ADR 0029).
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

/// Build a challenge response (0x3D) with caller-chosen framing bits. Shared by the
/// controller-role and device-role builders below, which differ only in those bits — see
/// create_challenge_req_framed() above for the identical pattern on the request side.
static bool create_challenge_resp_framed(IoFrame &f, const uint8_t *dst, const uint8_t *src,
                                         const uint8_t challenge[HMAC_SIZE], const IoFrame &origin, const uint8_t *key,
                                         bool end, bool low_power) {
  init_frame(f, true, /*start=*/false, end, low_power);
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

/// Build a challenge response (0x3D) proving we know the system key.
/// The HMAC is computed over [original_command_id + original_data] using the challenge.
bool create_challenge_resp(IoFrame &f, const uint8_t *dst, const uint8_t *src, const uint8_t challenge[HMAC_SIZE],
                           const IoFrame &origin, const uint8_t *key) {
  return create_challenge_resp_framed(f, dst, src, challenge, origin, key, /*end=*/false, /*low_power=*/true);
}

/// Build an address response (0x37) — device side, used only by the key-extraction responder.
/// See proto_commands.h for the full contract, including why the payload (our own node ID, not a
/// separately-tracked backbone identity) is a known simplification rather than a confirmed match
/// to real-device behavior.
///
/// TODO(hardware-verify): ctrl1 is 0x00 here (no CTRL1_PRIORITY), matching every other device-role
/// builder in this file, but the one real capture of this command
/// (tests/corpus/captures/pairing/velux_kux100_pairing_full.yaml line 87) shows CTRL1_PRIORITY set on the
/// KLR200's 0x37. In that same capture the device also mirrors CTRL1_PRIORITY from whatever the
/// hub's preceding request set, and its 0x36 request is the one request in the whole exchange that
/// sets PRIORITY — but every other device-role builder here also emits ctrl1=0 against frames that
/// same capture shows with reserved/other bits set, and those builders are hardware-confirmed
/// working (issue #45's own captures), so this one bit's necessity is unproven rather than known
/// missing. Not mirroring the request's PRIORITY bit until a second real capture settles it either
/// way.
bool create_address_resp_device_role(IoFrame &f, const uint8_t *own, const uint8_t *dst) {
  init_frame(f, true, /*start=*/false, /*end=*/false, /*low_power=*/false);
  set_dst(f, dst);
  set_src(f, own);
  return set_cmd(f, CMD_ADDRESS_RESP, own, NODE_ID_SIZE);
}

/// Build a device-role challenge response (0x3D) — device side, used only by the key-extraction
/// responder answering a hub-issued 0x3C challenging our own 0x37. See proto_commands.h for why
/// the framing bits differ from the controller-role builder above.
bool create_challenge_resp_device_role(IoFrame &f, const uint8_t *dst, const uint8_t *src,
                                       const uint8_t challenge[HMAC_SIZE], const IoFrame &origin, const uint8_t *key) {
  return create_challenge_resp_framed(f, dst, src, challenge, origin, key, /*end=*/true, /*low_power=*/false);
}

/// Build a status-update acknowledgment (0x72). Sent after authenticating a device's status update.
/// The response is sent on all 3 channels to ensure the device receives it.
bool create_status_update_resp(IoFrame &f, const uint8_t *own, const uint8_t *dst) {
  // end=true: final frame. low_power=true: this device-role response keeps a fixed shape,
  // hardware-validated as-is, and stays outside the per-device low_power rule (ADR 0029).
  init_frame(f, true, false, true, true);
  set_dst(f, dst);
  set_src(f, own);
  // Status update acknowledgment payload matched from working controller captures.
  return set_cmd(f, CMD_STATUS_UPDATE_RESP, STATUS_UPDATE_ACK_PAYLOAD, sizeof(STATUS_UPDATE_ACK_PAYLOAD));
}

/// Build a set-config command (0x6F) to tell the device to automatically send status updates
/// when controlled by any remote (not just us). Not all devices support this.
bool create_set_config1(IoFrame &f, const uint8_t *own, const uint8_t *dst) {
  // low_power=true: this pairing phase-3 config write keeps the fixed long-preamble shape it was
  // hardware-validated at; the per-device low_power rule deliberately does not reach pairing
  // frames (ADR 0029).
  init_frame(f, true, true, false, true);
  set_dst(f, dst);
  set_src(f, own);
  // Set-config payload matched from working controller captures.
  return set_cmd(f, CMD_SET_CONFIG1, SET_CONFIG1_STATUS_BROADCAST_PAYLOAD,
                 sizeof(SET_CONFIG1_STATUS_BROADCAST_PAYLOAD));
}

}  // namespace home_io_control
}  // namespace esphome
