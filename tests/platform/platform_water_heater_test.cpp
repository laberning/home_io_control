/// @file platform_water_heater_test.cpp
/// @brief Tests for the water heater entity (platform_water_heater.h), its companion sensors and its boost switch.

#include "platform_water_heater.h"
#include "hub_core.h"

#include "test_helpers.h"

#include <cmath>
#include <string>
#include <vector>

using namespace esphome::home_io_control;
using namespace esphome::water_heater;

namespace {

class MockHubWaterHeater : public test::MockPlatformHubBase {
 public:
  std::vector<DhwMode> modes;  ///< Modes sent through set_water_heater_mode(), in order.
  std::vector<float> targets;  ///< Targets sent through set_water_heater_target(), in order.
  std::vector<bool> boosts;    ///< Values sent through set_water_heater_boost(), in order.
  std::vector<bool> aways;     ///< Values sent through set_water_heater_away(), in order.
  bool away_result{true};
  /// What the status read after an away write finds; -1 for no read-back.
  int8_t away_read_back{-1};
  bool mode_result{true};
  bool target_result{true};
  bool boost_result{true};
  /// What the read-back of a mode write finds; -1 for no read-back (the heater did not answer it).
  int16_t mode_read_back{-1};
  /// What the read-back of a boost write finds; -1 for no read-back.
  int8_t boost_read_back{-1};

  bool set_water_heater_mode(const std::string &device_id, DhwMode mode) override {
    this->modes.push_back(mode);
    if (this->mode_result && this->mode_read_back >= 0) {
      // What the real hub does on a read-back: store it on the record and notify.
      if (IoDevice *dev = this->registry_.get(device_id); dev != nullptr) {
        dev->dhw.mode = this->mode_read_back;
        this->registry_.notify(device_id);
      }
    }
    return this->mode_result;
  }

  bool set_water_heater_target(const std::string & /*device_id*/, float celsius) override {
    this->targets.push_back(celsius);
    return this->target_result;
  }

  bool set_water_heater_away(const std::string &device_id, bool on) override {
    this->aways.push_back(on);
    if (this->away_result && this->away_read_back >= 0) {
      if (IoDevice *dev = this->registry_.get(device_id); dev != nullptr) {
        dev->dhw.away = this->away_read_back;
        this->registry_.notify(device_id);
      }
    }
    return this->away_result;
  }

  bool set_water_heater_boost(const std::string &device_id, bool on) override {
    this->boosts.push_back(on);
    if (this->boost_result && this->boost_read_back >= 0) {
      if (IoDevice *dev = this->registry_.get(device_id); dev != nullptr) {
        dev->dhw.boost = this->boost_read_back;
        this->registry_.notify(device_id);
      }
    }
    return this->boost_result;
  }
};

class TestableBoostSwitch : public IOHomeDhwBoostSwitch {
 public:
  void trigger_write(bool state) { this->write_state(state); }
};

/// Bind `sw` to "ABC123" and set it up.
void bind_boost(TestableBoostSwitch &sw, MockHubWaterHeater &hub) {
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");
  sw.setup();
}

/// Bind `entity` to "ABC123", declared as io_device_type: domestic_hot_water, and set it up. In place: setup()
/// registers a callback on the entity's address.
void bind_water_heater(IOHomeWaterHeater &entity, MockHubWaterHeater &hub) {
  entity.set_parent(&hub);
  entity.set_device_id("ABC123");
  entity.set_device_type(DeviceType::DOMESTIC_HOT_WATER);
  entity.setup();
}

/// Publish readings for "ABC123" as if a poll had just stored them.
void publish(MockHubWaterHeater &hub, const DhwReadings &readings) {
  IoDevice dev = *hub.get_device("ABC123");
  dev.dhw = readings;
  hub.trigger_device_update("ABC123", dev, /*cache_device=*/true);
}

}  // namespace

// === Mode mapping ===

