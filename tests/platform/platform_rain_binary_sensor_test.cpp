#include "platform_cover_controls.h"

#include "hub_core.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;

// ============================================================================
// PlatformRainBinarySensor test suite
// ============================================================================
// The generated per-cover rain binary sensor. It is only created when a cover declares
// `rain_sensor_poll_interval:`; these tests cover how it mirrors the device's rain state once it exists.

namespace {

constexpr const char *DEVICE_ID = "ABC123";

class RainSensorHub : public test::MockPlatformHubBase {};

/// A hub with the device registered and a rain sensor bound to it.
class PlatformRainBinarySensor : public ::testing::Test {
 protected:
  void SetUp() override {
    hub.add_device(DEVICE_ID);
    sensor.set_parent(&hub);
    sensor.set_device_id(DEVICE_ID);
  }

  /// Publish the device's rain state to the entity the way the hub does after a poll.
  void publish_rain_state(RainSensorState state, const std::string &id = DEVICE_ID) {
    IoDevice dev = *hub.get_device(DEVICE_ID);
    dev.rain_sensor = state;
    hub.trigger_device_update(id, dev, /*cache_device=*/true);
  }

  RainSensorHub hub;
  IOHomeRainBinarySensor sensor;
};

}  // namespace

TEST_F(PlatformRainBinarySensor, HasNoStateBeforeTheFirstReading) {
  sensor.setup();

  EXPECT_FALSE(sensor.has_state()) << "dry must mean a reply said so, so boot publishes nothing";
}

TEST_F(PlatformRainBinarySensor, ReportsRainAndDryFromTheDeviceState) {
  sensor.setup();

  publish_rain_state(RainSensorState::RAIN);
  ASSERT_TRUE(sensor.has_state());
  EXPECT_TRUE(sensor.state);

  publish_rain_state(RainSensorState::DRY);
  ASSERT_TRUE(sensor.has_state());
  EXPECT_FALSE(sensor.state);
}

TEST_F(PlatformRainBinarySensor, LosesItsStateWhenTheReadingBecomesUnknown) {
  sensor.setup();
  publish_rain_state(RainSensorState::RAIN);
  ASSERT_TRUE(sensor.has_state());

  publish_rain_state(RainSensorState::UNKNOWN);

  EXPECT_FALSE(sensor.has_state());
}

TEST_F(PlatformRainBinarySensor, PublishesAnAlreadyKnownReadingAtSetup) {
  hub.get_device(DEVICE_ID)->rain_sensor = RainSensorState::RAIN;

  sensor.setup();

  ASSERT_TRUE(sensor.has_state());
  EXPECT_TRUE(sensor.state);
}

TEST_F(PlatformRainBinarySensor, IgnoresUpdatesForAnotherDevice) {
  hub.add_device("ABC124");
  sensor.setup();

  publish_rain_state(RainSensorState::RAIN, "ABC124");

  EXPECT_FALSE(sensor.has_state());
}
