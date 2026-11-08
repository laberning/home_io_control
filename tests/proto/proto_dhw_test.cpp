/// @file proto_dhw_test.cpp
/// @brief Byte-exact tests for the domestic hot water codec (proto_dhw.h): family-0x00 register requests and
/// replies, the mode write, and the target temperature on the main-parameter scale.

#include "proto_dhw.h"

#include "proto_constants.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace esphome::home_io_control;

namespace {

/// A reply with the given command and data bytes.
IoFrame reply_with(const std::vector<uint8_t> &data, uint8_t cmd = CMD_WRITE_PRIVATE_ACK) {
  IoFrame f{};
  f.cmd = cmd;
  f.data_len = static_cast<uint8_t>(data.size());
  for (size_t i = 0; i < data.size(); i++)
    f.data[i] = data[i];
  return f;
}

/// The value bytes of a register reply, or an empty vector when the reply is not for `index`.
std::vector<uint8_t> value_of(const IoFrame &reply, uint8_t index, bool *ok = nullptr) {
  const uint8_t *value = nullptr;
  size_t len = 0;
  const bool decoded = decode_dhw_register_reply(reply, index, value, len);
  if (ok != nullptr)
    *ok = decoded;
  return decoded ? std::vector<uint8_t>(value, value + len) : std::vector<uint8_t>();
}

}  // namespace

// === Requests ===

TEST(ProtoDhw, RegisterReadIsFamilyZeroGet) {
  uint8_t out[HEATING_PAYLOAD_MAX_SIZE] = {0};
  ASSERT_EQ(encode_dhw_register_read(DHW_REG_WATER_TEMPERATURE, out), 4U);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 4), (std::vector<uint8_t>{0x0C, 0x60, 0x00, 0x17}));
}

TEST(ProtoDhw, ModeWriteIsFamilyZeroSetOfRegisterSix) {
  uint8_t out[HEATING_PAYLOAD_MAX_SIZE] = {0};
  for (uint8_t mode = 0; mode <= 2; mode++) {
    ASSERT_EQ(encode_dhw_mode_write(mode, out), 5U) << static_cast<unsigned>(mode);
    EXPECT_EQ(std::vector<uint8_t>(out, out + 5), (std::vector<uint8_t>{0x0C, 0x61, 0x00, 0x06, mode}));
  }
}

TEST(ProtoDhw, ModeWriteRefusesAValueTheHeaterHasNeverShown) {
  uint8_t out[HEATING_PAYLOAD_MAX_SIZE] = {0};
  EXPECT_EQ(encode_dhw_mode_write(3, out), 0U);
  EXPECT_EQ(encode_dhw_mode_write(0xFF, out), 0U);
}

TEST(ProtoDhw, BoostWriteIsFamilyZeroSetOfRegisterZero) {
  uint8_t out[HEATING_PAYLOAD_MAX_SIZE] = {0};
  ASSERT_EQ(encode_dhw_boost_write(true, out), 5U);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 5), (std::vector<uint8_t>{0x0C, 0x61, 0x00, 0x00, 0x01}))
      << "on writes the value read during a one-day boost";
  ASSERT_EQ(encode_dhw_boost_write(false, out), 5U);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 5), (std::vector<uint8_t>{0x0C, 0x61, 0x00, 0x00, 0x00}));
}

TEST(ProtoDhw, TargetEncodesOnTheRelativeScaleOverTwentyToSixtyTwo) {
  uint16_t mp = 0;
  ASSERT_TRUE(encode_dhw_target(54.0F, mp));
  EXPECT_EQ(mp, 0xA1E8);
  ASSERT_TRUE(encode_dhw_target(62.0F, mp));
  EXPECT_EQ(mp, 0xC800);
  ASSERT_TRUE(encode_dhw_target(20.0F, mp));
  EXPECT_EQ(mp, 0x0000);
  ASSERT_TRUE(encode_dhw_target(60.0F, mp));
  EXPECT_EQ(mp, 0xBE7A);
}

TEST(ProtoDhw, TargetOutsideTheHeatersRangeIsNeverEncoded) {
  uint16_t mp = 0x1234;
  EXPECT_FALSE(encode_dhw_target(19.9F, mp));
  EXPECT_FALSE(encode_dhw_target(62.1F, mp));
  EXPECT_FALSE(encode_dhw_target(NAN, mp));
  EXPECT_FALSE(encode_dhw_target(INFINITY, mp));
  EXPECT_EQ(mp, 0x1234);
}

