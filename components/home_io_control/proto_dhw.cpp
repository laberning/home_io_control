/// @file proto_dhw.cpp
/// @brief Pure codec for a domestic hot water heater: its family-0x00 registers and its target temperature.
/// @ingroup hioc_protocol

#include "proto_dhw.h"

#include "proto_constants.h"
#include "proto_heating_profiles.h"
#include "proto_sizes.h"

namespace esphome {
namespace home_io_control {

namespace {

/// Length of a register request header: prefix, direction, register high byte, register low byte.
constexpr size_t DHW_HEADER_LEN = 4;
/// Length of a one-byte register write (mode, durations): the header plus one value byte.
constexpr size_t DHW_BYTE_WRITE_LEN = 5;
/// Value bytes each typed decoder needs.
constexpr size_t DHW_WATER_TEMPERATURE_LEN = 2;
constexpr size_t DHW_ENERGY_LEN = 4;
constexpr size_t DHW_HEATING_STATE_LEN = 5;
/// Byte positions inside the value of register 0x0021.
constexpr size_t DHW_HEATING_BYTE = 0;
constexpr size_t DHW_ELEMENT_BYTE = 2;
constexpr size_t DHW_COMPRESSOR_BYTE = 3;
constexpr size_t DHW_FAN_BYTE = 4;
/// Tenths of a degree per degree in register 0x0017.
constexpr float DHW_TENTHS_PER_DEGREE = 10.0F;
/// Status reply layout: byte offsets of the FPI1 and FPI2 echoes, and the length of the part before the values.
constexpr size_t DHW_STATUS_FPI1_OFFSET = 4;
constexpr size_t DHW_STATUS_FPI2_OFFSET = 5;
constexpr size_t DHW_STATUS_VALUES_OFFSET = 6;
/// Bytes per functional parameter value (FP9-FP16 are 16 bits wide).
constexpr size_t DHW_FP_VALUE_LEN = 2;
/// FPI2 bit of FP15 and FP16.
constexpr uint8_t DHW_FPI2_FP15_BIT = fp_bitmap_bit(DHW_FP_CAPABILITIES);
constexpr uint8_t DHW_FPI2_FP16_BIT = fp_bitmap_bit(DHW_FP_OPERATING_MODE);
/// FP16 fields: two bits each, 1 = on (0 keep, 2 off).
constexpr uint16_t DHW_FP16_FIELD_MASK = 0x3;
constexpr uint16_t DHW_FP16_FIELD_ON = 1;
constexpr uint8_t DHW_FP16_BOOST_SHIFT = 8;  ///< The relaunch field: bits 8-9.
constexpr uint8_t DHW_FP16_AWAY_SHIFT = 10;  ///< The absence field: bits 10-11.
/// One row per DhwOperatingFeature, in its order.
constexpr DhwOperatingFeatureWire DHW_OPERATING_FEATURES[] = {
    {DHW_CAPABILITY_BOOST, DHW_FP16_BOOST_SHIFT, DHW_OPERATING_MODE_BOOST_ON, DHW_REG_BOOST_DURATION,
     DHW_BOOST_START_DURATION},
    {DHW_CAPABILITY_AWAY, DHW_FP16_AWAY_SHIFT, DHW_OPERATING_MODE_AWAY_ON, DHW_REG_AWAY_DURATION,
     DHW_AWAY_UNTIL_TURNED_OFF},
};
static_assert(sizeof(DHW_OPERATING_FEATURES) / sizeof(DHW_OPERATING_FEATURES[0]) ==
                  static_cast<size_t>(DhwOperatingFeature::AWAY) + 1,
              "one row per DhwOperatingFeature");
/// Highest DhwMode value.
constexpr uint8_t DHW_MODE_MAX = static_cast<uint8_t>(DhwMode::MANUAL_ECO_OFF);

/// Two bytes, high byte first: every 16-bit value of this codec (temperature, main parameter, FP15, FP16).
uint16_t read_be16(const uint8_t *bytes) { return static_cast<uint16_t>((bytes[0] << BITS_PER_BYTE) | bytes[1]); }

/// Whether the two-bit FP16 field of `which` reads "on".
bool fp16_field_on(uint16_t operating_mode, DhwOperatingFeature which) {
  return ((operating_mode >> dhw_operating_feature_wire(which).fp16_shift) & DHW_FP16_FIELD_MASK) == DHW_FP16_FIELD_ON;
}

/// @brief Encode a one-byte write of a family-0x00 register: `0C 61 00 <index> <value>`.
size_t encode_dhw_byte_write(uint8_t index, uint8_t value, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]) {
  out[0] = HEATING_PAYLOAD_PREFIX;
  out[1] = HEATING_DIRECTION_SET;
  out[2] = DHW_REGISTER_HIGH_BYTE;
  out[3] = index;
  out[DHW_HEADER_LEN] = value;
  return DHW_BYTE_WRITE_LEN;
}

}  // namespace

size_t encode_dhw_register_read(uint8_t index, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]) {
  return encode_heating_register_read(DHW_REGISTER_HIGH_BYTE, index, out);
}

size_t encode_dhw_mode_write(uint8_t mode, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]) {
  if (mode > DHW_MODE_MAX)
    return 0;
  return encode_dhw_byte_write(DHW_REG_MODE, mode, out);
}

const DhwOperatingFeatureWire &dhw_operating_feature_wire(DhwOperatingFeature which) {
  return DHW_OPERATING_FEATURES[static_cast<size_t>(which)];
}

size_t encode_dhw_duration_write(DhwOperatingFeature which, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]) {
  const DhwOperatingFeatureWire &wire = dhw_operating_feature_wire(which);
  return encode_dhw_byte_write(wire.duration_register, wire.start_duration, out);
}

