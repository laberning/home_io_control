/// @file corpus_dhw_test.cpp
/// @brief The domestic hot water registers (proto_heating.h), readings (proto_device_readings.h) and operating-mode
/// word (proto_dhw.h) against real replies from an Atlantic Calypso Connecte heat-pump water heater (issue #198).
///
/// Only the replies were logged, so these captures pin the decoders, not the requests or the exchange. The
/// reporter read the display in the same session, which is what each expectation below compares with.

#include "corpus_generated.h"
#include "corpus_test_helpers.h"
#include "proto_codecs.h"
#include "proto_device_readings.h"
#include "proto_device_types.h"
#include "proto_dhw.h"
#include "proto_frame.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"
#include "reading_store.h"
#include "test_helpers.h"
#include "water_heater_values.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace esphome::home_io_control;

namespace {

/// A water heater record that has read one captured register reply, as the hub's poll stores it.
template<size_t N> IoDevice stored_from(const uint8_t (&bytes)[N], DeviceRegister fn) {
  IoDevice dev;
  const IoFrame frame = corpus_test::frame_of(bytes);
  const auto value = register_reply_value(frame, fn);
  EXPECT_TRUE(value.has_value()) << device_register_name(fn);
  if (value) {
    EXPECT_TRUE(store_register_reply(dev, fn, *value)) << device_register_name(fn);
  }
  return dev;
}

/// The value of a register's own reading, or NaN.
template<size_t N> float number_of(const uint8_t (&bytes)[N], DeviceRegister fn) {
  return test::number_read(stored_from(bytes, fn), fn);
}

template<size_t N> int wire_of(const uint8_t (&bytes)[N], DeviceRegister fn) {
  return test::wire_read(stored_from(bytes, fn), fn);
}

/// A water heater record that has read one captured status reply, as the hub's status decoder stores it.
template<size_t N> IoDevice status_stored_from(const uint8_t (&bytes)[N]) {
  IoDevice dev = test::water_heater_record();
  test::apply_status_reply(dev, corpus_test::frame_of(bytes));
  return dev;
}

template<size_t N> float target_of(const uint8_t (&bytes)[N]) {
  return test::number_read(status_stored_from(bytes), DeviceReading::HOT_WATER_TARGET);
}

}  // namespace

TEST(CorpusDhw, WaterTemperatureIsTheFirstTwoValueBytes) {
  EXPECT_FLOAT_EQ(
      number_of(corpus::atlantic_calypso_exchange_register_read_frame0_bytes, DeviceRegister::DHW_WATER_TEMPERATURE),
      49.6F);
  EXPECT_FLOAT_EQ(
      number_of(corpus::atlantic_calypso_exchange_register_read_frame12_bytes, DeviceRegister::DHW_WATER_TEMPERATURE),
      58.1F);
}

TEST(CorpusDhw, EachDisplayedModeIsItsRegisterValue) {
  EXPECT_EQ(wire_of(corpus::atlantic_calypso_exchange_register_read_frame1_bytes, DeviceRegister::DHW_MODE),
            static_cast<int>(DhwMode::AUTO));
  EXPECT_EQ(wire_of(corpus::atlantic_calypso_exchange_register_read_frame2_bytes, DeviceRegister::DHW_MODE),
            static_cast<int>(DhwMode::MANUAL_ECO_ON))
      << "manual, ECO on";
  EXPECT_EQ(wire_of(corpus::atlantic_calypso_exchange_register_read_frame3_bytes, DeviceRegister::DHW_MODE),
            static_cast<int>(DhwMode::MANUAL_ECO_OFF))
      << "manual, ECO off";
}

