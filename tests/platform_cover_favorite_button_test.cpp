#include "platform_cover_favorite_button.h"

#include "hub_core.h"
#include "proto_frame.h"

#include <deque>

#include "test_helpers.h"

using namespace esphome::home_io_control;

// ============================================================================
// PlatformCoverFavoriteButton test suite
// ============================================================================
// Covers the generated favorite-position button entity and its hub queueing behavior.

class TestableIOHomeCoverFavoriteButton : public IOHomeCoverFavoriteButton {
 public:
  void trigger_press() { this->press_action(); }
};

class FavoriteButtonMockHub : public IOHomeControlComponent {
 public:
  FavoriteButtonMockHub() = default;
  ~FavoriteButtonMockHub() override = default;

  bool set_device_position(const std::string &device_id, uint8_t position) override {
    last_set_device_id_ = device_id;
    last_set_position_ = position;
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
    last_set_device_id_ = device_id;
    last_set_position_ = position;
    queued_operations_.push_back({PendingOperationType::SET_POSITION, device_id, position});
  }
  void queue_device_command(const std::string &device_id, CoverCommand cmd) override {
    last_set_device_id_ = device_id;
    PendingOperation op{};
    op.type = PendingOperationType::DEVICE_COMMAND;
    op.device_id = device_id;
    op.command = cmd;
    queued_operations_.push_back(op);
  }
  void queue_set_device_tilt(const std::string &device_id, uint8_t tilt_percent) override {
    (void) device_id;
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

  const std::string &last_set_device_id() const { return last_set_device_id_; }
  uint8_t last_set_position() const { return last_set_position_; }
  const std::deque<PendingOperation> &queued_operations() const { return queued_operations_; }

 private:
  std::string last_set_device_id_;
  uint8_t last_set_position_{0};
  std::deque<PendingOperation> queued_operations_;
  std::map<std::string, IoDevice> devices_;
  std::vector<DeviceUpdateCallback> callbacks_;
};

TEST(PlatformCoverFavoriteButton, PressQueuesFavoritePosition) {
  FavoriteButtonMockHub hub;
  hub.add_device("ABC123", DeviceType::AWNING, 0, false);

  TestableIOHomeCoverFavoriteButton button;
  button.set_parent(&hub);
  button.set_device_id("ABC123");

  button.trigger_press();

  ASSERT_FALSE(hub.queued_operations().empty()) << "pressing the favorite button should queue a hub operation";
  EXPECT_EQ(hub.queued_operations().back().type, PendingOperationType::DEVICE_COMMAND)
      << "favorite button should queue a DEVICE_COMMAND operation";
  EXPECT_EQ(hub.queued_operations().back().command, CoverCommand::FAVORITE)
      << "favorite button should queue CoverCommand::FAVORITE";
  EXPECT_EQ(hub.queued_operations().back().device_id, "ABC123")
      << "favorite button should target the configured device";
}

TEST(PlatformCoverFavoriteButton, PressIsSafeWhenDeviceMissing) {
  FavoriteButtonMockHub hub;

  TestableIOHomeCoverFavoriteButton button;
  button.set_parent(&hub);
  button.set_device_id("ABC123");

  button.trigger_press();

  EXPECT_TRUE(hub.queued_operations().empty())
      << "favorite button should not queue an operation when the device is not registered";
}