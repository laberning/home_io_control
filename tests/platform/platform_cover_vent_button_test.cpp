#include "platform_cover_controls.h"
#include "proto_device_types.h"

#include "hub_core.h"

#include <deque>

#include "test_helpers.h"

using namespace esphome::home_io_control;

// ============================================================================
// PlatformCoverVentButton test suite
// ============================================================================
// Covers the generated ventilation-position button entity and its hub queueing behavior.

class TestableIOHomeCoverVentButton : public IOHomeCoverCommandButton {
 public:
  void trigger_press() { this->press_action(); }
};

/// Only overrides the methods this test suite actually exercises; device registry, callback
/// fan-out, and every other command path are inherited from MockPlatformHubBase so they don't
/// need to be re-stubbed here (see test_helpers.h).
class VentButtonMockHub : public test::MockPlatformHubBase {
 public:
  VentButtonMockHub() = default;
  ~VentButtonMockHub() override = default;

  bool queue_device_command(const std::string &device_id, CoverCommand cmd) override {
    last_device_id_ = device_id;
    last_command_ = cmd;
    PendingOperation op{};
    op.type = PendingOperationType::DEVICE_COMMAND;
    op.device_id = device_id;
    op.command = cmd;
    queued_operations_.push_back(op);
    return true;
  }
  void queue_set_device_position_and_tilt(const std::string &device_id, uint8_t position,
                                          uint8_t tilt_percent) override {
    (void) device_id;
    (void) position;
    (void) tilt_percent;
  }

  const std::string &last_device_id() const { return last_device_id_; }
  CoverCommand last_command() const { return last_command_; }
  const std::deque<PendingOperation> &queued_operations() const { return queued_operations_; }

 private:
  std::string last_device_id_;
  CoverCommand last_command_{CoverCommand::STOP};
  std::deque<PendingOperation> queued_operations_;
};

TEST(PlatformCoverVentButton, PressQueuesVentCommand) {
  VentButtonMockHub hub;
  hub.add_device("ABC123", {DeviceType::WINDOW_OPENER, 0});

  TestableIOHomeCoverVentButton button;
  button.set_parent(&hub);
  button.set_device_id("ABC123");
  button.set_command(CoverCommand::VENT);

  button.trigger_press();

  ASSERT_FALSE(hub.queued_operations().empty()) << "pressing the vent button should queue a hub operation";
  EXPECT_EQ(hub.queued_operations().back().type, PendingOperationType::DEVICE_COMMAND)
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
  button.set_command(CoverCommand::VENT);

  button.trigger_press();

  EXPECT_TRUE(hub.queued_operations().empty())
      << "vent button should not queue an operation when the device is not registered";
}

TEST(PlatformCoverVentButton, PressIsSafeWhenParentNull) {
  TestableIOHomeCoverVentButton button;
  button.set_device_id("ABC123");
  button.set_command(CoverCommand::VENT);

  // Should not crash
  button.trigger_press();
}

// ========================================================================================
// Ventilation position (device_feature::VENT) tests
// ========================================================================================

TEST(DeviceProfile, VentSupportedForWindowOpeners) {
  EXPECT_TRUE(has_feature(device_features(DeviceType::WINDOW_OPENER, 0), device_feature::VENT))
      << "window opener should support vent";
  EXPECT_FALSE(has_feature(device_features(DeviceType::VENTILATION_POINT, 0), device_feature::VENT))
      << "a ventilation point is not a cover, so it offers no ventilation position";
}

TEST(DeviceProfile, VentNotSupportedForOtherTypes) {
  EXPECT_FALSE(has_feature(device_features(DeviceType::ROLLER_SHUTTER, 0), device_feature::VENT))
      << "roller shutter should not support vent";
  EXPECT_FALSE(has_feature(device_features(DeviceType::VERTICAL_EXTERIOR_AWNING, 0), device_feature::VENT))
      << "awning should not support vent";
  EXPECT_FALSE(has_feature(device_features(DeviceType::LIGHT, 0), device_feature::VENT))
      << "light should not support vent";
  EXPECT_FALSE(has_feature(device_features(DeviceType::INTERIOR_VENETIAN_BLIND, 0), device_feature::VENT))
      << "venetian blind should not support vent";
  EXPECT_FALSE(has_feature(device_features(DeviceType::GARAGE_OPENER, 0), device_feature::VENT))
      << "garage opener should not support vent";
  EXPECT_FALSE(has_feature(device_features(DeviceType::UNKNOWN, 0), device_feature::VENT))
      << "unknown should not support vent";
}
