/// @file proto_heating_test.cpp
/// @brief Byte-exact tests for the heater register codec (CMD_WRITE_PRIVATE 0x20 / ack 0x21).

#include "proto_heating.h"

#include "proto_constants.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace esphome::home_io_control;

namespace {

std::vector<uint8_t> encode(HeatingFunction fn, float value) {
  uint8_t out[HEATING_PAYLOAD_MAX_SIZE] = {0};
  const size_t n = encode_heating_payload(fn, value, out);
  return std::vector<uint8_t>(out, out + n);
}

std::vector<uint8_t> encode_read(HeatingFunction fn) {
  uint8_t out[HEATING_PAYLOAD_MAX_SIZE] = {0};
  const size_t n = encode_heating_read(fn, out);
  return std::vector<uint8_t>(out, out + n);
}

size_t write_len(HeatingFunction fn, float value) {
  uint8_t out[HEATING_PAYLOAD_MAX_SIZE] = {0};
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
  EXPECT_EQ(encode(HeatingFunction::POWER_ON, 0.0f), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x2C}));
}

TEST(ProtoHeating, SetTemperaturePayload) {
  // Register 0x0103, little-endian tenths: 20.5 C -> 205 -> 0x00CD -> CD 00.
  EXPECT_EQ(encode(HeatingFunction::SET_TEMPERATURE, 20.5f),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00}));
}

TEST(ProtoHeating, SetModePayload) {
  EXPECT_EQ(encode(HeatingFunction::SET_MODE, static_cast<float>(HeatingMode::MANUAL)),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x00, 0x01}));
}

TEST(ProtoHeating, SetPresencePayload) {
  EXPECT_EQ(encode(HeatingFunction::SET_PRESENCE, 1.0f), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x10, 0x01}));
  EXPECT_EQ(encode(HeatingFunction::SET_PRESENCE, 0.0f), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x10, 0x00}));
}

TEST(ProtoHeating, OpenWindowDetectionWritesRegister0106) {
  EXPECT_EQ(encode(HeatingFunction::SET_OPEN_WINDOW_DETECTION, 1.0f),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x06, 0x01}))
      << "set_open_window_detection on";
  EXPECT_EQ(encode(HeatingFunction::SET_OPEN_WINDOW_DETECTION, 0.0f),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x06, 0x00}));
}

TEST(ProtoHeating, MidnightSyncPayload) {
  EXPECT_EQ(encode(HeatingFunction::MIDNIGHT_SYNC, 0.0f), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x30}));
}

// --- Reads: one table drives get as well as set ---

TEST(ProtoHeating, ReadPayloadsAddressTheSameRegisterAsTheWrite) {
  EXPECT_EQ(encode_read(HeatingFunction::SET_TEMPERATURE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x03}));
  EXPECT_EQ(encode_read(HeatingFunction::SET_MODE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x00}));
  EXPECT_EQ(encode_read(HeatingFunction::SET_OPEN_WINDOW_DETECTION), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x06}));
  EXPECT_EQ(encode_read(HeatingFunction::ROOM_TEMPERATURE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x15}));
  EXPECT_EQ(encode_read(HeatingFunction::REGULATOR_STATE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x17}));
  EXPECT_EQ(encode_read(HeatingFunction::MAX_SETPOINT), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x02}));
  EXPECT_EQ(encode_read(HeatingFunction::WINDOW_OPEN), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x0E}));
}

TEST(ProtoHeating, WriteOnlyRegisterHasNoRead) {
  EXPECT_TRUE(encode_read(HeatingFunction::SET_PRESENCE).empty());
  EXPECT_FALSE(heating_function_is_readable(HeatingFunction::SET_PRESENCE));
  EXPECT_TRUE(heating_function_is_writable(HeatingFunction::SET_PRESENCE));
}

