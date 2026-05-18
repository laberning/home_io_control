#include "platform_light.h"
#include "hub_core.h"
#include "proto_frame.h"

#include "test_helpers.h"

#include <cstring>

using namespace esphome::home_io_control;

// ============================================================================
// PlatformLight test suite
// ============================================================================
// IOHomeLight binary entity: position-based on/off mapping, write_state() command
// queuing, and inbound status updates. Covers moving vs stopped filtering.

// Mock hub similar to test_platform_cover.cpp but with light state
class MockHubLight : public IOHomeControlComponent {
 public:
  MockHubLight() {}
  ~MockHubLight() override = default;

  bool set_device_position(const std::string &, uint8_t) override { return false; }
  bool request_device_status(const std::string &) override { return false; }
  bool discover_and_pair() override { return false; }
  bool set_light_state(const std::string &device_id, bool on) override {
    last_light_device_id_ = device_id;
    last_light_on_ = on;
    return true;
  }
  bool set_switch_state(const std::string &device_id, bool on) override {
    (void) device_id;
    (void) on;
    return false;
  }

  void queue_set_device_position(const std::string &, uint8_t) override {}
  void queue_request_device_status(const std::string &) override {}
  void queue_discover_and_pair() override {}
  void queue_set_light_state(const std::string &device_id, bool on) override {
    last_light_device_id_ = device_id;
    last_light_on_ = on;
    queued_light_ops_.push_back({device_id, on});
  }
  void queue_set_switch_state(const std::string &, bool) override {}

  IoDevice *get_device(const std::string &device_id) override {
    auto it = devices_.find(device_id);
    return it != devices_.end() ? &it->second : nullptr;
  }
  void add_device(const std::string &device_id) override {
    if (devices_.count(device_id))
      return;
    devices_[device_id] = IoDevice{};
  }
  void add_device(const std::string &device_id, DeviceType type, uint8_t subtype, bool inverted) override {
    if (devices_.count(device_id))
      return;
    devices_[device_id] = IoDevice{};
    devices_[device_id].type = type;
    devices_[device_id].subtype = subtype;
    if (inverted)
      devices_[device_id].inverted = true;
  }
  void register_device_callback(DeviceUpdateCallback cb) override { callbacks_.push_back(std::move(cb)); }

  // Test accessors
  const std::string &last_light_device_id() const { return last_light_device_id_; }
  bool last_light_on() const { return last_light_on_; }
  const std::deque<std::pair<std::string, bool>> &queued_light_ops() const { return queued_light_ops_; }

  void trigger_device_update(const std::string &device_id, const IoDevice &dev) {
    for (auto &cb : callbacks_) {
      cb(device_id, dev);
    }
  }

 private:
  std::string last_light_device_id_;
  bool last_light_on_{false};
  std::deque<std::pair<std::string, bool>> queued_light_ops_;
};

// ========================================================================================
// IOHomeLight: binary on/off via position mapping
// ========================================================================================

TEST(PlatformLight, WriteStateSendsOnOff) {
  MockHubLight hub;
  IOHomeLight light;
  light.set_parent(&hub);
  light.set_device_id("ABC123");
  light.setup_state(new esphome::light::LightState());

  // Simulate HA turning light on via call
  auto *on_state = new esphome::light::LightState();
  on_state->set_current_on(true);
  light.write_state(on_state);
  EXPECT_EQ(hub.last_light_device_id(), "ABC123") << "device ID should match configured ID for on command";
  EXPECT_TRUE(hub.last_light_on()) << "last_light_on should be true after on command";

  // Turn off
  auto *off_state = new esphome::light::LightState();
  off_state->set_current_on(false);
  light.write_state(off_state);
  EXPECT_FALSE(hub.last_light_on()) << "last_light_on should be false after off command";
}

TEST(PlatformLight, DeviceUpdateSetsHAPosition) {
  MockHubLight hub;
  IOHomeLight light;
  light.set_parent(&hub);
  light.set_device_id("ABC123");
  light.setup_state(new esphome::light::LightState());
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
  IOHomeLight light;
  light.set_parent(&hub);
  light.set_device_id("ABC123");
  light.setup_state(new esphome::light::LightState());
  light.setup();

  IoDevice dev{};
  dev.position = 30.0f;
  dev.is_stopped = false;
  hub.trigger_device_update("ABC123", dev);

  // Should not publish (state stays as initial false)
  EXPECT_FALSE(light.state_->current_values.is_on()) << "moving device should not update state";
}