TEST(CorpusDhw, HeatingStateTellsIdleHeatPumpAndBoostApart) {
  const IoDevice idle =
      stored_from(corpus::atlantic_calypso_exchange_register_read_frame4_bytes, DeviceRegister::DHW_HEATING);
  EXPECT_EQ(reading_wire(idle, DeviceReading::HOT_WATER_HEATING), 0);
  EXPECT_EQ(reading_wire(idle, DeviceReading::HOT_WATER_HEAT_PUMP), 0);
  EXPECT_EQ(reading_wire(idle, DeviceReading::HOT_WATER_ELECTRIC_BACKUP), 0);
  const IoDevice heat_pump =
      stored_from(corpus::atlantic_calypso_exchange_register_read_frame5_bytes, DeviceRegister::DHW_HEATING);
  EXPECT_EQ(reading_wire(heat_pump, DeviceReading::HOT_WATER_HEATING), 1);
  EXPECT_EQ(reading_wire(heat_pump, DeviceReading::HOT_WATER_HEAT_PUMP), 1);
  EXPECT_EQ(reading_wire(heat_pump, DeviceReading::HOT_WATER_ELECTRIC_BACKUP), 0);
  const IoDevice boost =
      stored_from(corpus::atlantic_calypso_exchange_register_read_frame6_bytes, DeviceRegister::DHW_HEATING);
  EXPECT_EQ(reading_wire(boost, DeviceReading::HOT_WATER_HEATING), 1);
  EXPECT_EQ(reading_wire(boost, DeviceReading::HOT_WATER_HEAT_PUMP), 1);
  EXPECT_EQ(reading_wire(boost, DeviceReading::HOT_WATER_ELECTRIC_BACKUP), 1);
}

TEST(CorpusDhw, EnergyIsTheDisplaysSinceStartTotalAndRisesWhileHeating) {
  const float before_kwh =
      number_of(corpus::atlantic_calypso_exchange_register_read_frame7_bytes, DeviceRegister::DHW_ENERGY);
  const float after_kwh =
      number_of(corpus::atlantic_calypso_exchange_register_read_frame8_bytes, DeviceRegister::DHW_ENERGY);
  EXPECT_FLOAT_EQ(before_kwh, 1831.746F) << "display: 1 831 kWh since start";
  EXPECT_FLOAT_EQ(after_kwh, 1831.831F) << "85 Wh more while the heat pump ran";
}

TEST(CorpusDhw, TheBoostDurationReadsOneDuringABoostAndZeroAfter) {
  EXPECT_EQ(wire_of(corpus::atlantic_calypso_exchange_register_read_frame9_bytes, DeviceRegister::DHW_BOOST_DURATION),
            DHW_BOOST_START_DURATION)
      << "the value a boost start writes is the one a display-started boost holds";
  EXPECT_EQ(wire_of(corpus::atlantic_calypso_exchange_register_read_frame10_bytes, DeviceRegister::DHW_BOOST_DURATION),
            0);
}

TEST(CorpusDhw, TheHolidayDurationCountsTheDaysLeftAndFfHasNoEnd) {
  EXPECT_EQ(wire_of(corpus::atlantic_calypso_exchange_register_read_frame13_bytes, DeviceRegister::DHW_AWAY_DURATION),
            0)
      << "a holiday scheduled for later has not started";
  EXPECT_EQ(wire_of(corpus::atlantic_calypso_exchange_register_read_frame14_bytes, DeviceRegister::DHW_AWAY_DURATION),
            2)
      << "two days before the end";
  EXPECT_EQ(wire_of(corpus::atlantic_calypso_exchange_register_read_frame16_bytes, DeviceRegister::DHW_AWAY_DURATION),
            4)
      << "four days before the end";
  EXPECT_EQ(wire_of(corpus::atlantic_calypso_exchange_register_read_frame15_bytes, DeviceRegister::DHW_AWAY_DURATION),
            DHW_AWAY_UNTIL_TURNED_OFF)
      << "the value an away start writes is the one the heater holds for a holiday with no end date";
}

TEST(CorpusDhw, BrandIsOneLetter) {
  EXPECT_EQ(wire_of(corpus::atlantic_calypso_exchange_register_read_frame11_bytes, DeviceRegister::DHW_BRAND), 'A');
}

