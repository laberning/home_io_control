/// @file proto_dhw_test.cpp
/// @brief Byte-exact tests for a water heater on the shared tables: its family-0x00 register requests and replies
/// (proto_heating.h), the readings that are its registers and status parameters (proto_device_readings.h), the target
/// on the main-parameter scale and the operating-mode word (proto_dhw.h).

#include "proto_dhw.h"

#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_device_readings.h"
#include "proto_device_types.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"
#include "proto_private_registers.h"
#include "reading_store.h"
#include "water_heater_values.h"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <span>
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

/// The bytes of a payload an encoder wrote.
std::vector<uint8_t> payload_of(const uint8_t (&out)[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE], size_t length) {
  return std::vector<uint8_t>(out, out + length);
}

/// A water heater's record, with its capabilities as the hub resolves them for the kind.
IoDevice water_heater_record() {
  IoDevice dev;
  dev.type = DeviceType::DOMESTIC_HOT_WATER;
  dev.features = device_features(DeviceType::DOMESTIC_HOT_WATER, 0, DeviceModel::NONE);
  return dev;
}

}  // namespace

// === Requests ===

TEST(ProtoDhw, RegisterReadIsFamilyZeroGet) {
  uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE] = {0};
  ASSERT_EQ(encode_heating_read(DeviceRegister::DHW_WATER_TEMPERATURE, out), 4U);
  EXPECT_EQ(payload_of(out, 4), (std::vector<uint8_t>{0x0C, 0x60, 0x00, 0x17}));
}

TEST(ProtoDhw, EveryWaterHeaterRegisterIsAFamilyZeroRegisterOfTheWire) {
  for (const DeviceRegister fn :
       {DeviceRegister::DHW_WATER_TEMPERATURE, DeviceRegister::DHW_MODE, DeviceRegister::DHW_HEATING,
        DeviceRegister::DHW_BOOST_DURATION, DeviceRegister::DHW_AWAY_DURATION, DeviceRegister::DHW_ENERGY,
        DeviceRegister::DHW_BRAND}) {
    uint8_t high = 0xFF;
    uint8_t low = 0xFF;
    ASSERT_TRUE(device_register_bytes(fn, high, low));
    EXPECT_EQ(high, DHW_REGISTER_HIGH_BYTE) << device_register_name(fn);
    EXPECT_EQ(low, test::dhw_register_byte(fn)) << device_register_name(fn);
    EXPECT_EQ(device_register_family(fn), DHW_REGISTER_HIGH_BYTE) << device_register_name(fn);
  }
  uint8_t high = 0;
  uint8_t low = 0;
  ASSERT_TRUE(device_register_bytes(DeviceRegister::SET_TEMPERATURE, high, low));
  EXPECT_EQ(high, HEATING_REGISTER_HIGH_BYTE) << "a heater's register keeps its family";
}

TEST(ProtoDhw, ModeWriteIsFamilyZeroSetOfRegisterSix) {
  uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE] = {0};
  for (uint8_t mode = 0; mode <= 2; mode++) {
    ASSERT_EQ(encode_heating_write(DeviceRegister::DHW_MODE, static_cast<float>(mode), out), 5U)
        << static_cast<unsigned>(mode);
    EXPECT_EQ(payload_of(out, 5), (std::vector<uint8_t>{0x0C, 0x61, 0x00, 0x06, mode}));
  }
}

TEST(ProtoDhw, ModeWriteRefusesAValueTheHeaterHasNeverShown) {
  uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE] = {0};
  EXPECT_EQ(encode_heating_write(DeviceRegister::DHW_MODE, 3.0F, out), 0U);
  EXPECT_EQ(encode_heating_write(DeviceRegister::DHW_MODE, 255.0F, out), 0U);
  EXPECT_EQ(encode_heating_write(DeviceRegister::DHW_MODE, 1.5F, out), 0U);
  EXPECT_EQ(encode_heating_write(DeviceRegister::DHW_MODE, NAN, out), 0U);
  EXPECT_EQ(encode_heating_write(DeviceRegister::DHW_MODE, -1.0F, out), 0U);
}

