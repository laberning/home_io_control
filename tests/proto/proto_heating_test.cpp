/// @file proto_heating_test.cpp
/// @brief Byte-exact tests for the heater register codec (CMD_WRITE_PRIVATE 0x20 / ack 0x21).

#include "proto_heating.h"
#include "reading_store.h"

#include "proto_constants.h"
#include "proto_private_registers.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

using namespace esphome::home_io_control;

namespace {

std::vector<uint8_t> encode(DeviceRegister fn, float value) {
  uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE] = {0};
  const size_t n = encode_heating_payload(fn, value, out);
  return std::vector<uint8_t>(out, out + n);
}

std::vector<uint8_t> encode_read(DeviceRegister fn) {
  uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE] = {0};
  const size_t n = encode_heating_read(fn, out);
  return std::vector<uint8_t>(out, out + n);
}

size_t write_len(DeviceRegister fn, float value) {
  uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE] = {0};
  return encode_heating_write(fn, value, out);
}

/// A 0x21 reply with the given data bytes.
IoFrame ack_with(const std::vector<uint8_t> &data, uint8_t cmd = CMD_WRITE_PRIVATE_ACK) {
  IoFrame f{};
  f.cmd = cmd;
  f.data_len = static_cast<uint8_t>(data.size());
  for (size_t i = 0; i < data.size(); i++)
    f.data[i] = data[i];
  return f;
}

}  // namespace

// --- Byte-exact payloads for every function ---

TEST(ProtoHeating, PowerOnPayload) {
  EXPECT_EQ(encode(DeviceRegister::PAIRED_DEVICES, 0.0f), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x2C}));
}

TEST(ProtoHeating, SetTemperaturePayload) {
  // Register 0x0103, little-endian tenths: 20.5 C -> 205 -> 0x00CD -> CD 00.
  EXPECT_EQ(encode(DeviceRegister::COMFORT_SETPOINT, 20.5f),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00}));
}

TEST(ProtoHeating, SetModePayload) {
  EXPECT_EQ(encode(DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::MANUAL)),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x00, 0x01}));
}

TEST(ProtoHeating, SetPresencePayload) {
  EXPECT_EQ(encode(DeviceRegister::PRESENCE, 1.0f), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x10, 0x01}));
  EXPECT_EQ(encode(DeviceRegister::PRESENCE, 0.0f), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x10, 0x00}));
}

TEST(ProtoHeating, OpenWindowDetectionWritesRegister0106) {
  EXPECT_EQ(encode(DeviceRegister::OPEN_WINDOW_DETECTION, 1.0f), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x06, 0x01}))
      << "set_open_window_detection on";
  EXPECT_EQ(encode(DeviceRegister::OPEN_WINDOW_DETECTION, 0.0f), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x06, 0x00}));
}

TEST(ProtoHeating, MidnightSyncPayload) {
  EXPECT_EQ(encode(DeviceRegister::SETPOINT_BLOCK, 0.0f), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x30}));
}

// --- Reads: one table drives get as well as set ---

TEST(ProtoHeating, ReadPayloadsAddressTheSameRegisterAsTheWrite) {
  EXPECT_EQ(encode_read(DeviceRegister::COMFORT_SETPOINT), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x03}));
  EXPECT_EQ(encode_read(DeviceRegister::OPERATING_MODE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x00}));
  EXPECT_EQ(encode_read(DeviceRegister::OPEN_WINDOW_DETECTION), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x06}));
  EXPECT_EQ(encode_read(DeviceRegister::ROOM_TEMPERATURE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x15}));
  EXPECT_EQ(encode_read(DeviceRegister::REGULATOR_STATE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x17}));
  EXPECT_EQ(encode_read(DeviceRegister::MAX_SETPOINT), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x02}));
  EXPECT_EQ(encode_read(DeviceRegister::WINDOW_OPEN), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x0E}));
}

TEST(ProtoHeating, PresenceRegisterIsReadAndWritten) {
  EXPECT_EQ(encode_read(DeviceRegister::PRESENCE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x10}));
  EXPECT_TRUE(device_register_is_readable(DeviceRegister::PRESENCE));
  EXPECT_TRUE(device_register_is_writable(DeviceRegister::PRESENCE));
}

TEST(ProtoHeating, AFunctionPastTheTableHasNoRow) {
  EXPECT_FALSE(device_register_is_readable(DeviceRegister::COUNT));
  EXPECT_FALSE(device_register_is_writable(DeviceRegister::COUNT));
}

TEST(ProtoHeating, ReadOnlyRegistersAreNeverWritten) {
  // The window sensor is not a setting: 0x010E must not be writable whatever the value.
  EXPECT_EQ(write_len(DeviceRegister::WINDOW_OPEN, 1.0f), 0u);
  EXPECT_EQ(write_len(DeviceRegister::WINDOW_OPEN, 0.0f), 0u);
  EXPECT_EQ(write_len(DeviceRegister::ROOM_TEMPERATURE, 20.0f), 0u);
  EXPECT_EQ(write_len(DeviceRegister::REGULATOR_STATE, 1.0f), 0u);
  EXPECT_EQ(write_len(DeviceRegister::MAX_SETPOINT, 28.0f), 0u);
  EXPECT_EQ(write_len(DeviceRegister::PAIRED_DEVICES, 0.0f), 0u);
  EXPECT_EQ(write_len(DeviceRegister::SETPOINT_BLOCK, 0.0f), 0u);
  EXPECT_FALSE(device_register_is_writable(DeviceRegister::WINDOW_OPEN));
  EXPECT_TRUE(device_register_is_readable(DeviceRegister::WINDOW_OPEN));
}

TEST(ProtoHeating, ActionPayloadOfAReadOnlyRegisterIsItsRead) {
  // The heating_control action runs a read for a read-only register, so `window_open` reads 0x010E.
  EXPECT_EQ(encode(DeviceRegister::WINDOW_OPEN, 1.0f), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x0E}));
}