TEST(CorpusDhw, StatusMainParameterIsTheDisplayedTarget) {
  EXPECT_FLOAT_EQ(target_of(corpus::atlantic_calypso_probe_status_mp_fp_frame0_bytes), 54.0F) << "manual, ECO on";
  EXPECT_FLOAT_EQ(target_of(corpus::atlantic_calypso_probe_status_mp_fp_frame1_bytes), 62.0F) << "manual, ECO off";
  EXPECT_FLOAT_EQ(target_of(corpus::atlantic_calypso_probe_status_mp_fp_frame2_bytes), 60.0F) << "AUTO";
}

TEST(CorpusDhw, DuringAHolidayTheMainParameterIsTheBottomOfTheScale) {
  // 00 00 decodes as the bottom of the range; it is no target, which is why the entity hides it while away.
  EXPECT_FLOAT_EQ(target_of(corpus::atlantic_calypso_probe_status_mp_fp_frame3_bytes), DHW_TARGET_MIN_C);
}

TEST(CorpusDhw, TheOperatingStatusReadReportsCapabilitiesAndBothFieldsOffAtRest) {
  const struct {
    IoDevice stored;
    float target_c;
    const char *display;
  } cases[] = {
      {status_stored_from(corpus::atlantic_calypso_probe_status_fp_high_frame0_bytes), 60.0F, "AUTO"},
      {status_stored_from(corpus::atlantic_calypso_probe_status_fp_high_frame1_bytes), 54.0F, "manual, ECO on"},
      {status_stored_from(corpus::atlantic_calypso_probe_status_fp_high_frame2_bytes), 62.0F, "manual, ECO off"},
  };
  for (const auto &c : cases) {
    EXPECT_FLOAT_EQ(test::number_read(c.stored, DeviceReading::HOT_WATER_TARGET), c.target_c)
        << "the main parameter stays in place: " << c.display;
    EXPECT_EQ(reading_wire(c.stored, DeviceReading::HOT_WATER_CAPABILITIES), DHW_CAPABILITY_AWAY | DHW_CAPABILITY_BOOST)
        << "FP15 000C: " << c.display;
    EXPECT_EQ(reading_wire(c.stored, DeviceReading::HOT_WATER_BOOST), 0)
        << "FP16 4A00 reads both fields off: " << c.display;
    EXPECT_EQ(reading_wire(c.stored, DeviceReading::HOT_WATER_AWAY), 0) << c.display;
  }
}

TEST(CorpusDhw, TheOffWordIsTheFp16TheHeaterReportsAtRest) {
  const auto &bytes = corpus::atlantic_calypso_probe_status_fp_high_frame0_bytes;
  EXPECT_EQ((bytes[19] << 8) | bytes[20], DHW_OPERATING_MODE_OFF);
  uint16_t fp16 = 0;
  ASSERT_TRUE(decode_status_functional_parameter(corpus_test::frame_of(bytes), DHW_FP_OPERATING_MODE, fp16));
  EXPECT_EQ(fp16, DHW_OPERATING_MODE_OFF);
}

TEST(CorpusDhw, TargetEncoderReproducesTheCapturedMainParameters) {
  const HeatingCardRange range{DHW_TARGET_MIN_C, DHW_TARGET_MAX_C};
  uint16_t mp = 0;
  ASSERT_EQ(encode_temperature_target(range, 54.0F, mp), HeatingRefusal::NONE);
  EXPECT_EQ(mp, (corpus::atlantic_calypso_probe_status_mp_fp_frame0_bytes[11] << 8) |
                    corpus::atlantic_calypso_probe_status_mp_fp_frame0_bytes[12]);
  ASSERT_EQ(encode_temperature_target(range, 62.0F, mp), HeatingRefusal::NONE);
  EXPECT_EQ(mp, (corpus::atlantic_calypso_probe_status_mp_fp_frame1_bytes[11] << 8) |
                    corpus::atlantic_calypso_probe_status_mp_fp_frame1_bytes[12]);
}