TEST(ProtoHeating, ReadOnlyRegistersAreNeverWritten) {
  // The window sensor is not a setting: 0x010E must not be writable whatever the value.
  EXPECT_EQ(write_len(HeatingFunction::WINDOW_OPEN, 1.0f), 0u);
  EXPECT_EQ(write_len(HeatingFunction::WINDOW_OPEN, 0.0f), 0u);
  EXPECT_EQ(write_len(HeatingFunction::ROOM_TEMPERATURE, 20.0f), 0u);
  EXPECT_EQ(write_len(HeatingFunction::REGULATOR_STATE, 1.0f), 0u);
  EXPECT_EQ(write_len(HeatingFunction::MAX_SETPOINT, 28.0f), 0u);
  EXPECT_EQ(write_len(HeatingFunction::POWER_ON, 0.0f), 0u);
  EXPECT_EQ(write_len(HeatingFunction::MIDNIGHT_SYNC, 0.0f), 0u);
  EXPECT_FALSE(heating_function_is_writable(HeatingFunction::WINDOW_OPEN));
  EXPECT_TRUE(heating_function_is_readable(HeatingFunction::WINDOW_OPEN));
}

TEST(ProtoHeating, ActionPayloadOfAReadOnlyRegisterIsItsRead) {
  // The heating_control action runs a read for a read-only register, so `window_open` reads 0x010E.
  EXPECT_EQ(encode(HeatingFunction::WINDOW_OPEN, 1.0f), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x0E}));
}

// --- Modes ---

TEST(ProtoHeating, AllWritableModes) {
  EXPECT_EQ(encode(HeatingFunction::SET_MODE, static_cast<float>(HeatingMode::AUTO)).back(), 0x00);
  EXPECT_EQ(encode(HeatingFunction::SET_MODE, static_cast<float>(HeatingMode::MANUAL)).back(), 0x01);
  EXPECT_EQ(encode(HeatingFunction::SET_MODE, static_cast<float>(HeatingMode::PROG)).back(), 0x02);
  EXPECT_EQ(encode(HeatingFunction::SET_MODE, static_cast<float>(HeatingMode::OFF)).back(), 0x04);
}

TEST(ProtoHeating, RejectsUnwritableModes) {
  // 0x03 (external) is readable but this component never writes it.
  EXPECT_EQ(write_len(HeatingFunction::SET_MODE, 3.0f), 0u);
  EXPECT_EQ(write_len(HeatingFunction::SET_MODE, 1.5f), 0u);
  EXPECT_EQ(write_len(HeatingFunction::SET_MODE, 5.0f), 0u);
}

TEST(ProtoHeating, RejectsNonBinaryPresenceAndDetection) {
  EXPECT_EQ(write_len(HeatingFunction::SET_PRESENCE, 2.0f), 0u);
  EXPECT_EQ(write_len(HeatingFunction::SET_OPEN_WINDOW_DETECTION, 0.5f), 0u);
}

// --- Temperature matrix: little-endian tenths of a degree ---

TEST(ProtoHeating, TemperatureMatrix) {
  const std::vector<uint8_t> prefix = {0x0C, 0x61, 0x01, 0x03};
  auto expect_temp = [&](float degrees, uint8_t lo, uint8_t hi) {
    std::vector<uint8_t> want = prefix;
    want.push_back(lo);
    want.push_back(hi);
    EXPECT_EQ(encode(HeatingFunction::SET_TEMPERATURE, degrees), want) << degrees << " C";
  };
  expect_temp(7.0f, 0x46, 0x00);    // 70   = 0x0046
  expect_temp(20.5f, 0xCD, 0x00);   // 205  = 0x00CD
  expect_temp(20.55f, 0xCE, 0x00);  // 205.5f -> round-half-away-from-zero -> 206 = 0x00CE
  expect_temp(25.5f, 0xFF, 0x00);   // 255  = 0x00FF
  expect_temp(26.0f, 0x04, 0x01);   // 260  = 0x0104
  expect_temp(28.0f, 0x18, 0x01);   // 280  = 0x0118
}

TEST(ProtoHeating, TemperatureRejectsOutOfRangeAndNaN) {
  EXPECT_EQ(write_len(HeatingFunction::SET_TEMPERATURE, 6.9f), 0u);
  EXPECT_EQ(write_len(HeatingFunction::SET_TEMPERATURE, 28.1f), 0u);
  EXPECT_EQ(write_len(HeatingFunction::SET_TEMPERATURE, 53.5f), 0u);
  EXPECT_EQ(write_len(HeatingFunction::SET_TEMPERATURE, std::nanf("")), 0u);
  EXPECT_EQ(write_len(HeatingFunction::SET_TEMPERATURE, INFINITY), 0u);
}