// --- Modes ---

TEST(ProtoHeating, AllWritableModes) {
  EXPECT_EQ(encode(DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::AUTO)).back(), 0x00);
  EXPECT_EQ(encode(DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::MANUAL)).back(), 0x01);
  EXPECT_EQ(encode(DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::PROG)).back(), 0x02);
  EXPECT_EQ(encode(DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::OFF)).back(), 0x04);
}

TEST(ProtoHeating, RejectsUnwritableModes) {
  // 0x03 (external) is readable but this component never writes it.
  EXPECT_EQ(write_len(DeviceRegister::OPERATING_MODE, 3.0f), 0u);
  EXPECT_EQ(write_len(DeviceRegister::OPERATING_MODE, 1.5f), 0u);
  EXPECT_EQ(write_len(DeviceRegister::OPERATING_MODE, 5.0f), 0u);
  // Values outside a byte stay the codec's to refuse; none of them is narrowed onto a mode number.
  EXPECT_EQ(write_len(DeviceRegister::OPERATING_MODE, -1.0f), 0u);
  EXPECT_EQ(write_len(DeviceRegister::OPERATING_MODE, 259.0f), 0u);
  EXPECT_EQ(write_len(DeviceRegister::OPERATING_MODE, std::numeric_limits<float>::quiet_NaN()), 0u);
  EXPECT_EQ(write_len(DeviceRegister::OPERATING_MODE, std::numeric_limits<float>::infinity()), 0u);
}

TEST(ProtoHeating, OnlyTheExternalSetpointModeIsNotWritable) {
  for (const HeatingMode mode : {HeatingMode::AUTO, HeatingMode::MANUAL, HeatingMode::PROG, HeatingMode::OFF})
    EXPECT_TRUE(heating_mode_is_writable(mode)) << static_cast<int>(mode);
  EXPECT_FALSE(heating_mode_is_writable(HeatingMode::EXTERNAL_SETPOINT));
  // The encoder follows the rule for every mode number of the register's range.
  for (uint8_t v = 0; v <= 4; v++)
    EXPECT_EQ(write_len(DeviceRegister::OPERATING_MODE, static_cast<float>(v)) != 0u,
              heating_mode_is_writable(static_cast<HeatingMode>(v)))
        << static_cast<int>(v);
}

TEST(ProtoHeating, RejectsNonBinaryPresenceAndDetection) {
  EXPECT_EQ(write_len(DeviceRegister::PRESENCE, 2.0f), 0u);
  EXPECT_EQ(write_len(DeviceRegister::OPEN_WINDOW_DETECTION, 0.5f), 0u);
}

// --- Temperature matrix: little-endian tenths of a degree ---

TEST(ProtoHeating, TemperatureMatrix) {
  const std::vector<uint8_t> prefix = {0x0C, 0x61, 0x01, 0x03};
  auto expect_temp = [&](float degrees, uint8_t lo, uint8_t hi) {
    std::vector<uint8_t> want = prefix;
    want.push_back(lo);
    want.push_back(hi);
    EXPECT_EQ(encode(DeviceRegister::COMFORT_SETPOINT, degrees), want) << degrees << " C";
  };
  expect_temp(7.0f, 0x46, 0x00);    // 70   = 0x0046
  expect_temp(20.5f, 0xCD, 0x00);   // 205  = 0x00CD
  expect_temp(20.55f, 0xCE, 0x00);  // 205.5f -> round-half-away-from-zero -> 206 = 0x00CE
  expect_temp(25.5f, 0xFF, 0x00);   // 255  = 0x00FF
  expect_temp(26.0f, 0x04, 0x01);   // 260  = 0x0104
  expect_temp(28.0f, 0x18, 0x01);   // 280  = 0x0118
}

TEST(ProtoHeating, TemperatureRejectsOutOfRangeAndNaN) {
  EXPECT_EQ(write_len(DeviceRegister::COMFORT_SETPOINT, 6.9f), 0u);
  EXPECT_EQ(write_len(DeviceRegister::COMFORT_SETPOINT, 28.1f), 0u);
  EXPECT_EQ(write_len(DeviceRegister::COMFORT_SETPOINT, 53.5f), 0u);
  EXPECT_EQ(write_len(DeviceRegister::COMFORT_SETPOINT, std::nanf("")), 0u);
  EXPECT_EQ(write_len(DeviceRegister::COMFORT_SETPOINT, INFINITY), 0u);
}

TEST(ProtoHeating, PayloadLengths) {
  EXPECT_EQ(encode(DeviceRegister::PAIRED_DEVICES, 0.0f).size(), 4u);
  EXPECT_EQ(encode(DeviceRegister::SETPOINT_BLOCK, 0.0f).size(), 4u);
  EXPECT_EQ(encode(DeviceRegister::OPERATING_MODE, 0.0f).size(), 5u);
  EXPECT_EQ(encode(DeviceRegister::PRESENCE, 0.0f).size(), 5u);
  EXPECT_EQ(encode(DeviceRegister::OPEN_WINDOW_DETECTION, 0.0f).size(), 5u);
  EXPECT_EQ(encode(DeviceRegister::COMFORT_SETPOINT, 20.0f).size(), 6u);
}

