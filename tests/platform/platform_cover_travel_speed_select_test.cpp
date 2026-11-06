#include "platform_cover_controls.h"

#include "hub_core.h"

#include "test_helpers.h"

#include <string>
#include <utility>
#include <vector>

using namespace esphome::home_io_control;

// ============================================================================
// PlatformCoverTravelSpeedSelect test suite
// ============================================================================
// The generated per-cover travel speed select. It is only created when a cover declares
// `travel_speed:` in YAML; these tests cover what it does once it exists.

class TestableIOHomeCoverTravelSpeedSelect : public IOHomeCoverTravelSpeedSelect {
 public:
  void trigger_control(const std::string &value) { this->control(value); }
};

/// Records the runtime setter and serves a device back, so the select's boot-state publish and its
/// control path can both be observed. Everything else comes from MockPlatformHubBase.
class TravelSpeedSelectMockHub : public test::MockPlatformHubBase {
 public:
  TravelSpeedSelectMockHub() = default;
  ~TravelSpeedSelectMockHub() override = default;

  void set_device_travel_speed(const std::string &device_id, TravelSpeed speed) override {
    last_device_id_ = device_id;
    last_speed_ = speed;
    calls_++;
    device_.travel_speed = speed;
  }

  IoDevice *get_device(const std::string &device_id) override { return device_id == known_id_ ? &device_ : nullptr; }

  std::string last_device_id_;
  TravelSpeed last_speed_{TravelSpeed::FAST};
  int calls_{0};
  std::string known_id_{"ABC123"};
  IoDevice device_{};
};

TEST(PlatformCoverTravelSpeedSelect, EachOptionSetsItsSpeedOnItsOwnDevice) {
  const std::vector<std::pair<std::string, TravelSpeed>> options = {
      {"default", TravelSpeed::MOTOR_DEFAULT},
      {"silent", TravelSpeed::SILENT},
      {"fast", TravelSpeed::FAST},
  };
  for (const auto &[option, speed] : options) {
    TravelSpeedSelectMockHub hub;
    TestableIOHomeCoverTravelSpeedSelect speed_select;
    speed_select.set_parent(&hub);
    speed_select.set_device_id("ABC123");

    speed_select.trigger_control(option);

    EXPECT_EQ(hub.calls_, 1) << option;
    EXPECT_EQ(hub.last_device_id_, "ABC123") << "the select must only affect the cover it belongs to";
    EXPECT_EQ(hub.last_speed_, speed) << option;
    EXPECT_EQ(speed_select.get_state(), option) << "the select reports the option it just applied";
  }
}

TEST(PlatformCoverTravelSpeedSelect, AnUnknownOptionChangesNothing) {
  TravelSpeedSelectMockHub hub;
  TestableIOHomeCoverTravelSpeedSelect speed_select;
  speed_select.set_parent(&hub);
  speed_select.set_device_id("ABC123");

  speed_select.trigger_control("turbo");

  EXPECT_EQ(hub.calls_, 0);
  EXPECT_EQ(speed_select.get_state(), "") << "nothing was applied, so nothing is published";
}

TEST(PlatformCoverTravelSpeedSelect, SetupPublishesTheYamlDeclaredBootState) {
  TravelSpeedSelectMockHub hub;
  hub.device_.travel_speed = TravelSpeed::MOTOR_DEFAULT;
  TestableIOHomeCoverTravelSpeedSelect speed_select;
  speed_select.set_parent(&hub);
  speed_select.set_device_id("ABC123");

  speed_select.setup();

  EXPECT_EQ(speed_select.get_state(), "default") << "boot state comes from the device the YAML already configured";
  EXPECT_EQ(hub.calls_, 0) << "publishing the existing state must not re-apply it";
}

TEST(PlatformCoverTravelSpeedSelect, UnknownDeviceIsInertRatherThanCrashing) {
  TravelSpeedSelectMockHub hub;
  hub.known_id_ = "OTHER1";
  TestableIOHomeCoverTravelSpeedSelect speed_select;
  speed_select.set_parent(&hub);
  speed_select.set_device_id("ABC123");

  speed_select.setup();  // get_device() returns nullptr

  EXPECT_EQ(hub.calls_, 0);
  EXPECT_EQ(speed_select.get_state(), "");
}
