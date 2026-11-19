/// @file heater_status_decoding_test.cpp
/// @brief HeaterController: what the status answer of a level, temperature, register or water heater stores
/// (HeaterControllerFixture).

#include "heater_controller.h"
#include "payload_frame.h"
#include "reading_store.h"
#include "water_heater_values.h"
#include "decisions_status.h"
#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_device_readings.h"
#include "proto_dhw.h"
#include "proto_frame.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"
#include "proto_private_registers.h"

#include "heater_controller_fixture.h"
#include "heater_controller_helpers.h"

#include <array>
#include <cmath>
#include <functional>
#include <vector>

using namespace esphome::home_io_control;
using namespace heater_controller_test;

// === Decoding a status answer ===

TEST(HeaterStatusDecoding, ALevelHeaterKeepsTheLevelTheReplyCarries) {
  IoDevice dev;
  test::set_kind(dev, DeviceType::ELECTRICAL_HEATER, 0);

  const auto bytes = test::main_parameter_bytes(heating_level_main_parameter(heating_level::ECO));
  apply_heating_main_parameter("ABC123", dev, bytes);

  EXPECT_EQ(test::wire_read(dev, DeviceReading::HEATING_LEVEL), heating_level::ECO);
  EXPECT_TRUE(std::isnan(test::number_read(dev, DeviceReading::STATUS_SETPOINT))) << "a level is no setpoint";
}

TEST(HeaterStatusDecoding, APilotWireInterfaceTakesNoLevelFromItsStatus) {
  // The interface's status carries no level (issue #228: the one answered read had 0xC800), so even a main parameter
  // that decodes as one is not kept: neither the card nor a sensor shows it.
  IoDevice dev;
  test::set_kind(dev, DeviceType::ELECTRICAL_HEATER, 0, DeviceModel::PILOT_WIRE_INTERFACE);

  apply_heating_main_parameter("ABC123", dev,
                               test::main_parameter_bytes(heating_level_main_parameter(heating_level::ECO)));
  apply_heating_main_parameter("ABC123", dev, test::main_parameter_bytes(0xC800));

  EXPECT_EQ(test::wire_read(dev, DeviceReading::HEATING_LEVEL), -1);
  EXPECT_TRUE(std::isnan(test::number_read(dev, DeviceReading::STATUS_SETPOINT)));
}

TEST(HeaterStatusDecoding, ATemperatureHeaterKeepsTheSetpointAndClearsAStaleAlias) {
  IoDevice dev;
  test::set_kind(dev, DeviceType::HEAT_PUMP, 0);
  test::store_reading(dev, DeviceReading::HEATING_LEVEL, 3);
  uint16_t mp = 0;
  ASSERT_TRUE(encode_temperature_main_parameter(21.0F, dev.temperature_range_min_c, dev.temperature_range_max_c, mp));

  const auto bytes = test::main_parameter_bytes(mp);
  apply_heating_main_parameter("ABC123", dev, bytes);

  EXPECT_NEAR(test::number_read(dev, DeviceReading::STATUS_SETPOINT), 21.0F, 0.1F);
  EXPECT_EQ(test::wire_read(dev, DeviceReading::HEATING_LEVEL), -1) << "a plain temperature clears a stale alias";
  EXPECT_TRUE(std::isnan(test::number_read(dev, DeviceReading::SETPOINT)))
      << "the status setpoint is not the comfort-setpoint register";
}

TEST(HeaterStatusDecoding, ARegisterHeaterAndADeviceWithoutAProfileAreLeftAlone) {
  const auto bytes = test::main_parameter_bytes(0x1234);
  IoDevice radiator;
  test::set_kind(radiator, DeviceType::ELECTRICAL_HEATER, 1);
  apply_heating_main_parameter("ABC123", radiator, bytes);
  EXPECT_EQ(test::wire_read(radiator, DeviceReading::HEATING_LEVEL), -1);
  EXPECT_TRUE(std::isnan(test::number_read(radiator, DeviceReading::STATUS_SETPOINT)));

  IoDevice cover;
  test::set_kind(cover, DeviceType::ROLLER_SHUTTER, 0);
  apply_heating_main_parameter("ABC123", cover, bytes);
  EXPECT_EQ(test::wire_read(cover, DeviceReading::HEATING_LEVEL), -1);
  EXPECT_TRUE(std::isnan(test::number_read(cover, DeviceReading::STATUS_SETPOINT)));
}

TEST(HeaterStatusDecoding, AWaterHeaterStatusReplyGivesTheTargetCapabilitiesAndBoostAndAway) {
  IoDevice dev = test::water_heater_record();
  IoFrame frame{};
  const uint8_t payload[] = {0x05,
                             0x00,
                             0xA1,
                             0xE8,
                             DHW_STATUS_FPI1,
                             DHW_STATUS_FPI2,
                             0x00,
                             0x00,
                             static_cast<uint8_t>(DHW_CAPABILITY_BOOST >> 8U),
                             static_cast<uint8_t>(DHW_CAPABILITY_BOOST),
                             static_cast<uint8_t>(DHW_OPERATING_MODE_BOOST_ON >> 8U),
                             static_cast<uint8_t>(DHW_OPERATING_MODE_BOOST_ON)};
  set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload));

  apply_dhw_status_reply("ABC123", dev, frame, true);

  EXPECT_FLOAT_EQ(test::water_heater_values(dev).target_c, 54.0F);
  EXPECT_EQ(test::water_heater_values(dev).capabilities, DHW_CAPABILITY_BOOST);
  EXPECT_EQ(test::water_heater_values(dev).boost, 1);
  EXPECT_EQ(test::water_heater_values(dev).away, 0);
}

TEST(HeaterStatusDecoding, TheAckOfAnExecuteIsNotDecodedAndANonTemperatureLeavesTheTargetUnknown) {
  IoDevice dev = test::water_heater_record();
  test::seed_water_heater(dev, {.target_c = 54.0F});
  IoFrame frame{};
  const uint8_t payload[] = {0x2D, 0x00, 0xD8, 0x12, 0x00, 0x00};
  set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload));

  apply_dhw_status_reply("ABC123", dev, frame, false);
  EXPECT_FLOAT_EQ(test::water_heater_values(dev).target_c, 54.0F)
      << "an EXECUTE ack echoes the request and carries no report";

  apply_dhw_status_reply("ABC123", dev, frame, true);
  EXPECT_TRUE(std::isnan(test::water_heater_values(dev).target_c)) << "an alias is not a target temperature";
}