TEST(ProtoDhw, TheDurationWritesAreFamilyZeroSetsOfTheBoostAndAwayRegisters) {
  uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE] = {0};
  for (const DhwOperatingFeature which : {DhwOperatingFeature::BOOST, DhwOperatingFeature::AWAY}) {
    const DhwOperatingFeatureWire &wire = dhw_operating_feature_wire(which);
    ASSERT_EQ(encode_heating_write(wire.duration_register, static_cast<float>(wire.start_duration), out), 5U);
    EXPECT_EQ(
        payload_of(out, 5),
        (std::vector<uint8_t>{0x0C, 0x61, 0x00, test::dhw_register_byte(wire.duration_register), wire.start_duration}));
  }
  ASSERT_EQ(encode_heating_write(DeviceRegister::DHW_BOOST_DURATION, 1.0F, out), 5U);
  EXPECT_EQ(payload_of(out, 5), (std::vector<uint8_t>{0x0C, 0x61, 0x00, 0x00, 0x01}))
      << "boost: the value read during a boost started on the display";
  ASSERT_EQ(encode_heating_write(DeviceRegister::DHW_AWAY_DURATION, 255.0F, out), 5U);
  EXPECT_EQ(payload_of(out, 5), (std::vector<uint8_t>{0x0C, 0x61, 0x00, 0x05, 0xFF})) << "away: until turned off";
  EXPECT_EQ(encode_heating_write(DeviceRegister::DHW_AWAY_DURATION, 256.0F, out), 0U) << "a byte, never wrapped";
}

TEST(ProtoDhw, TheReadOnlyRegistersAreNeverWritten) {
  uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE] = {0};
  for (const DeviceRegister fn : {DeviceRegister::DHW_WATER_TEMPERATURE, DeviceRegister::DHW_HEATING,
                                  DeviceRegister::DHW_ENERGY, DeviceRegister::DHW_BRAND}) {
    EXPECT_FALSE(device_register_is_writable(fn)) << device_register_name(fn);
    EXPECT_EQ(encode_heating_write(fn, 1.0F, out), 0U) << device_register_name(fn);
    EXPECT_FALSE(heating_write_accepts(fn, 1.0F)) << device_register_name(fn);
  }
  for (const DeviceRegister fn :
       {DeviceRegister::DHW_MODE, DeviceRegister::DHW_BOOST_DURATION, DeviceRegister::DHW_AWAY_DURATION})
    EXPECT_TRUE(device_register_is_writable(fn)) << device_register_name(fn);
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
  const IoDevice dev = water_heater_record();
  const HeatingCardRange range = device_temperature_range(dev);
  EXPECT_FLOAT_EQ(range.min_c, DHW_TARGET_MIN_C);
  EXPECT_FLOAT_EQ(range.max_c, DHW_TARGET_MAX_C);
  uint16_t mp = 0;
  ASSERT_EQ(encode_temperature_target(range, 54.0F, mp), HeatingRefusal::NONE);
  EXPECT_EQ(mp, 0xA1E8);
  ASSERT_EQ(encode_temperature_target(range, 62.0F, mp), HeatingRefusal::NONE);
  EXPECT_EQ(mp, 0xC800);
  ASSERT_EQ(encode_temperature_target(range, 20.0F, mp), HeatingRefusal::NONE);
  EXPECT_EQ(mp, 0x0000);
  ASSERT_EQ(encode_temperature_target(range, 60.0F, mp), HeatingRefusal::NONE);
  EXPECT_EQ(mp, 0xBE7A);
}

TEST(ProtoDhw, TargetOutsideTheHeatersRangeIsNeverEncoded) {
  const HeatingCardRange range = device_temperature_range(water_heater_record());
  uint16_t mp = 0x1234;
  EXPECT_EQ(encode_temperature_target(range, 19.9F, mp), HeatingRefusal::OUT_OF_RANGE);
  EXPECT_EQ(encode_temperature_target(range, 62.1F, mp), HeatingRefusal::OUT_OF_RANGE);
  EXPECT_EQ(encode_temperature_target(range, NAN, mp), HeatingRefusal::UNENCODABLE);
  EXPECT_EQ(encode_temperature_target(range, INFINITY, mp), HeatingRefusal::UNENCODABLE);
  EXPECT_EQ(mp, 0x1234);
}

TEST(ProtoDhw, TheWaterHeatersRangeIsItsOwnWhateverTheRecordHolds) {
  IoDevice dev = water_heater_record();
  dev.temperature_range_min_c = 5.0F;
  dev.temperature_range_max_c = 30.0F;
  const HeatingCardRange range = device_temperature_range(dev);
  EXPECT_FLOAT_EQ(range.min_c, DHW_TARGET_MIN_C);
  EXPECT_FLOAT_EQ(range.max_c, DHW_TARGET_MAX_C);
}