TEST(ProtoHeating, PayloadLengths) {
  EXPECT_EQ(encode(HeatingFunction::POWER_ON, 0.0f).size(), 4u);
  EXPECT_EQ(encode(HeatingFunction::MIDNIGHT_SYNC, 0.0f).size(), 4u);
  EXPECT_EQ(encode(HeatingFunction::SET_MODE, 0.0f).size(), 5u);
  EXPECT_EQ(encode(HeatingFunction::SET_PRESENCE, 0.0f).size(), 5u);
  EXPECT_EQ(encode(HeatingFunction::SET_OPEN_WINDOW_DETECTION, 0.0f).size(), 5u);
  EXPECT_EQ(encode(HeatingFunction::SET_TEMPERATURE, 20.0f).size(), 6u);
}

TEST(ProtoHeating, FunctionNames) {
  EXPECT_STREQ(heating_function_name(HeatingFunction::POWER_ON), "power_on");
  EXPECT_STREQ(heating_function_name(HeatingFunction::SET_TEMPERATURE), "set_temperature");
  EXPECT_STREQ(heating_function_name(HeatingFunction::SET_MODE), "set_mode");
  EXPECT_STREQ(heating_function_name(HeatingFunction::SET_PRESENCE), "set_presence");
  EXPECT_STREQ(heating_function_name(HeatingFunction::SET_OPEN_WINDOW_DETECTION), "set_open_window_detection");
  EXPECT_STREQ(heating_function_name(HeatingFunction::MIDNIGHT_SYNC), "midnight_sync");
  EXPECT_STREQ(heating_function_name(HeatingFunction::ROOM_TEMPERATURE), "room_temperature");
  EXPECT_STREQ(heating_function_name(HeatingFunction::REGULATOR_STATE), "regulator_state");
  EXPECT_STREQ(heating_function_name(HeatingFunction::MAX_SETPOINT), "max_setpoint");
  EXPECT_STREQ(heating_function_name(HeatingFunction::WINDOW_OPEN), "window_open");
}

TEST(ProtoHeating, PowerOnAndMidnightIgnoreValue) {
  EXPECT_EQ(encode(HeatingFunction::POWER_ON, 999.0f), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x2C}));
  EXPECT_EQ(encode(HeatingFunction::MIDNIGHT_SYNC, std::nanf("")), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x30}));
}

TEST(ProtoHeating, RejectsOutOfRangeFunctionEnum) {
  // An enum value not in the table must be refused and must never touch the output buffer.
  uint8_t out[HEATING_PAYLOAD_MAX_SIZE];
  for (auto &b : out)
    b = 0xEE;
  const auto bogus = static_cast<HeatingFunction>(0x7F);
  EXPECT_EQ(encode_heating_payload(bogus, 20.0f, out), 0u);
  EXPECT_EQ(encode_heating_read(bogus, out), 0u);
  EXPECT_EQ(encode_heating_write(bogus, 20.0f, out), 0u);
  for (uint8_t b : out)
    EXPECT_EQ(b, 0xEE) << "a rejected function must not write the output buffer";
  EXPECT_STREQ(heating_function_name(bogus), "unknown");
  EXPECT_FALSE(heating_function_is_readable(bogus));
  EXPECT_FALSE(heating_function_is_writable(bogus));
}

// --- Decoding the device's reply ---

TEST(ProtoHeating, DecodesRoomTemperatureInHundredths) {
  float v = 0.0f;
  // 0x081B = 2075 -> 20.75 C
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x15, 0x1B, 0x08}), HeatingFunction::ROOM_TEMPERATURE, v));
  EXPECT_FLOAT_EQ(v, 20.75f);
  // 0x0659 = 1625 -> 16.25 C
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x15, 0x59, 0x06}), HeatingFunction::ROOM_TEMPERATURE, v));
  EXPECT_FLOAT_EQ(v, 16.25f);
}

