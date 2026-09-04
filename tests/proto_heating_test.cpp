/// @file proto_heating_test.cpp
/// @brief Byte-exact tests for the CMD_WRITE_PRIVATE (0x20) heating payload codec.
///
/// Every expected payload is transcribed from reference/iohomecontrol/src/iohcCozyDevice2W.cpp
/// and cited by line; the 16-bit little-endian setpoint encoding is from the vendored register
/// map reference/iown-homecontrol/docs/devices/misc/AtlanticThermor/README.md.

#include "proto_heating.h"

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

size_t encode_len(HeatingFunction fn, float value) {
  uint8_t out[HEATING_PAYLOAD_MAX_SIZE] = {0};
  return encode_heating_payload(fn, value, out);
}

}  // namespace

// --- Byte-exact payloads for all six functions (iohcCozyDevice2W.cpp) ---

TEST(ProtoHeating, PowerOnPayload) {
  // :105  {0x0C, 0x60, 0x01, 0x2C}
  EXPECT_EQ(encode(HeatingFunction::POWER_ON, 0.0f), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x2C}));
}

TEST(ProtoHeating, SetTemperaturePayload) {
  // :125 {0x0C, 0x61, 0x01, 0x03, lo, hi}; LE16 tenths (AtlanticThermor/README.md 0x0103).
  // 20.5 C -> 205 -> 0x00CD -> CD 00.
  EXPECT_EQ(encode(HeatingFunction::SET_TEMPERATURE, 20.5f),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00}));
}

TEST(ProtoHeating, SetModePayload) {
  // :155  {0x0C, 0x61, 0x01, 0x00, mode}; manual = 0x01 (:159)
  EXPECT_EQ(encode(HeatingFunction::SET_MODE, static_cast<float>(HeatingMode::MANUAL)),
            (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x00, 0x01}));
}

TEST(ProtoHeating, SetPresencePayload) {
  // :195  {0x0C, 0x61, 0x01, 0x10, val}; on = 0x01 (:198), off = 0x00 (:199)
  EXPECT_EQ(encode(HeatingFunction::SET_PRESENCE, 1.0f), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x10, 0x01}));
  EXPECT_EQ(encode(HeatingFunction::SET_PRESENCE, 0.0f), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x10, 0x00}));
}

TEST(ProtoHeating, SetWindowPayload) {
  // :218  {0x0C, 0x61, 0x01, 0x0E, val}; open = 0x01 (:221), close = 0x00 (:222)
  EXPECT_EQ(encode(HeatingFunction::SET_WINDOW, 1.0f), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x0E, 0x01}));
  EXPECT_EQ(encode(HeatingFunction::SET_WINDOW, 0.0f), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x0E, 0x00}));
}

TEST(ProtoHeating, MidnightSyncPayload) {
  // :248  {0x0c, 0x60, 0x01, 0x30}
  EXPECT_EQ(encode(HeatingFunction::MIDNIGHT_SYNC, 0.0f), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x30}));
}

// --- All four modes (iohcCozyDevice2W.cpp:158-162) ---

TEST(ProtoHeating, AllModes) {
  EXPECT_EQ(encode(HeatingFunction::SET_MODE, static_cast<float>(HeatingMode::AUTO)).back(), 0x00);    // :158
  EXPECT_EQ(encode(HeatingFunction::SET_MODE, static_cast<float>(HeatingMode::MANUAL)).back(), 0x01);  // :159
  EXPECT_EQ(encode(HeatingFunction::SET_MODE, static_cast<float>(HeatingMode::PROG)).back(), 0x02);    // :160
  EXPECT_EQ(encode(HeatingFunction::SET_MODE, static_cast<float>(HeatingMode::OFF)).back(), 0x04);     // :162
}

TEST(ProtoHeating, RejectsUnknownMode) {
  // 0x03 "special" is commented out in the reference (:161) and must not encode.
  EXPECT_EQ(encode_len(HeatingFunction::SET_MODE, 3.0f), 0u);
  EXPECT_EQ(encode_len(HeatingFunction::SET_MODE, 1.5f), 0u);
  EXPECT_EQ(encode_len(HeatingFunction::SET_MODE, 5.0f), 0u);
}