// === Replies ===

TEST(ProtoDhw, RegisterReplyYieldsTheValueAfterTheEcho) {
  const IoFrame reply = reply_with({0x0C, 0x60, 0x00, 0x04, 0x00, 0x1B, 0xF3, 0x42});
  const auto value = register_reply_value(reply, DeviceRegister::DHW_ENERGY);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(std::vector<uint8_t>(value->begin(), value->end()), (std::vector<uint8_t>{0x00, 0x1B, 0xF3, 0x42}));
}

TEST(ProtoDhw, AnIndexTheHeaterDoesNotHoldIsAnEmptyValue) {
  const IoFrame reply = reply_with({0x0C, 0x60, 0x00, 0x04});
  const auto value = register_reply_value(reply, DeviceRegister::DHW_ENERGY);
  ASSERT_TRUE(value.has_value());
  EXPECT_TRUE(value->empty());
  float decoded = 0.0F;
  EXPECT_FALSE(decode_heating_reply(reply, DeviceRegister::DHW_ENERGY, decoded)) << "an empty value is no value";
}

TEST(ProtoDhw, RegisterReplyForAnotherIndexFamilyOrCommandIsRejected) {
  const IoFrame mode = reply_with({0x0C, 0x60, 0x00, 0x06, 0x01});
  EXPECT_TRUE(register_reply_value(mode, DeviceRegister::DHW_MODE).has_value());
  EXPECT_FALSE(register_reply_value(mode, DeviceRegister::DHW_ENERGY).has_value()) << "another index";
  EXPECT_FALSE(register_reply_value(reply_with({0x0C, 0x60, 0x01, 0x06, 0x01}), DeviceRegister::DHW_MODE).has_value())
      << "the radiator family 0x01";
  EXPECT_FALSE(
      register_reply_value(reply_with({0x0C, 0x60, 0x00, 0x06, 0x01}, CMD_PRIVATE_RESP), DeviceRegister::DHW_MODE)
          .has_value())
      << "not a CMD_WRITE_PRIVATE_ACK";
  EXPECT_FALSE(register_reply_value(reply_with({0x0C, 0x60, 0x00}), DeviceRegister::DHW_MODE).has_value())
      << "short echo";
  EXPECT_FALSE(register_reply_value(reply_with({0x0C, 0x60, 0x00, 0x06, 0x01}), DeviceRegister::SET_MODE).has_value())
      << "a heater's mode register is another register";
}

TEST(ProtoDhw, WaterTemperatureIsSignedBigEndianTenths) {
  const uint8_t warm[] = {0x01, 0xF0, 0x00, 0x5C, 0x02, 0x78};
  EXPECT_FLOAT_EQ(*decode_register_value(DeviceRegister::DHW_WATER_TEMPERATURE, warm), 49.6F);
  const uint8_t below_zero[] = {0xFF, 0xF2};
  EXPECT_FLOAT_EQ(*decode_register_value(DeviceRegister::DHW_WATER_TEMPERATURE, below_zero), -1.4F);
  const uint8_t short_value[] = {0x01};
  EXPECT_FALSE(decode_register_value(DeviceRegister::DHW_WATER_TEMPERATURE, short_value).has_value());
}

TEST(ProtoDhw, ModeDecodesTheThreeValuesSeenAndNothingElse) {
  for (uint8_t wire = 0; wire <= 2; wire++) {
    const auto mode = decode_register_value(DeviceRegister::DHW_MODE, std::span<const uint8_t>(&wire, 1));
    ASSERT_TRUE(mode.has_value());
    EXPECT_EQ(static_cast<uint8_t>(*mode), wire);
    EXPECT_NE(dhw_mode_name(wire), nullptr);
  }
  const uint8_t unknown = 3;
  EXPECT_FALSE(decode_register_value(DeviceRegister::DHW_MODE, std::span<const uint8_t>(&unknown, 1)).has_value());
  EXPECT_FALSE(decode_register_value(DeviceRegister::DHW_MODE, std::span<const uint8_t>(&unknown, 0)).has_value());
  EXPECT_EQ(dhw_mode_name(unknown), nullptr);
  EXPECT_STREQ(dhw_mode_name(0), "auto");
  EXPECT_STREQ(dhw_mode_name(1), "manual eco on");
  EXPECT_STREQ(dhw_mode_name(2), "manual eco off");
}