// === A Sauter Guelma connecte, an electric storage water heater (pull request #248) ===

namespace {

/// The payload a register write encodes to, from its value.
std::vector<uint8_t> encoded_write(DeviceRegister fn, float value) {
  uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE] = {};
  return std::vector<uint8_t>(out, out + encode_heating_write(fn, value, out));
}

/// The four header bytes and the value of a captured acknowledgement: `0C 61 00 <register> <value>`.
template<size_t N> std::vector<uint8_t> acknowledged_payload(const uint8_t (&bytes)[N]) {
  const IoFrame frame = corpus_test::frame_of(bytes);
  return std::vector<uint8_t>(frame.data, frame.data + frame.data_len);
}

}  // namespace

TEST(CorpusGuelma, ItsWaterTemperatureModeAndEnergyAreTheCalypsosRegisters) {
  EXPECT_FLOAT_EQ(
      number_of(corpus::sauter_guelma_exchange_register_read_frame0_bytes, DeviceRegister::DHW_WATER_TEMPERATURE),
      39.8F)
      << "two value bytes only, where the Calypso sends six";
  EXPECT_EQ(wire_of(corpus::sauter_guelma_exchange_register_read_frame1_bytes, DeviceRegister::DHW_MODE),
            static_cast<int>(DhwMode::AUTO));
  EXPECT_EQ(wire_of(corpus::sauter_guelma_exchange_register_read_frame2_bytes, DeviceRegister::DHW_MODE),
            static_cast<int>(DhwMode::MANUAL_ECO_OFF))
      << "manual: the element heats";
  EXPECT_NEAR(number_of(corpus::sauter_guelma_exchange_register_read_frame24_bytes, DeviceRegister::DHW_ENERGY),
              15373.428F, 0.01F);
  EXPECT_STREQ(dhw_electric_mode_name(0), "auto");
  EXPECT_STREQ(dhw_electric_mode_name(2), "manual electric");
  EXPECT_EQ(dhw_electric_mode_name(1), nullptr) << "it has no heat pump";
}

TEST(CorpusGuelma, BoostAndAbsenceAreFlagRegisters) {
  EXPECT_EQ(wire_of(corpus::sauter_guelma_exchange_register_read_frame7_bytes, DeviceRegister::DHW_BOOST_FLAG), 0);
  EXPECT_EQ(wire_of(corpus::sauter_guelma_exchange_register_read_frame8_bytes, DeviceRegister::DHW_BOOST_FLAG), 1);
  EXPECT_EQ(wire_of(corpus::sauter_guelma_exchange_register_read_frame9_bytes, DeviceRegister::DHW_AWAY_FLAG), 0);
  EXPECT_EQ(wire_of(corpus::sauter_guelma_exchange_register_read_frame10_bytes, DeviceRegister::DHW_AWAY_FLAG), 1);
}

TEST(CorpusGuelma, TheTargetIsTheTemperatureItHeatsTo) {
  const struct {
    float target_c;
    const char *display;
  } expected[] = {{42.0F, "AUTO with one shower"},
                  {50.0F, "manual with two showers"},
                  {70.0F, "manual with four showers"},
                  {7.0F, "absence"}};
  const float read[] = {
      number_of(corpus::sauter_guelma_exchange_register_read_frame11_bytes, DeviceRegister::DHW_ACTIVE_TARGET),
      number_of(corpus::sauter_guelma_exchange_register_read_frame12_bytes, DeviceRegister::DHW_ACTIVE_TARGET),
      number_of(corpus::sauter_guelma_exchange_register_read_frame13_bytes, DeviceRegister::DHW_ACTIVE_TARGET),
      number_of(corpus::sauter_guelma_exchange_register_read_frame14_bytes, DeviceRegister::DHW_ACTIVE_TARGET)};
  for (size_t i = 0; i < std::size(expected); i++)
    EXPECT_FLOAT_EQ(read[i], expected[i].target_c) << expected[i].display;
}

