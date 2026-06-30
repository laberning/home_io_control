#include "platform_cover_vent_button.h"

#include "hub_core.h"
#include "proto_frame.h"

#include <deque>

#include "test_helpers.h"

using namespace esphome::home_io_control;

// ============================================================================
// PlatformCoverVentButton test suite
// ============================================================================
// Covers the generated ventilation-position button entity and its hub queueing behavior.

class TestableIOHomeCoverVentButton : public IOHomeCoverVentButton {
 public:
  void trigger_press() { this->press_action(); }
};

class VentButtonMockHub : public IOHomeControlComponent {
 public:
  VentButtonMockHub() = default;
  ~VentButtonMockHub() override = default;

  bool set_device_position(const std::string &device_id, uint8_t position) override {
    (void) device_id;
    (void) position;
    return true;
  }
  bool set_device_tilt(const std::string &device_id, uint8_t tilt_percent) override {
    (void) device_id;
    (void) tilt_percent;
    return false;
  }
  bool request_device_status(const std::string &device_id) override {
    (void) device_id;
    return false;
  }
  bool discover_and_pair() override { return false; }
  bool set_light_state(const std::string &device_id, bool on) override {
    (void) device_id;
    (void) on;
    return false;
  }
  bool set_switch_state(const std::string &device_id, bool on) override {
    (void) device_id;
    (void) on;
    return false;
  }

  void queue_set_device_position(const std::string &device_id, uint8_t position) override {
    (void) device_id;
    (void) position;
  }
  void queue_device_command(const std::string &device_id, CoverCommand cmd) override {
    last_device_id_ = device_id;
    last_command_ = cmd;
    PendingOperation op{};
    op.type = IOHomeControlComponent::PendingOperationType::DEVICE_COMMAND;
    op.device_id = device_id;
    op.command = cmd;
    queued_operations_.push_back(op);
  }
  void queue_set_device_tilt(const std::string &device_id, uint8_t tilt_percent) override {
    (void) device_id;
    (void) tilt_percent;
  }
  void queue_set_device_position_and_tilt(const std::string &device_id, uint8_t position,
                                          uint8_t tilt_percent) override {
    (void) device_id;
    (void) position;
    (void) tilt_percent;
  }
  void queue_request_device_status(const std::string &device_id) override { (void) device_id; }
  void queue_discover_and_pair() override {}
  void queue_set_light_state(const std::string &device_id, bool on) override {
    (void) device_id;
    (void) on;
  }
  void queue_set_switch_state(const std::string &device_id, bool on) override {
    (void) device_id;
    (void) on;
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
    devices_[device_id].inverted = inverted;
  }
  void register_device_callback(DeviceUpdateCallback cb) override { callbacks_.push_back(std::move(cb)); }

  const std::string &last_device_id() const { return last_device_id_; }
  CoverCommand last_command() const { return last_command_; }
  const std::deque<PendingOperation> &queued_operations() const { return queued_operations_; }

 private:
  std::string last_device_id_;
  CoverCommand last_command_{CoverCommand::STOP};
  std::deque<PendingOperation> queued_operations_;
  std::map<std::string, IoDevice> devices_;
  std::vector<DeviceUpdateCallback> callbacks_;
};

TEST(PlatformCoverVentButton, PressQueuesVentCommand) {
  VentButtonMockHub hub;
  hub.add_device("ABC123", DeviceType::WINDOW_OPENER, 0, false);

  TestableIOHomeCoverVentButton button;
  button.set_parent(&hub);
  button.set_device_id("ABC123");

  button.trigger_press();

  ASSERT_FALSE(hub.queued_operations().empty()) << "pressing the vent button should queue a hub operation";
  EXPECT_EQ(hub.queued_operations().back().type, IOHomeControlComponent::PendingOperationType::DEVICE_COMMAND)
      << "vent button should queue a DEVICE_COMMAND operation";
  EXPECT_EQ(hub.queued_operations().back().command, CoverCommand::VENT)
      << "vent button should queue CoverCommand::VENT";
  EXPECT_EQ(hub.queued_operations().back().device_id, "ABC123") << "vent button should target the configured device";
}

TEST(PlatformCoverVentButton, PressIsSafeWhenDeviceMissing) {
  VentButtonMockHub hub;

  TestableIOHomeCoverVentButton button;
  button.set_parent(&hub);
  button.set_device_id("ABC123");

  button.trigger_press();

  EXPECT_TRUE(hub.queued_operations().empty())
      << "vent button should not queue an operation when the device is not registered";
}

TEST(PlatformCoverVentButton, PressIsSafeWhenParentNull) {
  TestableIOHomeCoverVentButton button;
  button.set_device_id("ABC123");

  // Should not crash
  button.trigger_press();
}

// ========================================================================================
// device_supports_vent() tests
// ========================================================================================

TEST(DeviceProfile, VentSupportedForWindowTypes) {
  EXPECT_TRUE(device_supports_vent(DeviceType::WINDOW_OPENER)) << "window opener should support vent";
  EXPECT_TRUE(device_supports_vent(DeviceType::VENTILATION_POINT)) << "ventilation point should support vent";
}

TEST(DeviceProfile, VentNotSupportedForOtherTypes) {
  EXPECT_FALSE(device_supports_vent(DeviceType::ROLLER_SHUTTER)) << "roller shutter should not support vent";
  EXPECT_FALSE(device_supports_vent(DeviceType::AWNING)) << "awning should not support vent";
  EXPECT_FALSE(device_supports_vent(DeviceType::LIGHT)) << "light should not support vent";
  EXPECT_FALSE(device_supports_vent(DeviceType::VENETIAN_BLIND)) << "venetian blind should not support vent";
  EXPECT_FALSE(device_supports_vent(DeviceType::GARAGE_OPENER)) << "garage opener should not support vent";
  EXPECT_FALSE(device_supports_vent(DeviceType::UNKNOWN)) << "unknown should not support vent";
}