TEST(ProtoDhw, TheHeatingRegisterHasNoValueOfItsOwn) {
  const uint8_t value[] = {0x01, 0xFF, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00};
  EXPECT_FALSE(decode_register_value(DeviceRegister::DHW_HEATING, value).has_value())
      << "its readings are slices of it";
  EXPECT_FALSE(heating_reply_carries_value(reply_with({0x0C, 0x60, 0x00, 0x21, 0x01, 0xFF, 0x00, 0x01, 0x01}),
                                           DeviceRegister::DHW_HEATING));
}

TEST(ProtoDhw, HeatingStateReadsTheThreeStatesSeen) {
  const struct {
    std::vector<uint8_t> value;
    int heating;
    int element;
    int heat_pump;
    const char *state;
  } cases[] = {
      {{0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 0, 0, 0, "idle"},
      {{0x01, 0xFF, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00}, 1, 0, 1, "heat pump"},
      {{0x01, 0xFF, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00}, 1, 1, 1, "boost"},
  };
  for (const auto &c : cases) {
    IoDevice dev = water_heater_record();
    ASSERT_TRUE(store_register_reply(dev, DeviceRegister::DHW_HEATING, c.value)) << c.state;
    EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_HEATING), c.heating) << c.state;
    EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_ELECTRIC_BACKUP), c.element) << c.state;
    EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_HEAT_PUMP), c.heat_pump) << c.state;
  }
}

TEST(ProtoDhw, TheHeatPumpIsOnWhenEitherOfItsBytesIsSet) {
  // Bytes 3 and 4 were always set together; either one alone still says the heat pump runs.
  for (const std::vector<uint8_t> &value : {std::vector<uint8_t>{0x01, 0xFF, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00},
                                            std::vector<uint8_t>{0x01, 0xFF, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}}) {
    IoDevice dev = water_heater_record();
    ASSERT_TRUE(store_register_reply(dev, DeviceRegister::DHW_HEATING, value));
    EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_HEAT_PUMP), 1);
  }
}

TEST(ProtoDhw, ACutOffHeatingValueStoresOnlyTheSlicesItHolds) {
  IoDevice dev = water_heater_record();
  // Three bytes: the heating byte and the element byte are there, the heat pump's two are not.
  ASSERT_TRUE(store_register_reply(dev, DeviceRegister::DHW_HEATING, std::vector<uint8_t>{0x01, 0xFF, 0x01}));
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_HEATING), 1);
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_ELECTRIC_BACKUP), 1);
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_HEAT_PUMP), -1) << "a cut-off slice says nothing";
  EXPECT_FALSE(store_register_reply(dev, DeviceRegister::DHW_HEATING, std::vector<uint8_t>{})) << "no byte at all";
}

TEST(ProtoDhw, EnergyIsUnsignedBigEndianWattHoursShownInKilowattHours) {
  const uint8_t value[] = {0x00, 0x1B, 0xF3, 0x42};
  EXPECT_FLOAT_EQ(*decode_register_value(DeviceRegister::DHW_ENERGY, value), 1831.746F);
  const uint8_t top[] = {0xFF, 0xFF, 0xFF, 0xFF};
  EXPECT_FLOAT_EQ(*decode_register_value(DeviceRegister::DHW_ENERGY, top), 4294967.295F);
  EXPECT_FALSE(decode_register_value(DeviceRegister::DHW_ENERGY, std::span<const uint8_t>(value, 3)).has_value());
}

TEST(ProtoDhw, ADurationIsTheFirstValueByte) {
  const uint8_t all[] = {0xFF, 0x00};
  EXPECT_FLOAT_EQ(*decode_register_value(DeviceRegister::DHW_AWAY_DURATION, all), 255.0F)
      << "255 is 'until turned off'";
  const uint8_t zero = 0;
  EXPECT_FLOAT_EQ(*decode_register_value(DeviceRegister::DHW_BOOST_DURATION, std::span<const uint8_t>(&zero, 1)), 0.0F);
  EXPECT_FALSE(
      decode_register_value(DeviceRegister::DHW_BOOST_DURATION, std::span<const uint8_t>(&zero, 0)).has_value());
}

