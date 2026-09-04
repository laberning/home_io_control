/// @file platform_rssi_sensor_test.cpp
/// @brief Tests for the generated RSSI diagnostic sensor.

#include "platform_companion_sensors.h"

#include "hub_core.h"
#include "hub_internal.h"
#include "test_helpers.h"

#include <cmath>

using namespace esphome::home_io_control;

namespace {

class TestableRssiHub : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::notify_device_update_;
};

}  // namespace

TEST(PlatformRssiSensor, SetupWithNoSampleDoesNotPublish) {
  TestableRssiHub hub;
  hub.add_device("ABC123");

  IOHomeRssiSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_TRUE(std::isnan(sensor.state)) << "a device with no RSSI sample yet should not publish a state";
}

TEST(PlatformRssiSensor, SetupWithSamplePublishesEma) {
  TestableRssiHub hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  device->rssi_ema_scaled = -73 * RSSI_EMA_SCALE;

  IOHomeRssiSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_FLOAT_EQ(sensor.state, -73.0f) << "a previously recorded EMA should publish on setup";
}

TEST(PlatformRssiSensor, DeviceUpdatePublishesNewEma) {
  TestableRssiHub hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);

  IOHomeRssiSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_TRUE(std::isnan(sensor.state));

  device->rssi_ema_scaled = -60 * RSSI_EMA_SCALE;
  hub.notify_device_update_("ABC123");

  EXPECT_FLOAT_EQ(sensor.state, -60.0f) << "a device update should publish the newly recorded EMA";
}

TEST(PlatformRssiSensor, DeviceUpdateForOtherDeviceIsIgnored) {
  TestableRssiHub hub;
  hub.add_device("ABC123");
  hub.add_device("DEF456");
  auto *other = hub.get_device("DEF456");
  ASSERT_NE(other, nullptr);
  other->rssi_ema_scaled = -50 * RSSI_EMA_SCALE;

  IOHomeRssiSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_TRUE(std::isnan(sensor.state));

  hub.notify_device_update_("DEF456");

  EXPECT_TRUE(std::isnan(sensor.state)) << "updates for a different device id must not change this sensor's state";
}
