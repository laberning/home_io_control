/// @file platform_last_command_text_sensors_test.cpp
/// @brief Tests for the generated "Last Commanded By" / "Last Command Source" diagnostic text
/// sensors.

#include "platform_companion_sensors.h"

#include "hub_core.h"
#include "hub_internal.h"
#include "proto_constants.h"
#include "test_helpers.h"

using namespace esphome::home_io_control;
using test::TestableHubComponent;

TEST(PlatformLastCommandTextSensors, SetupWithNoRecordPublishesEmptyString) {
  TestableHubComponent hub;
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
  TestableHubComponent hub;
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
  TestableHubComponent hub;
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
  hub.notify_device_update_("ABC123");

  EXPECT_EQ(by_sensor.state, "2FE2D2") << "a device update should publish the newly decoded commander";
  EXPECT_EQ(source_sensor.state, "local_user(0x00)") << "a device update should publish the newly decoded originator";
}

TEST(PlatformLastCommandTextSensors, DeviceUpdateForOtherDeviceIsIgnored) {
  TestableHubComponent hub;
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

  hub.notify_device_update_("DEF456");

  EXPECT_EQ(by_sensor.state, "") << "updates for a different device id must not change this sensor's published state";
}

TEST(PlatformLastCommandTextSensors, ThisHubQualifierUsesTheHubsConfiguredNodeId) {
  TestableHubComponent hub;
  hub.node_id_[0] = 0xC0;
  hub.node_id_[1] = 0xFF;
  hub.node_id_[2] = 0xEE;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  device->last_commander[0] = 0xC0;
  device->last_commander[1] = 0xFF;
  device->last_commander[2] = 0xEE;
  device->last_command_originator = 0x01;
  device->has_last_command = true;

  IOHomeLastCommandedByTextSensor by_sensor;
  by_sensor.set_parent(&hub);
  by_sensor.set_device_id("ABC123");
  by_sensor.setup();

  EXPECT_EQ(by_sensor.state, "C0FFEE (this hub)")
      << "the qualifier must resolve against this hub's own configured node_id_, not a hardcoded value";
}
