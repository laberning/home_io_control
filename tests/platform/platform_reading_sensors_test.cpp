/// @file platform_reading_sensors_test.cpp
/// @brief Tests for the per-cover Sun level and Battery level sensors (ADR 0064), the generic reading sensor bound
///        to DeviceReading::SUN_LEVEL and DeviceReading::BATTERY_LEVEL.
///
/// Each is only created when the cover declares its poll interval; these tests cover how it mirrors
/// the device record once it exists. An unknown reading is published as NAN, ESPHome's "unknown". The sun level is
/// stored in lux, so the sensor shows the stored value as it is.

#include "platform_device_readings.h"

#include "hub_port_doubles.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace esphome::home_io_control;

namespace {

constexpr const char *DEVICE_ID = "ABC123";

/// A hub with the device registered and both reading sensors bound to it.
class PlatformReadingSensors : public ::testing::Test {
 protected:
  void SetUp() override {
    hub.add_device(DEVICE_ID);
    sun.set_parent(&hub);
    sun.set_device_id(DEVICE_ID);
    sun.set_reading(DeviceReading::SUN_LEVEL);
    battery.set_parent(&hub);
    battery.set_device_id(DEVICE_ID);
    battery.set_reading(DeviceReading::BATTERY_LEVEL);
  }

  /// Keep a reading in a device record the way the hub does after a poll: a value is stored, none clears it.
  static void keep(IoDevice &dev, DeviceReading reading, std::optional<float> value) {
    if (value.has_value()) {
      dev.reading_values.store(reading_slot(reading), *value);
    } else {
      dev.reading_values.clear(reading_slot(reading));
    }
  }

  /// Publish a device record the way the hub does after a poll.
  void publish(std::optional<float> sun_lux, std::optional<float> battery_level, const std::string &id = DEVICE_ID) {
    IoDevice dev = *hub.get_device(DEVICE_ID);
    keep(dev, DeviceReading::SUN_LEVEL, sun_lux);
    keep(dev, DeviceReading::BATTERY_LEVEL, battery_level);
    hub.trigger_device_update(id, dev, /*cache_device=*/true);
  }

  test::DevicePortDouble hub;
  IOHomeDeviceReadingSensor sun;
  IOHomeDeviceReadingSensor battery;
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

  publish(74400.0F, 100.0F);

  EXPECT_FLOAT_EQ(sun.state, 74400.0F) << "the sensor shows the stored lux as it is";
  EXPECT_FLOAT_EQ(battery.state, 100.0F);

  publish(0.0F, 99.0F);
  EXPECT_FLOAT_EQ(sun.state, 0.0F) << "0 is a reading (night), not unknown";
  EXPECT_FLOAT_EQ(battery.state, 99.0F);
}

TEST_F(PlatformReadingSensors, BecomeUnknownWhenTheReadingIsDropped) {
  sun.setup();
  battery.setup();
  publish(74400.0F, 100.0F);

  publish(std::nullopt, std::nullopt);

  EXPECT_TRUE(std::isnan(sun.state));
  EXPECT_TRUE(std::isnan(battery.state));
}

TEST_F(PlatformReadingSensors, PublishAnAlreadyKnownReadingAtSetup) {
  keep(*hub.get_device(DEVICE_ID), DeviceReading::SUN_LEVEL, 18300.0F);
  keep(*hub.get_device(DEVICE_ID), DeviceReading::BATTERY_LEVEL, 100.0F);

  sun.setup();
  battery.setup();

  EXPECT_FLOAT_EQ(sun.state, 18300.0F);
  EXPECT_FLOAT_EQ(battery.state, 100.0F);
}

TEST_F(PlatformReadingSensors, IgnoreUpdatesForAnotherDevice) {
  sun.setup();
  battery.setup();

  publish(74400.0F, 100.0F, "FFFFFF");

  EXPECT_TRUE(std::isnan(sun.state));
  EXPECT_TRUE(std::isnan(battery.state));
}