TEST(PlatformWaterHeater, TheThreeHeaterModesMapBothWays) {
  EXPECT_EQ(water_heater_mode_for(DhwMode::AUTO), WATER_HEATER_MODE_HEAT_PUMP);
  EXPECT_EQ(water_heater_mode_for(DhwMode::MANUAL_ECO_ON), WATER_HEATER_MODE_ECO);
  EXPECT_EQ(water_heater_mode_for(DhwMode::MANUAL_ECO_OFF), WATER_HEATER_MODE_PERFORMANCE);
  for (const DhwMode mode : {DhwMode::AUTO, DhwMode::MANUAL_ECO_ON, DhwMode::MANUAL_ECO_OFF}) {
    DhwMode back = DhwMode::AUTO;
    ASSERT_TRUE(dhw_mode_for(water_heater_mode_for(mode), back));
    EXPECT_EQ(back, mode);
  }
  DhwMode untouched = DhwMode::MANUAL_ECO_ON;
  for (const WaterHeaterMode mode :
       {WATER_HEATER_MODE_OFF, WATER_HEATER_MODE_ELECTRIC, WATER_HEATER_MODE_HIGH_DEMAND, WATER_HEATER_MODE_GAS}) {
    EXPECT_FALSE(dhw_mode_for(mode, untouched)) << static_cast<unsigned>(mode);
  }
  EXPECT_EQ(untouched, DhwMode::MANUAL_ECO_ON);
}

// === Traits and setup ===

TEST(PlatformWaterHeater, TraitsOfferTheReadingsTheThreeModesAndTheHeatersRange) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  const WaterHeaterTraits traits = entity.get_traits();

  EXPECT_TRUE(traits.has_feature_flags(WATER_HEATER_SUPPORTS_CURRENT_TEMPERATURE));
  EXPECT_TRUE(traits.has_feature_flags(WATER_HEATER_SUPPORTS_TARGET_TEMPERATURE));
  EXPECT_TRUE(traits.has_feature_flags(WATER_HEATER_SUPPORTS_OPERATION_MODE));
  EXPECT_TRUE(traits.has_feature_flags(WATER_HEATER_SUPPORTS_AWAY_MODE)) << "away mode is offered (experimental)";
  EXPECT_FALSE(traits.has_feature_flags(WATER_HEATER_SUPPORTS_ON_OFF)) << "no off state has been seen";
  EXPECT_FLOAT_EQ(traits.get_min_temperature(), 20.0F);
  EXPECT_FLOAT_EQ(traits.get_max_temperature(), 62.0F);
  EXPECT_FLOAT_EQ(traits.get_target_temperature_step(), 1.0F) << "every target the heater shows is a whole degree";
  EXPECT_TRUE(traits.supports_mode(WATER_HEATER_MODE_HEAT_PUMP));
  EXPECT_TRUE(traits.supports_mode(WATER_HEATER_MODE_ECO));
  EXPECT_TRUE(traits.supports_mode(WATER_HEATER_MODE_PERFORMANCE));
  EXPECT_FALSE(traits.supports_mode(WATER_HEATER_MODE_OFF));
}

TEST(PlatformWaterHeater, SetupRegistersTheDeviceAsAWaterHeater) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  const IoDevice *dev = hub.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FALSE(entity.is_failed());
  EXPECT_EQ(dev->entity_class, DeviceCapabilityClass::WATER_HEATER);
  EXPECT_TRUE(has_feature(dev->features, device_feature::WATER_HEATER));
}

TEST(PlatformWaterHeater, SetupFailsAKindThatIsNoWaterHeater) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  entity.set_parent(&hub);
  entity.set_device_id("ABC123");
  entity.set_device_type(DeviceType::ROLLER_SHUTTER);
  entity.setup();
  EXPECT_TRUE(entity.is_failed());
  EXPECT_EQ(hub.get_device("ABC123"), nullptr) << "a misconfigured binding never reaches the registry";
}

// === Readings ===

