#include "platform_cover_favorite_button.h"

#include "hub_core.h"

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

/// Only overrides the methods this test suite actually exercises; device registry, callback
/// fan-out, and every other command path are inherited from MockPlatformHubBase so they don't
/// need to be re-stubbed here (see test_helpers.h).
class FavoriteButtonMockHub : public test::MockPlatformHubBase {
 public:
  FavoriteButtonMockHub() = default;
  ~FavoriteButtonMockHub() override = default;

  bool set_device_position(const std::string &device_id, uint8_t position) override {
    last_set_device_id_ = device_id;
    last_set_position_ = position;
    return true;
  }

  void queue_set_device_position(const std::string &device_id, uint8_t position) override {
    last_set_device_id_ = device_id;
    last_set_position_ = position;
    queued_operations_.push_back({PendingOperationType::SET_POSITION, device_id, position});
  }
  bool queue_device_command(const std::string &device_id, CoverCommand cmd) override {
    last_set_device_id_ = device_id;
    PendingOperation op{};
    op.type = PendingOperationType::DEVICE_COMMAND;
    op.device_id = device_id;
    op.command = cmd;
    queued_operations_.push_back(op);
    return true;
  }

  const std::string &last_set_device_id() const { return last_set_device_id_; }
  uint8_t last_set_position() const { return last_set_position_; }
  const std::deque<PendingOperation> &queued_operations() const { return queued_operations_; }

 private:
  std::string last_set_device_id_;
  uint8_t last_set_position_{0};
  std::deque<PendingOperation> queued_operations_;
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