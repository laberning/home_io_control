/// @file platform_active_issue_text_sensor_test.cpp
/// @brief Tests for the generated active-issue diagnostic text sensor.

#include "platform_companion_sensors.h"

#include "hub_port_doubles.h"
#include "proto_constants.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

TEST(PlatformActiveIssueTextSensor, SetupWithNoRecordedResultPublishesEmptyString) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");

  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_EQ(sensor.state, "") << "a device with no recorded CMD_ERROR_RESP should publish an empty string";
}

TEST(PlatformActiveIssueTextSensor, SetupWithRecordedResultPublishesSymbolicName) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  device->last_result_code = RESULT_LIMITATION_BY_RAIN;

  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_EQ(sensor.state, "LIMITATION_BY_RAIN")
      << "a previously recorded result should publish its symbolic name on setup";
}

TEST(PlatformActiveIssueTextSensor, DeviceUpdatePublishesNewResultAndClears) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);

  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_EQ(sensor.state, "");

  device->last_result_code = RESULT_LIMITATION_BY_WIND;
  hub.notify_device_update("ABC123");

  EXPECT_EQ(sensor.state, "LIMITATION_BY_WIND") << "a device update should publish the newly recorded result";

  device->last_result_code = 0;
  hub.notify_device_update("ABC123");

  EXPECT_EQ(sensor.state, "") << "a device update after clearing should publish an empty string again";
}

TEST(PlatformActiveIssueTextSensor, SessionInterruptedIsNotShownAsAnIssue) {
  // A garage drive answered status reads with SESSION_INTERRUPTED right after an unconfirmed
  // command (issue #163); the code describes that exchange, not the device.
  test::DevicePortDouble hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);

  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  device->last_result_code = RESULT_SESSION_INTERRUPTED;
  hub.notify_device_update("ABC123");
  EXPECT_EQ(sensor.state, "");

  device->limited_by_rain = true;
  hub.notify_device_update("ABC123");
  EXPECT_EQ(sensor.state, "LIMITATION_BY_RAIN") << "a derived limitation still shows behind a transient result";
}

TEST(PlatformActiveIssueTextSensor, DerivedRainLimitationPublishesLimitationByRain) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);

  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_EQ(sensor.state, "");

  device->limited_by_rain = true;
  hub.notify_device_update("ABC123");
  EXPECT_EQ(sensor.state, "LIMITATION_BY_RAIN");

  device->limited_by_rain = false;
  hub.notify_device_update("ABC123");
  EXPECT_EQ(sensor.state, "") << "the derived source clears with the flag";
}

TEST(PlatformActiveIssueTextSensor, ExplicitResultWinsOverDerivedRainLimitation) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  device->limited_by_rain = true;
  device->last_result_code = RESULT_LIMITATION_BY_WIND;

  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_EQ(sensor.state, "LIMITATION_BY_WIND");
}

TEST(PlatformActiveIssueTextSensor, DeviceUpdateForOtherDeviceIsIgnored) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");
  hub.add_device("DEF456");
  auto *other = hub.get_device("DEF456");
  ASSERT_NE(other, nullptr);
  other->last_result_code = RESULT_LIMITATION_BY_RAIN;

  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_EQ(sensor.state, "");

  hub.notify_device_update("DEF456");

  EXPECT_EQ(sensor.state, "") << "updates for a different device id must not change this sensor's published state";
}

TEST(PlatformActiveIssueTextSensor, RadioThatStopsRespondingWinsOverAResult) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");
  hub.get_device("ABC123")->last_result_code = RESULT_LIMITATION_BY_RAIN;

  IOHomeActiveIssueTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_EQ(sensor.state, "LIMITATION_BY_RAIN");

  hub.radio_stopped = true;
  hub.notify_device_update("ABC123");

  EXPECT_EQ(sensor.state, ACTIVE_ISSUE_RADIO_STOPPED_RESPONDING) << "a dead radio outranks a device result";
}