TEST(PlatformWaterHeater, AReadingIsPublishedWithItsMappedMode) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  const int published_before = entity.publish_count_;

  DhwReadings readings;
  readings.water_c = 58.1F;
  readings.target_c = 54.0F;
  readings.mode = static_cast<int16_t>(DhwMode::MANUAL_ECO_ON);
  publish(hub, readings);

  EXPECT_FLOAT_EQ(entity.get_current_temperature(), 58.1F);
  EXPECT_FLOAT_EQ(entity.get_target_temperature(), 54.0F);
  EXPECT_EQ(entity.get_mode(), WATER_HEATER_MODE_ECO);
  EXPECT_EQ(entity.publish_count_, published_before + 1);

  publish(hub, readings);
  EXPECT_EQ(entity.publish_count_, published_before + 1) << "the same readings publish nothing";

  readings.target_c = NAN;  // a main parameter that was not a temperature
  publish(hub, readings);
  EXPECT_TRUE(std::isnan(entity.get_target_temperature()));
  EXPECT_EQ(entity.publish_count_, published_before + 2);
}

TEST(PlatformWaterHeater, AutomaticModeShowsNoTargetAndManualShowsTheReadOne) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);

  // In AUTO the heater still answers a main parameter (60 C), but it chooses its own target.
  DhwReadings readings;
  readings.target_c = 60.0F;
  readings.mode = static_cast<int16_t>(DhwMode::AUTO);
  publish(hub, readings);
  EXPECT_EQ(entity.get_mode(), WATER_HEATER_MODE_HEAT_PUMP);
  EXPECT_TRUE(std::isnan(entity.get_target_temperature()));

  // Manual: the mode write's status read brings the heater's manual target.
  readings.mode = static_cast<int16_t>(DhwMode::MANUAL_ECO_ON);
  readings.target_c = 54.0F;
  publish(hub, readings);
  EXPECT_FLOAT_EQ(entity.get_target_temperature(), 54.0F);

  // Back to AUTO: unknown again, even though the reading still holds a value.
  const int published = entity.publish_count_;
  readings.mode = static_cast<int16_t>(DhwMode::AUTO);
  publish(hub, readings);
  EXPECT_TRUE(std::isnan(entity.get_target_temperature()));
  EXPECT_EQ(entity.publish_count_, published + 1);

  // Manual again with the same reading: the target comes back.
  readings.mode = static_cast<int16_t>(DhwMode::MANUAL_ECO_OFF);
  publish(hub, readings);
  EXPECT_FLOAT_EQ(entity.get_target_temperature(), 54.0F);
}

TEST(PlatformWaterHeater, UpdatesForAnotherDeviceAreIgnored) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  DeviceConfig other;
  other.type = DeviceType::DOMESTIC_HOT_WATER;
  other.entity_class = DeviceCapabilityClass::WATER_HEATER;
  hub.add_device("DEF456", other);
  IoDevice dev = *hub.get_device("DEF456");
  dev.dhw.water_c = 40.0F;
  hub.trigger_device_update("DEF456", dev, true);
  EXPECT_TRUE(std::isnan(entity.get_current_temperature()));
}

// === Control ===

TEST(PlatformWaterHeater, AModeIsSentAndTheReadBackIsWhatIsShown) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  hub.mode_read_back = static_cast<int16_t>(DhwMode::AUTO);  // the heater did not take the new mode

  auto call = entity.make_call();
  call.set_mode(WATER_HEATER_MODE_PERFORMANCE);
  call.perform();

  ASSERT_EQ(hub.modes.size(), 1u);
  EXPECT_EQ(hub.modes[0], DhwMode::MANUAL_ECO_OFF);
  EXPECT_EQ(entity.get_mode(), WATER_HEATER_MODE_HEAT_PUMP) << "the read-back wins over the command";
}

TEST(PlatformWaterHeater, BeforeAnyReadingAnAcceptedWriteShowsTheCommand) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);  // no read-back configured

  auto call = entity.make_call();
  call.set_mode(WATER_HEATER_MODE_ECO);
  call.set_target_temperature(55.0F);
  call.perform();

  ASSERT_EQ(hub.targets.size(), 1u);
  EXPECT_FLOAT_EQ(hub.targets[0], 55.0F);
  EXPECT_EQ(entity.get_mode(), WATER_HEATER_MODE_ECO);
  EXPECT_FLOAT_EQ(entity.get_target_temperature(), 55.0F);
}