TEST(ProtoHeating, RoomTemperatureSensorFaultDecodesAsNaN) {
  float v = 0.0f;
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x15, 0xFF, 0xFF}), HeatingFunction::ROOM_TEMPERATURE, v))
      << "the heater did answer";
  EXPECT_TRUE(std::isnan(v));
}

TEST(ProtoHeating, DecodesSetpointAndMaximumInTenths) {
  float v = 0.0f;
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x03, 0xC3, 0x00}), HeatingFunction::SET_TEMPERATURE, v));
  EXPECT_FLOAT_EQ(v, 19.5f);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x02, 0x18, 0x01}), HeatingFunction::MAX_SETPOINT, v));
  EXPECT_FLOAT_EQ(v, 28.0f);
}

TEST(ProtoHeating, DecodesModeBytes) {
  float v = -1.0f;
  for (uint8_t mode : {0, 1, 2, 3, 4}) {
    ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x00, mode}), HeatingFunction::SET_MODE, v));
    EXPECT_FLOAT_EQ(v, static_cast<float>(mode));
  }
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x00, 0x05}), HeatingFunction::SET_MODE, v))
      << "an undefined mode is not a value";
}

TEST(ProtoHeating, DecodesRegulatorStates) {
  float v = -1.0f;
  for (uint8_t state : {0, 1, 2}) {
    ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x17, state}), HeatingFunction::REGULATOR_STATE, v));
    EXPECT_FLOAT_EQ(v, static_cast<float>(state));
  }
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x17, 0x03}), HeatingFunction::REGULATOR_STATE, v));
}

TEST(ProtoHeating, DecodesWindowSensor) {
  float v = -1.0f;
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x0E, 0x00}), HeatingFunction::WINDOW_OPEN, v));
  EXPECT_FLOAT_EQ(v, 0.0f);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x0E, 0x01}), HeatingFunction::WINDOW_OPEN, v));
  EXPECT_FLOAT_EQ(v, 1.0f);
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x0E, 0x02}), HeatingFunction::WINDOW_OPEN, v));
}

TEST(ProtoHeating, DecodesTheAckOfAWriteToo) {
  // A set is answered with the header of the set (0x61) followed by the value.
  float v = 0.0f;
  ASSERT_TRUE(
      decode_heating_reply(ack_with({0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00}), HeatingFunction::SET_TEMPERATURE, v));
  EXPECT_FLOAT_EQ(v, 20.5f);
}

TEST(ProtoHeating, ReplyWithoutAValueForThisRegisterIsNoValue) {
  float v = 123.0f;
  // Another register's answer.
  EXPECT_FALSE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x03, 0xC3, 0x00}), HeatingFunction::ROOM_TEMPERATURE, v));
  // A set acknowledged without a value.
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x61, 0x01, 0x03}), HeatingFunction::SET_TEMPERATURE, v));
  // Truncated 16-bit value.
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x15, 0x1B}), HeatingFunction::ROOM_TEMPERATURE, v));
  // Wrong prefix, high byte or direction.
  EXPECT_FALSE(
      decode_heating_reply(ack_with({0x0D, 0x60, 0x01, 0x15, 0x1B, 0x08}), HeatingFunction::ROOM_TEMPERATURE, v));
  EXPECT_FALSE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x02, 0x15, 0x1B, 0x08}), HeatingFunction::ROOM_TEMPERATURE, v));
  EXPECT_FALSE(
      decode_heating_reply(ack_with({0x0C, 0x62, 0x01, 0x15, 0x1B, 0x08}), HeatingFunction::ROOM_TEMPERATURE, v));
  // Not a CMD_WRITE_PRIVATE_ACK.
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x15, 0x1B, 0x08}, CMD_ERROR_RESP),
                                    HeatingFunction::ROOM_TEMPERATURE, v));
  // A register with no typed decode.
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x2C, 0x01, 0x02}), HeatingFunction::POWER_ON, v));
  EXPECT_FLOAT_EQ(v, 123.0f) << "no value leaves the output untouched";
}

