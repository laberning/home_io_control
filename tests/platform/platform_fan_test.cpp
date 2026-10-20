/// @file platform_fan_test.cpp
/// @brief Tests for the experimental exterior-heating fan platform.

#include "platform_fan.h"

#include "hub_core.h"

#include "test_helpers.h"

#include <string>
#include <vector>

using namespace esphome::home_io_control;
using namespace esphome::fan;

namespace {

class MockHubFan : public test::MockPlatformHubBase {
 public:
  struct Queued {
    std::string device_id;
    uint8_t position;
  };
  std::vector<Queued> queued;

  void queue_set_fan_position(const std::string &device_id, uint8_t position) override {
    this->queued.push_back({device_id, position});
  }
};

IOHomeFan make_fan(MockHubFan &hub, uint8_t subtype = 0, DeviceType type = DeviceType::EXTERIOR_HEATING) {
  IOHomeFan entity;
  entity.set_parent(&hub);
  entity.set_device_id("ABC123");
  entity.set_device_type(type);
  entity.set_subtype(subtype);
  return entity;
}

void set_position(MockHubFan &hub, float position) {
  IoDevice dev = *hub.get_device("ABC123");
  dev.position = position;
  dev.is_stopped = true;
  hub.trigger_device_update("ABC123", dev, /*cache_device=*/true);
}

}  // namespace

TEST(PlatformFan, FullRangeFanHasAHundredSpeedSteps) {
  MockHubFan hub;
  IOHomeFan entity = make_fan(hub, 0);
  const FanTraits traits = entity.get_traits();
  EXPECT_TRUE(traits.supports_speed());
  EXPECT_EQ(traits.supported_speed_count(), 100);
  EXPECT_FALSE(traits.supports_oscillation());
  EXPECT_FALSE(traits.supports_direction());
}

TEST(PlatformFan, TheMainParameterOnlyFrameDoesNotTakeTheLevelAway) {
  // 21/0 is sent the main parameter only like 21/58, but it still has a heat level to set.
  ASSERT_TRUE(device_takes_main_parameter_only(DeviceType::EXTERIOR_HEATING, 0));
  MockHubFan hub;
  EXPECT_TRUE(make_fan(hub, 0).get_traits().supports_speed());
}

TEST(PlatformFan, OnOffSubtypeAdvertisesNoSpeed) {
  MockHubFan hub;
  IOHomeFan entity = make_fan(hub, 58);
  EXPECT_FALSE(entity.get_traits().supports_speed());
}

TEST(PlatformFan, SpeedIsTheHeatLevelAndSendsTheInvertedPosition) {
  MockHubFan hub;
  IOHomeFan entity = make_fan(hub, 0);
  entity.setup();
  const struct {
    int speed;
    uint8_t position;
  } cases[] = {{1, 99}, {50, 50}, {99, 1}, {100, 0}};
  for (const auto &tc : cases) {
    auto call = entity.make_call();
    call.set_speed(tc.speed);
    call.perform();
    ASSERT_FALSE(hub.queued.empty());
    EXPECT_EQ(hub.queued.back().device_id, "ABC123");
    EXPECT_EQ(hub.queued.back().position, tc.position) << "heat " << tc.speed << " %";
    EXPECT_TRUE(entity.state);
    EXPECT_EQ(entity.speed, tc.speed);
  }
}

TEST(PlatformFan, OffSendsFullClosePosition) {
  MockHubFan hub;
  IOHomeFan entity = make_fan(hub, 0);
  entity.setup();
  auto call = entity.make_call();
  call.set_state(false);
  call.perform();
  ASSERT_EQ(hub.queued.size(), 1u);
  EXPECT_EQ(hub.queued[0].position, 100) << "0 % heat is position 100 (MP 0xC800)";
  EXPECT_FALSE(entity.state);
}

TEST(PlatformFan, OnWithoutALevelUsesTheLastLevelElseFullHeat) {
  MockHubFan hub;
  IOHomeFan entity = make_fan(hub, 0);
  entity.setup();

  auto first = entity.make_call();
  first.set_state(true);
  first.perform();
  EXPECT_EQ(hub.queued.back().position, 0) << "no level known: full heat";

  auto set = entity.make_call();
  set.set_speed(40);
  set.perform();
  auto off = entity.make_call();
  off.set_state(false);
  off.perform();
  auto on = entity.make_call();
  on.set_state(true);
  on.perform();
  EXPECT_EQ(hub.queued.back().position, 60) << "back to the last level, 40 % heat";
  EXPECT_EQ(entity.speed, 40);
}

