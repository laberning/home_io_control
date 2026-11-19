#include "platform_device_readings.h"

#include "hub_port_doubles.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

// ============================================================================
// PlatformRainBinarySensor test suite
// ============================================================================
// The generated per-cover rain binary sensor, the generic reading binary sensor bound to DeviceReading::RAIN. It is
// only created when a cover declares `rain_sensor_poll_interval:`; these tests cover how it mirrors the device's rain
// reading once it exists: 1 is rain, 0 is dry, and no reading leaves it without a state.

namespace {

constexpr const char *DEVICE_ID = "ABC123";

/// A hub with the device registered and a rain sensor bound to it.
class PlatformRainBinarySensor : public ::testing::Test {
 protected:
  void SetUp() override {
    hub.add_device(DEVICE_ID);
    sensor.set_parent(&hub);
    sensor.set_device_id(DEVICE_ID);
    sensor.set_reading(DeviceReading::RAIN);
  }

  /// Keep the rain reading in a device record the way the hub does after a poll: a value is stored, none clears it.
  static void keep_rain(IoDevice &dev, std::optional<bool> rain) {
    if (rain.has_value()) {
      dev.reading_values.store(reading_slot(DeviceReading::RAIN), *rain ? 1.0F : 0.0F);
    } else {
      dev.reading_values.clear(reading_slot(DeviceReading::RAIN));
    }
  }

  /// Publish the device's rain reading to the entity the way the hub does after a poll.
  void publish_rain_state(std::optional<bool> rain, const std::string &id = DEVICE_ID) {
    IoDevice dev = *hub.get_device(DEVICE_ID);
    keep_rain(dev, rain);
    hub.trigger_device_update(id, dev, /*cache_device=*/true);
  }

  test::DevicePortDouble hub;
  IOHomeDeviceReadingBinarySensor sensor;
};

}  // namespace

TEST_F(PlatformRainBinarySensor, HasNoStateBeforeTheFirstReading) {
  sensor.setup();

  EXPECT_FALSE(sensor.has_state()) << "dry must mean a reply said so, so boot publishes nothing";
}

TEST_F(PlatformRainBinarySensor, ReportsRainAndDryFromTheDeviceState) {
  sensor.setup();

  publish_rain_state(true);
  ASSERT_TRUE(sensor.has_state());
  EXPECT_TRUE(sensor.state);

  publish_rain_state(false);
  ASSERT_TRUE(sensor.has_state());
  EXPECT_FALSE(sensor.state);
}

TEST_F(PlatformRainBinarySensor, LosesItsStateWhenTheReadingBecomesUnknown) {
  sensor.setup();
  publish_rain_state(true);
  ASSERT_TRUE(sensor.has_state());

  publish_rain_state(std::nullopt);

  EXPECT_FALSE(sensor.has_state());
}

TEST_F(PlatformRainBinarySensor, PublishesAnAlreadyKnownReadingAtSetup) {
  keep_rain(*hub.get_device(DEVICE_ID), true);

  sensor.setup();

  ASSERT_TRUE(sensor.has_state());
  EXPECT_TRUE(sensor.state);
}

TEST_F(PlatformRainBinarySensor, IgnoresUpdatesForAnotherDevice) {
  hub.add_device("ABC124");
  sensor.setup();

  publish_rain_state(true, "ABC124");

  EXPECT_FALSE(sensor.has_state());
}