TEST(ProtoHeating, FunctionNames) {
  EXPECT_STREQ(device_register_name(DeviceRegister::PAIRED_DEVICES), "power_on");
  EXPECT_STREQ(device_register_name(DeviceRegister::COMFORT_SETPOINT), "set_temperature");
  EXPECT_STREQ(device_register_name(DeviceRegister::OPERATING_MODE), "set_mode");
  EXPECT_STREQ(device_register_name(DeviceRegister::PRESENCE), "set_presence");
  EXPECT_STREQ(device_register_name(DeviceRegister::OPEN_WINDOW_DETECTION), "set_open_window_detection");
  EXPECT_STREQ(device_register_name(DeviceRegister::SETPOINT_BLOCK), "midnight_sync");
  EXPECT_STREQ(device_register_name(DeviceRegister::ROOM_TEMPERATURE), "room_temperature");
  EXPECT_STREQ(device_register_name(DeviceRegister::REGULATOR_STATE), "regulator_state");
  EXPECT_STREQ(device_register_name(DeviceRegister::MAX_SETPOINT), "max_setpoint");
  EXPECT_STREQ(device_register_name(DeviceRegister::WINDOW_OPEN), "window_open");
}

TEST(ProtoHeating, PowerOnAndMidnightIgnoreValue) {
  EXPECT_EQ(encode(DeviceRegister::PAIRED_DEVICES, 999.0f), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x2C}));
  EXPECT_EQ(encode(DeviceRegister::SETPOINT_BLOCK, std::nanf("")), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x30}));
}

TEST(ProtoHeating, RejectsOutOfRangeFunctionEnum) {
  // An enum value not in the table must be refused and must never touch the output buffer.
  uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE];
  for (auto &b : out)
    b = 0xEE;
  const auto bogus = static_cast<DeviceRegister>(0x7F);
  EXPECT_EQ(encode_heating_payload(bogus, 20.0f, out), 0u);
  EXPECT_EQ(encode_heating_read(bogus, out), 0u);
  EXPECT_EQ(encode_heating_write(bogus, 20.0f, out), 0u);
  for (uint8_t b : out)
    EXPECT_EQ(b, 0xEE) << "a rejected function must not write the output buffer";
  EXPECT_STREQ(device_register_name(bogus), "unknown");
  EXPECT_FALSE(device_register_is_readable(bogus));
  EXPECT_FALSE(device_register_is_writable(bogus));
}

// --- Decoding the device's reply ---

TEST(ProtoHeating, DecodesRoomTemperatureInHundredths) {
  float v = 0.0f;
  // 0x081B = 2075 -> 20.75 C
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x15, 0x1B, 0x08}), DeviceRegister::ROOM_TEMPERATURE, v));
  EXPECT_FLOAT_EQ(v, 20.75f);
  // 0x0659 = 1625 -> 16.25 C
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x15, 0x59, 0x06}), DeviceRegister::ROOM_TEMPERATURE, v));
  EXPECT_FLOAT_EQ(v, 16.25f);
}

TEST(ProtoHeating, RoomTemperatureSensorFaultDecodesAsNaN) {
  float v = 0.0f;
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x15, 0xFF, 0xFF}), DeviceRegister::ROOM_TEMPERATURE, v))
      << "the heater did answer";
  EXPECT_TRUE(std::isnan(v));
}

TEST(ProtoHeating, DecodesSetpointAndMaximumInTenths) {
  float v = 0.0f;
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x03, 0xC3, 0x00}), DeviceRegister::COMFORT_SETPOINT, v));
  EXPECT_FLOAT_EQ(v, 19.5f);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x02, 0x18, 0x01}), DeviceRegister::MAX_SETPOINT, v));
  EXPECT_FLOAT_EQ(v, 28.0f);
}

TEST(ProtoHeating, DecodesModeBytes) {
  float v = -1.0f;
  for (uint8_t mode : {0, 1, 2, 3, 4}) {
    ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x00, mode}), DeviceRegister::OPERATING_MODE, v));
    EXPECT_FLOAT_EQ(v, static_cast<float>(mode));
  }
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x00, 0x05}), DeviceRegister::OPERATING_MODE, v))
      << "an undefined mode is not a value";
}

TEST(ProtoHeating, DecodesRegulatorStates) {
  float v = -1.0f;
  for (uint8_t state : {0, 1, 2}) {
    ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x17, state}), DeviceRegister::REGULATOR_STATE, v));
    EXPECT_FLOAT_EQ(v, static_cast<float>(state));
  }
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x17, 0x03}), DeviceRegister::REGULATOR_STATE, v));
}

TEST(ProtoHeating, DecodesWindowSensor) {
  float v = -1.0f;
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x0E, 0x00}), DeviceRegister::WINDOW_OPEN, v));
  EXPECT_FLOAT_EQ(v, 0.0f);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x0E, 0x01}), DeviceRegister::WINDOW_OPEN, v));
  EXPECT_FLOAT_EQ(v, 1.0f);
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x0E, 0x02}), DeviceRegister::WINDOW_OPEN, v));
}

TEST(ProtoHeating, DecodesTheAckOfAWriteToo) {
  // A set is answered with the header of the set (0x61) followed by the value.
  float v = 0.0f;
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00}), DeviceRegister::COMFORT_SETPOINT, v));
  EXPECT_FLOAT_EQ(v, 20.5f);
}

TEST(ProtoHeating, ReplyWithoutAValueForThisRegisterIsNoValue) {
  float v = 123.0f;
  // Another register's answer.
  EXPECT_FALSE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x03, 0xC3, 0x00}), DeviceRegister::ROOM_TEMPERATURE, v));
  // A set acknowledged without a value.
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x61, 0x01, 0x03}), DeviceRegister::COMFORT_SETPOINT, v));
  // Truncated 16-bit value.
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x15, 0x1B}), DeviceRegister::ROOM_TEMPERATURE, v));
  // Wrong prefix, high byte or direction.
  EXPECT_FALSE(
      decode_heating_reply(ack_with({0x0D, 0x60, 0x01, 0x15, 0x1B, 0x08}), DeviceRegister::ROOM_TEMPERATURE, v));
  EXPECT_FALSE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x02, 0x15, 0x1B, 0x08}), DeviceRegister::ROOM_TEMPERATURE, v));
  EXPECT_FALSE(
      decode_heating_reply(ack_with({0x0C, 0x62, 0x01, 0x15, 0x1B, 0x08}), DeviceRegister::ROOM_TEMPERATURE, v));
  // Not a CMD_WRITE_PRIVATE_ACK.
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x15, 0x1B, 0x08}, CMD_ERROR_RESP),
                                    DeviceRegister::ROOM_TEMPERATURE, v));
  // A register with no typed decode.
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x2C, 0x01, 0x02}), DeviceRegister::PAIRED_DEVICES, v));
  EXPECT_FLOAT_EQ(v, 123.0f) << "no value leaves the output untouched";
}