TEST(ProtoDhw, TheBrandIsTheLetterItsByteSays) {
  const uint8_t letter = 'A';
  EXPECT_FLOAT_EQ(*decode_register_value(DeviceRegister::DHW_BRAND, std::span<const uint8_t>(&letter, 1)), 65.0F);
  EXPECT_FALSE(decode_register_value(DeviceRegister::DHW_BRAND, std::span<const uint8_t>(&letter, 0)).has_value());
}

// === Target from the status reply ===

namespace {

/// What a status reply's main parameter reads as on a water heater's range: the status setpoint reading.
std::optional<float> target_of(const IoFrame &reply) {
  const IoDevice dev = water_heater_record();
  const HeatingCardRange range = device_temperature_range(dev);
  if (reply.cmd != CMD_PRIVATE_RESP || reply.data_len < STATUS_MP_FP_MAIN_PARAMETER_OFFSET + STATUS_MAIN_PARAMETER_SIZE)
    return std::nullopt;
  const std::span<const uint8_t> main_parameter(reply.data + STATUS_MP_FP_MAIN_PARAMETER_OFFSET,
                                                STATUS_MAIN_PARAMETER_SIZE);
  return decode_status_reading(DeviceReading::STATUS_SETPOINT, main_parameter, 0, range.min_c, range.max_c);
}

}  // namespace

TEST(ProtoDhw, TargetDecodesTheThreeMainParametersSeen) {
  const auto decode = [](uint8_t high, uint8_t low) {
    return target_of(reply_with({0x2D, 0x00, high, low, 0x00, 0x00}, CMD_PRIVATE_RESP));
  };
  EXPECT_FLOAT_EQ(*decode(0xA1, 0xE8), 54.0F);
  EXPECT_FLOAT_EQ(*decode(0xC8, 0x00), 62.0F);
  EXPECT_FLOAT_EQ(*decode(0xBE, 0x7A), 60.0F) << "59.9998 unrounded";
  EXPECT_FLOAT_EQ(*decode(0x00, 0x00), 20.0F);
}

TEST(ProtoDhw, AMainParameterBeyondTheScaleIsNotATarget) {
  const auto decode = [](uint8_t high, uint8_t low) {
    return target_of(reply_with({0x2D, 0x00, high, low, 0x00, 0x00}, CMD_PRIVATE_RESP));
  };
  EXPECT_FALSE(decode(0xC8, 0x01).has_value());
  EXPECT_FALSE(decode(0xD8, 0x12).has_value()) << "an alias";
  EXPECT_FALSE(decode(0xD4, 0x00).has_value()) << "unknown position";
}

TEST(ProtoDhw, EveryEncodedTargetDecodesBackWithinATenth) {
  const HeatingCardRange range = device_temperature_range(water_heater_record());
  for (float t = DHW_TARGET_MIN_C; t <= DHW_TARGET_MAX_C; t += 0.5F) {
    uint16_t mp = 0;
    ASSERT_EQ(encode_temperature_target(range, t, mp), HeatingRefusal::NONE) << t;
    const uint8_t bytes[] = {static_cast<uint8_t>(mp >> 8), static_cast<uint8_t>(mp & 0xFF)};
    const auto back = decode_status_reading(DeviceReading::STATUS_SETPOINT, bytes, 0, range.min_c, range.max_c);
    ASSERT_TRUE(back.has_value()) << t;
    EXPECT_NEAR(*back, t, 0.05F) << t;
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

/// The record after the status decoder has stored a reply's functional parameter readings.
IoDevice stored_from(const IoFrame &reply) {
  IoDevice dev = water_heater_record();
  store_status_parameter_readings(dev, reply);
  return dev;
}

}  // namespace

TEST(ProtoDhw, OperatingStatusReadsCapabilitiesAndTheTwoFields) {
  IoDevice dev = stored_from(operating_reply(DHW_OPERATING_MODE_OFF));
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_CAPABILITIES), DHW_CAPABILITY_AWAY | DHW_CAPABILITY_BOOST);
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_BOOST), 0);
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_AWAY), 0);

  dev = stored_from(operating_reply(DHW_OPERATING_MODE_BOOST_ON));
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_BOOST), 1);
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_AWAY), 0);

  dev = stored_from(operating_reply(DHW_OPERATING_MODE_AWAY_ON));
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_BOOST), 0);
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_AWAY), 1);
}

