/// @file proto_frame.cpp
/// @brief IO-Homecontrol 2W protocol implementation.

#include "proto_frame.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace esphome {
namespace home_io_control {

static int hex_nibble(char ch) {
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  if (ch >= 'A' && ch <= 'F')
    return 10 + (ch - 'A');
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
    const int low = hex_nibble(hex[i * 2 + 1]);
    if (high < 0 || low < 0)
      return false;
    out[i] = static_cast<uint8_t>((high << 4) | low);
  }

  return true;
}

std::string node_id_to_string(const uint8_t id[NODE_ID_SIZE]) {
  char buf[7];
  snprintf(buf, sizeof(buf), "%02X%02X%02X", id[0], id[1], id[2]);
  return std::string(buf);
}

bool default_inverted_for_type(DeviceType type) { return type == DeviceType::HORIZONTAL_AWNING; }

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
    for (uint8_t j = 0; j < 8; j++)
      crc = ((crc & 0x0001) != 0) ? (crc >> 1) ^ 0x8408 : crc >> 1;
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
    case DeviceType::ADJUSTABLE_SLAT_SHUTTER:
      return "adjustable_slat_shutter";
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
    case DeviceType::EXTERIOR_BLIND:
      return "exterior_blind";
    case DeviceType::EXTERNAL_VENETIAN_BLIND:
      return "external_venetian_blind";
    case DeviceType::LOUVRE_BLIND:
      return "louvre_blind";
    case DeviceType::CURTAIN:
      return "curtain";
    case DeviceType::CURTAIN_TRACK:
      return "curtain_track";
    case DeviceType::PERGOLA:
      return "pergola";
    case DeviceType::EXTERIOR_SCREEN:
      return "exterior_screen";
    case DeviceType::SWINGING_SHUTTER:
      return "swinging_shutter";
    case DeviceType::LOCK:
      return "lock";
    case DeviceType::HEATING:
      return "heating";
    case DeviceType::BEACON:
      return "beacon";
    case DeviceType::SENSOR:
      return "sensor";
  }

  return "unknown";
}

DeviceCapabilityClass device_capability_class(DeviceType type) {
  switch (type) {
    case DeviceType::ADJUSTABLE_SLAT_SHUTTER:
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
    case DeviceType::EXTERIOR_BLIND:
    case DeviceType::EXTERNAL_VENETIAN_BLIND:
    case DeviceType::LOUVRE_BLIND:
    case DeviceType::CURTAIN:
    case DeviceType::CURTAIN_TRACK:
    case DeviceType::PERGOLA:
    case DeviceType::EXTERIOR_SCREEN:
    case DeviceType::SWINGING_SHUTTER:
      return DeviceCapabilityClass::COVER;
    case DeviceType::LIGHT:
      return DeviceCapabilityClass::LIGHT;
    case DeviceType::ON_OFF_SWITCH:
      return DeviceCapabilityClass::SWITCH;
    case DeviceType::LOCK:
      return DeviceCapabilityClass::LOCK;
    case DeviceType::HEATING:
      return DeviceCapabilityClass::CLIMATE;
    case DeviceType::BEACON:
      return DeviceCapabilityClass::BEACON;
    case DeviceType::SENSOR:
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

bool device_supports_status_requests(DeviceType type) {
  return device_supports_position_control(type) || device_supports_binary_control(type);
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