// --- Towel-dryer registers (values from two Atlantic Adelis units, issue #202) ---

TEST(ProtoHeatingTowelDryer, ModeAndTemporaryStateWritesAreByteExact) {
  EXPECT_EQ(encode(DeviceRegister::TOWEL_MODE, static_cast<float>(TowelDryerMode::MANUAL)),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x36, 0x03}));
  EXPECT_EQ(encode(DeviceRegister::TOWEL_MODE, static_cast<float>(TowelDryerMode::PROG)),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x36, 0x02}));
  EXPECT_EQ(encode(DeviceRegister::TOWEL_MODE, static_cast<float>(TowelDryerMode::OFF)),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x36, 0x00}));
  EXPECT_EQ(encode(DeviceRegister::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::BOOST)),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x3E, 0x01}));
  EXPECT_EQ(encode(DeviceRegister::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::DRYING)),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x3E, 0x02}));
  EXPECT_EQ(encode(DeviceRegister::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::NONE)),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x3E, 0x00}));
}

TEST(ProtoHeatingTowelDryer, WritesOutsideTheRegisterRangeOrNotIntegralAreRefused) {
  EXPECT_EQ(write_len(DeviceRegister::TOWEL_MODE, 4.0F), 0u);
  EXPECT_EQ(write_len(DeviceRegister::TOWEL_MODE, -1.0F), 0u);
  EXPECT_EQ(write_len(DeviceRegister::TOWEL_MODE, 2.5F), 0u);
  EXPECT_EQ(write_len(DeviceRegister::TOWEL_MODE, NAN), 0u);
  EXPECT_EQ(write_len(DeviceRegister::TEMPORARY_STATE, 3.0F), 0u);
  EXPECT_EQ(write_len(DeviceRegister::WORKING_RATE, 50.0F), 0u) << "read-only";
  EXPECT_EQ(write_len(DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE, 19.0F), 0u) << "read-only";
  EXPECT_EQ(write_len(DeviceRegister::ECO_SETBACK, 3.5F), 0u) << "read-only";
  EXPECT_EQ(write_len(DeviceRegister::REGULATOR_STATE, 1.0F), 0u) << "still read-only";
}

TEST(ProtoHeatingTowelDryer, DerogationIsWrittenInTenthsAndZeroCancelsIt) {
  EXPECT_EQ(encode(DeviceRegister::DEROGATED_SETPOINT, 16.5F),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x1D, 0xA5, 0x00}));
  EXPECT_EQ(encode(DeviceRegister::DEROGATED_SETPOINT, 0.0F),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x1D, 0x00, 0x00}))
      << "0 is the cancel encoding";
  EXPECT_EQ(write_len(DeviceRegister::DEROGATED_SETPOINT, 6.9F), 0u) << "below the range, and not 0";
  EXPECT_EQ(write_len(DeviceRegister::DEROGATED_SETPOINT, 28.1F), 0u);
  EXPECT_EQ(write_len(DeviceRegister::DEROGATED_SETPOINT, NAN), 0u);
  EXPECT_EQ(write_len(DeviceRegister::COMFORT_SETPOINT, 0.0F), 0u) << "only the override register takes 0";
  EXPECT_EQ(encode_read(DeviceRegister::DEROGATED_SETPOINT), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x1D}));
  EXPECT_STREQ(device_register_name(DeviceRegister::DEROGATED_SETPOINT), "set_derogation");
}

TEST(ProtoHeatingTowelDryer, DerogationRepliesDecodeToDegreesAndZero) {
  float value = NAN;
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x1D, 0xA5, 0x00}), DeviceRegister::DEROGATED_SETPOINT, value));
  EXPECT_FLOAT_EQ(value, 16.5F);
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x1D, 0x00, 0x00}), DeviceRegister::DEROGATED_SETPOINT, value));
  EXPECT_FLOAT_EQ(value, 0.0F);
}

TEST(ProtoHeatingTowelDryer, ReadsAddressTheirRegisters) {
  EXPECT_EQ(encode_read(DeviceRegister::TOWEL_MODE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x36}));
  EXPECT_EQ(encode_read(DeviceRegister::TEMPORARY_STATE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x3E}));
  EXPECT_EQ(encode_read(DeviceRegister::WORKING_RATE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x26}));
  EXPECT_EQ(encode_read(DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x37}));
  EXPECT_EQ(encode_read(DeviceRegister::ECO_SETBACK), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x04}));
  EXPECT_EQ(encode(DeviceRegister::WORKING_RATE, 0.0F), encode_read(DeviceRegister::WORKING_RATE))
      << "the action payload of a read-only register is its read";
  EXPECT_STREQ(device_register_name(DeviceRegister::TOWEL_MODE), "towel_mode");
  EXPECT_STREQ(device_register_name(DeviceRegister::TEMPORARY_STATE), "temporary_state");
  EXPECT_STREQ(device_register_name(DeviceRegister::WORKING_RATE), "working_rate");
  EXPECT_STREQ(device_register_name(DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE), "temperature_in_force");
  EXPECT_STREQ(device_register_name(DeviceRegister::ECO_SETBACK), "eco_setback");
}

