/// @file platform_last_result_text_sensor_test.cpp
/// @brief Tests for the generated last-command-result diagnostic text sensor.

#include "platform_last_result_text_sensor.h"

#include "hub_core.h"
#include "hub_internal.h"
#include "proto_constants.h"
#include "test_helpers.h"

using namespace esphome::home_io_control;

namespace {

class TestableLastResultHub : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::notify_device_update_;
};

}  // namespace

TEST(PlatformLastResultTextSensor, SetupWithNoRecordedResultPublishesEmptyString) {
  TestableLastResultHub hub;
  hub.add_device("ABC123");

  IOHomeLastResultTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_EQ(sensor.state, "") << "a device with no recorded CMD_ERROR_RESP should publish an empty string";
}

TEST(PlatformLastResultTextSensor, SetupWithRecordedResultPublishesSymbolicName) {
  TestableLastResultHub hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  device->last_result_code = RESULT_LIMITATION_BY_RAIN;

  IOHomeLastResultTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_EQ(sensor.state, "LIMITATION_BY_RAIN")
      << "a previously recorded result should publish its symbolic name on setup";
}

TEST(PlatformLastResultTextSensor, DeviceUpdatePublishesNewResultAndClears) {
  TestableLastResultHub hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);

  IOHomeLastResultTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_EQ(sensor.state, "");

  device->last_result_code = RESULT_LIMITATION_BY_WIND;
  hub.notify_device_update_("ABC123");

  EXPECT_EQ(sensor.state, "LIMITATION_BY_WIND") << "a device update should publish the newly recorded result";

  device->last_result_code = 0;
  hub.notify_device_update_("ABC123");

  EXPECT_EQ(sensor.state, "") << "a device update after clearing should publish an empty string again";
}

TEST(PlatformLastResultTextSensor, DeviceUpdateForOtherDeviceIsIgnored) {
  TestableLastResultHub hub;
  hub.add_device("ABC123");
  hub.add_device("DEF456");
  auto *other = hub.get_device("DEF456");
  ASSERT_NE(other, nullptr);
  other->last_result_code = RESULT_LIMITATION_BY_RAIN;

  IOHomeLastResultTextSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_EQ(sensor.state, "");

  hub.notify_device_update_("DEF456");

  EXPECT_EQ(sensor.state, "") << "updates for a different device id must not change this sensor's published state";
}
