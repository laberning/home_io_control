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
/// Length of the mode write: the header plus one value byte.
constexpr size_t DHW_MODE_WRITE_LEN = 5;
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
/// Highest DhwMode value.
constexpr uint8_t DHW_MODE_MAX = static_cast<uint8_t>(DhwMode::MANUAL_ECO_OFF);

}  // namespace

size_t encode_dhw_register_read(uint8_t index, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]) {
  return encode_heating_register_read(DHW_REGISTER_HIGH_BYTE, index, out);
}

size_t encode_dhw_mode_write(uint8_t mode, uint8_t out[HEATING_PAYLOAD_MAX_SIZE]) {
  if (mode > DHW_MODE_MAX)
    return 0;
  out[0] = HEATING_PAYLOAD_PREFIX;
  out[1] = HEATING_DIRECTION_SET;
  out[2] = DHW_REGISTER_HIGH_BYTE;
  out[3] = DHW_REG_MODE;
  out[DHW_HEADER_LEN] = mode;
  return DHW_MODE_WRITE_LEN;
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
  const auto raw = static_cast<int16_t>((value[0] << BITS_PER_BYTE) | value[1]);
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

bool decode_dhw_boost(const uint8_t *value, size_t len, bool &on) {
  if (value == nullptr || len < 1)
    return false;
  on = value[0] != 0;
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
  const auto main_parameter = static_cast<uint16_t>((reply.data[DHW_STATUS_MAIN_PARAMETER_OFFSET] << BITS_PER_BYTE) |
                                                    reply.data[DHW_STATUS_MAIN_PARAMETER_OFFSET + 1]);
  return decode_dhw_target(main_parameter, celsius);
}

}  // namespace home_io_control
}  // namespace esphome
