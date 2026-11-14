/// @file platform_exchange_failures_sensor_test.cpp
/// @brief Tests for the generated Exchange Failures diagnostic sensor.

#include "platform_companion_sensors.h"

#include "hub_port_doubles.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

TEST(PlatformExchangeFailuresSensor, SetupWithNoFailuresPublishesZero) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");

  IOHomeExchangeFailuresSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_FLOAT_EQ(sensor.state, 0.0f) << "zero failures is a meaningful value and should publish immediately";
}

TEST(PlatformExchangeFailuresSensor, SetupWithRecordedFailuresPublishesCount) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  device->exchange_timeout_count = 3;

  IOHomeExchangeFailuresSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_FLOAT_EQ(sensor.state, 3.0f) << "a previously recorded count should publish on setup";
}

TEST(PlatformExchangeFailuresSensor, DeviceUpdatePublishesNewCount) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);

  IOHomeExchangeFailuresSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_FLOAT_EQ(sensor.state, 0.0f);

  device->exchange_timeout_count = 1;
  hub.notify_device_update("ABC123");

  EXPECT_FLOAT_EQ(sensor.state, 1.0f) << "a device update should publish the newly recorded failure count";
}

TEST(PlatformExchangeFailuresSensor, DeviceUpdateForOtherDeviceIsIgnored) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");
  hub.add_device("DEF456");
  auto *other = hub.get_device("DEF456");
  ASSERT_NE(other, nullptr);
  other->exchange_timeout_count = 5;

  IOHomeExchangeFailuresSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_FLOAT_EQ(sensor.state, 0.0f);

  hub.notify_device_update("DEF456");

  EXPECT_FLOAT_EQ(sensor.state, 0.0f) << "updates for a different device id must not change this sensor's state";
}

// ============================================================================
// Unconfirmed Exchanges — the companion counter for "challenged us, then went silent"
// ============================================================================

TEST(PlatformUnconfirmedExchangesSensor, SetupWithNoneRecordedPublishesZero) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");

  IOHomeUnconfirmedExchangesSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_FLOAT_EQ(sensor.state, 0.0f) << "zero is a meaningful value here and should publish immediately";
}

TEST(PlatformUnconfirmedExchangesSensor, PublishesItsOwnCounterNotTheFailureOne) {
  // The two counters answer different questions — "the device never heard us" versus "it heard us
  // and the reply was lost" — so reading the wrong field would make the new sensor useless.
  test::DevicePortDouble hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  device->exchange_timeout_count = 7;
  device->exchange_unconfirmed_count = 2;

  IOHomeUnconfirmedExchangesSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_FLOAT_EQ(sensor.state, 2.0f);
}

TEST(PlatformUnconfirmedExchangesSensor, DeviceUpdatePublishesNewCount) {
  test::DevicePortDouble hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);

  IOHomeUnconfirmedExchangesSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  device->exchange_unconfirmed_count = 1;
  hub.notify_device_update("ABC123");

  EXPECT_FLOAT_EQ(sensor.state, 1.0f);
}
