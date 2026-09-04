/// @file platform_last_contact_sensor_test.cpp
/// @brief Tests for the generated Last Contact diagnostic sensor.

#include "platform_companion_sensors.h"

#include "hub_core.h"
#include "hub_internal.h"
#include "test_helpers.h"

#include <cmath>

using namespace esphome::home_io_control;

namespace {

class TestableLastContactHub : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::notify_device_update_;
};

}  // namespace

TEST(PlatformLastContactSensor, SetupWithNoFrameSeenDoesNotPublish) {
  TestableLastContactHub hub;
  hub.add_device("ABC123");

  IOHomeLastContactSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_TRUE(std::isnan(sensor.state)) << "a device never seen should not publish a state";
}

TEST(PlatformLastContactSensor, SetupWithPriorContactPublishesAgeInSeconds) {
  TestableLastContactHub hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);

  uint32_t const before_ms = esphome::millis();
  device->last_seen_ms = before_ms;

  IOHomeLastContactSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  uint32_t const after_ms = esphome::millis();
  EXPECT_GE(sensor.state, 0.0f) << "age must never be negative";
  EXPECT_LE(sensor.state, static_cast<float>(after_ms - before_ms) / 1000.0f)
      << "published age should be a small elapsed time, not the raw last_seen_ms value";
}

TEST(PlatformLastContactSensor, DeviceUpdateResetsAgeToNearZero) {
  TestableLastContactHub hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);

  IOHomeLastContactSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_TRUE(std::isnan(sensor.state));

  // Simulate a large amount of prior idle time, then a fresh frame arriving "now".
  device->last_seen_ms = esphome::millis();
  for (int i = 0; i < 5000; i++)
    esphome::millis();  // advance the fake clock without a fresh frame
  uint32_t const contact_ms = esphome::millis();
  device->last_seen_ms = contact_ms;
  hub.notify_device_update_("ABC123");

  EXPECT_LE(sensor.state, static_cast<float>(esphome::millis() - contact_ms) / 1000.0f)
      << "a fresh frame should reset the published age back down near zero";
}

TEST(PlatformLastContactSensor, DeviceUpdateForOtherDeviceIsIgnored) {
  TestableLastContactHub hub;
  hub.add_device("ABC123");
  hub.add_device("DEF456");
  auto *other = hub.get_device("DEF456");
  ASSERT_NE(other, nullptr);
  other->last_seen_ms = esphome::millis();

  IOHomeLastContactSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_TRUE(std::isnan(sensor.state));

  hub.notify_device_update_("DEF456");

  EXPECT_TRUE(std::isnan(sensor.state)) << "updates for a different device id must not change this sensor's state";
}

TEST(PlatformLastContactSensor, HeartbeatRepublishesAgeWithoutNewFrame) {
  TestableLastContactHub hub;
  hub.add_device("ABC123");
  auto *device = hub.get_device("ABC123");
  ASSERT_NE(device, nullptr);
  device->last_seen_ms = esphome::millis();

  IOHomeLastContactSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  ASSERT_TRUE(static_cast<bool>(sensor.last_interval_callback_)) << "setup() must arm a heartbeat interval";
  float const state_before = sensor.state;
  ASSERT_FALSE(std::isnan(state_before));

  for (int i = 0; i < 1000; i++)
    esphome::millis();  // advance the fake clock, simulating idle time between heartbeats
  sensor.last_interval_callback_();

  EXPECT_GT(sensor.state, state_before) << "the heartbeat should republish a larger age when idle";
}