// --- Readings ---

TEST(ProtoHeating, ReadingsStartUnreadAndStoreOnlyTheirRegisters) {
  HeatingReadings r;
  EXPECT_TRUE(std::isnan(r.room_temperature_c));
  EXPECT_TRUE(std::isnan(r.setpoint_c));
  EXPECT_TRUE(std::isnan(r.max_setpoint_c));
  EXPECT_EQ(r.mode, -1);
  EXPECT_EQ(r.regulator, -1);
  EXPECT_EQ(r.window_open, -1);

  EXPECT_TRUE(r.store(HeatingFunction::ROOM_TEMPERATURE, 20.75f));
  EXPECT_TRUE(r.store(HeatingFunction::SET_TEMPERATURE, 19.5f));
  EXPECT_TRUE(r.store(HeatingFunction::MAX_SETPOINT, 28.0f));
  EXPECT_TRUE(r.store(HeatingFunction::SET_MODE, 2.0f));
  EXPECT_TRUE(r.store(HeatingFunction::REGULATOR_STATE, 1.0f));
  EXPECT_TRUE(r.store(HeatingFunction::WINDOW_OPEN, 1.0f));
  EXPECT_FLOAT_EQ(r.room_temperature_c, 20.75f);
  EXPECT_FLOAT_EQ(r.setpoint_c, 19.5f);
  EXPECT_FLOAT_EQ(r.max_setpoint_c, 28.0f);
  EXPECT_EQ(r.mode, 2);
  EXPECT_EQ(r.regulator, 1);
  EXPECT_EQ(r.window_open, 1);

  EXPECT_FALSE(r.store(HeatingFunction::SET_PRESENCE, 1.0f));
  EXPECT_FALSE(r.store(HeatingFunction::POWER_ON, 1.0f));
}

// === Register-read probe payload (diagnostic register_00 / register_01) ===

TEST(HeatingRegisterRead, BuildsTheReadRequestForBothRegisterFamilies) {
  uint8_t out[HEATING_PAYLOAD_MAX_SIZE] = {0};
  ASSERT_EQ(encode_heating_register_read(0x00, 0x2A, out), 4U);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 4), (std::vector<uint8_t>{0x0C, 0x60, 0x00, 0x2A}));
  ASSERT_EQ(encode_heating_register_read(0x01, 0x15, out), 4U);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 4), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x15}));
}

TEST(HeatingRegisterRead, NoRegisterNumberEverProducesTheWriteDirection) {
  // The probe's read-only guarantee: the direction byte is the GET constant for every one of the
  // 65536 register numbers, and the GET and SET constants really differ.
  static_assert(HEATING_DIRECTION_GET != HEATING_DIRECTION_SET);
  for (unsigned reg_hi = 0; reg_hi <= 0xFF; reg_hi++) {
    for (unsigned reg_lo = 0; reg_lo <= 0xFF; reg_lo++) {
      uint8_t out[HEATING_PAYLOAD_MAX_SIZE] = {0};
      ASSERT_EQ(encode_heating_register_read(static_cast<uint8_t>(reg_hi), static_cast<uint8_t>(reg_lo), out), 4U);
      ASSERT_EQ(out[0], HEATING_PAYLOAD_PREFIX);
      ASSERT_EQ(out[1], HEATING_DIRECTION_GET);
      ASSERT_EQ(out[2], reg_hi);
      ASSERT_EQ(out[3], reg_lo);
    }
  }
}

// --- Towel-dryer registers (values from two Atlantic Adelis units, issue #202) ---

TEST(ProtoHeatingTowelDryer, ModeAndTemporaryStateWritesAreByteExact) {
  EXPECT_EQ(encode(HeatingFunction::TOWEL_MODE, static_cast<float>(TowelDryerMode::MANUAL)),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x36, 0x03}));
  EXPECT_EQ(encode(HeatingFunction::TOWEL_MODE, static_cast<float>(TowelDryerMode::PROG)),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x36, 0x02}));
  EXPECT_EQ(encode(HeatingFunction::TOWEL_MODE, static_cast<float>(TowelDryerMode::OFF)),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x36, 0x00}));
  EXPECT_EQ(encode(HeatingFunction::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::BOOST)),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x3E, 0x01}));
  EXPECT_EQ(encode(HeatingFunction::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::DRYING)),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x3E, 0x02}));
  EXPECT_EQ(encode(HeatingFunction::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::NONE)),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x3E, 0x00}));
}

