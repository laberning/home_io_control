/// @file platform_reading_sensors_test.cpp
/// @brief Tests for the per-cover Sun level and Battery level sensors (ADR 0064).
///
/// Each is only created when the cover declares its poll interval; these tests cover how it mirrors
/// the device record once it exists. An unknown reading is published as NAN, ESPHome's "unknown".

#include "platform_cover_controls.h"

#include "hub_core.h"

#include "test_helpers.h"

#include <cmath>

using namespace esphome::home_io_control;

namespace {

constexpr const char *DEVICE_ID = "ABC123";

class ReadingSensorHub : public test::MockPlatformHubBase {};

/// A hub with the device registered and both reading sensors bound to it.
class PlatformReadingSensors : public ::testing::Test {
 protected:
  void SetUp() override {
    hub.add_device(DEVICE_ID);
    sun.set_parent(&hub);
    sun.set_device_id(DEVICE_ID);
    battery.set_parent(&hub);
    battery.set_device_id(DEVICE_ID);
  }

  /// Publish a device record the way the hub does after a poll.
  void publish(std::optional<uint16_t> sun_level, std::optional<uint16_t> battery_level,
               const std::string &id = DEVICE_ID) {
    IoDevice dev = *hub.get_device(DEVICE_ID);
    dev.sun_level = sun_level;
    dev.battery_level = battery_level;
    hub.trigger_device_update(id, dev, /*cache_device=*/true);
  }

  ReadingSensorHub hub;
  IOHomeSunLevelSensor sun;
  IOHomeBatteryLevelSensor battery;
};

}  // namespace

TEST_F(PlatformReadingSensors, AreUnknownBeforeTheFirstReading) {
  sun.setup();
  battery.setup();

  EXPECT_TRUE(std::isnan(sun.state));
  EXPECT_TRUE(std::isnan(battery.state));
}

TEST_F(PlatformReadingSensors, PublishTheDevicesReadings) {
  sun.setup();
  battery.setup();

  publish(744, 100);

  EXPECT_FLOAT_EQ(sun.state, 744.0F);
  EXPECT_FLOAT_EQ(battery.state, 100.0F);

  publish(0, 99);
  EXPECT_FLOAT_EQ(sun.state, 0.0F) << "0 is a reading (night), not unknown";
  EXPECT_FLOAT_EQ(battery.state, 99.0F);
}

TEST_F(PlatformReadingSensors, BecomeUnknownWhenTheReadingIsDropped) {
  sun.setup();
  battery.setup();
  publish(744, 100);

  publish(std::nullopt, std::nullopt);

  EXPECT_TRUE(std::isnan(sun.state));
  EXPECT_TRUE(std::isnan(battery.state));
}

TEST_F(PlatformReadingSensors, PublishAnAlreadyKnownReadingAtSetup) {
  hub.get_device(DEVICE_ID)->sun_level = 183;
  hub.get_device(DEVICE_ID)->battery_level = 100;

  sun.setup();
  battery.setup();

  EXPECT_FLOAT_EQ(sun.state, 183.0F);
  EXPECT_FLOAT_EQ(battery.state, 100.0F);
}

TEST_F(PlatformReadingSensors, IgnoreUpdatesForAnotherDevice) {
  sun.setup();
  battery.setup();

  publish(744, 100, "FFFFFF");

  EXPECT_TRUE(std::isnan(sun.state));
  EXPECT_TRUE(std::isnan(battery.state));
}
