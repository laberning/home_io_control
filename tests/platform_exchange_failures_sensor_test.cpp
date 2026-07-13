/// @file platform_exchange_failures_sensor_test.cpp
/// @brief Tests for the generated Exchange Failures diagnostic sensor.

#include "platform_exchange_failures_sensor.h"

#include "hub_core.h"
#include "hub_internal.h"
#include "test_helpers.h"

using namespace esphome::home_io_control;

namespace {

class TestableExchangeFailuresHub : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::notify_device_update_;
};

}  // namespace

TEST(PlatformExchangeFailuresSensor, SetupWithNoFailuresPublishesZero) {
  TestableExchangeFailuresHub hub;
  hub.add_device("ABC123");

  IOHomeExchangeFailuresSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_FLOAT_EQ(sensor.state, 0.0f) << "zero failures is a meaningful value and should publish immediately";
}

TEST(PlatformExchangeFailuresSensor, SetupWithRecordedFailuresPublishesCount) {
  TestableExchangeFailuresHub hub;
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
  TestableExchangeFailuresHub hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);

  IOHomeExchangeFailuresSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_FLOAT_EQ(sensor.state, 0.0f);

  device->exchange_timeout_count = 1;
  hub.notify_device_update_("ABC123");

  EXPECT_FLOAT_EQ(sensor.state, 1.0f) << "a device update should publish the newly recorded failure count";
}

TEST(PlatformExchangeFailuresSensor, DeviceUpdateForOtherDeviceIsIgnored) {
  TestableExchangeFailuresHub hub;
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

  hub.notify_device_update_("DEF456");

  EXPECT_FLOAT_EQ(sensor.state, 0.0f) << "updates for a different device id must not change this sensor's state";
}