TEST(ProtoDhw, EachFeaturesOnWordSetsItsOwnFieldAndEndsTheOther) {
  // The on words and the fields they are read from must agree: a word sent to start a feature reads back as
  // that feature on and the other off.
  for (const DhwOperatingFeature which : {DhwOperatingFeature::BOOST, DhwOperatingFeature::AWAY}) {
    const IoDevice dev = stored_from(operating_reply(dhw_operating_feature_wire(which).on_word));
    EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_BOOST), which == DhwOperatingFeature::BOOST ? 1 : 0);
    EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_AWAY), which == DhwOperatingFeature::AWAY ? 1 : 0);
  }
  EXPECT_EQ(dhw_operating_feature_wire(DhwOperatingFeature::BOOST).capability, DHW_CAPABILITY_BOOST);
  EXPECT_EQ(dhw_operating_feature_wire(DhwOperatingFeature::AWAY).capability, DHW_CAPABILITY_AWAY);
  EXPECT_EQ(dhw_operating_feature_wire(DhwOperatingFeature::BOOST).fp16_shift, DHW_FP16_BOOST_SHIFT);
  EXPECT_EQ(dhw_operating_feature_wire(DhwOperatingFeature::AWAY).fp16_shift, DHW_FP16_AWAY_SHIFT);
}

TEST(ProtoDhw, OnlyAFieldValueOfOneIsOn) {
  // 0 keep, 2 off and 3 are not "on" in either field; bit 14 and the low byte do not matter.
  IoDevice dev = stored_from(operating_reply(0x4000));
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_BOOST), 0);
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_AWAY), 0);
  dev = stored_from(operating_reply(0x4F00));
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_BOOST), 0) << "field 3";
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_AWAY), 0) << "field 3";
  dev = stored_from(operating_reply(0x45FF));
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_BOOST), 1);
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_AWAY), 1)
      << "the word can say both; the decoder reports what it says";
}

TEST(ProtoDhw, OperatingStatusDecodesWhateverTheMainParameter) {
  // The main parameter is the setpoint reading's; an alias there does not fail the functional parameters.
  const IoDevice dev = stored_from(operating_reply(DHW_OPERATING_MODE_AWAY_ON, 0xD812));
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_AWAY), 1);
}

TEST(ProtoDhw, OperatingStatusSkipsOtherParametersOfTheEcho) {
  // The heater echoes FP10 when asked for 0x43: FPI2 43, values FP10, FP15, FP16 (issue #198).
  const IoDevice dev = stored_from(
      reply_with({0x05, 0x00, 0xBE, 0x7A, 0x00, 0x43, 0x00, 0x00, 0x00, 0x0C, 0x4A, 0x00}, CMD_PRIVATE_RESP));
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_CAPABILITIES), 0x000C);
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_BOOST), 0);
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_AWAY), 0);
}

TEST(ProtoDhw, AFunctionalParameterIsReadFromTheEchoItAnnounces) {
  uint16_t word = 0xFFFF;
  EXPECT_TRUE(decode_status_functional_parameter(operating_reply(0x4A00), DHW_FP_CAPABILITIES, word));
  EXPECT_EQ(word, 0x000C);
  EXPECT_TRUE(decode_status_functional_parameter(operating_reply(0x4A00), DHW_FP_OPERATING_MODE, word));
  EXPECT_EQ(word, 0x4A00);
  word = 0x1234;
  EXPECT_FALSE(decode_status_functional_parameter(operating_reply(0x4A00), 10, word)) << "FP10 is not in the echo 03";
  EXPECT_FALSE(decode_status_functional_parameter(operating_reply(0x4A00), 8, word)) << "an FPI1 parameter";
  EXPECT_FALSE(decode_status_functional_parameter(operating_reply(0x4A00), 17, word)) << "beyond FP16";
  EXPECT_EQ(word, 0x1234) << "untouched on failure";
}