TEST(ProtoHeatingTowelDryer, DecodesWhatTheAdelisReports) {
  float value = -1.0F;
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x36, 0x03}), DeviceRegister::TOWEL_MODE, value));
  EXPECT_FLOAT_EQ(value, 3.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x3E, 0x02}), DeviceRegister::TEMPORARY_STATE, value));
  EXPECT_FLOAT_EQ(value, 2.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x26, 0x64}), DeviceRegister::WORKING_RATE, value));
  EXPECT_FLOAT_EQ(value, 100.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x26, 0x0D}), DeviceRegister::WORKING_RATE, value));
  EXPECT_FLOAT_EQ(value, 13.0F);
}

TEST(ProtoHeatingTowelDryer, DecodesTheTemperatureInForceAndTheEcoSetbackInTenths) {
  // Replies of unit BD684B in PROG (issue #202, corpus atlantic_adelis_probe_temperature_in_force) and the
  // setback of unit BB0B50 (corpus atlantic_adelis_probe_register_01_sweep).
  float value = -1.0F;
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x37, 0xC8, 0x00}),
                                   DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE, value));
  EXPECT_FLOAT_EQ(value, 20.0F) << "comfort period";
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x37, 0xA5, 0x00}),
                                   DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE, value));
  EXPECT_FLOAT_EQ(value, 16.5F) << "eco period";
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x04, 0x23, 0x00}), DeviceRegister::ECO_SETBACK, value));
  EXPECT_FLOAT_EQ(value, 3.5F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x37, 0x01, 0x00}),
                                   DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE, value));
  EXPECT_FLOAT_EQ(value, 0.1F) << "in boost the register is not a temperature; the profile's gate keeps it unshown";
  EXPECT_FALSE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x37}), DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE, value))
      << "empty is no value";
}

TEST(ProtoHeatingRadiator, TheTemperatureInForceIsRegister012AInTenthsAndSharesTheReading) {
  EXPECT_EQ(encode_read(DeviceRegister::TEMPERATURE_IN_FORCE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x2A}));
  EXPECT_EQ(write_len(DeviceRegister::TEMPERATURE_IN_FORCE, 19.0F), 0u) << "read-only";
  EXPECT_STREQ(device_register_name(DeviceRegister::TEMPERATURE_IN_FORCE), "heater_temperature_in_force");
  float value = -1.0F;
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x2A, 0xA5, 0x00}), DeviceRegister::TEMPERATURE_IN_FORCE,
                                   value));
  EXPECT_FLOAT_EQ(value, 16.5F);
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x2A}), DeviceRegister::TEMPERATURE_IN_FORCE, value))
      << "a radiator without the register answers it empty";
  EXPECT_FALSE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x37, 0xA5, 0x00}), DeviceRegister::TEMPERATURE_IN_FORCE, value))
      << "the reply of another register is no value";
}

TEST(ProtoHeatingTowelDryer, AByteAboveTheRegistersRangeIsStillAnAnswerThatCarriesTheValue) {
  // The model check tells a towel dryer from a radiator by whether the heater has the mode register, so an answer
  // with a mode byte the codec refuses counts: the register is there.
  const IoFrame above_range = ack_with({0x0C, 0x60, 0x01, 0x36, 0x04});
  float value = -1.0F;
  EXPECT_FALSE(decode_heating_reply(above_range, DeviceRegister::TOWEL_MODE, value));
  EXPECT_TRUE(heating_reply_carries_value(above_range, DeviceRegister::TOWEL_MODE));
  EXPECT_TRUE(heating_reply_carries_value(ack_with({0x0C, 0x60, 0x01, 0x36, 0x02}), DeviceRegister::TOWEL_MODE));

  EXPECT_FALSE(heating_reply_carries_value(ack_with({0x0C, 0x60, 0x01, 0x36}), DeviceRegister::TOWEL_MODE))
      << "an empty answer: the heater has no such register";
  EXPECT_FALSE(heating_reply_carries_value(ack_with({0x0C, 0x60, 0x01, 0x00, 0x02}), DeviceRegister::TOWEL_MODE))
      << "the reply of another register";
  EXPECT_FALSE(
      heating_reply_carries_value(ack_with({0x0C, 0x60, 0x01, 0x42, 0x32}), DeviceRegister::NIGHT_LIGHT_BRIGHTNESS))
      << "a two-byte register needs both bytes";
  EXPECT_FALSE(heating_reply_carries_value(ack_with({0x0C, 0x60, 0x01, 0x2C, 0x01}), DeviceRegister::PAIRED_DEVICES))
      << "a register with no typed value carries none";
}

TEST(ProtoHeatingTowelDryer, OutOfRangeAndEmptyRepliesAreNoValue) {
  float value = -1.0F;
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x36, 0x04}), DeviceRegister::TOWEL_MODE, value));
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x3E, 0x03}), DeviceRegister::TEMPORARY_STATE, value));
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x26, 0x65}), DeviceRegister::WORKING_RATE, value));
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x36}), DeviceRegister::TOWEL_MODE, value))
      << "a register the device does not hold is answered without a value";
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x17, 0x03}), DeviceRegister::REGULATOR_STATE, value))
      << "the regulator range is unchanged";
  EXPECT_FLOAT_EQ(value, -1.0F);
}

TEST(ProtoHeatingTowelDryer, EachFunctionHasItsRegisterLowByte) {
  uint8_t family = 0xFF;
  uint8_t reg = 0;
  ASSERT_TRUE(device_register_bytes(DeviceRegister::TOWEL_MODE, family, reg));
  EXPECT_EQ(family, HEATING_REGISTER_HIGH_BYTE);
  EXPECT_EQ(reg, 0x36);
  ASSERT_TRUE(device_register_bytes(DeviceRegister::OPERATING_MODE, family, reg));
  EXPECT_EQ(family, HEATING_REGISTER_HIGH_BYTE);
  EXPECT_EQ(reg, 0x00);
  EXPECT_FALSE(device_register_bytes(static_cast<DeviceRegister>(0xEE), family, reg));
}