// === Replies ===

TEST(ProtoDhw, RegisterReplyYieldsTheValueAfterTheEcho) {
  bool ok = false;
  const auto value = value_of(reply_with({0x0C, 0x60, 0x00, 0x04, 0x00, 0x1B, 0xF3, 0x42}), DHW_REG_ENERGY, &ok);
  ASSERT_TRUE(ok);
  EXPECT_EQ(value, (std::vector<uint8_t>{0x00, 0x1B, 0xF3, 0x42}));
}

TEST(ProtoDhw, AnIndexTheHeaterDoesNotHoldIsAnEmptyValue) {
  bool ok = false;
  const auto value = value_of(reply_with({0x0C, 0x60, 0x00, 0x18}), 0x18, &ok);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(value.empty());
}

TEST(ProtoDhw, RegisterReplyForAnotherIndexFamilyOrCommandIsRejected) {
  bool ok = true;
  value_of(reply_with({0x0C, 0x60, 0x00, 0x06, 0x01}), DHW_REG_MODE + 1, &ok);
  EXPECT_FALSE(ok) << "another index";
  value_of(reply_with({0x0C, 0x60, 0x01, 0x06, 0x01}), DHW_REG_MODE, &ok);
  EXPECT_FALSE(ok) << "the radiator family 0x01";
  value_of(reply_with({0x0C, 0x60, 0x00, 0x06, 0x01}, CMD_PRIVATE_RESP), DHW_REG_MODE, &ok);
  EXPECT_FALSE(ok) << "not a CMD_WRITE_PRIVATE_ACK";
  value_of(reply_with({0x0C, 0x60, 0x00}), DHW_REG_MODE, &ok);
  EXPECT_FALSE(ok) << "short echo";
}

TEST(ProtoDhw, WaterTemperatureIsSignedBigEndianTenths) {
  float c = NAN;
  const uint8_t warm[] = {0x01, 0xF0, 0x00, 0x5C, 0x02, 0x78};
  ASSERT_TRUE(decode_dhw_water_temperature(warm, sizeof(warm), c));
  EXPECT_FLOAT_EQ(c, 49.6F);
  const uint8_t below_zero[] = {0xFF, 0xF2};
  ASSERT_TRUE(decode_dhw_water_temperature(below_zero, sizeof(below_zero), c));
  EXPECT_FLOAT_EQ(c, -1.4F);
  const uint8_t short_value[] = {0x01};
  c = 1.0F;
  EXPECT_FALSE(decode_dhw_water_temperature(short_value, sizeof(short_value), c));
  EXPECT_FLOAT_EQ(c, 1.0F);
}

TEST(ProtoDhw, ModeDecodesTheThreeValuesSeenAndNothingElse) {
  DhwMode mode = DhwMode::AUTO;
  for (uint8_t wire = 0; wire <= 2; wire++) {
    ASSERT_TRUE(decode_dhw_mode(&wire, 1, mode));
    EXPECT_EQ(static_cast<uint8_t>(mode), wire);
  }
  const uint8_t unknown = 3;
  mode = DhwMode::MANUAL_ECO_ON;
  EXPECT_FALSE(decode_dhw_mode(&unknown, 1, mode));
  EXPECT_EQ(mode, DhwMode::MANUAL_ECO_ON);
  EXPECT_FALSE(decode_dhw_mode(&unknown, 0, mode));
}

