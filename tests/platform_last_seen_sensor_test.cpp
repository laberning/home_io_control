/// @file platform_last_seen_sensor_test.cpp
/// @brief Tests for the generated Last Seen diagnostic sensor.

#include "platform_last_seen_sensor.h"

#include "hub_core.h"
#include "hub_internal.h"
#include "test_helpers.h"

#include <cmath>

using namespace esphome::home_io_control;

namespace {

class TestableLastSeenHub : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::notify_device_update_;
};

}  // namespace

TEST(PlatformLastSeenSensor, SetupWithNoFrameSeenDoesNotPublish) {
  TestableLastSeenHub hub;
  hub.add_device("ABC123");

  IOHomeLastSeenSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_TRUE(std::isnan(sensor.state)) << "a device never seen should not publish a state";
}

TEST(PlatformLastSeenSensor, SetupWithLastSeenPublishesSeconds) {
  TestableLastSeenHub hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  device->last_seen_ms = 12500;

  IOHomeLastSeenSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_FLOAT_EQ(sensor.state, 12.5f) << "last_seen_ms should publish converted to seconds";
}

TEST(PlatformLastSeenSensor, DeviceUpdatePublishesNewLastSeen) {
  TestableLastSeenHub hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);

  IOHomeLastSeenSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_TRUE(std::isnan(sensor.state));

  device->last_seen_ms = 4000;
  hub.notify_device_update_("ABC123");

  EXPECT_FLOAT_EQ(sensor.state, 4.0f) << "a device update should publish the newly recorded last-seen time";
}

TEST(PlatformLastSeenSensor, DeviceUpdateForOtherDeviceIsIgnored) {
  TestableLastSeenHub hub;
  hub.add_device("ABC123");
  hub.add_device("DEF456");
  auto *other = hub.get_device("DEF456");
  ASSERT_NE(other, nullptr);
  other->last_seen_ms = 9000;

  IOHomeLastSeenSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_TRUE(std::isnan(sensor.state));

  hub.notify_device_update_("DEF456");

  EXPECT_TRUE(std::isnan(sensor.state)) << "updates for a different device id must not change this sensor's state";
}
