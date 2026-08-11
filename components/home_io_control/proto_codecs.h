#pragma once

/// @file proto_codecs.h
/// @brief Device-name, address-classification and 1W-frame codecs.
/// @ingroup hioc_protocol
///
/// Higher-level decode/encode helpers layered on the frame and device model:
/// Latin-1 device-name round-tripping, broadcast address classification and
/// structured decoding of overheard 1W remote frames.

#include "proto_device_model.h"
#include "proto_sizes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace esphome {
namespace home_io_control {

struct IoFrame;  // Defined in proto_frame.h; decode_1w_frame takes it by reference.

// ============================================================================
// Device Name Codec
// ============================================================================

static constexpr uint8_t DEVICE_NAME_WRITE_CHAR_LIMIT = 15;  ///< Reference write limit before the trailing null.
static constexpr uint8_t DEVICE_NAME_WRITE_PAYLOAD_SIZE =
    DEVICE_NAME_WRITE_CHAR_LIMIT + 1;  ///< Fixed write payload: 15 visible chars plus trailing null/padding.
static constexpr uint16_t LATIN1_CODEPOINT_MAX = 0x00FF;  ///< Highest Unicode code point representable in Latin-1.

/// @brief Validation result for outbound device-name writes.
enum class DeviceNameValidationError : uint8_t {
  NONE = 0x00,              ///< Name is valid and encodable.
  EMPTY = 0x01,             ///< Name is empty after normalization.
  TOO_LONG = 0x02,          ///< Name exceeds the 15-character write limit.
  INVALID_UTF8 = 0x03,      ///< Name contains malformed UTF-8 bytes.
  UNSUPPORTED_CHAR = 0x04,  ///< Name contains characters outside Latin-1.
};

/// @brief Decode a device-name payload from IO-homecontrol's Latin-1 wire format into UTF-8.
/// Some devices prepend an extra byte before the first character and many pad the payload with
/// trailing 0x00 or 0x20 bytes. This helper normalizes those quirks and truncates the result to
/// fit DEVICE_NAME_BUFFER_SIZE - 1 bytes when copied into IoDevice::name.
/// @param data Raw payload pointer from CMD_GET_NAME_RESP.
/// @param len Raw payload length in bytes.
/// @return Normalized UTF-8 name, or an empty string when the payload carries no usable characters.
std::string decode_device_name_payload(const uint8_t *data, uint8_t len);
/// @brief Trim leading and trailing ASCII whitespace from a string.
/// This is shared by device-name validation and management actions so both paths normalize input
/// consistently before comparing or writing device metadata.
/// @param value Input string.
/// @return Copy of the string with leading and trailing ASCII whitespace removed.
std::string trim_ascii_whitespace(const std::string &value);
/// @brief Validate and encode a user-supplied UTF-8 device name into the fixed Latin-1 write payload.
/// The outbound payload is a fixed 16-byte field: up to 15 Latin-1 characters followed by trailing
/// zero padding. Leading and trailing ASCII whitespace are trimmed before validation so write-back
/// verification against the device's padded storage is deterministic.
/// @param name User-supplied UTF-8 device name.
/// @param payload Output buffer for the fixed write payload (16 bytes, zero-padded on success).
/// @param normalized_name Output normalized UTF-8 name used for later verification/logging.
/// @return Validation status indicating success or the reason the name cannot be written.
DeviceNameValidationError encode_device_name_payload(const std::string &name,
                                                     uint8_t payload[DEVICE_NAME_WRITE_PAYLOAD_SIZE],
                                                     std::string &normalized_name);
/// @brief Return a stable symbolic name for a device-name validation result.
/// @param error Validation result.
/// @return Uppercase symbolic name.
const char *device_name_validation_error_name(DeviceNameValidationError error);
/// @brief Return a human-readable explanation for a device-name validation result.
/// @param error Validation result.
/// @return Short message suitable for logs and Home Assistant action events.
const char *device_name_validation_error_description(DeviceNameValidationError error);

// ============================================================================
// Address Classification
// ============================================================================

/// @brief Well-known address suffix values in the broadcast address space.
///
/// When the first byte of a 3-byte IO-Homecontrol address is 0x00, the low 6 bits
/// of the third byte carry a suffix that identifies the broadcast category.
/// @{
static constexpr uint8_t ADDRESS_SUFFIX_MASK = 0x3F;       ///< Mask to extract the 6-bit suffix from addr[2].
static constexpr uint8_t ADDRESS_SUFFIX_BROADCAST = 0x3F;  ///< Suffix for "all devices of this type" broadcast.
static constexpr uint8_t ADDRESS_SUFFIX_DISCOVERY = 0x3B;  ///< Suffix for discovery-related broadcasts.
/// @}

/// @brief Address classification categories for diagnostic purposes.
///
/// IO-Homecontrol uses the target address field to encode both unicast
/// device addresses and broadcast targets. The first byte being 0x00
/// indicates a broadcast; the remaining bytes encode the target device type.
enum class AddressClass : uint8_t {
  UNICAST = 0,        ///< Normal device-to-device unicast address (first byte != 0x00).
  BROADCAST_ALL,      ///< Broadcast to all devices of a type (address suffix 0x3F).
  BROADCAST_TYPE,     ///< Broadcast to specific device type with non-standard suffix.
  DISCOVERY,          ///< Discovery-related broadcast (address suffix 0x3B).
  UNKNOWN_BROADCAST,  ///< Broadcast pattern that does not match known suffixes.
};

/// @brief Classify an IO-Homecontrol 3-byte address.
///
/// Determines whether an address is unicast, a typed broadcast, or a
/// discovery-related broadcast based on protocol addressing rules.
/// @param addr Three-byte node address to classify.
/// @return Classification indicating the address type.
AddressClass classify_address(const uint8_t addr[NODE_ID_SIZE]);

/// @brief Get a human-readable name for an address classification.
/// @param address_class Classification returned by classify_address().
/// @return Null-terminated string such as "broadcast_all" or "unicast".
const char *address_class_name(AddressClass address_class);

/// @brief Extract the target device type from a typed broadcast address.
///
/// Broadcast addresses encode the device type in bits [9:2] of the combined
/// address bytes 1–2. This function extracts that type. Returns UNKNOWN
/// if the address is not a broadcast (first byte != 0x00).
/// @param addr Three-byte broadcast address.
/// @return DeviceType encoded in the address, or DeviceType::UNKNOWN for unicast.
DeviceType broadcast_target_type(const uint8_t addr[NODE_ID_SIZE]);

/// @brief Encode a device type into its typed-broadcast destination address — the exact inverse
/// of broadcast_target_type(), kept immediately beside it so the pair cannot drift apart.
///
/// The class occupies bits [9:2] and therefore **spans two bytes**: `out[0]` is always 0,
/// `out[1] = raw >> DEVICE_TYPE_LOW_BITS_SHIFT` carries the high bits, and
/// `out[2] = (raw << DEVICE_TYPE_HIGH_BITS_SHIFT) | DEVICE_SUBTYPE_MASK` carries the low bits
/// plus the all-ones subtype field that makes the address a broadcast ("any subtype of this
/// class"). A single-byte encoding looks right for classes 0–3 and silently produces the wrong
/// class from 4 upward — e.g. the light class (0x06) encodes to `00 01 BF`, not `00 00 BF`.
/// This is the addressing primitive 1W execute frames use: 1W commands a device *class*, never
/// an individual node, so this is the only destination a 1W builder ever needs.
/// @param type Device class to address.
/// @param out Output: 3-byte typed-broadcast destination address.
void encode_broadcast_address(DeviceType type, uint8_t out[NODE_ID_SIZE]);

// ============================================================================
// 1W Remote Frame Decode
// ============================================================================

/// @brief Decode the "main" position/command bytes from a 1W execute payload.
///
/// 1W remotes encode their command intent in a 2-byte main field:
/// - main[0]: position (0–200 mapped to 0–100%), or a special command code.
/// - main[1]: modifier byte (0x03 = ventilation for POS_FAVORITE).
///
/// @param main0 First main byte (position or special code).
/// @param main1 Second main byte (modifier).
/// @param out Buffer to write the decoded string into (e.g., "CLOSE", "position 75%").
/// @param out_size Size of the output buffer.
void decode_1w_main_intent(uint8_t main0, uint8_t main1, char *out, size_t out_size);

/// @brief Resolve a 1W main-byte pair to an optimistic IO target position, if unambiguous.
///
/// Shares decode_1w_main_intent()'s special-code checks so the two never disagree. Returns
/// empty for POS_STOP (the caller must clear any optimistic target instead — stop is not a
/// target) and for codes with no settled position (FAVORITE/VENT/FORCE_OPEN/SECURED_TARGET/
/// DEFAULT/UNKNOWN) — those cases still get a confirmation poll, just no optimistic claim.
/// @param main0 First main byte (position or special code).
/// @param main1 Second main byte (modifier); unused by every branch that resolves a target.
/// @return IO target position (0=open, 100=closed) if resolvable; empty otherwise.
std::optional<float> oneway_intent_to_target(uint8_t main0, uint8_t main1);

/// @brief Buffer size for the decoded 1W main-intent string.
static constexpr size_t ONEWAY_INTENT_BUFFER_SIZE = 24;

/// @brief Decoded representation of a 1W remote frame.
///
/// Captures all fields extractable from a 1W broadcast frame in a structured form
/// that can be used for logging, events, or future sensor exposure.
struct OneWayFrameInfo {
  uint8_t src[NODE_ID_SIZE]{};                                  ///< Remote source node ID (3 bytes).
  AddressClass address_class{AddressClass::UNKNOWN_BROADCAST};  ///< Classification of the broadcast address.
  DeviceType target_type{DeviceType::UNKNOWN};                  ///< Target device class from broadcast address.
  uint8_t cmd{0};                                               ///< Command ID (e.g., CMD_EXECUTE, CMD_ACTIVATE_MODE).
  bool has_intent{false};                                       ///< True if originator/ACEI/intent fields were decoded.
  uint8_t originator{0};                     ///< Command originator byte (e.g., ORIGINATOR_USER_REMOTE).
  uint8_t acei_level{0};                     ///< ACEI priority level (0–7).
  char intent[ONEWAY_INTENT_BUFFER_SIZE]{};  ///< Human-readable command intent (e.g., "CLOSE").
  uint8_t main0{0};     ///< Raw first main byte (has_intent only); feeds oneway_intent_to_target().
  uint8_t main1{0};     ///< Raw second main byte (has_intent only); feeds oneway_intent_to_target().
  uint8_t data_len{0};  ///< Raw data length (for commands without decoded intent).
};

/// @brief Decode a parsed 1W frame into a structured OneWayFrameInfo.
///
/// Extracts target device type from the broadcast address. For execute/activate-mode
/// commands, also decodes originator, ACEI priority, and position/command intent.
/// @param frame Parsed IoFrame with CTRL0_PROTOCOL_1W set.
/// @return Populated OneWayFrameInfo.
OneWayFrameInfo decode_1w_frame(const IoFrame &frame);

// ============================================================================
// Discovery Response Decode
// ============================================================================

/// @brief Extended discovery-response fields (manufacturer, Multi Information Byte, backbone
/// address, timestamp) plus flags recording how much of the payload was actually present.
struct DiscoveryResponseInfo {
  bool metadata_complete{false};     ///< data_len >= DEVICE_METADATA_SIZE (type/subtype present).
  bool has_extended{false};          ///< data_len >= DISCOVERY_RESP_FULL_SIZE (mfr/flags/timestamp present).
  uint8_t manufacturer{0};           ///< Raw manufacturer ID; name via manufacturer_name().
  uint8_t flags{0};                  ///< Multi Information Byte; decode with DISCOVERY_FLAGS_* masks.
  uint8_t backbone[NODE_ID_SIZE]{};  ///< Backbone address as reported by the device.
  uint16_t timestamp{0};             ///< Device timestamp field (advances between replies).
};

/// @brief Decode a discovery-response payload (CMD_DISCOVER_RESP 0x29 or CMD_DISCOVER_SPE_RESP 0x2B —
/// both carry the identical DISCOVERY_RESP_FULL_SIZE layout) into device metadata.
///
/// Pure: no logging, no side effects. Extended fields (manufacturer, flags, timestamp) are
/// returned rather than logged so each caller can present them its own way. Sets
/// `device.node_id`/`type`/`subtype`/`inverted`/`position`/`target`/`is_stopped` and
/// `device_id` exactly as a discovery reply implies: type/subtype and inversion come from the
/// packed metadata bytes when present, position/target default to the unknown sentinel, and
/// the device is assumed stopped. Every field read is guarded on `frame.data_len`, so a short
/// or truncated payload degrades gracefully instead of reading past the end.
/// @param frame Parsed discovery-response frame.
/// @param device Output: device record populated from the frame.
/// @param device_id Output: hex device ID string derived from `frame.src`.
/// @return Extended discovery fields (manufacturer/flags/timestamp) and length flags.
DiscoveryResponseInfo decode_discovery_response(const IoFrame &frame, IoDevice &device, std::string &device_id);

// ============================================================================
// 1W Add-Controller Key Adoption (CMD 0x30)
// ============================================================================

/// @brief Outcome of checking the out-of-length MAC trailer (`IoFrame::has_mac`) on a decoded
/// CMD_ONEWAY_ADD_CONTROLLER frame.
///
/// Kept distinct from a bare bool because a caller-facing report needs to tell these three
/// situations apart: no MAC was ever present to check (the reference implementation's own
/// `_p0x30` struct omits the MAC field entirely, so this is a normal, non-error outcome — not
/// every 0x30 on the wire carries one), a MAC was present and matched, or a MAC was present and
/// did not match (evidence the decrypted key is wrong, or the frame was corrupted/forged).
enum class OneWayMacStatus : uint8_t {
  NOT_PRESENT = 0,  ///< `frame.has_mac` was false; nothing to verify.
  VERIFIED = 1,     ///< `frame.has_mac` was true and the MAC verified under the recovered key.
  FAILED = 2,       ///< `frame.has_mac` was true and the MAC did NOT verify under the recovered key.
};

/// @brief Recovered controller identity from a decoded CMD_ONEWAY_ADD_CONTROLLER (0x30) frame.
///
/// Fixed-size, no heap, no `std::string`, no vectors. Deliberately carries no logging or
/// `to_string()` helper: `system_key` is a real 1W installation's network key, and the surest
/// way to leak a key is to make it convenient to print. Route `system_key` through the single
/// intentional user-facing "adopted key" emission (a later step) and nowhere else — see
/// `crypto::crypt_1w_key()`'s `@warning` in proto_crypto.h.
/// @warning Carries real key material (`system_key`). Never log, print, or otherwise let this
///          struct's contents reach any path other than the one intentional emission.
struct OneWayAdoptedKey {
  uint8_t system_key[AES_KEY_SIZE]{};   ///< Recovered network system key (crypto::crypt_1w_key() output).
  uint8_t manufacturer{0};              ///< Manufacturer ID byte from the payload (`man_id`).
  uint8_t sender_node[NODE_ID_SIZE]{};  ///< Sender's node address (`frame.src`) — the new identity's node.
  uint16_t sequence{0};                 ///< 2-byte rolling sequence from the payload (big-endian on wire).
  OneWayMacStatus mac_status{OneWayMacStatus::NOT_PRESENT};  ///< MAC-verification outcome; see OneWayMacStatus.
};

/// @brief Decoding outcome for decode_1w_add_controller().
enum class OneWayAddControllerDecodeError : uint8_t {
  NONE = 0,               ///< Decode succeeded; `out` is populated.
  NOT_ONEWAY = 1,         ///< `CTRL0_PROTOCOL_1W` is not set — not a 1W frame at all.
  WRONG_COMMAND = 2,      ///< `frame.cmd` is not CMD_ONEWAY_ADD_CONTROLLER.
  BAD_LENGTH = 3,         ///< Declared payload is not exactly the expected 20 bytes.
  KEY_UNWRAP_FAILED = 4,  ///< crypto::crypt_1w_key() itself reported failure.
};

/// @brief Decode a CMD_ONEWAY_ADD_CONTROLLER (0x30) frame into a recovered controller identity.
///
/// A 1W device broadcasts this frame while its key-copy gesture is active, handing its network's
/// wrapped system key to whichever controller is listening (see CMD_ONEWAY_ADD_CONTROLLER's
/// Doxygen in proto_constants.h). Three facts a future reader cannot re-derive from the code
/// alone:
///   - The AES key that wraps the payload is the public TRANSFER_KEY (proto_constants.h), not
///     the recovered system key — the system key is the *output* of unwrapping, not an input.
///   - The unwrap IV is derived only from the sender's own node address (`frame.src`), which is
///     plaintext in the frame header — no secret or challenge is needed to begin unwrapping.
///   - The wrap is self-inverse (crypto::crypt_1w_key()'s XOR-with-AES-keystream construction),
///     so the same call that would encrypt a plaintext key for transmission also decrypts an
///     overheard ciphertext; there is no direction flag to get wrong.
///
/// Validates before decrypting rather than decrypting garbage: the 1W protocol bit must be set,
/// the command must be CMD_ONEWAY_ADD_CONTROLLER, and the declared payload must be exactly the
/// expected 20 bytes (`enc_key[16] + man_id[1] + data[1] + sequence[2]`) — any mismatch is
/// rejected before `crypto::crypt_1w_key()` is ever called.
///
/// When `frame.has_mac` is true (the out-of-length MAC trailer modeled by `IoFrame::mac`, see
/// proto_frame.h), the MAC is verified under the *recovered* key with span `cmd + enc_key` (17
/// bytes) — the only span CMD_ONEWAY_ADD_CONTROLLER authenticates (see
/// `crypto::create_1w_hmac()`'s `@warning`). A verifying MAC is strong evidence the decryption
/// recovered the correct key: it is the decisive self-check available at adoption time, before
/// any device is ever commanded. A frame with no MAC is not an error — the reference
/// implementation's own `_p0x30` struct omits the MAC field entirely — so decoding still
/// succeeds and `out.mac_status` simply reports `NOT_PRESENT`.
///
/// Pure function of the frame: no logging, no timers, no I/O (ADR 0005). Never transmits.
/// @param frame Parsed IoFrame, expected to be a CMD_ONEWAY_ADD_CONTROLLER 1W frame.
/// @param out Output: recovered controller identity. Only meaningfully populated when the return
///            value is NONE; left default-constructed (zeroed) otherwise.
/// @return NONE on success; otherwise the specific validation/decode failure.
/// @warning `out` carries real key material once populated. See OneWayAdoptedKey's warning.
OneWayAddControllerDecodeError decode_1w_add_controller(const IoFrame &frame, OneWayAdoptedKey &out);

}  // namespace home_io_control
}  // namespace esphome