TEST(ProtoDhw, HeatingStateReadsTheThreeStatesSeen) {
  DhwHeatingState s;
  const uint8_t idle[] = {0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  ASSERT_TRUE(decode_dhw_heating_state(idle, sizeof(idle), s));
  EXPECT_FALSE(s.heating);
  EXPECT_FALSE(s.element);
  EXPECT_FALSE(s.heat_pump);

  const uint8_t heat_pump[] = {0x01, 0xFF, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00};
  ASSERT_TRUE(decode_dhw_heating_state(heat_pump, sizeof(heat_pump), s));
  EXPECT_TRUE(s.heating);
  EXPECT_FALSE(s.element);
  EXPECT_TRUE(s.heat_pump);

  const uint8_t boost[] = {0x01, 0xFF, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00};
  ASSERT_TRUE(decode_dhw_heating_state(boost, sizeof(boost), s));
  EXPECT_TRUE(s.heating);
  EXPECT_TRUE(s.element);
  EXPECT_TRUE(s.heat_pump);

  EXPECT_FALSE(decode_dhw_heating_state(boost, 4, s));
}

TEST(ProtoDhw, EnergyIsUnsignedBigEndianWattHours) {
  uint32_t wh = 0;
  const uint8_t value[] = {0x00, 0x1B, 0xF3, 0x42};
  ASSERT_TRUE(decode_dhw_energy_wh(value, sizeof(value), wh));
  EXPECT_EQ(wh, 1831746U);
  const uint8_t top[] = {0xFF, 0xFF, 0xFF, 0xFF};
  ASSERT_TRUE(decode_dhw_energy_wh(top, sizeof(top), wh));
  EXPECT_EQ(wh, 0xFFFFFFFFU);
  EXPECT_FALSE(decode_dhw_energy_wh(value, 3, wh));
}

TEST(ProtoDhw, BoostIsOnForAnyNonZeroByte) {
  bool on = false;
  const uint8_t one = 1;
  ASSERT_TRUE(decode_dhw_boost(&one, 1, on));
  EXPECT_TRUE(on);
  const uint8_t zero = 0;
  ASSERT_TRUE(decode_dhw_boost(&zero, 1, on));
  EXPECT_FALSE(on);
  EXPECT_FALSE(decode_dhw_boost(&one, 0, on));
}

TEST(ProtoDhw, TargetDecodesTheThreeMainParametersSeen) {
  float c = NAN;
  ASSERT_TRUE(decode_dhw_target(0xA1E8, c));
  EXPECT_FLOAT_EQ(c, 54.0F);
  ASSERT_TRUE(decode_dhw_target(0xC800, c));
  EXPECT_FLOAT_EQ(c, 62.0F);
  ASSERT_TRUE(decode_dhw_target(0xBE7A, c));
  EXPECT_FLOAT_EQ(c, 60.0F);
  ASSERT_TRUE(decode_dhw_target(0x0000, c));
  EXPECT_FLOAT_EQ(c, 20.0F);
}

TEST(ProtoDhw, AMainParameterBeyondTheScaleIsNotATarget) {
  float c = 42.0F;
  EXPECT_FALSE(decode_dhw_target(0xC801, c));
  EXPECT_FALSE(decode_dhw_target(0xD812, c)) << "an alias";
  EXPECT_FALSE(decode_dhw_target(0xD400, c)) << "unknown position";
  EXPECT_FLOAT_EQ(c, 42.0F);
}

TEST(ProtoDhw, StatusTargetReadsTheMainParameterAtOffsetTwo) {
  float c = NAN;
  ASSERT_TRUE(decode_dhw_status_target(reply_with({0x2D, 0x00, 0xA1, 0xE8, 0x00, 0x00}, CMD_PRIVATE_RESP), c));
  EXPECT_FLOAT_EQ(c, 54.0F);
  EXPECT_FALSE(decode_dhw_status_target(reply_with({0x2D, 0x00, 0xA1, 0xE8}, CMD_WRITE_PRIVATE_ACK), c))
      << "another command";
  EXPECT_FALSE(decode_dhw_status_target(reply_with({0x2D, 0x00, 0xA1}, CMD_PRIVATE_RESP), c)) << "too short";
  EXPECT_FALSE(decode_dhw_status_target(reply_with({0x2D, 0x00, 0xD8, 0x12, 0x00, 0x00}, CMD_PRIVATE_RESP), c))
      << "alias";
}

TEST(ProtoDhw, EveryEncodedTargetDecodesBackWithinATenth) {
  for (float t = DHW_TARGET_MIN_C; t <= DHW_TARGET_MAX_C; t += 0.5F) {
    uint16_t mp = 0;
    ASSERT_TRUE(encode_dhw_target(t, mp)) << t;
    float back = NAN;
    ASSERT_TRUE(decode_dhw_target(mp, back)) << t;
    EXPECT_NEAR(back, t, 0.05F) << t;
  }
}