bool encode_dhw_target(float celsius, uint16_t &main_parameter) {
  // encode_temperature_main_parameter() clamps; a target outside the heater's own range is refused instead.
  if (!std::isfinite(celsius) || celsius < DHW_TARGET_MIN_C || celsius > DHW_TARGET_MAX_C)
    return false;
  return encode_temperature_main_parameter(celsius, DHW_TARGET_MIN_C, DHW_TARGET_MAX_C, main_parameter);
}

bool decode_dhw_register_reply(const IoFrame &reply, uint8_t index, const uint8_t *&value, size_t &len) {
  if (!heating_reply_echoes_register(reply, DHW_REGISTER_HIGH_BYTE, index))
    return false;
  value = reply.data + HEATING_REPLY_VALUE_OFFSET;
  len = reply.data_len - HEATING_REPLY_VALUE_OFFSET;
  return true;
}

bool decode_dhw_water_temperature(const uint8_t *value, size_t len, float &celsius) {
  if (value == nullptr || len < DHW_WATER_TEMPERATURE_LEN)
    return false;
  const auto raw = static_cast<int16_t>(read_be16(value));
  celsius = static_cast<float>(raw) / DHW_TENTHS_PER_DEGREE;
  return true;
}

bool decode_dhw_mode(const uint8_t *value, size_t len, DhwMode &mode) {
  if (value == nullptr || len < 1 || value[0] > DHW_MODE_MAX)
    return false;
  mode = static_cast<DhwMode>(value[0]);
  return true;
}

bool decode_dhw_heating_state(const uint8_t *value, size_t len, DhwHeatingState &state) {
  if (value == nullptr || len < DHW_HEATING_STATE_LEN)
    return false;
  state.heating = value[DHW_HEATING_BYTE] != 0;
  state.element = value[DHW_ELEMENT_BYTE] != 0;
  state.heat_pump = value[DHW_COMPRESSOR_BYTE] != 0 || value[DHW_FAN_BYTE] != 0;
  return true;
}

bool decode_dhw_energy_wh(const uint8_t *value, size_t len, uint32_t &wh) {
  if (value == nullptr || len < DHW_ENERGY_LEN)
    return false;
  wh = (static_cast<uint32_t>(value[0]) << (3U * BITS_PER_BYTE)) |
       (static_cast<uint32_t>(value[1]) << (2U * BITS_PER_BYTE)) | (static_cast<uint32_t>(value[2]) << BITS_PER_BYTE) |
       static_cast<uint32_t>(value[3]);
  return true;
}

bool decode_dhw_duration(const uint8_t *value, size_t len, uint8_t &duration) {
  if (value == nullptr || len < 1)
    return false;
  duration = value[0];
  return true;
}

bool decode_dhw_target(uint16_t main_parameter, float &celsius) {
  float raw = NAN;
  if (!decode_temperature_main_parameter(main_parameter, DHW_TARGET_MIN_C, DHW_TARGET_MAX_C, raw))
    return false;
  // One step of the scale is under a thousandth of a degree, so the heater's 60 C reads 59.9998 unrounded;
  // the display shows whole degrees, and tenths keep any finer setting visible.
  celsius = std::round(raw * DHW_TENTHS_PER_DEGREE) / DHW_TENTHS_PER_DEGREE;
  return true;
}

bool decode_dhw_status_target(const IoFrame &reply, float &celsius) {
  if (reply.cmd != CMD_PRIVATE_RESP || reply.data_len < DHW_STATUS_MAIN_PARAMETER_OFFSET + 2)
    return false;
  return decode_dhw_target(read_be16(reply.data + DHW_STATUS_MAIN_PARAMETER_OFFSET), celsius);
}

bool decode_dhw_operating_status(const IoFrame &reply, DhwOperatingStatus &status) {
  if (reply.cmd != CMD_PRIVATE_RESP || reply.data_len < DHW_STATUS_VALUES_OFFSET)
    return false;
  const uint8_t fpi2 = reply.data[DHW_STATUS_FPI2_OFFSET];
  if (reply.data[DHW_STATUS_FPI1_OFFSET] != DHW_STATUS_FPI1 || (fpi2 & DHW_FPI2_FP15_BIT) == 0 ||
      (fpi2 & DHW_FPI2_FP16_BIT) == 0)
    return false;

  // One two-byte value per FPI2 bit, bit 7 (FP9) first: walk the bits, keeping FP15 and FP16.
  size_t offset = DHW_STATUS_VALUES_OFFSET;
  uint16_t capabilities = 0;
  uint16_t operating_mode = 0;
  for (int bit = BITS_PER_BYTE - 1; bit >= 0; bit--) {
    if ((fpi2 & (1U << bit)) == 0)
      continue;
    if (offset + DHW_FP_VALUE_LEN > reply.data_len)
      return false;
    const uint16_t value = read_be16(reply.data + offset);
    offset += DHW_FP_VALUE_LEN;
    if ((1U << bit) == DHW_FPI2_FP15_BIT) {
      capabilities = value;
    } else if ((1U << bit) == DHW_FPI2_FP16_BIT) {
      operating_mode = value;
    }
  }

  DhwOperatingStatus decoded;
  decoded.capabilities = capabilities;
  decoded.boost = fp16_field_on(operating_mode, DhwOperatingFeature::BOOST);
  decoded.away = fp16_field_on(operating_mode, DhwOperatingFeature::AWAY);
  status = decoded;
  return true;
}

}  // namespace home_io_control
}  // namespace esphome