TEST(PlatformFan, OnOffSubtypeIsFullHeatOrOff) {
  MockHubFan hub;
  IOHomeFan entity = make_fan(hub, 58);
  entity.setup();
  auto on = entity.make_call();
  on.set_state(true);
  on.perform();
  EXPECT_EQ(hub.queued.back().position, 0);
  auto off = entity.make_call();
  off.set_state(false);
  off.perform();
  EXPECT_EQ(hub.queued.back().position, 100);
}

TEST(PlatformFan, ARequestWithNothingInItSendsNothing) {
  MockHubFan hub;
  IOHomeFan entity = make_fan(hub, 0);
  entity.setup();
  auto call = entity.make_call();
  call.perform();
  EXPECT_TRUE(hub.queued.empty());
}

TEST(PlatformFan, AStatusReplyReplacesTheCommandedState) {
  MockHubFan hub;
  IOHomeFan entity = make_fan(hub, 0);
  entity.setup();
  auto call = entity.make_call();
  call.set_speed(80);
  call.perform();
  ASSERT_EQ(entity.speed, 80);

  set_position(hub, 70.0F);  // the heater says 30 % heat
  EXPECT_TRUE(entity.state);
  EXPECT_EQ(entity.speed, 30);

  set_position(hub, 0.0F);
  EXPECT_EQ(entity.speed, 100);

  const int published = entity.publish_count_;
  set_position(hub, 0.0F);
  EXPECT_EQ(entity.publish_count_, published) << "an unchanged reply publishes nothing";
}

TEST(PlatformFan, AnOffReplyKeepsTheLastLevelForTheNextTurnOn) {
  MockHubFan hub;
  IOHomeFan entity = make_fan(hub, 0);
  entity.setup();
  set_position(hub, 25.0F);  // 75 % heat
  set_position(hub, 100.0F);
  EXPECT_FALSE(entity.state);
  EXPECT_EQ(entity.speed, 75);
}

TEST(PlatformFan, AnUnknownPositionIsNotPublished) {
  MockHubFan hub;
  IOHomeFan entity = make_fan(hub, 0);
  entity.setup();
  set_position(hub, UNKNOWN_POSITION);
  EXPECT_EQ(entity.publish_count_, 0);
  EXPECT_FALSE(entity.state);
}

TEST(PlatformFan, OnlyItsOwnDeviceUpdatesTheEntity) {
  MockHubFan hub;
  IOHomeFan entity = make_fan(hub, 0);
  entity.setup();
  DeviceConfig config{};
  hub.add_device("DEF456", config);
  IoDevice other = *hub.get_device("DEF456");
  other.position = 0.0F;
  hub.trigger_device_update("DEF456", other, /*cache_device=*/true);
  EXPECT_EQ(entity.publish_count_, 0);
}

TEST(PlatformFan, AKnownDeviceOfAnotherFamilyFailsTheEntity) {
  MockHubFan hub;
  IOHomeFan cover = make_fan(hub, 0, DeviceType::ROLLER_SHUTTER);
  cover.setup();
  EXPECT_TRUE(cover.is_failed());

  IOHomeFan climate = make_fan(hub, 1, DeviceType::ELECTRICAL_HEATER);
  climate.setup();
  EXPECT_TRUE(climate.is_failed()) << "an electrical heater is a climate entity";

  IOHomeFan unknown = make_fan(hub, 0, DeviceType::UNKNOWN);
  unknown.setup();
  EXPECT_FALSE(unknown.is_failed()) << "an unknown type is left for discovery";
}

TEST(PlatformFan, SystemKeyOptionReachesTheRegisteredDevice) {
  MockHubFan hub;
  IOHomeFan entity = make_fan(hub, 0);
  entity.set_system_key("00112233445566778899AABBCCDDEEFF");
  entity.setup();
  const IoDevice *dev = hub.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_TRUE(dev->has_system_key);
}
