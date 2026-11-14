/// @file platform_device_name_text_sensor_test.cpp
/// @brief Tests for the generated device-name diagnostic text sensor.

#include "platform_companion_sensors.h"

#include "hub_port_doubles.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace esphome::home_io_control;

TEST(PlatformDeviceNameTextSensor, SetupPublishesCachedNameAndSchedulesFetch) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  std::strcpy(device->name, "Patio Awning");

  IOHomeDeviceNameTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_EQ(sensor.state, "Patio Awning");
  EXPECT_EQ(sensor.last_timeout_name_, "init_name");
  EXPECT_EQ(sensor.last_timeout_ms_, esphome::home_io_control::INITIAL_STATUS_REQUEST_DELAY_MS);
}

TEST(PlatformDeviceNameTextSensor, InitialFetchTimeoutQueuesNameRequest) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");

  IOHomeDeviceNameTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  ASSERT_TRUE(static_cast<bool>(sensor.last_timeout_callback_));
  sensor.last_timeout_callback_();
  ASSERT_EQ(hub.queued_operations().size(), 1u);
  EXPECT_EQ(hub.queued_operations().back().type, PendingOperationType::REQUEST_NAME);
  EXPECT_EQ(hub.queued_operations().back().device_id, "ABC123");
}

TEST(PlatformDeviceNameTextSensor, DeviceUpdatePublishesLatestName) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");

  IOHomeDeviceNameTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  std::strcpy(device->name, "Dexxo");
  hub.notify_device_update("ABC123");

  EXPECT_EQ(sensor.state, "Dexxo");
}