TEST(PlatformWaterHeater, AfterAReadingAnAcceptedTargetWaitsForTheSettlePoll) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  DhwReadings readings;
  readings.target_c = 60.0F;
  publish(hub, readings);

  auto call = entity.make_call();
  call.set_target_temperature(50.0F);
  call.perform();

  ASSERT_EQ(hub.targets.size(), 1u);
  EXPECT_FLOAT_EQ(entity.get_target_temperature(), 60.0F) << "the settle poll decides what is shown";
}

TEST(PlatformWaterHeater, ATargetIsNotSentWhileTheHeaterHoldsAutomaticMode) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  DhwReadings readings;
  readings.target_c = 60.0F;
  readings.mode = static_cast<int16_t>(DhwMode::AUTO);
  publish(hub, readings);

  auto call = entity.make_call();
  call.set_target_temperature(50.0F);
  call.perform();

  EXPECT_TRUE(hub.targets.empty()) << "the heater chooses its own target in AUTO";
  EXPECT_TRUE(std::isnan(entity.get_target_temperature()));
}

TEST(PlatformWaterHeater, AFailedModeWriteKeepsTheTargetFromBeingSentInAutomaticMode) {
  MockHubWaterHeater hub;
  hub.mode_result = false;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  DhwReadings readings;
  readings.mode = static_cast<int16_t>(DhwMode::AUTO);
  publish(hub, readings);

  auto call = entity.make_call();
  call.set_mode(WATER_HEATER_MODE_ECO);
  call.set_target_temperature(54.0F);
  call.perform();

  ASSERT_EQ(hub.modes.size(), 1u);
  EXPECT_TRUE(hub.targets.empty()) << "decided on the mode the heater holds, not the one asked for";
}

TEST(PlatformWaterHeater, AModeChangeToManualLetsTheTargetOfTheSameCallThrough) {
  MockHubWaterHeater hub;
  hub.mode_read_back = static_cast<int16_t>(DhwMode::MANUAL_ECO_ON);
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  DhwReadings readings;
  readings.mode = static_cast<int16_t>(DhwMode::AUTO);
  publish(hub, readings);

  auto call = entity.make_call();
  call.set_mode(WATER_HEATER_MODE_ECO);
  call.set_target_temperature(54.0F);
  call.perform();

  ASSERT_EQ(hub.targets.size(), 1u);
  EXPECT_FLOAT_EQ(hub.targets[0], 54.0F);
}

TEST(PlatformWaterHeater, ARefusedWriteChangesNothing) {
  MockHubWaterHeater hub;
  hub.mode_result = false;
  hub.target_result = false;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  const int published_before = entity.publish_count_;

  auto call = entity.make_call();
  call.set_mode(WATER_HEATER_MODE_ECO);
  call.set_target_temperature(55.0F);
  call.perform();

  EXPECT_EQ(entity.get_mode(), WATER_HEATER_MODE_OFF) << "the entity's initial state, untouched";
  EXPECT_TRUE(std::isnan(entity.get_target_temperature()));
  EXPECT_EQ(entity.publish_count_, published_before);
}

TEST(PlatformWaterHeater, AModeTheHeaterDoesNotOfferIsNeverSent) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  auto call = entity.make_call();
  call.set_mode(WATER_HEATER_MODE_OFF);
  call.perform();
  EXPECT_TRUE(hub.modes.empty());

  // A direct control() that skips the traits check is refused by the mapping too.
  WaterHeaterCall direct(&entity);
  direct.set_mode(WATER_HEATER_MODE_GAS);
  entity.control(direct);
  EXPECT_TRUE(hub.modes.empty());
}

// === Away mode ===