TEST(CorpusGuelma, ItsShowersAndHeatingPowerAreNumbers) {
  EXPECT_EQ(wire_of(corpus::sauter_guelma_exchange_register_read_frame15_bytes, DeviceRegister::DHW_TARGET_SHOWERS), 2);
  EXPECT_EQ(wire_of(corpus::sauter_guelma_exchange_register_read_frame16_bytes, DeviceRegister::DHW_TARGET_SHOWERS), 0)
      << "absence";
  EXPECT_EQ(wire_of(corpus::sauter_guelma_exchange_register_read_frame17_bytes, DeviceRegister::DHW_SHOWERS_AVAILABLE),
            1);
  EXPECT_EQ(wire_of(corpus::sauter_guelma_exchange_register_read_frame18_bytes, DeviceRegister::DHW_SHOWERS_AVAILABLE),
            2);
  EXPECT_FLOAT_EQ(
      number_of(corpus::sauter_guelma_exchange_register_read_frame21_bytes, DeviceRegister::DHW_HEATING_POWER), 0.0F);
  EXPECT_FLOAT_EQ(
      number_of(corpus::sauter_guelma_exchange_register_read_frame22_bytes, DeviceRegister::DHW_HEATING_POWER),
      1000.0F);
  EXPECT_FLOAT_EQ(
      number_of(corpus::sauter_guelma_exchange_register_read_frame23_bytes, DeviceRegister::DHW_HEATING_POWER),
      1500.0F);
}

TEST(CorpusGuelma, EachWriteTheHeaterAcknowledgedIsTheWriteTheHubEncodes) {
  EXPECT_EQ(encoded_write(DeviceRegister::DHW_MODE, static_cast<float>(DhwMode::MANUAL_ECO_OFF)),
            acknowledged_payload(corpus::sauter_guelma_exchange_register_write_frame0_bytes));
  EXPECT_EQ(encoded_write(DeviceRegister::DHW_BOOST_FLAG, 1.0F),
            acknowledged_payload(corpus::sauter_guelma_exchange_register_write_frame1_bytes));
  EXPECT_EQ(encoded_write(DeviceRegister::DHW_BOOST_FLAG, 0.0F),
            acknowledged_payload(corpus::sauter_guelma_exchange_register_write_frame2_bytes));
  EXPECT_EQ(encoded_write(DeviceRegister::DHW_AWAY_FLAG, 1.0F),
            acknowledged_payload(corpus::sauter_guelma_exchange_register_write_frame3_bytes));
  EXPECT_EQ(encoded_write(DeviceRegister::DHW_AWAY_FLAG, 0.0F),
            acknowledged_payload(corpus::sauter_guelma_exchange_register_write_frame4_bytes));
  EXPECT_EQ(encoded_write(DeviceRegister::DHW_MANUAL_SHOWERS, 60.0F),
            acknowledged_payload(corpus::sauter_guelma_exchange_register_write_frame5_bytes))
      << "60 C is three showers";
}

TEST(CorpusGuelma, ItsStatusReplyCarriesNothingToRead) {
  const IoFrame frame = corpus_test::frame_of(corpus::sauter_guelma_probe_status_fp_high_frame0_bytes);
  uint16_t fp15 = 0;
  uint16_t fp16 = 0;
  ASSERT_TRUE(decode_status_functional_parameter(frame, DHW_FP_CAPABILITIES, fp15));
  ASSERT_TRUE(decode_status_functional_parameter(frame, DHW_FP_OPERATING_MODE, fp16));
  EXPECT_EQ(fp15, 0xFFFF);
  EXPECT_EQ(fp16, 0xFFFF);
  IoDevice dev;
  dev.type = DeviceType::DOMESTIC_HOT_WATER;
  dev.model = DeviceModel::ELECTRIC_WATER_HEATER;
  dev.features = device_features(dev.type, 0, dev.model);
  EXPECT_FALSE(device_has_status_reading(dev)) << "so its poll sends no status read";
}