// --- Night light (Sauter Hekla, issue #222) ---

TEST(ProtoHeatingNightLight, WritesAreByteExact) {
  // On/off is the write the heater's own Cozytouch controller sends.
  EXPECT_EQ(encode(DeviceRegister::NIGHT_LIGHT, 1.0F), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x44, 0x01}));
  EXPECT_EQ(encode(DeviceRegister::NIGHT_LIGHT, 0.0F), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x44, 0x00}));
  // The brightness is written as a percentage and sent in the two-byte form it reads back in. The register runs
  // 0-10000; the four levels of a Hekla's panel (issue #222) as the heater writes them: 3.3 %, 33 %, 66 %, 100 %.
  EXPECT_EQ(encode(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, 3.3F),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x42, 0x64, 0x00}));
  EXPECT_EQ(encode(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, 33.0F),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x42, 0xE8, 0x03}));
  EXPECT_EQ(encode(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, 66.0F),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x42, 0xD0, 0x07}));
  EXPECT_EQ(encode(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, 100.0F),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x42, 0x10, 0x27}));
}

namespace {

/// The register value a percentage writes, read back out of the payload.
uint16_t night_light_register_of(float percent) {
  const std::vector<uint8_t> payload = encode(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, percent);
  EXPECT_EQ(payload.size(), 6u) << percent << " %";
  return payload.size() == 6 ? static_cast<uint16_t>(payload[4] | (payload[5] << 8)) : 0;
}

/// The percentage a register value shows as, through a heater's reply; NaN when the reply carries none.
float night_light_percent_of(uint16_t raw) {
  float value = NAN;
  decode_heating_reply(
      ack_with({0x0C, 0x60, 0x01, 0x42, static_cast<uint8_t>(raw & 0xFF), static_cast<uint8_t>(raw >> 8)}),
      DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, value);
  return value;
}

}  // namespace

TEST(ProtoHeatingNightLight, PercentagesFollowTheVendorAppsScale) {
  // The vendor app's slider: 1000 = 33 %, 2000 = 66 %, 10000 = 100 % (issue #222), linear in between.
  EXPECT_EQ(night_light_register_of(0.0F), 0);
  EXPECT_EQ(night_light_register_of(1.0F), 30);
  EXPECT_EQ(night_light_register_of(20.0F), 606);
  EXPECT_EQ(night_light_register_of(33.0F), 1000);
  EXPECT_EQ(night_light_register_of(50.0F), 1515);
  EXPECT_EQ(night_light_register_of(50.5F), 1530) << "a percentage between two whole ones";
  EXPECT_EQ(night_light_register_of(66.0F), 2000);
  EXPECT_EQ(night_light_register_of(70.0F), 2941);
  EXPECT_EQ(night_light_register_of(100.0F), HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX);

  EXPECT_FLOAT_EQ(night_light_percent_of(0), 0.0F);
  EXPECT_NEAR(night_light_percent_of(100), 3.3F, 1e-4F) << "the lowest panel level";
  EXPECT_FLOAT_EQ(night_light_percent_of(1000), 33.0F);
  EXPECT_FLOAT_EQ(night_light_percent_of(2000), 66.0F);
  EXPECT_NEAR(night_light_percent_of(3000), 70.25F, 1e-4F) << "the app's value between levels";
  EXPECT_FLOAT_EQ(night_light_percent_of(10000), 100.0F);
}

TEST(ProtoHeatingNightLight, EveryPercentageReadsBackAsItself) {
  for (unsigned percent = 0; percent <= HEATING_NIGHT_LIGHT_PERCENT_MAX; ++percent)
    EXPECT_NEAR(night_light_percent_of(night_light_register_of(static_cast<float>(percent))),
                static_cast<float>(percent), 0.2F)
        << percent;
}

TEST(ProtoHeatingNightLight, TheScaleRisesAndEndsAtFullBrightness) {
  for (size_t i = 1; i < std::size(HEATING_NIGHT_LIGHT_SCALE); ++i) {
    EXPECT_GT(HEATING_NIGHT_LIGHT_SCALE[i].natural, HEATING_NIGHT_LIGHT_SCALE[i - 1].natural);
    EXPECT_GT(HEATING_NIGHT_LIGHT_SCALE[i].raw, HEATING_NIGHT_LIGHT_SCALE[i - 1].raw);
  }
  EXPECT_EQ(HEATING_NIGHT_LIGHT_SCALE[0].natural, 0);
  EXPECT_EQ(HEATING_NIGHT_LIGHT_SCALE[0].raw, 0);
  const ValueScaleKnot &last = HEATING_NIGHT_LIGHT_SCALE[std::size(HEATING_NIGHT_LIGHT_SCALE) - 1];
  EXPECT_EQ(last.natural, HEATING_NIGHT_LIGHT_PERCENT_MAX);
  EXPECT_EQ(last.raw, HEATING_NIGHT_LIGHT_BRIGHTNESS_MAX);
}

TEST(ProtoHeatingNightLight, WritesOutsideTheRangeAreRefused) {
  EXPECT_EQ(write_len(DeviceRegister::NIGHT_LIGHT, 2.0F), 0u);
  EXPECT_EQ(write_len(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, 101.0F), 0u);
  EXPECT_EQ(write_len(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, -1.0F), 0u);
  EXPECT_EQ(write_len(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, NAN), 0u);
  EXPECT_EQ(write_len(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, 0.0F), 6u);
  EXPECT_EQ(write_len(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, 100.0F), 6u);
}

TEST(ProtoHeatingNightLight, ReadsAddressTheirRegisters) {
  EXPECT_EQ(encode_read(DeviceRegister::NIGHT_LIGHT), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x44}));
  EXPECT_EQ(encode_read(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x42}));
  EXPECT_STREQ(device_register_name(DeviceRegister::NIGHT_LIGHT), "night_light");
  EXPECT_STREQ(device_register_name(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS), "night_light_brightness");
}

