/// @file platform_last_command_text_sensors_test.cpp
/// @brief Tests for the generated "Last Commanded By" / "Last Command Source" diagnostic text
/// sensors.

#include "platform_companion_sensors.h"

#include "hub_port_doubles.h"
#include "proto_constants.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

TEST(PlatformLastCommandTextSensors, SetupWithNoRecordPublishesEmptyString) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");

  IOHomeLastCommandedByTextSensor by_sensor;
  by_sensor.set_parent(&hub);
  by_sensor.set_device_id("ABC123");
  by_sensor.setup();

  IOHomeLastCommandSourceTextSensor source_sensor;
  source_sensor.set_parent(&hub);
  source_sensor.set_device_id("ABC123");
  source_sensor.setup();

  EXPECT_EQ(by_sensor.state, "") << "a device with no decoded record should publish an empty string";
  EXPECT_EQ(source_sensor.state, "") << "a device with no decoded record should publish an empty string";
}

TEST(PlatformLastCommandTextSensors, SetupWithRecordPublishesTheStoredValue) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  device->last_commander[0] = 0x3B;
  device->last_commander[1] = 0x74;
  device->last_commander[2] = 0xDC;
  device->last_command_originator = 0x01;
  device->has_last_command = true;

  IOHomeLastCommandedByTextSensor by_sensor;
  by_sensor.set_parent(&hub);
  by_sensor.set_device_id("ABC123");
  by_sensor.setup();

  IOHomeLastCommandSourceTextSensor source_sensor;
  source_sensor.set_parent(&hub);
  source_sensor.set_device_id("ABC123");
  source_sensor.setup();

  EXPECT_EQ(by_sensor.state, "3B74DC") << "a previously decoded record should publish on setup";
  EXPECT_EQ(source_sensor.state, "user_remote(0x01)") << "a previously decoded record should publish on setup";
}

TEST(PlatformLastCommandTextSensors, DeviceUpdateRepublishes) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);

  IOHomeLastCommandedByTextSensor by_sensor;
  by_sensor.set_parent(&hub);
  by_sensor.set_device_id("ABC123");
  by_sensor.setup();

  IOHomeLastCommandSourceTextSensor source_sensor;
  source_sensor.set_parent(&hub);
  source_sensor.set_device_id("ABC123");
  source_sensor.setup();
  ASSERT_EQ(by_sensor.state, "");
  ASSERT_EQ(source_sensor.state, "");

  device->last_commander[0] = 0x2F;
  device->last_commander[1] = 0xE2;
  device->last_commander[2] = 0xD2;
  device->last_command_originator = 0x00;
  device->has_last_command = true;
  hub.notify_device_update("ABC123");

  EXPECT_EQ(by_sensor.state, "2FE2D2") << "a device update should publish the newly decoded commander";
  EXPECT_EQ(source_sensor.state, "local_user(0x00)") << "a device update should publish the newly decoded originator";
}

TEST(PlatformLastCommandTextSensors, DeviceUpdateForOtherDeviceIsIgnored) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");
  hub.add_device("DEF456");
  auto *other = hub.get_device("DEF456");
  ASSERT_NE(other, nullptr);
  other->last_commander[0] = 0x3B;
  other->last_commander[1] = 0x74;
  other->last_commander[2] = 0xDC;
  other->last_command_originator = 0x01;
  other->has_last_command = true;

  IOHomeLastCommandedByTextSensor by_sensor;
  by_sensor.set_parent(&hub);
  by_sensor.set_device_id("ABC123");
  by_sensor.setup();
  ASSERT_EQ(by_sensor.state, "");

  hub.notify_device_update("DEF456");

  EXPECT_EQ(by_sensor.state, "") << "updates for a different device id must not change this sensor's published state";
}

TEST(PlatformLastCommandTextSensors, ThisHubQualifierNamesTheCommanderThePortReportsAsTheHub) {
  test::DevicePortDouble hub;
  hub.hub_node_id[0] = 0x0A;
  hub.hub_node_id[1] = 0x0B;
  hub.hub_node_id[2] = 0x0C;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  device->last_commander[0] = 0x0A;
  device->last_commander[1] = 0x0B;
  device->last_commander[2] = 0x0C;
  device->last_command_originator = 0x01;
  device->has_last_command = true;

  IOHomeLastCommandedByTextSensor by_sensor;
  by_sensor.set_parent(&hub);
  by_sensor.set_device_id("ABC123");
  by_sensor.setup();

  EXPECT_EQ(by_sensor.state, "0A0B0C (this hub)");
}