TEST(PlatformWaterHeater, AwayIsSentToTheHubAndTheHeatersReportIsWhatIsShown) {
  MockHubWaterHeater hub;
  hub.away_read_back = 1;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  EXPECT_FALSE(entity.is_away());

  auto call = entity.make_call();
  call.set_away(true);
  call.perform();

  ASSERT_EQ(hub.aways.size(), 1u);
  EXPECT_TRUE(hub.aways[0]);
  EXPECT_TRUE(entity.is_away()) << "FP16 reported it";

  hub.away_read_back = 0;
  auto off = entity.make_call();
  off.set_away(false);
  off.perform();
  ASSERT_EQ(hub.aways.size(), 2u);
  EXPECT_FALSE(hub.aways[1]);
  EXPECT_FALSE(entity.is_away());
}

TEST(PlatformWaterHeater, AwayIsNeverShownFromTheCommandAlone) {
  MockHubWaterHeater hub;  // the heater reports nothing back
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);

  auto call = entity.make_call();
  call.set_away(true);
  call.perform();

  ASSERT_EQ(hub.aways.size(), 1u);
  EXPECT_FALSE(entity.is_away());
}

TEST(PlatformWaterHeater, AwayFollowsTheHeatersReportAndPublishesOnlyAChange) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  DhwReadings readings;
  readings.away = 1;  // a holiday set on the display
  const int before = entity.publish_count_;
  publish(hub, readings);
  EXPECT_TRUE(entity.is_away());
  EXPECT_EQ(entity.publish_count_, before + 1);

  publish(hub, readings);
  EXPECT_EQ(entity.publish_count_, before + 1) << "the same report publishes nothing";

  readings.away = 0;  // the holiday ended
  publish(hub, readings);
  EXPECT_FALSE(entity.is_away());
  EXPECT_EQ(entity.publish_count_, before + 2);
}

TEST(PlatformWaterHeater, AwayShowsNoTargetAndTheTargetComesBackAfterIt) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  DhwReadings readings;
  readings.mode = static_cast<int16_t>(DhwMode::MANUAL_ECO_ON);
  readings.target_c = 54.0F;
  publish(hub, readings);
  ASSERT_FLOAT_EQ(entity.get_target_temperature(), 54.0F);

  // During a holiday the heater reports main parameter 0, which decodes as the bottom of the range (issue #198).
  readings.away = 1;
  readings.target_c = 20.0F;
  publish(hub, readings);
  EXPECT_TRUE(std::isnan(entity.get_target_temperature())) << "20 C while away is no target";

  readings.away = 0;
  readings.target_c = 54.0F;
  publish(hub, readings);
  EXPECT_FLOAT_EQ(entity.get_target_temperature(), 54.0F);
}

TEST(PlatformWaterHeater, ARefusedAwayWriteChangesNothing) {
  MockHubWaterHeater hub;
  hub.away_result = false;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  auto call = entity.make_call();
  call.set_away(true);
  call.perform();
  EXPECT_EQ(hub.aways.size(), 1u);
  EXPECT_FALSE(entity.is_away());
}

TEST(PlatformWaterHeater, ATargetIsNotSentWhileAway) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  DhwReadings readings;
  readings.away = 1;
  readings.mode = static_cast<int16_t>(DhwMode::MANUAL_ECO_ON);
  publish(hub, readings);

  auto call = entity.make_call();
  call.set_target_temperature(50.0F);
  call.perform();

  EXPECT_TRUE(hub.targets.empty()) << "logged instead of sent";
}

TEST(PlatformWaterHeater, ATargetInTheCallThatStartsAwayIsNotSent) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);

  auto call = entity.make_call();
  call.set_away(true);
  call.set_target_temperature(50.0F);
  call.perform();

  EXPECT_EQ(hub.aways.size(), 1u);
  EXPECT_TRUE(hub.targets.empty());
}

TEST(PlatformWaterHeater, ATargetInTheCallThatEndsAwayIsSent) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  DhwReadings readings;
  readings.away = 1;
  publish(hub, readings);

  auto call = entity.make_call();
  call.set_away(false);
  call.set_target_temperature(50.0F);
  call.perform();

  EXPECT_EQ(hub.aways.size(), 1u);
  ASSERT_EQ(hub.targets.size(), 1u);
  EXPECT_FLOAT_EQ(hub.targets[0], 50.0F);
}

