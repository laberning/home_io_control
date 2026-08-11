/// @file proto_codecs.cpp
/// @brief Device-name, address-classification and 1W-frame codec implementations.
/// @ingroup hioc_protocol

#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace esphome {
namespace home_io_control {

namespace {

constexpr uint8_t UTF8_SINGLE_BYTE_MAX = 0x80;
constexpr uint8_t UTF8_TWO_BYTE_LEAD_BASE = 0xC0;
constexpr uint8_t UTF8_CONTINUATION_BASE = 0x80;
constexpr uint8_t UTF8_CONTINUATION_MASK = 0x3F;
constexpr uint8_t UTF8_TWO_BYTE_SHIFT = 6;
constexpr uint8_t NAME_PADDING_NUL = 0x00;
constexpr uint8_t NAME_PADDING_SPACE = 0x20;
constexpr uint8_t UTF8_TWO_BYTE_MASK = 0xE0;
constexpr uint8_t UTF8_TWO_BYTE_PREFIX = 0xC0;
constexpr uint8_t UTF8_THREE_BYTE_MASK = 0xF0;
constexpr uint8_t UTF8_THREE_BYTE_PREFIX = 0xE0;
constexpr uint8_t UTF8_FOUR_BYTE_MASK = 0xF8;
constexpr uint8_t UTF8_FOUR_BYTE_PREFIX = 0xF0;
constexpr uint8_t UTF8_CONTINUATION_PREFIX_MASK = 0xC0;
constexpr uint8_t UTF8_CONTINUATION_PREFIX = 0x80;
constexpr uint8_t UTF8_TWO_BYTE_VALUE_MASK = 0x1F;
constexpr uint8_t ASCII_MAX = 0x7F;
constexpr uint8_t DISCOVERY_TIMESTAMP_MSB_SHIFT = 8;  ///< Shift for the timestamp field's big-endian MSB.

// CMD_ONEWAY_ADD_CONTROLLER (0x30) declared-payload layout: enc_key[16] + man_id[1] + data[1] +
// sequence[2] = 20 bytes. Offsets are into `frame.data`, never into the out-of-length MAC trailer
// (`frame.mac`, see IoFrame::has_mac) which decode_1w_add_controller() reads separately.
constexpr uint8_t ONEWAY_ADD_CONTROLLER_ENC_KEY_OFFSET = 0;
constexpr uint8_t ONEWAY_ADD_CONTROLLER_MANUFACTURER_OFFSET = AES_KEY_SIZE;  // 16
constexpr uint8_t ONEWAY_ADD_CONTROLLER_SEQUENCE_OFFSET = AES_KEY_SIZE + 2;  // 18: man_id(1) + data(1) skipped
constexpr uint8_t ONEWAY_ADD_CONTROLLER_PAYLOAD_SIZE = AES_KEY_SIZE + 4;     // 20: enc_key+man_id+data+sequence
constexpr uint8_t ONEWAY_ADD_CONTROLLER_SEQUENCE_MSB_SHIFT = 8;  ///< Shift for the sequence field's big-endian MSB.
// The only span CMD_ONEWAY_ADD_CONTROLLER authenticates (create_1w_hmac()'s @warning): cmd byte
// followed by the 16 encrypted-key bytes, NOT the whole declared payload.
constexpr uint8_t ONEWAY_ADD_CONTROLLER_MAC_SPAN_SIZE = 1 + AES_KEY_SIZE;  // 17

std::string latin1_to_utf8(const uint8_t *data, size_t len) {
  std::string result;
  result.reserve(len * 2);

  for (size_t index = 0; index < len; index++) {
    uint8_t const byte = data[index];
    if (byte < UTF8_SINGLE_BYTE_MAX) {
      if (result.length() + 1 >= DEVICE_NAME_BUFFER_SIZE)
        break;
      result.push_back(static_cast<char>(byte));
      continue;
    }

    if (result.length() + 2 >= DEVICE_NAME_BUFFER_SIZE)
      break;

    result.push_back(static_cast<char>(UTF8_TWO_BYTE_LEAD_BASE | (byte >> UTF8_TWO_BYTE_SHIFT)));
    result.push_back(static_cast<char>(UTF8_CONTINUATION_BASE | (byte & UTF8_CONTINUATION_MASK)));
  }

  return result;
}

}  // namespace

std::string trim_ascii_whitespace(const std::string &value) {
  size_t begin = 0;
  while (begin < value.length() && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
    begin++;

  size_t end = value.length();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
    end--;

  return value.substr(begin, end - begin);
}

std::string decode_device_name_payload(const uint8_t *data, uint8_t len) {
  if (data == nullptr || len == 0)
    return {};

  const uint8_t begin = data[0] > NAME_PADDING_SPACE ? 0 : 1;
  if (begin >= len)
    return {};

  size_t raw_len = len - begin;
  while (raw_len > 0 &&
         (data[begin + raw_len - 1] == NAME_PADDING_NUL || data[begin + raw_len - 1] == NAME_PADDING_SPACE))
    raw_len--;

  if (raw_len == 0)
    return {};

  return latin1_to_utf8(data + begin, raw_len);
}

DeviceNameValidationError encode_device_name_payload(const std::string &name,
                                                     uint8_t payload[DEVICE_NAME_WRITE_PAYLOAD_SIZE],
                                                     std::string &normalized_name) {
  if (payload == nullptr)
    return DeviceNameValidationError::INVALID_UTF8;

  std::memset(payload, 0, DEVICE_NAME_WRITE_PAYLOAD_SIZE);
  normalized_name.clear();

  const std::string trimmed_name = trim_ascii_whitespace(name);
  if (trimmed_name.empty())
    return DeviceNameValidationError::EMPTY;

  uint8_t latin1_len = 0;
  for (size_t index = 0; index < trimmed_name.length();) {
    const auto byte = static_cast<uint8_t>(trimmed_name[index]);
    uint16_t codepoint = 0;
    size_t advance = 1;

    if (byte <= ASCII_MAX) {
      codepoint = byte;
    } else if ((byte & UTF8_TWO_BYTE_MASK) == UTF8_TWO_BYTE_PREFIX) {
      if (index + 1 >= trimmed_name.length())
        return DeviceNameValidationError::INVALID_UTF8;

      const auto continuation = static_cast<uint8_t>(trimmed_name[index + 1]);
      if ((continuation & UTF8_CONTINUATION_PREFIX_MASK) != UTF8_CONTINUATION_PREFIX)
        return DeviceNameValidationError::INVALID_UTF8;

      codepoint = static_cast<uint16_t>(((byte & UTF8_TWO_BYTE_VALUE_MASK) << UTF8_TWO_BYTE_SHIFT) |
                                        (continuation & UTF8_CONTINUATION_MASK));
      if (codepoint < UTF8_SINGLE_BYTE_MAX)
        return DeviceNameValidationError::INVALID_UTF8;
      advance = 2;
    } else if ((byte & UTF8_THREE_BYTE_MASK) == UTF8_THREE_BYTE_PREFIX ||
               (byte & UTF8_FOUR_BYTE_MASK) == UTF8_FOUR_BYTE_PREFIX) {
      return DeviceNameValidationError::UNSUPPORTED_CHAR;
    } else {
      return DeviceNameValidationError::INVALID_UTF8;
    }

    if (codepoint > LATIN1_CODEPOINT_MAX)
      return DeviceNameValidationError::UNSUPPORTED_CHAR;

    if (latin1_len >= DEVICE_NAME_WRITE_CHAR_LIMIT)
      return DeviceNameValidationError::TOO_LONG;

    payload[latin1_len++] = static_cast<uint8_t>(codepoint);
    index += advance;
  }

  normalized_name = latin1_to_utf8(payload, latin1_len);
  return DeviceNameValidationError::NONE;
}

const char *device_name_validation_error_name(DeviceNameValidationError error) {
  switch (error) {
    case DeviceNameValidationError::NONE:
      return "NONE";
    case DeviceNameValidationError::EMPTY:
      return "EMPTY";
    case DeviceNameValidationError::TOO_LONG:
      return "TOO_LONG";
    case DeviceNameValidationError::INVALID_UTF8:
      return "INVALID_UTF8";
    case DeviceNameValidationError::UNSUPPORTED_CHAR:
      return "UNSUPPORTED_CHAR";
    default:
      return "UNKNOWN_DEVICE_NAME_VALIDATION_ERROR";
  }
}

const char *device_name_validation_error_description(DeviceNameValidationError error) {
  switch (error) {
    case DeviceNameValidationError::NONE:
      return "name accepted";
    case DeviceNameValidationError::EMPTY:
      return "device name must not be empty";
    case DeviceNameValidationError::TOO_LONG:
      return "device name exceeds the 15-character write limit";
    case DeviceNameValidationError::INVALID_UTF8:
      return "device name must be valid UTF-8";
    case DeviceNameValidationError::UNSUPPORTED_CHAR:
      return "device name contains characters outside Latin-1";
    default:
      return "unknown device-name validation error";
  }
}

AddressClass classify_address(const uint8_t addr[NODE_ID_SIZE]) {
  if (addr[0] != 0x00)
    return AddressClass::UNICAST;

  uint8_t const suffix = addr[2] & ADDRESS_SUFFIX_MASK;
  bool const has_type_bits = (addr[1] != 0) || ((addr[2] & 0xC0) != 0);

  // Discovery suffix (0x3B) takes priority — typed discovery (e.g., 00 01 3B) is still DISCOVERY.
  if (suffix == ADDRESS_SUFFIX_DISCOVERY)
    return AddressClass::DISCOVERY;

  // When type bits are present with broadcast suffix (0x3F), this is a typed broadcast
  // addressing all devices of a specific type (e.g., 00 01 BF = "all light devices").
  // Only 00 00 3F (no type bits) is the true "all device types" broadcast.
  if (has_type_bits)
    return AddressClass::BROADCAST_TYPE;
  if (suffix == ADDRESS_SUFFIX_BROADCAST)
    return AddressClass::BROADCAST_ALL;
  if (addr[1] == 0 && addr[2] == 0)
    return AddressClass::BROADCAST_ALL;

  return AddressClass::UNKNOWN_BROADCAST;
}

const char *address_class_name(AddressClass address_class) {
  switch (address_class) {
    case AddressClass::UNICAST:
      return "unicast";
    case AddressClass::BROADCAST_ALL:
      return "broadcast_all";
    case AddressClass::BROADCAST_TYPE:
      return "broadcast_type";
    case AddressClass::DISCOVERY:
      return "discovery";
    case AddressClass::UNKNOWN_BROADCAST:
    default:
      return "unknown_broadcast";
  }
}

DeviceType broadcast_target_type(const uint8_t addr[NODE_ID_SIZE]) {
  if (addr[0] != 0x00)
    return DeviceType::UNKNOWN;

  // Device type is encoded in bits [9:2] of the combined bytes 1–2:
  // type = (addr[1] << 2) | (addr[2] >> 6)
  uint16_t const type_raw =
      (static_cast<uint16_t>(addr[1]) << DEVICE_TYPE_LOW_BITS_SHIFT) | (addr[2] >> DEVICE_TYPE_HIGH_BITS_SHIFT);

  if (type_raw > static_cast<uint16_t>(DeviceType::SWINGING_SHUTTER))
    return DeviceType::UNKNOWN;

  return static_cast<DeviceType>(type_raw);
}

void encode_broadcast_address(DeviceType type, uint8_t out[NODE_ID_SIZE]) {
  const auto type_raw = static_cast<uint16_t>(type);
  out[0] = 0;
  out[1] = static_cast<uint8_t>(type_raw >> DEVICE_TYPE_LOW_BITS_SHIFT);
  out[2] = static_cast<uint8_t>((type_raw << DEVICE_TYPE_HIGH_BITS_SHIFT) | DEVICE_SUBTYPE_MASK);
}

void decode_1w_main_intent(uint8_t main0, uint8_t main1, char *out, size_t out_size) {
  if (out_size == 0)
    return;
  // Special command codes (same wire values as 2W).
  if (main0 == POS_STOP) {
    snprintf(out, out_size, "STOP");
    return;
  }
  if (main0 == POS_FAVORITE) {
    if (main1 == POS_VENT_MODIFIER) {
      snprintf(out, out_size, "VENT");
    } else {
      snprintf(out, out_size, "FAVORITE");
    }
    return;
  }
  if (main0 == POS_UNKNOWN) {
    snprintf(out, out_size, "UNCHANGED");
    return;
  }
  if (main0 == POS_FORCE_OPEN) {
    // Note: 0x64 (100) is also the wire value for position 50% (50*2=100). The protocol
    // uses the same byte value for both. In practice, physical remotes rarely send numeric
    // 50% positions — they use open/close/stop/favorite. FORCE_OPEN is the more likely
    // interpretation for diagnostic decode of overheard 1W traffic.
    snprintf(out, out_size, "FORCE_OPEN");
    return;
  }
  if (main0 == POS_SECURED_TARGET) {
    snprintf(out, out_size, "SECURED_TARGET");
    return;
  }
  if (main0 == POS_DEFAULT) {
    snprintf(out, out_size, "DEFAULT");
    return;
  }
  // Numeric position: wire value is position_percent * POSITION_WIRE_SCALE (0=open, 200=closed).
  if (main0 <= POSITION_WIRE_MAX) {
    uint8_t const percent = main0 / POSITION_WIRE_SCALE;
    if (percent == 0) {
      snprintf(out, out_size, "OPEN");
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    } else if (percent == 100) {
      snprintf(out, out_size, "CLOSE");
    } else {
      snprintf(out, out_size, "position %u%%", percent);
    }
    return;
  }
  // Unknown special code.
  snprintf(out, out_size, "0x%02X", main0);
}

std::optional<float> oneway_intent_to_target(uint8_t main0, uint8_t main1) {
  (void) main1;
  // Special codes with no settled position (or explicitly "stop") never resolve to a target;
  // mirrors decode_1w_main_intent()'s branch order so the two stay in agreement.
  if (main0 == POS_STOP || main0 == POS_FAVORITE || main0 == POS_UNKNOWN || main0 == POS_FORCE_OPEN ||
      main0 == POS_SECURED_TARGET || main0 == POS_DEFAULT) {
    return std::nullopt;
  }
  // Wire value is position_percent * POSITION_WIRE_SCALE (0=open, 200=closed); divide as an
  // integer first, matching decode_1w_main_intent()'s percent computation, then convert to float.
  if (main0 <= POSITION_WIRE_MAX) {
    uint8_t const percent = main0 / POSITION_WIRE_SCALE;
    return static_cast<float>(percent);
  }
  return std::nullopt;
}

/// @brief Minimum data bytes for decode of execute/activate‑mode intent fields.
static constexpr uint8_t ONEWAY_EXECUTE_MIN_DATA_LEN = 4;  // originator(1) + ACEI(1) + main[2].

OneWayFrameInfo decode_1w_frame(const IoFrame &frame) {
  OneWayFrameInfo info{};
  memcpy(info.src, frame.src, NODE_ID_SIZE);
  info.address_class = classify_address(frame.dst);
  info.target_type = broadcast_target_type(frame.dst);
  info.cmd = frame.cmd;
  info.data_len = frame.data_len;

  // CMD 0x00 (execute) and 0x01 (activate mode) share the same initial payload layout:
  // originator(1) + ACEI(1) + main[2]. CMD 0x20 (write private) has a different layout
  // (register-based) and is not decoded here.
  if ((frame.cmd == CMD_EXECUTE || frame.cmd == CMD_ACTIVATE_MODE) && frame.data_len >= ONEWAY_EXECUTE_MIN_DATA_LEN) {
    info.has_intent = true;
    info.originator = frame.data[0];
    info.acei_level = (frame.data[1] & ACEI_LEVEL_MASK) >> ACEI_LEVEL_SHIFT;
    info.main0 = frame.data[2];
    info.main1 = frame.data[3];
    decode_1w_main_intent(frame.data[2], frame.data[3], info.intent, sizeof(info.intent));
  }

  return info;
}

DiscoveryResponseInfo decode_discovery_response(const IoFrame &frame, IoDevice &device, std::string &device_id) {
  DiscoveryResponseInfo info{};

  memcpy(device.node_id, frame.src, NODE_ID_SIZE);
  info.metadata_complete = frame.data_len >= DEVICE_METADATA_SIZE;
  if (info.metadata_complete) {
    device.type = decode_packed_device_type(frame.data[0], frame.data[1]);
    device.subtype = decode_packed_device_subtype(frame.data[1]);
    device.inverted = default_inverted_for_type(device.type);
  } else {
    device.type = DeviceType::UNKNOWN;
    device.subtype = 0;
    device.inverted = false;
  }
  device.position = UNKNOWN_POSITION;
  device.target = UNKNOWN_POSITION;
  device.is_stopped = true;
  device_id = node_id_to_string(device.node_id);

  info.has_extended = frame.data_len >= DISCOVERY_RESP_FULL_SIZE;
  if (frame.data_len > DISCOVERY_RESP_MANUFACTURER_OFFSET) {
    info.manufacturer = frame.data[DISCOVERY_RESP_MANUFACTURER_OFFSET];
  }
  if (frame.data_len > DISCOVERY_RESP_BACKBONE_OFFSET + NODE_ID_SIZE - 1) {
    memcpy(info.backbone, &frame.data[DISCOVERY_RESP_BACKBONE_OFFSET], NODE_ID_SIZE);
  }
  if (frame.data_len > DISCOVERY_RESP_FLAGS_OFFSET) {
    info.flags = frame.data[DISCOVERY_RESP_FLAGS_OFFSET];
  }
  if (frame.data_len > DISCOVERY_RESP_TIMESTAMP_OFFSET + 1) {
    info.timestamp =
        static_cast<uint16_t>((frame.data[DISCOVERY_RESP_TIMESTAMP_OFFSET] << DISCOVERY_TIMESTAMP_MSB_SHIFT) |
                              frame.data[DISCOVERY_RESP_TIMESTAMP_OFFSET + 1]);
  }

  return info;
}

// ============================================================================
// 1W Add-Controller Key Adoption (CMD 0x30)
// ============================================================================

OneWayAddControllerDecodeError decode_1w_add_controller(const IoFrame &frame, OneWayAdoptedKey &out) {
  out = OneWayAdoptedKey{};

  // Validate before decrypting -- reject rather than decrypt garbage. Order matches how a reader
  // would narrow down "what kind of frame is this" (protocol bit, then command, then shape).
  if ((frame.ctrl0 & CTRL0_PROTOCOL_1W) == 0)
    return OneWayAddControllerDecodeError::NOT_ONEWAY;
  if (frame.cmd != CMD_ONEWAY_ADD_CONTROLLER)
    return OneWayAddControllerDecodeError::WRONG_COMMAND;
  if (frame.data_len != ONEWAY_ADD_CONTROLLER_PAYLOAD_SIZE)
    return OneWayAddControllerDecodeError::BAD_LENGTH;

  const uint8_t *enc_key = &frame.data[ONEWAY_ADD_CONTROLLER_ENC_KEY_OFFSET];
  // Self-inverse wrap (crypto::crypt_1w_key()'s @warning) -- no direction flag: this same call
  // decrypts the overheard ciphertext back to the plaintext network key.
  if (!crypto::crypt_1w_key(frame.src, enc_key, out.system_key))
    return OneWayAddControllerDecodeError::KEY_UNWRAP_FAILED;

  out.manufacturer = frame.data[ONEWAY_ADD_CONTROLLER_MANUFACTURER_OFFSET];
  memcpy(out.sender_node, frame.src, NODE_ID_SIZE);
  out.sequence = static_cast<uint16_t>(
      (frame.data[ONEWAY_ADD_CONTROLLER_SEQUENCE_OFFSET] << ONEWAY_ADD_CONTROLLER_SEQUENCE_MSB_SHIFT) |
      frame.data[ONEWAY_ADD_CONTROLLER_SEQUENCE_OFFSET + 1]);

  // A frame with no MAC is not an error (the reference _p0x30 struct omits the field entirely) --
  // report NOT_PRESENT and stop; there is nothing to verify.
  if (!frame.has_mac) {
    out.mac_status = OneWayMacStatus::NOT_PRESENT;
    return OneWayAddControllerDecodeError::NONE;
  }

  // MAC span is command-specific: cmd + enc_key (17 bytes), NOT the whole declared payload --
  // see create_1w_hmac()'s @warning. Verified under the *recovered* key: a match is strong
  // evidence the decryption above landed on the correct key, before any device is ever commanded.
  uint8_t mac_span[ONEWAY_ADD_CONTROLLER_MAC_SPAN_SIZE];
  mac_span[0] = frame.cmd;
  memcpy(&mac_span[1], enc_key, AES_KEY_SIZE);

  uint8_t expected_mac[HMAC_SIZE];
  if (!crypto::create_1w_hmac(mac_span, sizeof(mac_span), out.sequence, out.system_key, expected_mac)) {
    out.mac_status = OneWayMacStatus::FAILED;
    return OneWayAddControllerDecodeError::NONE;
  }

  // Constant-time comparison, matching crypto::verify_hmac()'s convention.
  uint8_t diff = 0;
  for (uint8_t i = 0; i < HMAC_SIZE; i++)
    diff |= static_cast<uint8_t>(frame.mac[i] ^ expected_mac[i]);
  out.mac_status = (diff == 0) ? OneWayMacStatus::VERIFIED : OneWayMacStatus::FAILED;

  return OneWayAddControllerDecodeError::NONE;
}

}  // namespace home_io_control
}  // namespace esphome