TEST(ProtoHeatingNightLight, DecodesWhatTheHeklaReports) {
  // The register_01 sweeps of issue #222: 0x0144 reads 01 with the light on and 00 with it off; 0x0142
  // reads 64 00 either way (the lowest of its four levels, 100 on the 0-10000 scale).
  float value = -1.0F;
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x44, 0x01}), DeviceRegister::NIGHT_LIGHT, value));
  EXPECT_FLOAT_EQ(value, 1.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x44, 0x00}), DeviceRegister::NIGHT_LIGHT, value));
  EXPECT_FLOAT_EQ(value, 0.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x42, 0x64, 0x00}),
                                   DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, value));
  EXPECT_NEAR(value, 3.3F, 1e-4F) << "register value 100, as a percentage";
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x42, 0xE8, 0x03}),
                                   DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, value));
  EXPECT_FLOAT_EQ(value, 33.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x42, 0xD0, 0x07}),
                                   DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, value));
  EXPECT_FLOAT_EQ(value, 66.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x42, 0x10, 0x27}),
                                   DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, value));
  EXPECT_FLOAT_EQ(value, 100.0F);
  // The heater's answer to the Cozytouch's write carries the new value too.
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x61, 0x01, 0x44, 0x01}), DeviceRegister::NIGHT_LIGHT, value));
  EXPECT_FLOAT_EQ(value, 1.0F);
}

TEST(ProtoHeatingNightLight, OutOfRangeShortAndEmptyRepliesAreNoValue) {
  float value = -1.0F;
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x42, 0x11, 0x27}),
                                    DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, value))
      << "10001 is above the register's range";
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x42, 0xFF, 0xFF}),
                                    DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, value));
  EXPECT_FALSE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x42, 0x32}), DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, value))
      << "a two-byte register answered with one byte";
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x44}), DeviceRegister::NIGHT_LIGHT, value))
      << "a heater without the light answers without a value";
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x44, 0x02}), DeviceRegister::NIGHT_LIGHT, value));
  EXPECT_FLOAT_EQ(value, -1.0F);
}

// --- Central-heating mode (a mixed towel dryer, issue #243) ---

TEST(ProtoHeatingCentralHeatingMode, ReadRequestAndRegister) {
  EXPECT_EQ(encode_read(DeviceRegister::CENTRAL_HEATING_MODE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x53}));
  EXPECT_FALSE(device_register_is_writable(DeviceRegister::CENTRAL_HEATING_MODE)) << "never written";
}

TEST(ProtoHeatingCentralHeatingMode, DecodesCentralHeatingAndElectric) {
  float value = -1.0F;
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x53, 0x01}), DeviceRegister::CENTRAL_HEATING_MODE, value));
  EXPECT_FLOAT_EQ(value, 1.0F);
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x53, 0x00}), DeviceRegister::CENTRAL_HEATING_MODE, value));
  EXPECT_FLOAT_EQ(value, 0.0F);
}

TEST(ProtoHeatingCentralHeatingMode, EmptyAndUnknownRepliesAreNoValue) {
  float value = -1.0F;
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x53}), DeviceRegister::CENTRAL_HEATING_MODE, value))
      << "an electric unit answers without a value";
  EXPECT_FALSE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x53, 0x02}), DeviceRegister::CENTRAL_HEATING_MODE, value));
  EXPECT_FLOAT_EQ(value, -1.0F);
}

// --- Occupancy detection and detected presence (Sauter Hekla, issue #222) ---

TEST(ProtoHeatingOccupancy, DetectionIsRegister0x0101ReadAndWritten) {
  EXPECT_EQ(encode_read(DeviceRegister::OCCUPANCY_DETECTION), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x01}));
  EXPECT_EQ(encode(DeviceRegister::OCCUPANCY_DETECTION, 1.0f), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x01, 0x01}));
  EXPECT_EQ(encode(DeviceRegister::OCCUPANCY_DETECTION, 0.0f), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x01, 0x00}));
  EXPECT_EQ(write_len(DeviceRegister::OCCUPANCY_DETECTION, 2.0f), 0u) << "only 0 and 1 are values";
  EXPECT_STREQ(device_register_name(DeviceRegister::OCCUPANCY_DETECTION), "occupancy_detection");
}

TEST(ProtoHeatingOccupancy, DecodesTheSweepValuesOfTheDetectionAndThePresence) {
  float value = -1.0F;
  // The Hekla sweep: 0x0101 reads 00 with detection off and 01 with it on; 0x0110 reads 00 with nobody in
  // the room and 01 while someone walks around the heater.
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x01, 0x00}), DeviceRegister::OCCUPANCY_DETECTION, value));
  EXPECT_FLOAT_EQ(value, 0.0F);
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x01, 0x01}), DeviceRegister::OCCUPANCY_DETECTION, value));
  EXPECT_FLOAT_EQ(value, 1.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x10, 0x00}), DeviceRegister::PRESENCE, value));
  EXPECT_FLOAT_EQ(value, 0.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x10, 0x01}), DeviceRegister::PRESENCE, value));
  EXPECT_FLOAT_EQ(value, 1.0F);
}

TEST(ProtoHeatingOccupancy, EmptyAndUnknownRepliesAreNoValue) {
  float value = -1.0F;
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x10}), DeviceRegister::PRESENCE, value));
  EXPECT_FALSE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x01, 0x02}), DeviceRegister::OCCUPANCY_DETECTION, value));
  EXPECT_FLOAT_EQ(value, -1.0F);
}

// --- Energy total (register 0x0116: whole kWh, most significant byte first) ---

