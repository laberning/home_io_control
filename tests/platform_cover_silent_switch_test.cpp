#include "platform_cover_controls.h"

#include "hub_core.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;

// ============================================================================
// PlatformCoverSilentSwitch test suite
// ============================================================================
// The generated per-cover travel-profile toggle. The switch is only created when a cover declares
// `silent:` in YAML; these tests cover what it does once it exists.

class TestableIOHomeCoverSilentSwitch : public IOHomeCoverSilentSwitch {
 public:
  void trigger_write(bool state) { this->write_state(state); }
};

/// Records the runtime setter and serves a device back, so the switch's boot-state publish and its
/// toggle path can both be observed. Everything else comes from MockPlatformHubBase.
class SilentSwitchMockHub : public test::MockPlatformHubBase {
 public:
  SilentSwitchMockHub() = default;
  ~SilentSwitchMockHub() override = default;

  void set_device_silent(const std::string &device_id, bool silent) override {
    last_device_id_ = device_id;
    last_silent_ = silent;
    calls_++;
    device_.silent = silent;
  }

  IoDevice *get_device(const std::string &device_id) override { return device_id == known_id_ ? &device_ : nullptr; }

  std::string last_device_id_;
  bool last_silent_{false};
  int calls_{0};
  std::string known_id_{"ABC123"};
  IoDevice device_{};
};

TEST(PlatformCoverSilentSwitch, ToggleOnSelectsSilentTravelForItsOwnDevice) {
  SilentSwitchMockHub hub;
  TestableIOHomeCoverSilentSwitch sw;
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");

  sw.trigger_write(true);

  EXPECT_EQ(hub.calls_, 1);
  EXPECT_EQ(hub.last_device_id_, "ABC123") << "the toggle must only affect the cover it belongs to";
  EXPECT_TRUE(hub.last_silent_);
  EXPECT_TRUE(sw.get_state()) << "the switch reports the state it just applied";
}

TEST(PlatformCoverSilentSwitch, ToggleOffRestoresNormalTravel) {
  SilentSwitchMockHub hub;
  hub.device_.silent = true;
  TestableIOHomeCoverSilentSwitch sw;
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");

  sw.trigger_write(false);

  EXPECT_EQ(hub.calls_, 1);
  EXPECT_FALSE(hub.last_silent_);
  EXPECT_FALSE(sw.get_state());
}

TEST(PlatformCoverSilentSwitch, SetupPublishesTheYamlDeclaredBootState) {
  // Restore mode is DISABLED precisely so this, not flash, decides the boot state — otherwise Home
  // Assistant could start out disagreeing with what the hub will actually transmit.
  SilentSwitchMockHub hub;
  hub.device_.silent = true;
  TestableIOHomeCoverSilentSwitch sw;
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");

  sw.setup();

  EXPECT_TRUE(sw.get_state()) << "boot state comes from the device the YAML already configured";
  EXPECT_EQ(hub.calls_, 0) << "publishing the existing state must not re-apply it";
}

TEST(PlatformCoverSilentSwitch, UnknownDeviceIsInertRatherThanCrashing) {
  SilentSwitchMockHub hub;
  hub.known_id_ = "OTHER1";
  TestableIOHomeCoverSilentSwitch sw;
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");

  sw.setup();  // get_device() returns nullptr

  EXPECT_EQ(hub.calls_, 0);
}