TEST(ProtoHeatingTowelDryer, WritesOutsideTheRegisterRangeOrNotIntegralAreRefused) {
  EXPECT_EQ(write_len(HeatingFunction::TOWEL_MODE, 4.0F), 0u);
  EXPECT_EQ(write_len(HeatingFunction::TOWEL_MODE, -1.0F), 0u);
  EXPECT_EQ(write_len(HeatingFunction::TOWEL_MODE, 2.5F), 0u);
  EXPECT_EQ(write_len(HeatingFunction::TOWEL_MODE, NAN), 0u);
  EXPECT_EQ(write_len(HeatingFunction::TEMPORARY_STATE, 3.0F), 0u);
  EXPECT_EQ(write_len(HeatingFunction::WORKING_RATE, 50.0F), 0u) << "read-only";
  EXPECT_EQ(write_len(HeatingFunction::TOWEL_COMFORT_SETPOINT, 19.0F), 0u) << "read-only";
  EXPECT_EQ(write_len(HeatingFunction::REGULATOR_STATE, 1.0F), 0u) << "still read-only";
}

TEST(ProtoHeatingTowelDryer, ReadsAddressTheirRegisters) {
  EXPECT_EQ(encode_read(HeatingFunction::TOWEL_MODE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x36}));
  EXPECT_EQ(encode_read(HeatingFunction::TEMPORARY_STATE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x3E}));
  EXPECT_EQ(encode_read(HeatingFunction::WORKING_RATE), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x26}));
  EXPECT_EQ(encode_read(HeatingFunction::TOWEL_COMFORT_SETPOINT), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x38}));
  EXPECT_EQ(encode(HeatingFunction::WORKING_RATE, 0.0F), encode_read(HeatingFunction::WORKING_RATE))
      << "the action payload of a read-only register is its read";
  EXPECT_STREQ(heating_function_name(HeatingFunction::TOWEL_MODE), "towel_mode");
  EXPECT_STREQ(heating_function_name(HeatingFunction::TEMPORARY_STATE), "temporary_state");
  EXPECT_STREQ(heating_function_name(HeatingFunction::WORKING_RATE), "working_rate");
  EXPECT_STREQ(heating_function_name(HeatingFunction::TOWEL_COMFORT_SETPOINT), "towel_comfort_setpoint");
}

TEST(ProtoHeatingTowelDryer, DecodesWhatTheAdelisReports) {
  float value = -1.0F;
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x36, 0x03}), HeatingFunction::TOWEL_MODE, value));
  EXPECT_FLOAT_EQ(value, 3.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x3E, 0x02}), HeatingFunction::TEMPORARY_STATE, value));
  EXPECT_FLOAT_EQ(value, 2.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x26, 0x64}), HeatingFunction::WORKING_RATE, value));
  EXPECT_FLOAT_EQ(value, 100.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x26, 0x0D}), HeatingFunction::WORKING_RATE, value));
  EXPECT_FLOAT_EQ(value, 13.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x38, 0xBE, 0x00}),
                                   HeatingFunction::TOWEL_COMFORT_SETPOINT, value));
  EXPECT_FLOAT_EQ(value, 19.0F);
}

TEST(ProtoHeatingTowelDryer, OutOfRangeAndEmptyRepliesAreNoValue) {
  float value = -1.0F;
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x36, 0x04}), HeatingFunction::TOWEL_MODE, value));
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x3E, 0x03}), HeatingFunction::TEMPORARY_STATE, value));
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x26, 0x65}), HeatingFunction::WORKING_RATE, value));
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x36}), HeatingFunction::TOWEL_MODE, value))
      << "a register the device does not hold is answered without a value";
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x17, 0x03}), HeatingFunction::REGULATOR_STATE, value))
      << "the regulator range is unchanged";
  EXPECT_FLOAT_EQ(value, -1.0F);
}

