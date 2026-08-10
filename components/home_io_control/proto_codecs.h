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

}  // namespace home_io_control
}  // namespace esphome