TEST(ProtoDhw, OperatingStatusRefusesWhatItCannotRead) {
  const auto fp15_stored = [](const IoFrame &reply) {
    return reading_wire(stored_from(reply), DeviceReading::HOT_WATER_CAPABILITIES);
  };
  const auto fp16_stored = [](const IoFrame &reply) {
    return reading_wire(stored_from(reply), DeviceReading::HOT_WATER_BOOST);
  };
  const IoFrame good = operating_reply(DHW_OPERATING_MODE_OFF);
  ASSERT_EQ(fp15_stored(good), 0x000C);
  EXPECT_EQ(fp15_stored(reply_with({0x05, 0x00, 0xA1, 0xE8, 0x00, 0x00}, CMD_PRIVATE_RESP)), -1)
      << "the plain FP1-FP7 read has no FP15 or FP16";
  // Each parameter is stored on its own: a reply that carries one of them gives that one and not the other.
  const IoFrame fp16_only = reply_with({0x05, 0x00, 0xA1, 0xE8, 0x00, 0x01, 0x4A, 0x00}, CMD_PRIVATE_RESP);
  EXPECT_EQ(fp16_stored(fp16_only), 0);
  EXPECT_EQ(fp15_stored(fp16_only), -1);
  const IoFrame fp15_only = reply_with({0x05, 0x00, 0xA1, 0xE8, 0x00, 0x02, 0x00, 0x0C}, CMD_PRIVATE_RESP);
  EXPECT_EQ(fp15_stored(fp15_only), 0x000C);
  EXPECT_EQ(fp16_stored(fp15_only), -1);
  EXPECT_EQ(fp15_stored(reply_with({0x05, 0x00, 0xA1, 0xE8, 0x01, 0x03, 0x00, 0x0C, 0x4A, 0x00}, CMD_PRIVATE_RESP)), -1)
      << "an FPI1 echo shifts the values";
  IoFrame short_reply = good;
  short_reply.data_len -= 1;
  EXPECT_EQ(fp15_stored(short_reply), -1) << "FP16 cut short fails the whole decode";
  IoFrame wrong_command = good;
  wrong_command.cmd = CMD_WRITE_PRIVATE_ACK;
  EXPECT_EQ(fp15_stored(wrong_command), -1);
}

TEST(ProtoDhw, ASecondReplyWithoutTheParametersKeepsWhatWasStored) {
  IoDevice dev = stored_from(operating_reply(DHW_OPERATING_MODE_BOOST_ON));
  EXPECT_EQ(store_status_parameter_readings(dev, reply_with({0x05, 0x00, 0xA1, 0xE8, 0x00, 0x00}, CMD_PRIVATE_RESP)),
            0u);
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_BOOST), 1);
  EXPECT_EQ(reading_wire(dev, DeviceReading::HOT_WATER_CAPABILITIES), 0x000C);
}

TEST(DhwHeatingSource, NamesWhatHeatsFromTheTwoReadings) {
  EXPECT_STREQ(dhw_heating_source_name(1, 0), "heat pump");
  EXPECT_STREQ(dhw_heating_source_name(0, 1), "electric backup");
  EXPECT_STREQ(dhw_heating_source_name(1, 1), "heat pump and electric backup");
  EXPECT_STREQ(dhw_heating_source_name(0, 0), "idle");
}

TEST(DhwHeatingSource, HasNoNameUntilBothReadingsArrived) {
  EXPECT_EQ(dhw_heating_source_name(-1, -1), nullptr);
  EXPECT_EQ(dhw_heating_source_name(1, -1), nullptr);
  EXPECT_EQ(dhw_heating_source_name(-1, 0), nullptr);
}

// === Who takes a register ===

TEST(ProtoDhw, AFamilyZeroRegisterGoesToAWaterHeaterOnlyAndAHeaterRegisterToAHeaterOnly) {
  const IoDevice water_heater = water_heater_record();
  IoDevice radiator;
  radiator.type = DeviceType::ELECTRICAL_HEATER;
  radiator.subtype = 1;
  radiator.features = device_features(DeviceType::ELECTRICAL_HEATER, 1, DeviceModel::NONE);
  IoDevice cover;
  cover.type = DeviceType::ROLLER_SHUTTER;
  cover.features = device_features(DeviceType::ROLLER_SHUTTER, 0, DeviceModel::NONE);

  EXPECT_TRUE(device_takes_register(water_heater, DeviceRegister::DHW_MODE));
  EXPECT_FALSE(device_takes_register(radiator, DeviceRegister::DHW_MODE));
  EXPECT_FALSE(device_takes_register(cover, DeviceRegister::DHW_MODE));
  EXPECT_TRUE(device_takes_register(radiator, DeviceRegister::SET_MODE));
  EXPECT_FALSE(device_takes_register(water_heater, DeviceRegister::SET_MODE))
      << "a water heater has no heater registers";
  EXPECT_FALSE(device_takes_register(cover, DeviceRegister::SET_MODE));
  EXPECT_STREQ(device_register_owner(DeviceRegister::DHW_MODE), "water heater");
  EXPECT_STREQ(device_register_owner(DeviceRegister::SET_MODE), "heater with private registers");
}