TEST(ProtoHeatingTowelDryer, ReadingsStoreAndReportTheTowelRegisters) {
  HeatingReadings readings;
  for (const HeatingFunction fn : {HeatingFunction::TOWEL_MODE, HeatingFunction::TEMPORARY_STATE,
                                   HeatingFunction::WORKING_RATE, HeatingFunction::TOWEL_COMFORT_SETPOINT}) {
    EXPECT_FALSE(readings.has(fn));
    EXPECT_EQ(readings.value_of(fn), -1);
  }
  EXPECT_TRUE(readings.store(HeatingFunction::TOWEL_MODE, 2.0F));
  EXPECT_TRUE(readings.store(HeatingFunction::TEMPORARY_STATE, 1.0F));
  EXPECT_TRUE(readings.store(HeatingFunction::WORKING_RATE, 100.0F));
  EXPECT_TRUE(readings.store(HeatingFunction::TOWEL_COMFORT_SETPOINT, 19.5F));
  EXPECT_EQ(readings.value_of(HeatingFunction::TOWEL_MODE), 2);
  EXPECT_EQ(readings.value_of(HeatingFunction::TEMPORARY_STATE), 1);
  EXPECT_EQ(readings.value_of(HeatingFunction::WORKING_RATE), 100);
  EXPECT_EQ(readings.value_of(HeatingFunction::TOWEL_COMFORT_SETPOINT), -1) << "a temperature has no wire byte";
  EXPECT_TRUE(readings.has(HeatingFunction::TOWEL_COMFORT_SETPOINT));
  EXPECT_EQ(readings.mode, -1) << "the radiator's mode field is untouched";

  readings.store(HeatingFunction::SET_MODE, 3.0F);
  EXPECT_EQ(readings.value_of(HeatingFunction::SET_MODE), readings.mode);
  EXPECT_EQ(readings.value_of(HeatingFunction::REGULATOR_STATE), readings.regulator);
}

TEST(ProtoHeatingTowelDryer, ARegisterReplyEchoesItsRegister) {
  EXPECT_TRUE(heating_reply_echoes_register(ack_with({0x0C, 0x60, 0x01, 0x19, 'A'}), 0x01, 0x19));
  EXPECT_TRUE(heating_reply_echoes_register(ack_with({0x0C, 0x60, 0x01, 0x36}), 0x01, 0x36)) << "empty is an echo";
  EXPECT_TRUE(heating_reply_echoes_register(ack_with({0x0C, 0x61, 0x01, 0x36, 0x03}), 0x01, 0x36)) << "a write's ack";
  EXPECT_FALSE(heating_reply_echoes_register(ack_with({0x0C, 0x60, 0x01, 0x18, 0x41}), 0x01, 0x19));
  EXPECT_FALSE(heating_reply_echoes_register(ack_with({0x0C, 0x60, 0x00, 0x19}), 0x01, 0x19));
  EXPECT_FALSE(heating_reply_echoes_register(ack_with({0x0C, 0x62, 0x01, 0x19}), 0x01, 0x19));
  EXPECT_FALSE(heating_reply_echoes_register(ack_with({0x0C, 0x60, 0x01}), 0x01, 0x19)) << "truncated";
  EXPECT_FALSE(heating_reply_echoes_register(ack_with({0x0C, 0x60, 0x01, 0x19}, CMD_WRITE_PRIVATE), 0x01, 0x19));

  uint8_t reg = 0;
  ASSERT_TRUE(heating_function_register(HeatingFunction::TOWEL_MODE, reg));
  EXPECT_EQ(reg, 0x36);
  ASSERT_TRUE(heating_function_register(HeatingFunction::SET_MODE, reg));
  EXPECT_EQ(reg, 0x00);
  EXPECT_FALSE(heating_function_register(static_cast<HeatingFunction>(0xEE), reg));
}

// --- Night light (Sauter Hekla, issue #222) ---

