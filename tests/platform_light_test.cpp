#include "platform_light.h"
#include "hub_core.h"

#include "test_helpers.h"

#include <cstring>

using namespace esphome::home_io_control;

// ============================================================================
// PlatformLight test suite
// ============================================================================
// IOHomeLight binary entity: position-based on/off mapping, write_state() command
// queuing, and inbound status updates. Covers moving vs stopped filtering.

// Mock hub similar to test_platform_cover.cpp but with light state
class MockHubLight : public test::MockPlatformHubBase {
 public:
  MockHubLight() {}
  ~MockHubLight() override = default;

  bool set_light_state(const std::string &device_id, bool on) override {
    last_light_device_id_ = device_id;
    last_light_on_ = on;
    return true;
  }

  void queue_set_light_state(const std::string &device_id, bool on) override {
    last_light_device_id_ = device_id;
    last_light_on_ = on;
    queued_light_ops_.push_back({device_id, on});
  }

  // Test accessors
  const std::string &last_light_device_id() const { return last_light_device_id_; }
  bool last_light_on() const { return last_light_on_; }
  const std::deque<std::pair<std::string, bool>> &queued_light_ops() const { return queued_light_ops_; }

  void trigger_device_update(const std::string &device_id, const IoDevice &dev) {
    test::MockPlatformHubBase::trigger_device_update(device_id, dev);
  }

 private:
  std::string last_light_device_id_;
  bool last_light_on_{false};
  std::deque<std::pair<std::string, bool>> queued_light_ops_;
};

/// Exposes protected state_ for direct assertion in tests.
class TestableLight : public IOHomeLight {
 public:
  using IOHomeLight::state_;
};

// ========================================================================================
// IOHomeLight: binary on/off via position mapping
// ========================================================================================

TEST(PlatformLight, WriteStateSendsOnOff) {
  MockHubLight hub;
  IOHomeLight light;
  light.set_parent(&hub);
  light.set_device_id("ABC123");
  esphome::light::LightState state;
  light.setup_state(&state);

  // Simulate HA turning light on via call
  esphome::light::LightState on_state;
  on_state.set_current_on(true);
  light.write_state(&on_state);
  EXPECT_EQ(hub.last_light_device_id(), "ABC123") << "device ID should match configured ID for on command";
  EXPECT_TRUE(hub.last_light_on()) << "last_light_on should be true after on command";

  // Turn off
  esphome::light::LightState off_state;
  off_state.set_current_on(false);
  light.write_state(&off_state);
  EXPECT_FALSE(hub.last_light_on()) << "last_light_on should be false after off command";
}

TEST(PlatformLight, DeviceUpdateSetsHAPosition) {
  MockHubLight hub;
  TestableLight light;
  light.set_parent(&hub);
  light.set_device_id("ABC123");
  esphome::light::LightState state;
  light.setup_state(&state);
  light.setup();  // register device and callback

  // Device reports ON (position < 50)
  IoDevice dev{};
  dev.position = 30.0f;  // < 50 -> on
  dev.is_stopped = true;
  hub.trigger_device_update("ABC123", dev);

  // HA state becomes on
  EXPECT_TRUE(light.state_->current_values.is_on()) << "position < 50 should be interpreted as ON";

  // Device reports OFF
  dev.position = 70.0f;
  hub.trigger_device_update("ABC123", dev);
  EXPECT_FALSE(light.state_->current_values.is_on()) << "position >= 50 should be interpreted as OFF";
}

TEST(PlatformLight, IgnoresMovingDevice) {
  MockHubLight hub;
  TestableLight light;
  light.set_parent(&hub);
  light.set_device_id("ABC123");
  esphome::light::LightState state;
  light.setup_state(&state);
  light.setup();

  IoDevice dev{};
  dev.position = 30.0f;
  dev.is_stopped = false;
  hub.trigger_device_update("ABC123", dev);

  // Should not publish (state stays as initial false)
  EXPECT_FALSE(light.state_->current_values.is_on()) << "moving device should not update state";
}
