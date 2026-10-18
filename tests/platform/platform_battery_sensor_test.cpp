#include "platform_cover_controls.h"

#include "hub_core.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;

// ============================================================================
// PlatformBatterySensors test suite
// ============================================================================
// The two companions a cover gets with `battery: true`: the Battery Low binary sensor and the
// Battery text sensor. These tests cover how they mirror IoDevice::battery once they exist.

namespace {

constexpr const char *DEVICE_ID = "ABC123";

class BatteryHub : public test::MockPlatformHubBase {};

/// A hub with the device registered and both battery sensors bound to it.
class PlatformBatterySensors : public ::testing::Test {
 protected:
  void SetUp() override {
    hub.add_device(DEVICE_ID);
    low.set_parent(&hub);
    low.set_device_id(DEVICE_ID);
    level.set_parent(&hub);
    level.set_device_id(DEVICE_ID);
  }

  void setup_both() {
    low.setup();
    level.setup();
  }

  /// Publish a status the way the hub does after decoding a reply.
  void publish_battery(BatteryState state, const std::string &id = DEVICE_ID) {
    IoDevice dev = *hub.get_device(DEVICE_ID);
    dev.battery = state;
    dev.last_status = 1000;  // a status was decoded
    hub.trigger_device_update(id, dev, /*cache_device=*/true);
  }

  BatteryHub hub;
  IOHomeBatteryLowBinarySensor low;
  IOHomeBatteryTextSensor level;
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
  hub.get_device(DEVICE_ID)->battery = BatteryState::FULL_CHARGE;

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
