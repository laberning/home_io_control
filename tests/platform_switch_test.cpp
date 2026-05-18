#include "platform_switch.h"
#include "hub_core.h"
#include "proto_frame.h"

#include "test_helpers.h"

#include <cstring>

using namespace esphome::home_io_control;

// ============================================================================
// PlatformSwitch test suite
// ============================================================================
// IOHomeSwitch binary entity: write_state() command queuing and inbound status
// updates. Simple on/off mapping based on position < 50.

// Minimal mock hub for switch tests
class MockHubSwitch : public IOHomeControlComponent {
 public:
  MockHubSwitch() {}
  ~MockHubSwitch() override = default;

  bool set_device_position(const std::string &, uint8_t) override { return false; }
  bool request_device_status(const std::string &) override { return false; }
  bool discover_and_pair() override { return false; }
  bool set_light_state(const std::string &, bool) override { return false; }
  bool set_switch_state(const std::string &device_id, bool on) override {
    last_switch_device_id_ = device_id;
    last_switch_on_ = on;
    return true;
  }

  void queue_set_device_position(const std::string &, uint8_t) override {}
  void queue_request_device_status(const std::string &) override {}
  void queue_discover_and_pair() override {}
  void queue_set_light_state(const std::string &, bool) override {}
  void queue_set_switch_state(const std::string &device_id, bool on) override {
    last_switch_device_id_ = device_id;
    last_switch_on_ = on;
    queued_switch_ops_.push_back({device_id, on});
  }

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

  const std::string &last_switch_device_id() const { return last_switch_device_id_; }
  bool last_switch_on() const { return last_switch_on_; }
  const std::deque<std::pair<std::string, bool>> &queued_switch_ops() const { return queued_switch_ops_; }

  void trigger_device_update(const std::string &device_id, const IoDevice &dev) {
    for (auto &cb : callbacks_) {
      cb(device_id, dev);
    }
  }

 private:
  std::string last_switch_device_id_;
  bool last_switch_on_{false};
  std::deque<std::pair<std::string, bool>> queued_switch_ops_;
};

// ========================================================================================
// IOHomeSwitch: binary on/off via position mapping
// ========================================================================================

TEST(PlatformSwitch, WriteStateSendsOnOff) {
  MockHubSwitch hub;
  IOHomeSwitch sw;
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");

  // Turn on
  sw.write_state(true);
  EXPECT_EQ(hub.last_switch_device_id(), "ABC123") << "device ID should match configured ID for on command";
  EXPECT_TRUE(hub.last_switch_on()) << "last_switch_on should be true after on command";

  // Turn off
  sw.write_state(false);
  EXPECT_FALSE(hub.last_switch_on()) << "last_switch_on should be false after off command";
}

TEST(PlatformSwitch, DeviceUpdatePublishesState) {
  MockHubSwitch hub;
  IOHomeSwitch sw;
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");
  sw.setup();  // register callback

  // Device reports ON (position < 50)
  IoDevice dev{};
  dev.position = 30.0f;
  dev.is_stopped = true;
  hub.trigger_device_update("ABC123", dev);

  // Switch state should be on
  EXPECT_TRUE(sw.get_state()) << "position < 50 should set switch state ON";

  // Device reports OFF
  dev.position = 70.0f;
  hub.trigger_device_update("ABC123", dev);
  EXPECT_FALSE(sw.get_state()) << "position >= 50 should set switch state OFF";
}

TEST(PlatformSwitch, IgnoresMovingDevice) {
  MockHubSwitch hub;
  IOHomeSwitch sw;
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");

  IoDevice dev{};
  dev.position = 30.0f;
  dev.is_stopped = false;
  hub.trigger_device_update("ABC123", dev);

  // Should not change state (remains off initially)
  EXPECT_FALSE(sw.get_state()) << "moving device should not change switch state";
}
