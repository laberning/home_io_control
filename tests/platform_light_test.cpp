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

  void queue_set_light_position(const std::string &device_id, uint8_t position) override {
    last_light_device_id_ = device_id;
    last_light_position_ = position;
    queued_light_positions_.push_back({device_id, position});
  }

  // Test accessors
  const std::string &last_light_device_id() const { return last_light_device_id_; }
  bool last_light_on() const { return last_light_on_; }
  uint8_t last_light_position() const { return last_light_position_; }
  const std::deque<std::pair<std::string, bool>> &queued_light_ops() const { return queued_light_ops_; }
  const std::deque<std::pair<std::string, uint8_t>> &queued_light_positions() const { return queued_light_positions_; }

  void trigger_device_update(const std::string &device_id, const IoDevice &dev) {
    test::MockPlatformHubBase::trigger_device_update(device_id, dev);
  }

 private:
  std::string last_light_device_id_;
  bool last_light_on_{false};
  uint8_t last_light_position_{0};
  std::deque<std::pair<std::string, bool>> queued_light_ops_;
  std::deque<std::pair<std::string, uint8_t>> queued_light_positions_;
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
  light.write_state(&state);  // real LightState::setup()'s boot-time restore_mode write; suppressed

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

// Real-hardware regression: LightState::setup() calls setup_state() then, moments later in that
// same call, applies its own restore_mode (default ALWAYS_OFF) via a real write_state() call —
// unlike a cover, which has no restore-mode/active-push concept and only ever learns state from
// the delayed status poll. Without suppression, every reflash silently sent an actual "turn off"
// (or "turn on") command to the physical device regardless of its real state.
TEST(PlatformLight, SetupStateSuppressesBootTimeRestoreWrite) {
  MockHubLight hub;
  IOHomeLight light;
  light.set_parent(&hub);
  light.set_device_id("ABC123");
  esphome::light::LightState state;

  light.setup_state(&state);
  esphome::light::LightState boot_state;  // simulates LightState::setup()'s restore_mode call
  boot_state.set_current_on(false);
  light.write_state(&boot_state);
  EXPECT_TRUE(hub.queued_light_ops().empty())
      << "the boot-time restore_mode write must be suppressed, not sent as a real command";

  // One-shot: a genuine subsequent write should go through normally.
  esphome::light::LightState real_state;
  real_state.set_current_on(true);
  light.write_state(&real_state);
  EXPECT_FALSE(hub.queued_light_ops().empty()) << "a real write_state() call after boot should still work";
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

// ========================================================================================
// IOHomeLight: dimmable (ColorMode::BRIGHTNESS) via the same position field
// ========================================================================================

TEST(PlatformLight, SetupStampsDimmableOntoRegisteredDevice) {
  MockHubLight hub;
  IOHomeLight dimmable_light;
  dimmable_light.set_parent(&hub);
  dimmable_light.set_device_id("ABC123");
  dimmable_light.set_dimmable(true);
  dimmable_light.setup();
  ASSERT_NE(hub.get_device("ABC123"), nullptr);
  EXPECT_TRUE(hub.get_device("ABC123")->dimmable)
      << "setup() should stamp dimmable=true onto the registered device for profile-name logging";

  MockHubLight hub2;
  IOHomeLight binary_light;
  binary_light.set_parent(&hub2);
  binary_light.set_device_id("DEF456");
  binary_light.setup();
  ASSERT_NE(hub2.get_device("DEF456"), nullptr);
  EXPECT_FALSE(hub2.get_device("DEF456")->dimmable) << "default (dimmable unset) should stamp dimmable=false";
}

TEST(PlatformLight, GetTraitsReflectsDimmableFlag) {
  IOHomeLight binary_light;
  EXPECT_EQ(binary_light.get_traits().get_color_mode(), esphome::light::ColorMode::ON_OFF)
      << "default (dimmable unset) should report ON_OFF";

  IOHomeLight dimmable_light;
  dimmable_light.set_dimmable(true);
  EXPECT_EQ(dimmable_light.get_traits().get_color_mode(), esphome::light::ColorMode::BRIGHTNESS)
      << "dimmable=true should report BRIGHTNESS";
}

TEST(PlatformLight, DimmableWriteStateSendsIoPosition) {
  MockHubLight hub;
  IOHomeLight light;
  light.set_parent(&hub);
  light.set_device_id("ABC123");
  light.set_dimmable(true);
  esphome::light::LightState state;
  light.setup_state(&state);
  light.write_state(&state);  // real LightState::setup()'s boot-time restore_mode write; suppressed

  // HA brightness 1.0 (full) -> IO position 0 (this device family's "full brightness" convention)
  esphome::light::LightState full_state;
  full_state.set_current_on(true);
  full_state.set_current_brightness(1.0f);
  light.write_state(&full_state);
  EXPECT_EQ(hub.last_light_position(), 0) << "full brightness should map to IO position 0";

  // HA brightness 0.25 -> IO position 75
  esphome::light::LightState dim_state;
  dim_state.set_current_on(true);
  dim_state.set_current_brightness(0.25f);
  light.write_state(&dim_state);
  EXPECT_EQ(hub.last_light_position(), 75) << "25% brightness should map to IO position 75";

  // Off (brightness folded to 0 by current_values_as_brightness) -> IO position 100
  esphome::light::LightState off_state;
  off_state.set_current_on(false);
  off_state.set_current_brightness(0.6f);  // stored level irrelevant while off
  light.write_state(&off_state);
  EXPECT_EQ(hub.last_light_position(), 100) << "off should map to IO position 100 regardless of stored brightness";
}

// Real-hardware regression: HA quantizes brightness to 0-255 before it reaches us, so its "50%"
// is 128/255=0.50196, not exactly 0.5. A truncating cast in write_state() turned
// (1-0.50196)*100=49.8 into IO position 49 instead of 50 — a consistent 1% bias, caught on
// hardware (device settled at 49%, HA then displayed 51% on readback).
TEST(PlatformLight, DimmableWriteStateRoundsInsteadOfTruncating) {
  MockHubLight hub;
  IOHomeLight light;
  light.set_parent(&hub);
  light.set_device_id("ABC123");
  light.set_dimmable(true);
  esphome::light::LightState state;
  light.setup_state(&state);
  light.write_state(&state);  // real LightState::setup()'s boot-time restore_mode write; suppressed

  esphome::light::LightState quantized_state;
  quantized_state.set_current_on(true);
  quantized_state.set_current_brightness(128.0f / 255.0f);  // HA's actual "50%" value
  light.write_state(&quantized_state);
  EXPECT_EQ(hub.last_light_position(), 50) << "128/255 (HA's '50%') should round to IO position 50, not truncate to 49";
}

// Real-hardware regression: gamma_correct defaults to 2.8 for a BRIGHTNESS_ONLY light, and
// LightState::current_values_as_brightness() applies it. write_state() must read the linear
// value (current_values.as_brightness()) instead, or a HA-set 50% silently arrives on the wire
// as ~15% (0.5^2.8) — exactly what happened before this test existed.
TEST(PlatformLight, DimmableWriteStateIgnoresGammaCorrection) {
  MockHubLight hub;
  IOHomeLight light;
  light.set_parent(&hub);
  light.set_device_id("ABC123");
  light.set_dimmable(true);
  esphome::light::LightState state;
  light.setup_state(&state);
  light.write_state(&state);  // real LightState::setup()'s boot-time restore_mode write; suppressed

  esphome::light::LightState gamma_state;
  gamma_state.set_current_on(true);
  gamma_state.set_current_brightness(0.5f);
  gamma_state.test_gamma_scale_ = 0.144f;  // simulates gamma_correct=2.8 distorting 0.5 -> ~0.144
  light.write_state(&gamma_state);
  EXPECT_EQ(hub.last_light_position(), 50)
      << "write_state() must use the raw 50% brightness, not the gamma-distorted ~14%, which "
         "would have produced IO position 86";
}

TEST(PlatformLight, DimmableDeviceUpdateSetsHABrightness) {
  MockHubLight hub;
  TestableLight light;
  light.set_parent(&hub);
  light.set_device_id("ABC123");
  light.set_dimmable(true);
  esphome::light::LightState state;
  light.setup_state(&state);
  light.setup();

  // Device reports IO position 75 -> HA brightness 0.25, still on
  IoDevice dev{};
  dev.position = 75.0f;
  dev.is_stopped = true;
  hub.trigger_device_update("ABC123", dev);
  EXPECT_TRUE(light.state_->current_values.is_on()) << "position 75 (>0% brightness) should be on";
  EXPECT_FLOAT_EQ(light.state_->current_values.get_brightness(), 0.25f);

  // Device reports IO position 100 (off) -> HA brightness 0, off
  dev.position = 100.0f;
  hub.trigger_device_update("ABC123", dev);
  EXPECT_FALSE(light.state_->current_values.is_on()) << "position 100 should be off";
}