TEST(ProtoHeatingNightLight, WritesAreByteExact) {
  // On/off is the write the heater's own Cozytouch controller sends.
  EXPECT_EQ(encode(HeatingFunction::NIGHT_LIGHT, 1.0F), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x44, 0x01}));
  EXPECT_EQ(encode(HeatingFunction::NIGHT_LIGHT, 0.0F), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x44, 0x00}));
  // The brightness is written in the two-byte form it reads back in.
  EXPECT_EQ(encode(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, 50.0F),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x42, 0x32, 0x00}));
  EXPECT_EQ(encode(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, 100.0F),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x42, 0x64, 0x00}));
}

TEST(ProtoHeatingNightLight, WritesOutsideTheRangeOrNotIntegralAreRefused) {
  EXPECT_EQ(write_len(HeatingFunction::NIGHT_LIGHT, 2.0F), 0u);
  EXPECT_EQ(write_len(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, 101.0F), 0u);
  EXPECT_EQ(write_len(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, -1.0F), 0u);
  EXPECT_EQ(write_len(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, 50.5F), 0u);
  EXPECT_EQ(write_len(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, NAN), 0u);
  EXPECT_EQ(write_len(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, 0.0F), 6u);
}

TEST(ProtoHeatingNightLight, ReadsAddressTheirRegisters) {
  EXPECT_EQ(encode_read(HeatingFunction::NIGHT_LIGHT), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x44}));
  EXPECT_EQ(encode_read(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x42}));
  EXPECT_STREQ(heating_function_name(HeatingFunction::NIGHT_LIGHT), "night_light");
  EXPECT_STREQ(heating_function_name(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS), "night_light_brightness");
}

TEST(ProtoHeatingNightLight, DecodesWhatTheHeklaReports) {
  // The register_01 sweeps of issue #222: 0x0144 reads 01 with the light on and 00 with it off; 0x0142
  // reads 64 00 either way.
  float value = -1.0F;
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x44, 0x01}), HeatingFunction::NIGHT_LIGHT, value));
  EXPECT_FLOAT_EQ(value, 1.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x44, 0x00}), HeatingFunction::NIGHT_LIGHT, value));
  EXPECT_FLOAT_EQ(value, 0.0F);
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x42, 0x64, 0x00}),
                                   HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, value));
  EXPECT_FLOAT_EQ(value, 100.0F);
  // The heater's answer to the Cozytouch's write carries the new value too.
  ASSERT_TRUE(decode_heating_reply(ack_with({0x0C, 0x61, 0x01, 0x44, 0x01}), HeatingFunction::NIGHT_LIGHT, value));
  EXPECT_FLOAT_EQ(value, 1.0F);
}

TEST(ProtoHeatingNightLight, OutOfRangeShortAndEmptyRepliesAreNoValue) {
  float value = -1.0F;
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x42, 0x65, 0x00}),
                                    HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, value));
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x42, 0x32, 0x01}),
                                    HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, value))
      << "the high byte counts: 0x0132 is out of range";
  EXPECT_FALSE(
      decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x42, 0x32}), HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, value))
      << "a two-byte register answered with one byte";
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x44}), HeatingFunction::NIGHT_LIGHT, value))
      << "a heater without the light answers without a value";
  EXPECT_FALSE(decode_heating_reply(ack_with({0x0C, 0x60, 0x01, 0x44, 0x02}), HeatingFunction::NIGHT_LIGHT, value));
  EXPECT_FLOAT_EQ(value, -1.0F);
}

TEST(ProtoHeatingNightLight, ReadingsStoreAndReportTheLightRegisters) {
  HeatingReadings readings;
  EXPECT_FALSE(readings.has(HeatingFunction::NIGHT_LIGHT));
  EXPECT_FALSE(readings.has(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS));
  EXPECT_TRUE(readings.store(HeatingFunction::NIGHT_LIGHT, 1.0F));
  EXPECT_TRUE(readings.store(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, 40.0F));
  EXPECT_EQ(readings.night_light, 1);
  EXPECT_EQ(readings.night_light_brightness, 40);
  EXPECT_TRUE(readings.has(HeatingFunction::NIGHT_LIGHT));
  EXPECT_EQ(readings.value_of(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS), 40);
}