TEST(ProtoHeating, RejectsNonBinaryPresenceAndWindow) {
  EXPECT_EQ(encode_len(HeatingFunction::SET_PRESENCE, 2.0f), 0u);
  EXPECT_EQ(encode_len(HeatingFunction::SET_WINDOW, 0.5f), 0u);
}

// --- Temperature matrix ---
// LE16 tenths of a degree (AtlanticThermor/README.md). For <= 25.5 C the high byte is 0x00, so
// these bytes are identical to the old single-byte form; 26.0 and 28.0 exercise the high byte.

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
  expect_temp(28.0f, 0x18, 0x01);   // 280  = 0x0118 (AtlanticThermor/README.md 0x0130 block)
}

TEST(ProtoHeating, TemperatureRejectsOutOfRangeAndNaN) {
  uint8_t out[HEATING_PAYLOAD_MAX_SIZE] = {0};
  EXPECT_EQ(encode_heating_payload(HeatingFunction::SET_TEMPERATURE, 6.9f, out), 0u);
  EXPECT_EQ(encode_heating_payload(HeatingFunction::SET_TEMPERATURE, 28.1f, out), 0u);
  EXPECT_EQ(encode_heating_payload(HeatingFunction::SET_TEMPERATURE, 53.5f, out), 0u);
  EXPECT_EQ(encode_heating_payload(HeatingFunction::SET_TEMPERATURE, std::nanf(""), out), 0u);
  EXPECT_EQ(encode_heating_payload(HeatingFunction::SET_TEMPERATURE, INFINITY, out), 0u);
}

TEST(ProtoHeating, PayloadLengths) {
  EXPECT_EQ(encode(HeatingFunction::POWER_ON, 0.0f).size(), 4u);
  EXPECT_EQ(encode(HeatingFunction::MIDNIGHT_SYNC, 0.0f).size(), 4u);
  EXPECT_EQ(encode(HeatingFunction::SET_MODE, 0.0f).size(), 5u);
  EXPECT_EQ(encode(HeatingFunction::SET_PRESENCE, 0.0f).size(), 5u);
  EXPECT_EQ(encode(HeatingFunction::SET_WINDOW, 0.0f).size(), 5u);
  EXPECT_EQ(encode(HeatingFunction::SET_TEMPERATURE, 20.0f).size(), 6u);
}

TEST(ProtoHeating, FunctionNames) {
  EXPECT_STREQ(heating_function_name(HeatingFunction::POWER_ON), "power_on");
  EXPECT_STREQ(heating_function_name(HeatingFunction::SET_TEMPERATURE), "set_temperature");
  EXPECT_STREQ(heating_function_name(HeatingFunction::SET_MODE), "set_mode");
  EXPECT_STREQ(heating_function_name(HeatingFunction::SET_PRESENCE), "set_presence");
  EXPECT_STREQ(heating_function_name(HeatingFunction::SET_WINDOW), "set_window");
  EXPECT_STREQ(heating_function_name(HeatingFunction::MIDNIGHT_SYNC), "midnight_sync");
}

TEST(ProtoHeating, PowerOnAndMidnightIgnoreValue) {
  EXPECT_EQ(encode(HeatingFunction::POWER_ON, 999.0f), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x2C}));
  EXPECT_EQ(encode(HeatingFunction::MIDNIGHT_SYNC, std::nanf("")), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x30}));
}

TEST(ProtoHeating, RejectsOutOfRangeFunctionEnum) {
  // descriptor_for() returns nullptr for a value not in the table; encode must return 0 and never
  // touch the output buffer.
  uint8_t out[HEATING_PAYLOAD_MAX_SIZE];
  for (auto &b : out)
    b = 0xEE;
  EXPECT_EQ(encode_heating_payload(static_cast<HeatingFunction>(0x7F), 20.0f, out), 0u);
  for (uint8_t b : out)
    EXPECT_EQ(b, 0xEE) << "a rejected function must not write the output buffer";
  EXPECT_STREQ(heating_function_name(static_cast<HeatingFunction>(0x7F)), "unknown");
}
