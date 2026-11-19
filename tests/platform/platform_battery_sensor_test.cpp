#include "platform_device_readings.h"

#include "hub_port_doubles.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

// ============================================================================
// PlatformBatterySensors test suite
// ============================================================================
// The two entities a cover gets with `battery: true`: the Battery Low binary sensor (DeviceReading::BATTERY_LOW) and
// the Battery text sensor (DeviceReading::BATTERY_STATE), both generic reading entities. These tests cover how they
// mirror the device's battery readings once they exist.

namespace {

constexpr const char *DEVICE_ID = "ABC123";

/// A hub with the device registered and both battery sensors bound to it.
class PlatformBatterySensors : public ::testing::Test {
 protected:
  void SetUp() override {
    hub.add_device(DEVICE_ID);
    low.set_parent(&hub);
    low.set_device_id(DEVICE_ID);
    low.set_reading(DeviceReading::BATTERY_LOW);
    level.set_parent(&hub);
    level.set_device_id(DEVICE_ID);
    level.set_reading(DeviceReading::BATTERY_STATE);
  }

  void setup_both() {
    low.setup();
    level.setup();
  }

  /// Keep a battery level in a device record the way the hub's status decoder does: a level is stored, none marks the
  /// reading absent.
  static void keep_battery(IoDevice &dev, BatteryState state) {
    const size_t slot = reading_slot(DeviceReading::BATTERY_STATE);
    if (state == BatteryState::NOT_REPORTED) {
      dev.reading_values.mark_absent(slot);
    } else {
      dev.reading_values.store(slot, static_cast<float>(state));
    }
  }

  /// Publish a status the way the hub does after decoding a reply.
  void publish_battery(BatteryState state, const std::string &id = DEVICE_ID) {
    IoDevice dev = *hub.get_device(DEVICE_ID);
    keep_battery(dev, state);
    dev.last_status = 1000;  // a status was decoded
    hub.trigger_device_update(id, dev, /*cache_device=*/true);
  }

  test::DevicePortDouble hub;
  IOHomeDeviceReadingBinarySensor low;
  IOHomeDeviceReadingTextSensor level;
};

}  // namespace

TEST_F(PlatformBatterySensors, HaveNoStateBeforeTheFirstStatus) {
  setup_both();

  EXPECT_FALSE(low.has_state()) << "off must mean the device said its battery is fine";
  EXPECT_TRUE(level.state.empty()) << "nothing published";
}

TEST_F(PlatformBatterySensors, FullAndNormalAreNotLow) {
  setup_both();

  publish_battery(BatteryState::FULL_CHARGE);
  ASSERT_TRUE(low.has_state());
  EXPECT_FALSE(low.state);
  EXPECT_EQ(level.state, "full");

  publish_battery(BatteryState::NORMAL_CHARGE);
  EXPECT_FALSE(low.state);
  EXPECT_EQ(level.state, "normal");
}

TEST_F(PlatformBatterySensors, LowTurnsBatteryLowOn) {
  setup_both();

  publish_battery(BatteryState::LOW_CHARGE);

  ASSERT_TRUE(low.has_state());
  EXPECT_TRUE(low.state);
  EXPECT_EQ(level.state, "low");
}

TEST_F(PlatformBatterySensors, NotReportedPublishesNothing) {
  setup_both();

  publish_battery(BatteryState::NOT_REPORTED);

  EXPECT_FALSE(low.has_state());
  EXPECT_TRUE(level.state.empty()) << "nothing published";
}

TEST_F(PlatformBatterySensors, PublishAnAlreadyKnownLevelAtSetup) {
  keep_battery(*hub.get_device(DEVICE_ID), BatteryState::FULL_CHARGE);

  setup_both();

  ASSERT_TRUE(low.has_state());
  EXPECT_FALSE(low.state);
  EXPECT_EQ(level.state, "full");
}

TEST_F(PlatformBatterySensors, IgnoreUpdatesForAnotherDevice) {
  hub.add_device("ABC124");
  setup_both();

  publish_battery(BatteryState::LOW_CHARGE, "ABC124");

  EXPECT_FALSE(low.has_state());
  EXPECT_TRUE(level.state.empty()) << "nothing published";
}