// === Companions ===

TEST(PlatformWaterHeater, EnergyIsPublishedInKilowattHoursOnceRead) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  IOHomeDhwEnergySensor energy;
  energy.set_parent(&hub);
  energy.set_device_id("ABC123");
  energy.setup();
  EXPECT_TRUE(std::isnan(energy.state)) << "nothing before the first read";

  DhwReadings readings;
  readings.energy_wh = 1831746;
  publish(hub, readings);
  EXPECT_FLOAT_EQ(energy.state, 1831.746F);
}

TEST(PlatformWaterHeater, EachBinaryCompanionShowsItsReadingAndNothingBeforeIt) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  IOHomeDhwBinarySensor heat_pump;
  IOHomeDhwBinarySensor backup;
  heat_pump.set_reading(IOHomeDhwBinarySensor::Reading::HEAT_PUMP);
  backup.set_reading(IOHomeDhwBinarySensor::Reading::ELECTRIC_BACKUP);
  for (IOHomeDhwBinarySensor *sensor : {&heat_pump, &backup}) {
    sensor->set_parent(&hub);
    sensor->set_device_id("ABC123");
    sensor->setup();
    EXPECT_FALSE(sensor->has_state());
  }

  // The reporter's heat-pump-only state: heating, element off.
  DhwReadings readings;
  readings.heat_pump = 1;
  readings.element = 0;
  publish(hub, readings);
  EXPECT_TRUE(heat_pump.has_state());
  EXPECT_TRUE(heat_pump.state);
  EXPECT_TRUE(backup.has_state());
  EXPECT_FALSE(backup.state);

  readings.element = 1;
  publish(hub, readings);
  EXPECT_TRUE(backup.state);
}

// === Boost switch ===

TEST(PlatformDhwBoostSwitch, SetupWritesNothingAndShowsNothingBeforeTheFirstRead) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  TestableBoostSwitch sw;
  bind_boost(sw, hub);

  EXPECT_TRUE(hub.boosts.empty()) << "a reboot must not end a running boost";
  EXPECT_FALSE(sw.get_state());
}

TEST(PlatformDhwBoostSwitch, FollowsTheOperatingModeReading) {
  MockHubWaterHeater hub;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  TestableBoostSwitch sw;
  bind_boost(sw, hub);

  DhwReadings readings;
  readings.boost = 1;
  publish(hub, readings);
  EXPECT_TRUE(sw.get_state());
  readings.boost = 0;
  publish(hub, readings);
  EXPECT_FALSE(sw.get_state());
  EXPECT_TRUE(hub.boosts.empty()) << "showing a reading writes nothing";
}

TEST(PlatformDhwBoostSwitch, AWriteCallsTheHubAndTheReadBackIsShown) {
  MockHubWaterHeater hub;
  hub.boost_read_back = 1;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  TestableBoostSwitch sw;
  bind_boost(sw, hub);

  sw.trigger_write(true);
  ASSERT_EQ(hub.boosts.size(), 1u);
  EXPECT_TRUE(hub.boosts[0]);
  EXPECT_TRUE(sw.get_state()) << "shown from the read-back";

  hub.boost_read_back = 1;  // the heater kept its boost
  sw.trigger_write(false);
  ASSERT_EQ(hub.boosts.size(), 2u);
  EXPECT_FALSE(hub.boosts[1]);
  EXPECT_TRUE(sw.get_state()) << "the heater's answer wins over the command";
}

TEST(PlatformDhwBoostSwitch, AnUnacknowledgedWriteIsNotShown) {
  MockHubWaterHeater hub;
  hub.boost_result = false;
  IOHomeWaterHeater entity;
  bind_water_heater(entity, hub);
  TestableBoostSwitch sw;
  bind_boost(sw, hub);

  sw.trigger_write(true);
  EXPECT_EQ(hub.boosts.size(), 1u);
  EXPECT_FALSE(sw.get_state());
}
