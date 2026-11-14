/// @file proto_dhw_test.cpp
/// @brief Byte-exact tests for the domestic hot water codec (proto_dhw.h): family-0x00 register requests and
/// replies, the mode write, and the target temperature on the main-parameter scale.

#include "proto_dhw.h"

#include "proto_commands.h"
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

TEST(ProtoDhw, TheDurationWritesAreFamilyZeroSetsOfTheBoostAndAwayRegisters) {
  uint8_t out[HEATING_PAYLOAD_MAX_SIZE] = {0};
  ASSERT_EQ(encode_dhw_duration_write(DhwOperatingFeature::BOOST, out), 5U);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 5), (std::vector<uint8_t>{0x0C, 0x61, 0x00, 0x00, 0x01}))
      << "boost: the value read during a boost started on the display";
  ASSERT_EQ(encode_dhw_duration_write(DhwOperatingFeature::AWAY, out), 5U);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 5), (std::vector<uint8_t>{0x0C, 0x61, 0x00, 0x05, 0xFF}))
      << "away: until turned off";
}

TEST(ProtoDhw, TheOperatingModeExecuteCarriesKeepAndOneOfThreeFixedWords) {
  static constexpr uint8_t OWN[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  static constexpr uint8_t DST[NODE_ID_SIZE] = {0xAB, 0xC1, 0x23};
  struct Case {
    uint16_t word;
    uint8_t high;
    uint8_t low;
  };
  for (const Case c : {Case{DHW_OPERATING_MODE_BOOST_ON, 0x49, 0x00}, Case{DHW_OPERATING_MODE_OFF, 0x4A, 0x00},
                       Case{DHW_OPERATING_MODE_AWAY_ON, 0x46, 0x00}}) {
    IoFrame f{};
    ASSERT_TRUE(create_execute_heating_with_fp16(f, OWN, DST, false, MAIN_PARAMETER_KEEP, c.word));
    EXPECT_EQ(f.cmd, CMD_EXECUTE);
    ASSERT_GE(f.data_len, 6U);
    const uint8_t *tail = f.data + f.data_len - 6;
    EXPECT_EQ(std::vector<uint8_t>(tail, tail + 6), (std::vector<uint8_t>{0xD4, 0x00, 0x00, 0x01, c.high, c.low}))
        << "main parameter D400 (keep), no FP1-FP8, FP16 only";
  }
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

TEST(ProtoDhw, ADurationIsTheFirstValueByte) {
  uint8_t duration = 0;
  const uint8_t all[] = {0xFF, 0x00};
  ASSERT_TRUE(decode_dhw_duration(all, sizeof(all), duration));
  EXPECT_EQ(duration, 0xFF) << "255 is 'until turned off'";
  const uint8_t zero = 0;
  ASSERT_TRUE(decode_dhw_duration(&zero, 1, duration));
  EXPECT_EQ(duration, 0);
  duration = 7;
  EXPECT_FALSE(decode_dhw_duration(&zero, 0, duration));
  EXPECT_FALSE(decode_dhw_duration(nullptr, 1, duration));
  EXPECT_EQ(duration, 7) << "untouched on failure";
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

// === Operating status (FP15 and FP16) ===

namespace {
/// The reply to a status read with FPI1 00 / FPI2 03: flags, b1, MP, FPI1, FPI2, FP15, FP16.
IoFrame operating_reply(uint16_t fp16, uint16_t mp = 0xA1E8, uint16_t fp15 = 0x000C) {
  return reply_with({0x05, 0x00, static_cast<uint8_t>(mp >> 8), static_cast<uint8_t>(mp & 0xFF), 0x00, 0x03,
                     static_cast<uint8_t>(fp15 >> 8), static_cast<uint8_t>(fp15 & 0xFF),
                     static_cast<uint8_t>(fp16 >> 8), static_cast<uint8_t>(fp16 & 0xFF)},
                    CMD_PRIVATE_RESP);
}
}  // namespace

TEST(ProtoDhw, OperatingStatusReadsCapabilitiesAndTheTwoFields) {
  DhwOperatingStatus s;
  ASSERT_TRUE(decode_dhw_operating_status(operating_reply(DHW_OPERATING_MODE_OFF), s));
  EXPECT_EQ(s.capabilities, DHW_CAPABILITY_AWAY | DHW_CAPABILITY_BOOST);
  EXPECT_FALSE(s.boost);
  EXPECT_FALSE(s.away);

  ASSERT_TRUE(decode_dhw_operating_status(operating_reply(DHW_OPERATING_MODE_BOOST_ON), s));
  EXPECT_TRUE(s.boost);
  EXPECT_FALSE(s.away);

  ASSERT_TRUE(decode_dhw_operating_status(operating_reply(DHW_OPERATING_MODE_AWAY_ON), s));
  EXPECT_FALSE(s.boost);
  EXPECT_TRUE(s.away);
}

TEST(ProtoDhw, EachFeaturesOnWordSetsItsOwnFieldAndEndsTheOther) {
  // The on words and the fields they are read from must agree: a word sent to start a feature reads back as
  // that feature on and the other off.
  for (const DhwOperatingFeature which : {DhwOperatingFeature::BOOST, DhwOperatingFeature::AWAY}) {
    DhwOperatingStatus s;
    ASSERT_TRUE(decode_dhw_operating_status(operating_reply(dhw_operating_feature_wire(which).on_word), s));
    EXPECT_EQ(s.boost, which == DhwOperatingFeature::BOOST);
    EXPECT_EQ(s.away, which == DhwOperatingFeature::AWAY);
  }
  EXPECT_EQ(dhw_operating_feature_wire(DhwOperatingFeature::BOOST).capability, DHW_CAPABILITY_BOOST);
  EXPECT_EQ(dhw_operating_feature_wire(DhwOperatingFeature::AWAY).capability, DHW_CAPABILITY_AWAY);
}

TEST(ProtoDhw, OnlyAFieldValueOfOneIsOn) {
  DhwOperatingStatus s;
  // 0 keep, 2 off and 3 are not "on" in either field; bit 14 and the low byte do not matter.
  ASSERT_TRUE(decode_dhw_operating_status(operating_reply(0x4000), s));
  EXPECT_FALSE(s.boost);
  EXPECT_FALSE(s.away);
  ASSERT_TRUE(decode_dhw_operating_status(operating_reply(0x4F00), s));
  EXPECT_FALSE(s.boost) << "field 3";
  EXPECT_FALSE(s.away) << "field 3";
  ASSERT_TRUE(decode_dhw_operating_status(operating_reply(0x45FF), s));
  EXPECT_TRUE(s.boost);
  EXPECT_TRUE(s.away) << "the word can say both; the decoder reports what it says";
}

TEST(ProtoDhw, OperatingStatusDecodesWhateverTheMainParameter) {
  // The main parameter is decode_dhw_status_target()'s; an alias there does not fail this decode.
  DhwOperatingStatus s;
  ASSERT_TRUE(decode_dhw_operating_status(operating_reply(DHW_OPERATING_MODE_AWAY_ON, 0xD812), s));
  EXPECT_TRUE(s.away);
}

TEST(ProtoDhw, OperatingStatusSkipsOtherParametersOfTheEcho) {
  // The heater echoes FP10 when asked for 0x43: FPI2 43, values FP10, FP15, FP16 (issue #198).
  DhwOperatingStatus s;
  ASSERT_TRUE(decode_dhw_operating_status(
      reply_with({0x05, 0x00, 0xBE, 0x7A, 0x00, 0x43, 0x00, 0x00, 0x00, 0x0C, 0x4A, 0x00}, CMD_PRIVATE_RESP), s));
  EXPECT_EQ(s.capabilities, 0x000C);
  EXPECT_FALSE(s.boost);
  EXPECT_FALSE(s.away);
}

TEST(ProtoDhw, OperatingStatusRefusesWhatItCannotRead) {
  DhwOperatingStatus s;
  s.capabilities = 0x1234;
  const IoFrame good = operating_reply(DHW_OPERATING_MODE_OFF);
  EXPECT_FALSE(decode_dhw_operating_status(reply_with({0x05, 0x00, 0xA1, 0xE8, 0x00, 0x00}, CMD_PRIVATE_RESP), s))
      << "the plain FP1-FP7 read has no FP15 or FP16";
  EXPECT_FALSE(
      decode_dhw_operating_status(reply_with({0x05, 0x00, 0xA1, 0xE8, 0x00, 0x01, 0x4A, 0x00}, CMD_PRIVATE_RESP), s))
      << "FP16 without FP15";
  EXPECT_FALSE(
      decode_dhw_operating_status(reply_with({0x05, 0x00, 0xA1, 0xE8, 0x00, 0x02, 0x00, 0x0C}, CMD_PRIVATE_RESP), s))
      << "FP15 without FP16";
  EXPECT_FALSE(decode_dhw_operating_status(
      reply_with({0x05, 0x00, 0xA1, 0xE8, 0x01, 0x03, 0x00, 0x0C, 0x4A, 0x00}, CMD_PRIVATE_RESP), s))
      << "an FPI1 echo shifts the values";
  IoFrame short_reply = good;
  short_reply.data_len -= 1;
  EXPECT_FALSE(decode_dhw_operating_status(short_reply, s)) << "FP16 cut short";
  IoFrame wrong_command = good;
  wrong_command.cmd = CMD_WRITE_PRIVATE_ACK;
  EXPECT_FALSE(decode_dhw_operating_status(wrong_command, s));
  EXPECT_EQ(s.capabilities, 0x1234) << "untouched on failure";
}