TEST(ProtoHeatingEnergy, TheTotalIsRegister0x0116ReadOnly) {
  EXPECT_EQ(encode_read(DeviceRegister::ENERGY_TOTAL), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x16}));
  EXPECT_TRUE(device_register_is_readable(DeviceRegister::ENERGY_TOTAL));
  EXPECT_FALSE(device_register_is_writable(DeviceRegister::ENERGY_TOTAL));
  EXPECT_EQ(write_len(DeviceRegister::ENERGY_TOTAL, 1.0f), 0u) << "a meter is never written";
  EXPECT_STREQ(device_register_name(DeviceRegister::ENERGY_TOTAL), "energy_total");
}

TEST(ProtoHeatingEnergy, DecodesTheTotalMostSignificantByteFirst) {
  float value = -1.0F;
  // The displays of two Sauter Tahual radiators, and a Thermor EMOTION 3 H's answer (not compared with its display).
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x16, 0x0D, 0x5E}), DeviceRegister::ENERGY_TOTAL, value));
  EXPECT_FLOAT_EQ(value, 3422.0F);
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x16, 0x01, 0x08}), DeviceRegister::ENERGY_TOTAL, value));
  EXPECT_FLOAT_EQ(value, 264.0F);
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x16, 0x0C, 0xBE}), DeviceRegister::ENERGY_TOTAL, value));
  EXPECT_FLOAT_EQ(value, 3262.0F);
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x16, 0x00, 0x00}), DeviceRegister::ENERGY_TOTAL, value));
  EXPECT_FLOAT_EQ(value, 0.0F) << "a new heater";
}

TEST(ProtoHeatingEnergy, ATotalAboveWhatASignedSixteenBitIntegerHoldsDecodesExactly) {
  float value = -1.0F;
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x16, 0xFF, 0xFF}), DeviceRegister::ENERGY_TOTAL, value));
  EXPECT_FLOAT_EQ(value, 65535.0F);
  IoDevice readings;
  store_reading(readings, DeviceReading::ENERGY_TOTAL, value);
  EXPECT_EQ(register_value(readings, DeviceRegister::ENERGY_TOTAL), 65535.0F)
      << "the store holds a number, not a signed 16-bit field";
}

TEST(ProtoHeatingEnergy, ATruncatedOrEmptyReplyIsNoValue) {
  float value = -1.0F;
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x16}), DeviceRegister::ENERGY_TOTAL, value))
      << "a heater without the register answers without a value";
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x16, 0x0D}), DeviceRegister::ENERGY_TOTAL, value))
      << "one byte is not a total";
  EXPECT_FALSE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x15, 0x0D, 0x5E}), DeviceRegister::ENERGY_TOTAL, value))
      << "the reply of another register";
  EXPECT_FLOAT_EQ(value, -1.0F);
}

// --- Run timers of a towel dryer (registers 0x013A-0x013D, minutes) ---

TEST(ProtoHeatingRunTimers, EachTimerIsItsRegisterReadOnly) {
  const std::pair<DeviceRegister, uint8_t> timers[] = {{DeviceRegister::BOOST_REMAINING, 0x3A},
                                                       {DeviceRegister::BOOST_DURATION, 0x3B},
                                                       {DeviceRegister::DRYING_REMAINING, 0x3C},
                                                       {DeviceRegister::DRYING_DURATION, 0x3D}};
  for (const auto &[fn, low] : timers) {
    EXPECT_EQ(encode_read(fn), (std::vector<uint8_t>{0x0C, 0x60, 0x01, low})) << device_register_name(fn);
    EXPECT_TRUE(device_register_is_readable(fn)) << device_register_name(fn);
    EXPECT_FALSE(device_register_is_writable(fn)) << device_register_name(fn);
    EXPECT_EQ(write_len(fn, 30.0F), 0u) << "read-only until a write is tested: " << device_register_name(fn);
  }
}

TEST(ProtoHeatingRunTimers, TheTimeLeftIsTheOneByteInMinutes) {
  float value = -1.0F;
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x3A, 0x3C}), DeviceRegister::BOOST_REMAINING, value));
  EXPECT_FLOAT_EQ(value, 60.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x3C, 0x1C}), DeviceRegister::DRYING_REMAINING, value));
  EXPECT_FLOAT_EQ(value, 28.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x3C, 0x00}), DeviceRegister::DRYING_REMAINING, value));
  EXPECT_FLOAT_EQ(value, 0.0F) << "no run";
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x3A, 0xFF}), DeviceRegister::BOOST_REMAINING, value));
  EXPECT_FLOAT_EQ(value, 255.0F) << "a count, not an enumeration: no upper bound but the byte";
}

TEST(ProtoHeatingRunTimers, TheDurationIsTheFirstOfTwoBytesAndTheSecondIsThePanelsMaximum) {
  float value = -1.0F;
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x3B, 0x3C, 0x3C}), DeviceRegister::BOOST_DURATION, value));
  EXPECT_FLOAT_EQ(value, 60.0F);
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x3B, 0x23, 0x3C}), DeviceRegister::BOOST_DURATION, value));
  EXPECT_FLOAT_EQ(value, 35.0F);
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x3D, 0x1E, 0x78}), DeviceRegister::DRYING_DURATION, value));
  EXPECT_FLOAT_EQ(value, 30.0F);
}

TEST(ProtoHeatingRunTimers, AnEmptyTruncatedOrForeignReplyIsNoValue) {
  float value = -1.0F;
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x3A}), DeviceRegister::BOOST_REMAINING, value))
      << "a dryer without the register answers without a value";
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x3B, 0x3C}), DeviceRegister::BOOST_DURATION, value))
      << "a duration comes with the panel's maximum";
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x3C, 0x1C}), DeviceRegister::BOOST_REMAINING, value))
      << "the reply of another register";
  EXPECT_FLOAT_EQ(value, -1.0F);
}
