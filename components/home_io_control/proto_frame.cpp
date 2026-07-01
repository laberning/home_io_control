/// @file proto_frame.cpp
/// @brief IO-Homecontrol 2W protocol implementation.
/// @ingroup hioc_protocol

#include "proto_frame.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace esphome {
namespace home_io_control {

namespace {

constexpr int HEX_ALPHA_OFFSET = 10;
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

namespace {

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

static int hex_nibble(char ch) {
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  if (ch >= 'A' && ch <= 'F')
    return HEX_ALPHA_OFFSET + (ch - 'A');
  return -1;
}

bool hex_to_bytes(const std::string &hex, uint8_t *out, uint8_t len) {
  if (out == nullptr)
    return false;

  memset(out, 0, len);
  if (hex.length() != static_cast<size_t>(len) * 2)
    return false;

  for (uint8_t i = 0; i < len; i++) {
    const int high = hex_nibble(hex[i * 2]);
    const int low = hex_nibble(hex[(i * 2) + 1]);
    if (high < 0 || low < 0)
      return false;
    out[i] = static_cast<uint8_t>((high << 4) | low);
  }

  return true;
}

std::string node_id_to_string(const uint8_t id[NODE_ID_SIZE]) {
  char buf[NODE_ID_STRING_SIZE];
  snprintf(buf, sizeof(buf), "%02X%02X%02X", id[0], id[1], id[2]);
  return std::string(buf);
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

bool default_inverted_for_type(DeviceType type) { return type == DeviceType::HORIZONTAL_AWNING; }

const char *cover_command_name(CoverCommand cmd) {
  switch (cmd) {
    case CoverCommand::STOP:
      return "STOP";
    case CoverCommand::FAVORITE:
      return "FAVORITE";
    case CoverCommand::VENT:
      return "VENT";
    case CoverCommand::FORCE_OPEN:
      return "FORCE_OPEN";
    default:
      return "UNKNOWN_COVER_CMD";
  }
}

const char *command_name(uint8_t cmd) {
  switch (cmd) {
    case CMD_EXECUTE:
      return "EXECUTE";
    case CMD_ACTIVATE_MODE:
      return "ACTIVATE_MODE";
    case CMD_PRIVATE:
      return "PRIVATE";
    case CMD_PRIVATE_RESP:
      return "PRIVATE_RESP";
    case CMD_SET_SENSOR:
      return "SET_SENSOR";
    case CMD_SET_SENSOR_ACK:
      return "SET_SENSOR_ACK";
    case CMD_WRITE_PRIVATE:
      return "WRITE_PRIVATE";
    case CMD_WRITE_PRIVATE_ACK:
      return "WRITE_PRIVATE_ACK";
    case CMD_DISCOVER_REQ:
      return "DISCOVER_REQ";
    case CMD_DISCOVER_RESP:
      return "DISCOVER_RESP";
    case CMD_DISCOVER_SPE_REQ:
      return "DISCOVER_SPE_REQ";
    case CMD_DISCOVER_SPE_RESP:
      return "DISCOVER_SPE_RESP";
    case CMD_DISCOVER_CONFIRM:
      return "DISCOVER_CONFIRM";
    case CMD_DISCOVER_CONFIRM_ACK:
      return "DISCOVER_CONFIRM_ACK";
    case CMD_KEY_INIT:
      return "KEY_INIT";
    case CMD_KEY_TRANSFER:
      return "KEY_TRANSFER";
    case CMD_KEY_CONFIRM:
      return "KEY_CONFIRM";
    case CMD_ADDRESS_REQ:
      return "ADDRESS_REQ";
    case CMD_ADDRESS_RESP:
      return "ADDRESS_RESP";
    case CMD_LAUNCH_KEY_TRANSFER:
      return "LAUNCH_KEY_TRANSFER";
    case CMD_CHALLENGE_REQ:
      return "CHALLENGE_REQ";
    case CMD_CHALLENGE_RESP:
      return "CHALLENGE_RESP";
    case CMD_GET_NAME:
      return "GET_NAME";
    case CMD_GET_NAME_RESP:
      return "GET_NAME_RESP";
    case CMD_SET_NAME:
      return "SET_NAME";
    case CMD_SET_NAME_RESP:
      return "SET_NAME_RESP";
    case CMD_GET_INFO2:
      return "GET_INFO2";
    case CMD_GET_INFO2_RESP:
      return "GET_INFO2_RESP";
    case CMD_SET_CONFIG1:
      return "SET_CONFIG1";
    case CMD_SET_CONFIG1_RESP:
      return "SET_CONFIG1_RESP";
    case CMD_STATUS_UPDATE:
      return "STATUS_UPDATE";
    case CMD_STATUS_UPDATE_RESP:
      return "STATUS_UPDATE_RESP";
    case CMD_ERROR_RESP:
      return "ERROR_RESP";
    default:
      return "UNKNOWN_CMD";
  }
}

const char *manufacturer_name(uint8_t id) {
  switch (id) {
    case MANUFACTURER_VELUX:
      return "VELUX";
    case MANUFACTURER_SOMFY:
      return "Somfy";
    case MANUFACTURER_HONEYWELL:
      return "Honeywell";
    case MANUFACTURER_HORMANN:
      return "Hörmann";
    case MANUFACTURER_ASSA_ABLOY:
      return "ASSA ABLOY";
    case MANUFACTURER_NIKO:
      return "Niko";
    case MANUFACTURER_WINDOW_MASTER:
      return "WINDOW MASTER";
    case MANUFACTURER_RENSON:
      return "Renson";
    case MANUFACTURER_CIAT:
      return "CIAT";
    case MANUFACTURER_SECUYOU:
      return "Secuyou";
    case MANUFACTURER_OVERKIZ:
      return "OVERKIZ";
    case MANUFACTURER_ATLANTIC_GROUP:
      return "Atlantic Group";
    default:
      return "unknown";
  }
}

const char *att_class_name(uint8_t att_class) {
  switch (att_class) {
    case ATT_CLASS_5S:
      return "5s";
    case ATT_CLASS_10S:
      return "10s";
    case ATT_CLASS_20S:
      return "20s";
    case ATT_CLASS_40S:
      return "40s";
    default:
      return "unknown";
  }
}

const char *power_save_mode_name(uint8_t mode) {
  switch (mode) {
    case POWER_SAVE_ALWAYS_ALIVE:
      return "always_alive";
    case POWER_SAVE_LOW_POWER:
      return "low_power";
    default:
      return "unknown";
  }
}

const char *originator_name(uint8_t originator) {
  switch (originator) {
    case ORIGINATOR_LOCAL_USER:
      return "local_user";
    case ORIGINATOR_USER_REMOTE:
      return "user_remote";
    case ORIGINATOR_RAIN_SENSOR:
      return "rain_sensor";
    case ORIGINATOR_TIMER:
      return "timer";
    case ORIGINATOR_SECURITY:
      return "security";
    case ORIGINATOR_UPS:
      return "ups";
    case ORIGINATOR_SMART_CONTROLLER:
      return "smart_controller";
    case ORIGINATOR_LIFESTYLE:
      return "lifestyle";
    case ORIGINATOR_SAAC:
      return "saac";
    case ORIGINATOR_WIND_SENSOR:
      return "wind_sensor";
    case ORIGINATOR_LOAD_SHEDDING:
      return "load_shedding";
    case ORIGINATOR_LOCAL_LIGHT:
      return "local_light";
    case ORIGINATOR_ENVIRONMENT:
      return "environment";
    case ORIGINATOR_MYSELF:
      return "myself";
    case ORIGINATOR_AUTOMATIC_CYCLE:
      return "automatic_cycle";
    case ORIGINATOR_EMERGENCY:
      return "emergency";
    default:
      return "unknown";
  }
}

const char *acei_level_name(uint8_t level) {
  switch (level) {
    case ACEI_LEVEL_PROTECTION_HUMAN:
      return "protection_human";
    case ACEI_LEVEL_PROTECTION_SENSOR:
      return "protection_sensor";
    case ACEI_LEVEL_USER_HIGH:
      return "user_high";
    case ACEI_LEVEL_USER_DEFAULT:
      return "user_default";
    case ACEI_LEVEL_COMFORT_1:
      return "comfort_1";
    case ACEI_LEVEL_COMFORT_2:
      return "comfort_2";
    case ACEI_LEVEL_AUTO_SAAC:
      return "auto_saac";
    case ACEI_LEVEL_AUTO_DEFAULT:
      return "auto_default";
    default:
      return "unknown";
  }
}

AddressClass classify_address(const uint8_t addr[NODE_ID_SIZE]) {
  if (addr[0] != 0x00)
    return AddressClass::UNICAST;

  uint8_t const suffix = addr[2] & ADDRESS_SUFFIX_MASK;
  bool const has_type_bits = (addr[1] != 0) || ((addr[2] & 0xC0) != 0);

  if (suffix == ADDRESS_SUFFIX_BROADCAST)
    return AddressClass::BROADCAST_ALL;
  if (suffix == ADDRESS_SUFFIX_DISCOVERY)
    return AddressClass::DISCOVERY;
  if (has_type_bits)
    return AddressClass::BROADCAST_TYPE;
  if (addr[1] == 0 && addr[2] == 0)
    return AddressClass::BROADCAST_ALL;

  return AddressClass::UNKNOWN_BROADCAST;
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

const char *command_result_name(uint8_t result) {
  switch (result) {
    case RESULT_UNKNOWN_STATUS_REPLY:
      return "UNKNOWN_STATUS_REPLY";
    case RESULT_COMMAND_COMPLETED_OK:
      return "COMMAND_COMPLETED_OK";
    case RESULT_NO_CONTACT:
      return "NO_CONTACT";
    case RESULT_MANUALLY_OPERATED:
      return "MANUALLY_OPERATED";
    case RESULT_BLOCKED:
      return "BLOCKED";
    case RESULT_WRONG_SYSTEMKEY:
      return "WRONG_SYSTEMKEY";
    case RESULT_PRIORITY_LEVEL_LOCKED:
      return "PRIORITY_LEVEL_LOCKED";
    case RESULT_REACHED_WRONG_POSITION:
      return "REACHED_WRONG_POSITION";
    case RESULT_ERROR_DURING_EXECUTION:
      return "ERROR_DURING_EXECUTION";
    case RESULT_NO_EXECUTION:
      return "NO_EXECUTION";
    case RESULT_CALIBRATING:
      return "CALIBRATING";
    case RESULT_POWER_CONSUMPTION_TOO_HIGH:
      return "POWER_CONSUMPTION_TOO_HIGH";
    case RESULT_POWER_CONSUMPTION_TOO_LOW:
      return "POWER_CONSUMPTION_TOO_LOW";
    case RESULT_LOCK_POSITION_OPEN:
      return "LOCK_POSITION_OPEN";
    case RESULT_MOTION_TIME_TOO_LONG:
      return "MOTION_TIME_TOO_LONG";
    case RESULT_THERMAL_PROTECTION:
      return "THERMAL_PROTECTION";
    case RESULT_PRODUCT_NOT_OPERATIONAL:
      return "PRODUCT_NOT_OPERATIONAL";
    case RESULT_FILTER_MAINTENANCE_NEEDED:
      return "FILTER_MAINTENANCE_NEEDED";
    case RESULT_BATTERY_LEVEL:
      return "BATTERY_LEVEL";
    case RESULT_TARGET_MODIFIED:
      return "TARGET_MODIFIED";
    case RESULT_MODE_NOT_IMPLEMENTED:
      return "MODE_NOT_IMPLEMENTED";
    case RESULT_COMMAND_INCOMPATIBLE_TO_MOVEMENT:
      return "COMMAND_INCOMPATIBLE_TO_MOVEMENT";
    case RESULT_USER_ACTION:
      return "USER_ACTION";
    case RESULT_DEAD_BOLT_ERROR:
      return "DEAD_BOLT_ERROR";
    case RESULT_AUTOMATIC_CYCLE_ENGAGED:
      return "AUTOMATIC_CYCLE_ENGAGED";
    case RESULT_WRONG_LOAD_CONNECTED:
      return "WRONG_LOAD_CONNECTED";
    case RESULT_COLOUR_NOT_REACHABLE:
      return "COLOUR_NOT_REACHABLE";
    case RESULT_TARGET_NOT_REACHABLE:
      return "TARGET_NOT_REACHABLE";
    case RESULT_BAD_INDEX_RECEIVED:
      return "BAD_INDEX_RECEIVED";
    case RESULT_COMMAND_OVERRULED:
      return "COMMAND_OVERRULED";
    case RESULT_NODE_WAITING_FOR_POWER:
      return "NODE_WAITING_FOR_POWER";
    case RESULT_NODE_LOCKED:
      return "NODE_LOCKED";
    case RESULT_WRONG_POSITION:
      return "WRONG_POSITION";
    case RESULT_LIMITS_NOT_SET:
      return "LIMITS_NOT_SET";
    case RESULT_IP_NOT_SET:
      return "IP_NOT_SET";
    case RESULT_OUT_OF_RANGE:
      return "OUT_OF_RANGE";
    case RESULT_PRIORITY_LOCKED_NON_EXEC:
      return "PRIORITY_LOCKED";
    case RESULT_INFORMATION_CODE:
      return "INFORMATION_CODE";
    case RESULT_PARAMETER_LIMITED:
      return "PARAMETER_LIMITED";
    case RESULT_LIMITATION_BY_LOCAL_USER:
      return "LIMITATION_BY_LOCAL_USER";
    case RESULT_LIMITATION_BY_USER:
      return "LIMITATION_BY_USER";
    case RESULT_LIMITATION_BY_RAIN:
      return "LIMITATION_BY_RAIN";
    case RESULT_LIMITATION_BY_TIMER:
      return "LIMITATION_BY_TIMER";
    case RESULT_LIMITATION_BY_SCD:
      return "LIMITATION_BY_SCD";
    case RESULT_LIMITATION_BY_UPS:
      return "LIMITATION_BY_UPS";
    case RESULT_LIMITATION_BY_UNKNOWN_DEVICE:
      return "LIMITATION_BY_UNKNOWN_DEVICE";
    case RESULT_LIMITATION_BY_SAAC:
      return "LIMITATION_BY_SAAC";
    case RESULT_LIMITATION_BY_WIND:
      return "LIMITATION_BY_WIND";
    case RESULT_LIMITATION_BY_MYSELF:
      return "LIMITATION_BY_MYSELF";
    case RESULT_LIMITATION_BY_AUTOMATIC_CYCLE:
      return "LIMITATION_BY_AUTOMATIC_CYCLE";
    case RESULT_LIMITATION_BY_EMERGENCY:
      return "LIMITATION_BY_EMERGENCY";
    default:
      return "UNKNOWN_RESULT_CODE";
  }
}

const char *command_result_description(uint8_t result) {
  switch (result) {
    case RESULT_UNKNOWN_STATUS_REPLY:
      return "unknown reply";
    case RESULT_COMMAND_COMPLETED_OK:
      return "no errors detected";
    case RESULT_NO_CONTACT:
      return "no communication to node";
    case RESULT_MANUALLY_OPERATED:
      return "manually operated by a user";
    case RESULT_BLOCKED:
      return "node has been blocked by an object";
    case RESULT_WRONG_SYSTEMKEY:
      return "node contains the wrong system key";
    case RESULT_PRIORITY_LEVEL_LOCKED:
      return "node is locked on this priority level";
    case RESULT_REACHED_WRONG_POSITION:
      return "node stopped in another position than expected";
    case RESULT_ERROR_DURING_EXECUTION:
      return "an error occurred during command execution";
    case RESULT_NO_EXECUTION:
      return "no movement of the node parameter";
    case RESULT_CALIBRATING:
      return "node is calibrating the parameters";
    case RESULT_POWER_CONSUMPTION_TOO_HIGH:
      return "node power consumption is too high";
    case RESULT_POWER_CONSUMPTION_TOO_LOW:
      return "node power consumption is too low";
    case RESULT_LOCK_POSITION_OPEN:
      return "door open during lock command";
    case RESULT_MOTION_TIME_TOO_LONG:
      return "target was not reached in time";
    case RESULT_THERMAL_PROTECTION:
      return "node has gone into thermal protection mode";
    case RESULT_PRODUCT_NOT_OPERATIONAL:
      return "node is not currently operational";
    case RESULT_FILTER_MAINTENANCE_NEEDED:
      return "filter needs maintenance";
    case RESULT_BATTERY_LEVEL:
      return "battery level is low";
    case RESULT_TARGET_MODIFIED:
      return "node modified the requested target value";
    case RESULT_MODE_NOT_IMPLEMENTED:
      return "node does not support the received mode";
    case RESULT_COMMAND_INCOMPATIBLE_TO_MOVEMENT:
      return "node cannot move in the requested direction";
    case RESULT_USER_ACTION:
      return "user action overrode the command";
    case RESULT_DEAD_BOLT_ERROR:
      return "dead bolt error";
    case RESULT_AUTOMATIC_CYCLE_ENGAGED:
      return "node has gone into automatic cycle mode";
    case RESULT_WRONG_LOAD_CONNECTED:
      return "wrong load connected to node";
    case RESULT_COLOUR_NOT_REACHABLE:
      return "node cannot reach the requested colour";
    case RESULT_TARGET_NOT_REACHABLE:
      return "node cannot reach the requested target position";
    case RESULT_BAD_INDEX_RECEIVED:
      return "invalid index received";
    case RESULT_COMMAND_OVERRULED:
      return "command was overruled by a newer command";
    case RESULT_NODE_WAITING_FOR_POWER:
      return "node is waiting for power";
    case RESULT_NODE_LOCKED:
      return "node is locked";
    case RESULT_WRONG_POSITION:
      return "wrong position";
    case RESULT_LIMITS_NOT_SET:
      return "limits are not set";
    case RESULT_IP_NOT_SET:
      return "intermediate position is not set";
    case RESULT_OUT_OF_RANGE:
      return "requested value is out of range";
    case RESULT_PRIORITY_LOCKED_NON_EXEC:
      return "command priority too low, node rejected execution";
    case RESULT_INFORMATION_CODE:
      return "information-only result with unknown semantics";
    case RESULT_PARAMETER_LIMITED:
      return "parameter was limited by an unknown device";
    case RESULT_LIMITATION_BY_LOCAL_USER:
      return "parameter was limited by the local button";
    case RESULT_LIMITATION_BY_USER:
      return "parameter was limited by a remote control";
    case RESULT_LIMITATION_BY_RAIN:
      return "parameter was limited by a rain sensor";
    case RESULT_LIMITATION_BY_TIMER:
      return "parameter was limited by a timer";
    case RESULT_LIMITATION_BY_SCD:
      return "parameter was limited by a security controlling actuator";
    case RESULT_LIMITATION_BY_UPS:
      return "parameter was limited by a power supply";
    case RESULT_LIMITATION_BY_UNKNOWN_DEVICE:
      return "parameter was limited by an unknown device";
    case RESULT_LIMITATION_BY_SAAC:
      return "parameter was limited by a standalone automatic controller";
    case RESULT_LIMITATION_BY_WIND:
      return "parameter was limited by a wind sensor";
    case RESULT_LIMITATION_BY_MYSELF:
      return "parameter was limited by the node itself";
    case RESULT_LIMITATION_BY_AUTOMATIC_CYCLE:
      return "parameter was limited by an automatic cycle";
    case RESULT_LIMITATION_BY_EMERGENCY:
      return "parameter was limited by an emergency";
    default:
      return "unknown result code";
  }
}

bool is_limitation_result(uint8_t result) {
  switch (result) {
    case RESULT_PARAMETER_LIMITED:
    case RESULT_LIMITATION_BY_LOCAL_USER:
    case RESULT_LIMITATION_BY_USER:
    case RESULT_LIMITATION_BY_RAIN:
    case RESULT_LIMITATION_BY_TIMER:
    case RESULT_LIMITATION_BY_SCD:
    case RESULT_LIMITATION_BY_UPS:
    case RESULT_LIMITATION_BY_UNKNOWN_DEVICE:
    case RESULT_LIMITATION_BY_SAAC:
    case RESULT_LIMITATION_BY_WIND:
    case RESULT_LIMITATION_BY_MYSELF:
    case RESULT_LIMITATION_BY_AUTOMATIC_CYCLE:
    case RESULT_LIMITATION_BY_EMERGENCY:
      return true;
    default:
      return false;
  }
}

DeviceType decode_packed_device_type(uint8_t type_msb, uint8_t type_subtype) {
  return static_cast<DeviceType>((type_msb << DEVICE_TYPE_LOW_BITS_SHIFT) |
                                 (type_subtype >> DEVICE_TYPE_HIGH_BITS_SHIFT));
}

uint8_t decode_packed_device_subtype(uint8_t type_subtype) { return type_subtype & DEVICE_SUBTYPE_MASK; }

void decode_position_report(uint16_t target_raw, uint16_t current_raw, bool is_stopped, float &target,
                            float &position) {
  bool const target_valid = target_raw <= STATUS_POS_MAX;
  bool const current_valid = current_raw <= STATUS_POS_MAX;
  float const decoded_current = current_valid ? current_raw * 100.0F / STATUS_POS_MAX : UNKNOWN_POSITION;

  if (target_valid) {
    target = target_raw * 100.0F / STATUS_POS_MAX;
  } else if (is_stopped && current_valid) {
    // Marker values such as D2 (stop) and D4 (keep position during tilt) exceed STATUS_POS_MAX.
    // When the device says it is stopped and still gives a valid current position, use that as
    // the effective target instead of discarding the target entirely.
    target = decoded_current;
  } else {
    target = UNKNOWN_POSITION;
  }

  if (current_valid) {
    position = decoded_current;
  } else if (is_stopped && target_valid) {
    position = target;
  } else {
    position = UNKNOWN_POSITION;
  }
}

bool has_reached_target_position(float target, float position) {
  if (target == UNKNOWN_POSITION || position == UNKNOWN_POSITION)
    return false;
  float const tolerance = STATUS_POS_TOLERANCE_RAW * 100.0F / STATUS_POS_MAX;
  return std::fabs(target - position) <= tolerance;
}

float decode_tilt_report(uint16_t tilt_raw) {
  if (tilt_raw > STATUS_POS_MAX)
    return UNKNOWN_POSITION;
  return 100.0F - (tilt_raw * 100.0F / STATUS_POS_MAX);
}

/// CRC-CCITT used by the IO-Homecontrol protocol for frame validation.
/// Polynomial: 0x1021 (reversed 0x8408), initial value: 0x0000.
/// On SX1276 this is computed in hardware (IoHomeOn mode); on SX1262 it is
/// computed in software by the radio driver.
uint16_t crc_ccitt(const uint8_t *data, uint8_t len) {
  uint16_t crc = 0x0000;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < BITS_PER_BYTE; j++)
      crc = ((crc & CRC_LSB_MASK) != 0) ? (crc >> 1) ^ CRC_POLYNOMIAL_REVERSED : crc >> 1;
  }
  return crc;
}

void init_frame(IoFrame &f, bool is_2w, bool start, bool end, bool low_power) {
  memset(&f, 0, sizeof(IoFrame));
  if (end)
    f.ctrl0 |= CTRL0_END;
  if (start)
    f.ctrl0 |= CTRL0_START;
  if (!is_2w)
    f.ctrl0 |= CTRL0_PROTOCOL_1W;
  if (low_power)
    f.ctrl1 |= CTRL1_LOW_POWER;
}

void set_dst(IoFrame &f, const uint8_t id[NODE_ID_SIZE]) { memcpy(f.dst, id, NODE_ID_SIZE); }
void set_src(IoFrame &f, const uint8_t id[NODE_ID_SIZE]) { memcpy(f.src, id, NODE_ID_SIZE); }

bool set_cmd(IoFrame &f, uint8_t cmd, const uint8_t *params, uint8_t params_len) {
  if (params_len > FRAME_MAX_DATA_SIZE)
    return false;
  f.cmd = cmd;
  f.data_len = params_len;
  if (params != nullptr && params_len > 0)
    memcpy(f.data, params, params_len);
  uint8_t const total = FRAME_MIN_SIZE + f.data_len;
  // Refuse to encode inconsistent frame metadata here so malformed commands never make it onto
  // the radio path and later confuse the serializer or on-air retries.
  if (total > FRAME_MAX_SIZE)
    return false;
  f.ctrl0 = (f.ctrl0 & ~CTRL0_LENGTH_MASK) | ((total - 1) & CTRL0_LENGTH_MASK);
  return true;
}

uint8_t frame_length(const IoFrame &f) { return (f.ctrl0 & CTRL0_LENGTH_MASK) + 1; }
bool is_start(const IoFrame &f) { return (f.ctrl0 & CTRL0_START) != 0; }
bool is_end(const IoFrame &f) { return (f.ctrl0 & CTRL0_END) != 0; }

uint8_t serialize(const IoFrame &f, uint8_t *buf, uint8_t buf_size) {
  if (buf == nullptr)
    return 0;
  uint8_t const len = frame_length(f);
  if (len < FRAME_MIN_SIZE || len > FRAME_MAX_SIZE)
    return 0;
  if (f.data_len > FRAME_MAX_DATA_SIZE)
    return 0;
  // Keep the wire length derived from ctrl0 and the explicit payload length in lockstep. This
  // catches partially initialized frames before they are transmitted.
  if ((uint8_t) (FRAME_MIN_SIZE + f.data_len) != len)
    return 0;
  if (buf_size < len)
    return 0;
  uint8_t offset = 0;
  buf[offset++] = f.ctrl0;
  buf[offset++] = f.ctrl1;
  memcpy(&buf[offset], f.dst, NODE_ID_SIZE);
  offset += NODE_ID_SIZE;
  memcpy(&buf[offset], f.src, NODE_ID_SIZE);
  offset += NODE_ID_SIZE;
  buf[offset++] = f.cmd;
  memcpy(&buf[offset], f.data, f.data_len);
  offset += f.data_len;
  return offset;
}

bool parse(const uint8_t *buf, uint8_t buf_len, IoFrame &f) {
  if (buf == nullptr)
    return false;
  if (buf_len < FRAME_MIN_SIZE)
    return false;
  memset(&f, 0, sizeof(IoFrame));
  uint8_t offset = 0;
  f.ctrl0 = buf[offset++];
  f.ctrl1 = buf[offset++];
  uint8_t const len = frame_length(f);
  if (len < FRAME_MIN_SIZE || len > FRAME_MAX_SIZE)
    return false;
  if (buf_len != len)
    return false;
  if (offset + NODE_ID_SIZE > buf_len)
    return false;
  memcpy(f.dst, &buf[offset], NODE_ID_SIZE);
  offset += NODE_ID_SIZE;
  if (offset + NODE_ID_SIZE > buf_len)
    return false;
  memcpy(f.src, &buf[offset], NODE_ID_SIZE);
  offset += NODE_ID_SIZE;
  if (offset >= buf_len)
    return false;
  f.cmd = buf[offset++];
  f.data_len = len - FRAME_MIN_SIZE;
  if (f.data_len > FRAME_MAX_DATA_SIZE)
    return false;
  if (offset + f.data_len > buf_len)
    return false;
  memcpy(f.data, &buf[offset], f.data_len);
  return true;
}

const char *device_type_name(DeviceType type) {
  switch (type) {
    case DeviceType::UNKNOWN:
      return "unknown";
    case DeviceType::VENETIAN_BLIND:
      return "venetian_blind";
    case DeviceType::ROLLER_SHUTTER:
      return "roller_shutter";
    case DeviceType::SCREEN:
      return "screen";
    case DeviceType::AWNING:
      return "awning";
    case DeviceType::WINDOW_OPENER:
      return "window_opener";
    case DeviceType::GARAGE_OPENER:
      return "garage_opener";
    case DeviceType::LIGHT:
      return "light";
    case DeviceType::GATE_OPENER:
      return "gate_opener";
    case DeviceType::ROLLING_DOOR_OPENER:
      return "rolling_door_opener";
    case DeviceType::BLIND:
      return "blind";
    case DeviceType::DUAL_SHUTTER:
      return "dual_shutter";
    case DeviceType::ON_OFF_SWITCH:
      return "on_off_switch";
    case DeviceType::HORIZONTAL_AWNING:
      return "horizontal_awning";
    case DeviceType::EXTERNAL_VENETIAN_BLIND:
      return "external_venetian_blind";
    case DeviceType::LOUVRE_BLIND:
      return "louvre_blind";
    case DeviceType::CURTAIN_TRACK:
      return "curtain_track";
    case DeviceType::SWINGING_SHUTTER:
      return "swinging_shutter";
    case DeviceType::LOCK:
      return "lock";
    case DeviceType::BEACON:
      return "beacon";
    case DeviceType::HEATING_TEMPERATURE_INTERFACE:
      return "heating_temperature_interface";
    case DeviceType::VENTILATION_POINT:
      return "ventilation_point";
    case DeviceType::EXTERIOR_HEATING:
      return "exterior_heating";
    case DeviceType::HEAT_PUMP:
      return "heat_pump";
    case DeviceType::INTRUSION_ALARM:
      return "intrusion_alarm";
  }

  return "unknown";
}

const char *yaml_device_type_name(DeviceType type) {
  switch (type) {
    case DeviceType::UNKNOWN:
      return "unknown";
    case DeviceType::VENETIAN_BLIND:
      return "venetian_blind";
    case DeviceType::ROLLER_SHUTTER:
      return "roller_shutter";
    case DeviceType::AWNING:
      return "awning";
    case DeviceType::WINDOW_OPENER:
      return "window_opener";
    case DeviceType::GARAGE_OPENER:
      return "garage_opener";
    case DeviceType::LIGHT:
      return "light";
    case DeviceType::GATE_OPENER:
      return "gate_opener";
    case DeviceType::ROLLING_DOOR_OPENER:
      return "rolling_door_opener";
    case DeviceType::LOCK:
      return "lock";
    case DeviceType::BLIND:
      return "blind";
    case DeviceType::SCREEN:
      return "screen";
    case DeviceType::DUAL_SHUTTER:
      return "dual_shutter";
    case DeviceType::HEATING_TEMPERATURE_INTERFACE:
      return "heating_temperature_interface";
    case DeviceType::ON_OFF_SWITCH:
      return "on_off_switch";
    case DeviceType::HORIZONTAL_AWNING:
      return "horizontal_awning";
    case DeviceType::EXTERNAL_VENETIAN_BLIND:
      return "external_venetian_blind";
    case DeviceType::LOUVRE_BLIND:
      return "louvre_blind";
    case DeviceType::CURTAIN_TRACK:
      return "curtain_track";
    case DeviceType::INTRUSION_ALARM:
      return "intrusion_alarm";
    case DeviceType::SWINGING_SHUTTER:
      return "swinging_shutter";
    default:
      return nullptr;
  }
}

DeviceCapabilityClass device_capability_class(DeviceType type) {
  switch (type) {
    // Cover types (position-controlled)
    case DeviceType::VENETIAN_BLIND:
    case DeviceType::ROLLER_SHUTTER:
    case DeviceType::SCREEN:
    case DeviceType::AWNING:
    case DeviceType::WINDOW_OPENER:
    case DeviceType::GARAGE_OPENER:
    case DeviceType::GATE_OPENER:
    case DeviceType::ROLLING_DOOR_OPENER:
    case DeviceType::BLIND:
    case DeviceType::DUAL_SHUTTER:
    case DeviceType::HORIZONTAL_AWNING:
    case DeviceType::EXTERNAL_VENETIAN_BLIND:
    case DeviceType::LOUVRE_BLIND:
    case DeviceType::CURTAIN_TRACK:
    case DeviceType::SWINGING_SHUTTER:
      return DeviceCapabilityClass::COVER;

    // Binary and other capabilities
    case DeviceType::LIGHT:
      return DeviceCapabilityClass::LIGHT;
    case DeviceType::ON_OFF_SWITCH:
      return DeviceCapabilityClass::SWITCH;
    case DeviceType::LOCK:
      return DeviceCapabilityClass::LOCK;
    case DeviceType::HEATING_TEMPERATURE_INTERFACE:
    case DeviceType::EXTERIOR_HEATING:
    case DeviceType::HEAT_PUMP:
      return DeviceCapabilityClass::CLIMATE;
    case DeviceType::VENTILATION_POINT:
      // Binary ventilation on/off; treated as switch
      return DeviceCapabilityClass::SWITCH;
    case DeviceType::BEACON:
      return DeviceCapabilityClass::BEACON;
    case DeviceType::INTRUSION_ALARM:
      return DeviceCapabilityClass::SENSOR;

    case DeviceType::UNKNOWN:
    default:
      return DeviceCapabilityClass::UNKNOWN;
  }
}

const char *device_capability_class_name(DeviceType type) {
  switch (device_capability_class(type)) {
    case DeviceCapabilityClass::COVER:
      return "cover";
    case DeviceCapabilityClass::LIGHT:
      return "light";
    case DeviceCapabilityClass::SWITCH:
      return "switch";
    case DeviceCapabilityClass::SENSOR:
      return "sensor";
    case DeviceCapabilityClass::BEACON:
      return "beacon";
    case DeviceCapabilityClass::CLIMATE:
      return "climate";
    case DeviceCapabilityClass::LOCK:
      return "lock";
    case DeviceCapabilityClass::UNKNOWN:
    default:
      return "unknown";
  }
}

bool device_supports_position_control(DeviceType type) {
  return device_capability_class(type) == DeviceCapabilityClass::COVER;
}

bool device_supports_binary_control(DeviceType type) {
  DeviceCapabilityClass const capability_class = device_capability_class(type);
  return capability_class == DeviceCapabilityClass::LIGHT || capability_class == DeviceCapabilityClass::SWITCH;
}

bool device_supports_lock_control(DeviceType type) {
  return device_capability_class(type) == DeviceCapabilityClass::LOCK;
}

bool device_supports_status_requests(DeviceType type) {
  return device_supports_position_control(type) || device_supports_binary_control(type) ||
         device_supports_lock_control(type);
}

bool device_supports_tilt(DeviceType type) {
  switch (type) {
    case DeviceType::VENETIAN_BLIND:
    case DeviceType::BLIND:
    case DeviceType::EXTERNAL_VENETIAN_BLIND:
    case DeviceType::LOUVRE_BLIND:
      return true;
    default:
      return false;
  }
}

bool device_supports_vent(DeviceType type) {
  switch (type) {
    case DeviceType::WINDOW_OPENER:
    case DeviceType::VENTILATION_POINT:
      return true;
    default:
      return false;
  }
}

const char *device_operation_profile_name(DeviceType type) {
  if (device_supports_position_control(type))
    return device_supports_tilt(type) ? "cover_position_tilt" : "cover_position";
  if (device_supports_binary_control(type))
    return "binary_on_off";

  switch (device_capability_class(type)) {
    case DeviceCapabilityClass::LOCK:
      return "lock";
    case DeviceCapabilityClass::CLIMATE:
      return "climate";
    case DeviceCapabilityClass::SENSOR:
      return "sensor";
    case DeviceCapabilityClass::BEACON:
      return "beacon";
    case DeviceCapabilityClass::UNKNOWN:
    default:
      return "unknown";
  }
}

}  // namespace home_io_control
}  // namespace esphome
