/// @file platform_climate_controls_test.cpp
/// @brief Tests for the generated per-heater companions: the window sensor, the open-window detection
/// switch, the heating-level sensor and the central-heating-mode sensor.

#include "platform_climate_controls.h"
#include "reading_store.h"
#include "platform_device_readings.h"

#include "hub_port_doubles.h"

#include <gtest/gtest.h>

#include <map>

using namespace esphome::home_io_control;

namespace {

/// Serves a heater: the device port holds it, the heating port records the register traffic and answers
/// reads from a script. A test scripts what the heater's record holds after a read of a register with
/// heater_holds(): the read answers the value, which is stored on the record with one notification, as a decoded
/// reply is (HeaterController ReadHeatingRegisterNotifiesTheEntities).
class ClimateControlsHub : public test::DevicePortDouble, public test::HeatingPortDouble {
 public:
  bool read_heating_register(const std::string &device_id, DeviceRegister fn, float *value_out,
                             uint8_t max_tries) override {
    const bool read = HeatingPortDouble::read_heating_register(device_id, fn, value_out, max_tries);
    const auto held = this->held_after_read_.find(fn);
    if (held != this->held_after_read_.end()) {
      if (IoDevice *dev = this->registry_.get(device_id); dev != nullptr) {
        test::store_reading(*dev, fn, held->second);
        this->registry_.notify(device_id);
      }
    }
    return read;
  }

  /// A read of `fn` answers `value`, and the heater's record holds it afterwards.
  void heater_holds(DeviceRegister fn, float value) {
    this->read_replies[fn] = value;
    this->held_after_read_[fn] = value;
  }

  void add_heater() {
    DeviceConfig config{};
    config.type = DeviceType::ELECTRICAL_HEATER;
    config.subtype = 1;
    this->add_device("ABC123", config);
  }

  void deliver(const std::function<void(IoDevice &)> &edit) {
    IoDevice dev = *this->get_device("ABC123");
    edit(dev);
    this->trigger_device_update("ABC123", dev, /*cache_device=*/true);
  }

 private:
  std::map<DeviceRegister, float> held_after_read_;
};

class TestableDetectionSwitch : public IOHomeHeaterSettingSwitch {
 public:
  explicit TestableDetectionSwitch(DeviceReading reading = DeviceReading::OPEN_WINDOW_DETECTION) {
    this->set_reading(reading);
  }
  void trigger_write(bool state) { this->write_state(state); }
};

/// A numeric reading sensor on `reading`.
class NumberSensor : public IOHomeDeviceReadingSensor {
 public:
  explicit NumberSensor(DeviceReading reading) { this->set_reading(reading); }
};

/// The sensor of a climate entry's `remaining_time:` key.
class RemainingTimeSensor : public IOHomeDeviceReadingSensor {
 public:
  RemainingTimeSensor() { this->set_reading(DeviceReading::REMAINING_TIME); }
};

/// A binary reading sensor on `reading`.
class RegisterSensor : public IOHomeDeviceReadingBinarySensor {
 public:
  explicit RegisterSensor(DeviceReading reading) { this->set_reading(reading); }
};

/// The text sensor of a climate entry's `heating_level:` key.
class LevelSensor : public IOHomeDeviceReadingTextSensor {
 public:
  LevelSensor() { this->set_reading(DeviceReading::HEATING_LEVEL); }
};

}  // namespace

TEST(PlatformHeatingLevelSensor, HasNoStateUntilTheHeaterHasReportedALevel) {
  ClimateControlsHub hub;
  hub.add_heater();
  LevelSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  EXPECT_TRUE(sensor.state.empty());

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::ROOM_TEMPERATURE, 20.0F); });
  EXPECT_TRUE(sensor.state.empty()) << "a reading of something else is no level";
}

TEST(PlatformHeatingLevelSensor, NamesEveryLevelTheHeaterReports) {
  ClimateControlsHub hub;
  hub.add_heater();
  LevelSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  const struct {
    uint8_t level;
    const char *name;
  } expected[] = {
      {heating_level::OFF, "off"},
      {heating_level::FROST_PROTECTION, "frost protection"},
      {heating_level::ECO, "eco"},
      {heating_level::COMFORT_MINUS_2, "comfort -2"},
      {heating_level::COMFORT_MINUS_1, "comfort -1"},
      {heating_level::COMFORT, "comfort"},
      {heating_level::BOOST, "boost"},
      {heating_level::SECURED, "secured"},
  };
  for (const auto &entry : expected) {
    hub.deliver([&entry](IoDevice &r) { test::store_reading(r, DeviceReading::HEATING_LEVEL, entry.level); });
    EXPECT_EQ(sensor.state, entry.name);
  }
}

TEST(PlatformHeatingLevelSensor, AValueThatIsNoLevelKeepsTheLastLevelShown) {
  ClimateControlsHub hub;
  hub.add_heater();
  LevelSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_level::ECO); });
  hub.deliver(
      [](IoDevice &r) { test::store_reading(r, DeviceReading::HEATING_LEVEL, 0x40); });  // not a level of the table

  EXPECT_EQ(sensor.state, "eco");
}

TEST(PlatformHeatingLevelSensor, PublishesWhatIsAlreadyKnownAtSetup) {
  ClimateControlsHub hub;
  hub.add_heater();
  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_level::BOOST); });
  LevelSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_EQ(sensor.state, "boost");
}

TEST(PlatformWindowSensor, HasNoStateUntilTheSensorHasBeenRead) {
  ClimateControlsHub hub;
  hub.add_heater();
  RegisterSensor sensor(DeviceReading::WINDOW_OPEN);
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_FALSE(sensor.has_state()) << "closed must mean the heater said so";
}

TEST(PlatformWindowSensor, FollowsTheHeatersReadings) {
  ClimateControlsHub hub;
  hub.add_heater();
  RegisterSensor sensor(DeviceReading::WINDOW_OPEN);
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::WINDOW_OPEN, 1); });
  ASSERT_TRUE(sensor.has_state());
  EXPECT_TRUE(sensor.state);

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::WINDOW_OPEN, 0); });
  EXPECT_TRUE(sensor.has_state());
  EXPECT_FALSE(sensor.state);
}

TEST(PlatformWindowSensor, PublishesWhatIsAlreadyKnownAtSetup) {
  ClimateControlsHub hub;
  hub.add_heater();
  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::WINDOW_OPEN, 1); });
  RegisterSensor sensor(DeviceReading::WINDOW_OPEN);
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_TRUE(sensor.has_state());
  EXPECT_TRUE(sensor.state);
}

TEST(PlatformCentralHeatingModeSensor, HasNoStateUntilTheDryerHasAnswered) {
  ClimateControlsHub hub;
  hub.add_heater();
  RegisterSensor sensor(DeviceReading::CENTRAL_HEATING_MODE);
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  EXPECT_FALSE(sensor.has_state()) << "electric mode must mean the dryer said so";
}

TEST(PlatformCentralHeatingModeSensor, IsOnInCentralHeatingAndOffInElectricMode) {
  ClimateControlsHub hub;
  hub.add_heater();
  RegisterSensor sensor(DeviceReading::CENTRAL_HEATING_MODE);
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::CENTRAL_HEATING_MODE, 1); });
  ASSERT_TRUE(sensor.has_state());
  EXPECT_TRUE(sensor.state);

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::CENTRAL_HEATING_MODE, 0); });
  EXPECT_TRUE(sensor.has_state());
  EXPECT_FALSE(sensor.state);
}

TEST(PlatformCentralHeatingModeSensor, LosesItsStateWhenTheReadingIsDropped) {
  ClimateControlsHub hub;
  hub.add_heater();
  RegisterSensor sensor(DeviceReading::CENTRAL_HEATING_MODE);
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::CENTRAL_HEATING_MODE, 1); });
  ASSERT_TRUE(sensor.has_state());

  hub.deliver([](IoDevice &r) { test::clear_reading(r, DeviceRegister::CENTRAL_HEATING_MODE); });
  EXPECT_FALSE(sensor.has_state()) << "a dryer that stops answering a value shows no state";
}

TEST(PlatformHeaterSettingSwitch, WritesThenReadsTheRegisterBackWithTheFullBudget) {
  ClimateControlsHub hub;
  hub.add_heater();
  hub.heater_holds(DeviceRegister::OPEN_WINDOW_DETECTION, 1.0F);
  TestableDetectionSwitch sw;
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");
  sw.setup();

  sw.trigger_write(true);

  ASSERT_EQ(hub.writes.size(), 1u);
  EXPECT_EQ(hub.writes[0].fn, DeviceRegister::OPEN_WINDOW_DETECTION);
  EXPECT_FLOAT_EQ(hub.writes[0].value, 1.0F);
  ASSERT_EQ(hub.reads.size(), 1u);
  EXPECT_EQ(hub.reads[0], DeviceRegister::OPEN_WINDOW_DETECTION);
  EXPECT_EQ(hub.read_tries[0], EXCHANGE_RETRY_COUNT);
  EXPECT_TRUE(sw.get_state()) << "shown from the read-back";
}

TEST(PlatformHeaterSettingSwitch, ShowsWhatTheHeaterReportsNotWhatWasCommanded) {
  ClimateControlsHub hub;
  hub.add_heater();
  hub.heater_holds(DeviceRegister::OPEN_WINDOW_DETECTION, 0.0F);  // the heater kept it off
  TestableDetectionSwitch sw;
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");
  sw.setup();

  sw.trigger_write(true);
  EXPECT_FALSE(sw.get_state());
}

TEST(PlatformHeaterSettingSwitch, AnUnacknowledgedWriteIsNotReadBackOrShown) {
  ClimateControlsHub hub;
  hub.add_heater();
  hub.write_result = false;
  TestableDetectionSwitch sw;
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");
  sw.setup();

  sw.trigger_write(true);
  EXPECT_TRUE(hub.reads.empty());
  EXPECT_FALSE(sw.get_state());
}

TEST(PlatformHeaterSettingSwitch, FollowsTheBootPollReading) {
  ClimateControlsHub hub;
  hub.add_heater();
  TestableDetectionSwitch sw;
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");
  sw.setup();

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::OPEN_WINDOW_DETECTION, 1); });
  EXPECT_TRUE(sw.get_state());
}

// --- Night light (heater_with_light) ---

namespace {

/// A night light wired to the hub, with its boot-time restore write already swallowed.
struct NightLightRig {
  ClimateControlsHub hub;
  esphome::light::LightState state;
  IOHomeHeaterNightLight light;

  NightLightRig() {
    this->hub.add_heater();
    this->light.set_parent(&this->hub);
    this->light.set_device_id("ABC123");
    this->light.setup_state(&this->state);
    this->light.setup();
    this->light.write_state(&this->state);  // LightState's restore_mode at boot
  }

  /// Deliver readings, then run the write_state() that LightState makes when the light published a new
  /// state (a call it performed; the stub's perform() applies the values at once).
  void deliver(int16_t on, float percent) {
    const bool was_on = this->state.current_values.is_on();
    const float was_brightness = this->state.current_values.get_brightness();
    this->hub.deliver([on, percent](IoDevice &r) {
      test::store_reading(r, DeviceReading::NIGHT_LIGHT, on);
      test::store_reading(r, DeviceReading::NIGHT_LIGHT_BRIGHTNESS, percent);
    });
    if (this->state.current_values.is_on() != was_on || this->state.current_values.get_brightness() != was_brightness)
      this->light.write_state(&this->state);
  }

  /// What the user asks for from Home Assistant.
  void request(bool on, float brightness) {
    this->state.remote_values.set(on);
    this->state.set_current_on(on);
    this->state.set_current_brightness(brightness);
    this->light.write_state(&this->state);
  }
};

}  // namespace

TEST(PlatformHeaterNightLight, OffersBrightness) {
  IOHomeHeaterNightLight light;
  EXPECT_EQ(light.get_traits().get_color_mode(), esphome::light::ColorMode::BRIGHTNESS);
}

TEST(PlatformHeaterNightLight, TheBootRestoreAndPublishedReadingsAreNeverWritten) {
  NightLightRig rig;
  EXPECT_TRUE(rig.hub.writes.empty()) << "a reboot must not switch the heater's light";

  rig.deliver(1, 66.0F);
  EXPECT_TRUE(rig.state.current_values.is_on());
  EXPECT_FLOAT_EQ(rig.state.current_values.get_brightness(), 0.66F);
  rig.deliver(0, 66.0F);
  EXPECT_FALSE(rig.state.current_values.is_on());
  EXPECT_TRUE(rig.hub.writes.empty()) << "what the heater reports is not echoed back";
}

TEST(PlatformHeaterNightLight, NothingIsShownUntilTheLightHasBeenRead) {
  NightLightRig rig;
  rig.state.set_current_brightness(0.7F);
  rig.hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::NIGHT_LIGHT_BRIGHTNESS, 100.0F); });
  EXPECT_FALSE(rig.state.current_values.is_on());
  EXPECT_FLOAT_EQ(rig.state.current_values.get_brightness(), 0.7F) << "no on/off reading: nothing published";
}

TEST(PlatformHeaterNightLight, SwitchingOnAtANewBrightnessWritesBrightnessThenOnAndReadsBothBack) {
  NightLightRig rig;
  rig.deliver(0, 100.0F);
  rig.hub.heater_holds(DeviceRegister::NIGHT_LIGHT, 1.0F);
  rig.hub.heater_holds(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, 66.0F);

  rig.request(true, 0.66F);

  ASSERT_EQ(rig.hub.writes.size(), 2u);
  EXPECT_EQ(rig.hub.writes[0].fn, DeviceRegister::NIGHT_LIGHT_BRIGHTNESS);
  EXPECT_FLOAT_EQ(rig.hub.writes[0].value, 66.0F);
  EXPECT_EQ(rig.hub.writes[1].fn, DeviceRegister::NIGHT_LIGHT);
  EXPECT_FLOAT_EQ(rig.hub.writes[1].value, 1.0F);
  EXPECT_EQ(rig.hub.reads,
            (std::vector<DeviceRegister>{DeviceRegister::NIGHT_LIGHT, DeviceRegister::NIGHT_LIGHT_BRIGHTNESS}));
  EXPECT_EQ(rig.hub.read_tries[0], EXCHANGE_RETRY_COUNT);
  EXPECT_TRUE(rig.state.current_values.is_on());
  EXPECT_FLOAT_EQ(rig.state.current_values.get_brightness(), 0.66F);
}

TEST(PlatformHeaterNightLight, AWriteIsPublishedOnceWithBothReadBacks) {
  // Counts what reaches the LightState while a write runs: the first read-back alone (on, with the
  // old 100 %) must not be shown before the brightness read-back.
  NightLightRig rig;
  rig.deliver(0, 100.0F);
  rig.hub.heater_holds(DeviceRegister::NIGHT_LIGHT, 1.0F);
  rig.hub.heater_holds(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, 66.0F);
  std::vector<float> shown;
  rig.hub.register_device_callback([&rig, &shown](const std::string &, const IoDevice &) {
    shown.push_back(rig.state.current_values.get_brightness());
  });

  rig.request(true, 0.66F);

  for (const float brightness : shown)
    EXPECT_FLOAT_EQ(brightness, 0.66F) << "no stale brightness while the write runs";
  EXPECT_FLOAT_EQ(rig.state.current_values.get_brightness(), 0.66F);
  rig.request(false, 0.66F);
  EXPECT_EQ(rig.hub.writes.back().fn, DeviceRegister::NIGHT_LIGHT) << "the next request is not swallowed";
  EXPECT_FLOAT_EQ(rig.hub.writes.back().value, 0.0F);
}

TEST(PlatformHeaterNightLight, OnAtTheBrightnessTheHeaterHoldsWritesOnlyOnOff) {
  NightLightRig rig;
  rig.deliver(0, 100.0F);
  rig.hub.heater_holds(DeviceRegister::NIGHT_LIGHT, 1.0F);

  rig.request(true, 1.0F);

  ASSERT_EQ(rig.hub.writes.size(), 1u) << "on/off never depends on the brightness write";
  EXPECT_EQ(rig.hub.writes[0].fn, DeviceRegister::NIGHT_LIGHT);
  EXPECT_EQ(rig.hub.reads, (std::vector<DeviceRegister>{DeviceRegister::NIGHT_LIGHT}));
}

TEST(PlatformHeaterNightLight, OnWithoutAKnownBrightnessWritesTheBrightness) {
  // A brightness never delivered and one that reads 0 hold no percentage, even the full one: the brightness asked for
  // is written.
  for (const bool delivered : {false, true}) {
    NightLightRig rig;
    if (delivered)
      rig.deliver(0, 0.0F);
    rig.request(true, 1.0F);
    ASSERT_EQ(rig.hub.writes.size(), 2u) << "delivered " << delivered;
    EXPECT_EQ(rig.hub.writes[0].fn, DeviceRegister::NIGHT_LIGHT_BRIGHTNESS) << "delivered " << delivered;
    EXPECT_FLOAT_EQ(rig.hub.writes[0].value, 100.0F) << "delivered " << delivered;
  }
}

TEST(PlatformHeaterNightLight, OffWritesOnlyOnOff) {
  NightLightRig rig;
  rig.deliver(1, 100.0F);
  rig.hub.heater_holds(DeviceRegister::NIGHT_LIGHT, 0.0F);

  rig.request(false, 0.3F);

  ASSERT_EQ(rig.hub.writes.size(), 1u);
  EXPECT_EQ(rig.hub.writes[0].fn, DeviceRegister::NIGHT_LIGHT);
  EXPECT_FLOAT_EQ(rig.hub.writes[0].value, 0.0F);
  EXPECT_FALSE(rig.state.current_values.is_on());
}

TEST(PlatformHeaterNightLight, ABrightnessThatRoundsToZeroIsSentAsOnePercent) {
  NightLightRig rig;
  rig.deliver(0, 100.0F);
  rig.request(true, 0.004F);
  ASSERT_FALSE(rig.hub.writes.empty());
  EXPECT_EQ(rig.hub.writes[0].fn, DeviceRegister::NIGHT_LIGHT_BRIGHTNESS);
  EXPECT_FLOAT_EQ(rig.hub.writes[0].value, 1.0F);
}

TEST(PlatformHeaterNightLight, TheHeklasFourPanelLevelsShowAsTheVendorAppShowsThem) {
  NightLightRig rig;
  const struct {
    float percent;
    float fraction;
  } levels[] = {{3.3F, 0.033F}, {33.0F, 0.33F}, {66.0F, 0.66F}, {100.0F, 1.0F}};
  for (const auto &level : levels) {
    rig.deliver(1, level.percent);
    EXPECT_NEAR(rig.state.current_values.get_brightness(), level.fraction, 1e-6F) << level.percent << " %";
  }
  EXPECT_TRUE(rig.hub.writes.empty());
}

TEST(PlatformHeaterNightLight, OnAtAPanelLevelBetweenTwoPercentagesKeepsTheLevel) {
  // The panel's register value 100 shows as 3.3 %, which Home Assistant hands back as 3 % (register value 91):
  // switching on at the brightness shown must not move the heater off its own level.
  for (const float level : {3.3F, 33.0F, 70.25F}) {
    NightLightRig rig;
    rig.deliver(0, level);
    rig.hub.heater_holds(DeviceRegister::NIGHT_LIGHT, 1.0F);
    rig.request(true, rig.state.current_values.get_brightness());
    ASSERT_EQ(rig.hub.writes.size(), 1u) << level << " %";
    EXPECT_EQ(rig.hub.writes[0].fn, DeviceRegister::NIGHT_LIGHT) << level << " %";
  }
}

TEST(PlatformHeaterNightLight, HomeAssistantPercentagesAreWrittenOnTheVendorAppsScale) {
  const struct {
    float fraction;
    float percent;
  } requests[] = {{0.2F, 20.0F}, {0.33F, 33.0F}, {0.7F, 70.0F}, {0.01F, 1.0F}};
  for (const auto &request : requests) {
    NightLightRig rig;
    rig.deliver(0, 100.0F);
    rig.request(true, request.fraction);
    ASSERT_FALSE(rig.hub.writes.empty());
    EXPECT_EQ(rig.hub.writes[0].fn, DeviceRegister::NIGHT_LIGHT_BRIGHTNESS);
    EXPECT_FLOAT_EQ(rig.hub.writes[0].value, request.percent);
  }
}

TEST(PlatformHeaterNightLight, ARefusedBrightnessWriteStillSwitchesTheLightAndIsNotReadBack) {
  NightLightRig rig;
  rig.deliver(0, 100.0F);
  rig.hub.refused_writes = {DeviceRegister::NIGHT_LIGHT_BRIGHTNESS};
  rig.hub.heater_holds(DeviceRegister::NIGHT_LIGHT, 1.0F);

  rig.request(true, 0.5F);

  ASSERT_EQ(rig.hub.writes.size(), 2u);
  EXPECT_EQ(rig.hub.writes[1].fn, DeviceRegister::NIGHT_LIGHT);
  EXPECT_EQ(rig.hub.reads, (std::vector<DeviceRegister>{DeviceRegister::NIGHT_LIGHT}));
  EXPECT_TRUE(rig.state.current_values.is_on());
  EXPECT_FLOAT_EQ(rig.state.current_values.get_brightness(), 1.0F) << "the brightness the heater still holds";
}

TEST(PlatformHeaterNightLight, AnUnacknowledgedSwitchIsNotReadBackAndShowsTheHeatersLastReading) {
  NightLightRig rig;
  rig.deliver(0, 100.0F);
  rig.hub.write_result = false;

  rig.request(true, 1.0F);

  ASSERT_EQ(rig.hub.writes.size(), 1u);
  EXPECT_TRUE(rig.hub.reads.empty()) << "a heater that did not answer the write is not asked again";
  EXPECT_FALSE(rig.state.current_values.is_on()) << "never shows a command the heater did not confirm";
}

// --- Occupancy detection switch and occupancy sensor (Sauter Hekla, issue #222) ---

TEST(PlatformHeaterSettingSwitch, OccupancyDetectionWritesRegister0x0101ThenReadsItBack) {
  ClimateControlsHub hub;
  hub.add_heater();
  hub.heater_holds(DeviceRegister::OCCUPANCY_DETECTION, 1.0F);
  TestableDetectionSwitch sw(DeviceReading::OCCUPANCY_DETECTION);
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");
  sw.setup();
  EXPECT_FALSE(sw.get_state()) << "no restored guess: nothing shows until a reply";

  sw.trigger_write(true);

  ASSERT_EQ(hub.writes.size(), 1u);
  EXPECT_EQ(hub.writes[0].fn, DeviceRegister::OCCUPANCY_DETECTION);
  EXPECT_FLOAT_EQ(hub.writes[0].value, 1.0F);
  ASSERT_EQ(hub.reads.size(), 1u);
  EXPECT_EQ(hub.reads[0], DeviceRegister::OCCUPANCY_DETECTION);
  EXPECT_EQ(hub.read_tries[0], EXCHANGE_RETRY_COUNT);
  EXPECT_TRUE(sw.get_state());
}

TEST(PlatformHeaterSettingSwitch, OccupancyDetectionIgnoresTheOpenWindowReading) {
  ClimateControlsHub hub;
  hub.add_heater();
  TestableDetectionSwitch sw(DeviceReading::OCCUPANCY_DETECTION);
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");
  sw.setup();

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::OPEN_WINDOW_DETECTION, 1); });
  EXPECT_FALSE(sw.get_state()) << "each switch follows its own register";

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::OCCUPANCY_DETECTION, 1); });
  EXPECT_TRUE(sw.get_state());
  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::OCCUPANCY_DETECTION, 0); });
  EXPECT_FALSE(sw.get_state());
  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::OCCUPANCY_DETECTION, 1); });
  EXPECT_TRUE(sw.get_state()) << "a change made from the vendor app shows at the next poll";
}

TEST(PlatformHeaterSettingSwitch, ARefusedOccupancyDetectionWriteIsNotReadBackOrShown) {
  ClimateControlsHub hub;
  hub.add_heater();
  hub.write_result = false;
  TestableDetectionSwitch sw(DeviceReading::OCCUPANCY_DETECTION);
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");
  sw.setup();

  sw.trigger_write(true);
  EXPECT_TRUE(hub.reads.empty());
  EXPECT_FALSE(sw.get_state());
}

TEST(PlatformOccupancySensor, HasNoStateUntilTheHeaterHasReportedPresence) {
  ClimateControlsHub hub;
  hub.add_heater();
  RegisterSensor sensor(DeviceReading::OCCUPANCY);
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  EXPECT_FALSE(sensor.has_state()) << "nobody there must mean the heater said so";

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::OCCUPANCY_DETECTION, 1); });
  EXPECT_FALSE(sensor.has_state()) << "the detection setting is not the presence";
}

TEST(PlatformOccupancySensor, ShowsPresenceAsTheHeaterReportsIt) {
  ClimateControlsHub hub;
  hub.add_heater();
  RegisterSensor sensor(DeviceReading::OCCUPANCY);
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::OCCUPANCY, 1); });
  ASSERT_TRUE(sensor.has_state());
  EXPECT_TRUE(sensor.state);
  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::OCCUPANCY, 0); });
  EXPECT_TRUE(sensor.has_state());
  EXPECT_FALSE(sensor.state);
}

TEST(PlatformOccupancySensor, LosesItsStateWhenTheHeaterAnswersWithoutAValue) {
  ClimateControlsHub hub;
  hub.add_heater();
  RegisterSensor sensor(DeviceReading::OCCUPANCY);
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::OCCUPANCY, 1); });
  ASSERT_TRUE(sensor.has_state());

  hub.deliver([](IoDevice &r) { test::clear_reading(r, DeviceRegister::PRESENCE); });
  EXPECT_FALSE(sensor.has_state());
}

// --- Energy sensor: the numeric register sensor (register 0x0116, whole kWh) ---

TEST(PlatformEnergySensor, HasNoStateUntilTheHeaterHasReportedATotal) {
  ClimateControlsHub hub;
  hub.add_heater();
  NumberSensor sensor(DeviceReading::ENERGY_TOTAL);
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  EXPECT_TRUE(std::isnan(sensor.state)) << "a total of 0 must mean the heater said so";

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::ROOM_TEMPERATURE, 20.0F); });
  EXPECT_TRUE(std::isnan(sensor.state)) << "a reading of something else is no total";
}

TEST(PlatformEnergySensor, ShowsTheTotalAsTheHeaterReportsItAndFollowsEveryPoll) {
  ClimateControlsHub hub;
  hub.add_heater();
  NumberSensor sensor(DeviceReading::ENERGY_TOTAL);
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceRegister::ENERGY_TOTAL, 3422.0F); });
  EXPECT_FLOAT_EQ(sensor.state, 3422.0F);
  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceRegister::ENERGY_TOTAL, 3423.0F); });
  EXPECT_FLOAT_EQ(sensor.state, 3423.0F);
  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceRegister::ENERGY_TOTAL, 65535.0F); });
  EXPECT_FLOAT_EQ(sensor.state, 65535.0F) << "a total past 32767 is not wrapped";
}

TEST(PlatformEnergySensor, ATotalOfZeroIsShownAsZero) {
  ClimateControlsHub hub;
  hub.add_heater();
  NumberSensor sensor(DeviceReading::ENERGY_TOTAL);
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceRegister::ENERGY_TOTAL, 0.0F); });
  EXPECT_FLOAT_EQ(sensor.state, 0.0F) << "a new heater";
}

TEST(PlatformEnergySensor, LosesItsStateWhenTheHeaterAnswersWithoutAValue) {
  ClimateControlsHub hub;
  hub.add_heater();
  NumberSensor sensor(DeviceReading::ENERGY_TOTAL);
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceRegister::ENERGY_TOTAL, 3422.0F); });
  ASSERT_FLOAT_EQ(sensor.state, 3422.0F);

  hub.deliver([](IoDevice &r) { test::clear_reading(r, DeviceRegister::ENERGY_TOTAL); });
  EXPECT_TRUE(std::isnan(sensor.state));
}

// --- Run timers: the duration sensors and the remaining-time sensor (towel dryer, minutes) ---

TEST(PlatformRunTimerSensors, ADurationSensorShowsItsRegisterInMinutesAndHasNoStateBeforeThat) {
  ClimateControlsHub hub;
  hub.add_heater();
  NumberSensor boost(DeviceReading::BOOST_DURATION);
  NumberSensor drying(DeviceReading::DRYING_DURATION);
  for (NumberSensor *sensor : {&boost, &drying}) {
    sensor->set_parent(&hub);
    sensor->set_device_id("ABC123");
    sensor->setup();
    EXPECT_TRUE(std::isnan(sensor->state));
  }

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceRegister::BOOST_DURATION, 60.0F); });
  EXPECT_FLOAT_EQ(boost.state, 60.0F);
  EXPECT_TRUE(std::isnan(drying.state)) << "each sensor shows its own register";
  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceRegister::DRYING_DURATION, 30.0F); });
  EXPECT_FLOAT_EQ(drying.state, 30.0F);

  hub.deliver([](IoDevice &r) { test::clear_reading(r, DeviceRegister::BOOST_DURATION); });
  EXPECT_TRUE(std::isnan(boost.state)) << "a heater that answers without a value leaves no state";
}

TEST(PlatformRunTimerSensors, TheRemainingTimeFollowsTheRunThatIsActive) {
  ClimateControlsHub hub;
  hub.add_heater();
  RemainingTimeSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  EXPECT_TRUE(std::isnan(sensor.state)) << "state not read yet";

  hub.deliver([](IoDevice &r) {
    test::store_reading(r, DeviceRegister::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::BOOST));
    test::store_reading(r, DeviceRegister::BOOST_REMAINING, 55.0F);
  });
  EXPECT_FLOAT_EQ(sensor.state, 55.0F);

  hub.deliver([](IoDevice &r) {
    test::store_reading(r, DeviceRegister::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::DRYING));
    test::store_reading(r, DeviceRegister::DRYING_REMAINING, 28.0F);
  });
  EXPECT_FLOAT_EQ(sensor.state, 28.0F);

  hub.deliver([](IoDevice &r) {
    test::store_reading(r, DeviceRegister::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::NONE));
  });
  EXPECT_FLOAT_EQ(sensor.state, 0.0F) << "no run: 0 minutes left";
}

TEST(PlatformRunTimerSensors, TheRemainingTimeHasNoStateWhileTheActiveRunsRegisterIsUnread) {
  ClimateControlsHub hub;
  hub.add_heater();
  RemainingTimeSensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  hub.deliver([](IoDevice &r) {
    test::store_reading(r, DeviceRegister::TEMPORARY_STATE, static_cast<float>(HeatingTemporaryState::BOOST));
  });
  EXPECT_TRUE(std::isnan(sensor.state)) << "a boost with no reading is not 0 minutes";
}

// --- The generic reading entities publish only what changed ---

TEST(PlatformDeviceReadingSensor, PublishesAChangedValueOnceAndNotARepeatedOne) {
  ClimateControlsHub hub;
  hub.add_heater();
  NumberSensor sensor(DeviceReading::ENERGY_TOTAL);
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  const int after_setup = sensor.publish_count;

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceRegister::ENERGY_TOTAL, 3422.0F); });
  EXPECT_EQ(sensor.publish_count, after_setup + 1);
  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceRegister::ENERGY_TOTAL, 3422.0F); });
  hub.deliver([](IoDevice &r) {
    test::store_reading(r, DeviceReading::ROOM_TEMPERATURE, 21.0F);
  });  // another reading of the same device
  EXPECT_EQ(sensor.publish_count, after_setup + 1) << "an update that leaves the value alone publishes nothing";

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceRegister::ENERGY_TOTAL, 3423.0F); });
  EXPECT_EQ(sensor.publish_count, after_setup + 2);
  EXPECT_FLOAT_EQ(sensor.state, 3423.0F);
}

TEST(PlatformDeviceReadingSensor, TwoUpdatesWithoutAValueLeaveOneNoStatePublish) {
  ClimateControlsHub hub;
  hub.add_heater();
  NumberSensor sensor(DeviceReading::ENERGY_TOTAL);
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  const int after_setup = sensor.publish_count;

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::ROOM_TEMPERATURE, 20.0F); });
  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::ROOM_TEMPERATURE, 21.0F); });
  EXPECT_EQ(sensor.publish_count, after_setup);
  EXPECT_TRUE(std::isnan(sensor.state));
}

TEST(PlatformDeviceReadingBinarySensor, PublishesOnlyAChangedState) {
  ClimateControlsHub hub;
  hub.add_heater();
  RegisterSensor sensor(DeviceReading::OCCUPANCY);
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  const int after_setup = sensor.publish_count;

  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::OCCUPANCY, 1); });
  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::OCCUPANCY, 1); });
  EXPECT_EQ(sensor.publish_count, after_setup + 1);
  hub.deliver([](IoDevice &r) { test::store_reading(r, DeviceReading::OCCUPANCY, 0); });
  EXPECT_EQ(sensor.publish_count, after_setup + 2);
  EXPECT_FALSE(sensor.state);

  hub.deliver([](IoDevice &r) { test::clear_reading(r, DeviceRegister::PRESENCE); });
  EXPECT_FALSE(sensor.has_state());
  hub.deliver([](IoDevice &r) { test::clear_reading(r, DeviceRegister::PRESENCE); });
  EXPECT_EQ(sensor.publish_count, after_setup + 2) << "losing the state publishes nothing";
}

TEST(PlatformHeaterNightLight, AValueAfterAPublishedReadingAndBeforeTheLightsLoopIsWritten) {
  NightLightRig rig;
  rig.state.set_test_output(&rig.light);
  rig.hub.deliver([](IoDevice &r) {
    test::store_reading(r, DeviceReading::NIGHT_LIGHT, 1);
    test::store_reading(r, DeviceReading::NIGHT_LIGHT_BRIGHTNESS, 66.0F);
  });  // shown on at 66 %, its write waits
  ASSERT_TRUE(rig.state.current_values.is_on());

  auto call = rig.state.make_call();
  call.set_state(false);
  call.perform();
  rig.state.loop();

  ASSERT_EQ(rig.hub.writes.size(), 1u) << "the user's off is not taken for the echo of the reading";
  EXPECT_EQ(rig.hub.writes[0].fn, DeviceRegister::NIGHT_LIGHT);
  EXPECT_FLOAT_EQ(rig.hub.writes[0].value, 0.0F);
}

TEST(PlatformHeaterNightLight, AReadingPublishedTwiceIsNeverWritten) {
  NightLightRig rig;
  rig.state.set_test_output(&rig.light);
  const auto reading = [](IoDevice &r) {
    test::store_reading(r, DeviceReading::NIGHT_LIGHT, 1);
    test::store_reading(r, DeviceReading::NIGHT_LIGHT_BRIGHTNESS, 66.0F);
  };

  rig.hub.deliver(reading);
  rig.hub.deliver(reading);
  rig.state.loop();
  rig.state.loop();

  EXPECT_TRUE(rig.hub.writes.empty());
}

TEST(PlatformHeaterNightLight, AReadingThatArrivesWhileAValueWaitsIsDroppedAndTheNextOneIsShown) {
  NightLightRig rig;
  rig.state.set_test_output(&rig.light);
  rig.hub.heater_holds(DeviceRegister::NIGHT_LIGHT, 1.0F);
  rig.hub.heater_holds(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, 66.0F);
  rig.deliver(1, 66.0F);

  auto call = rig.state.make_call();  // a value waits for the light's loop,
  call.set_state(true);
  call.set_brightness(0.9F);
  call.perform();
  rig.hub.deliver([](IoDevice &r) {  // a reading arrives before it
    test::store_reading(r, DeviceReading::NIGHT_LIGHT, 1);
    test::store_reading(r, DeviceReading::NIGHT_LIGHT_BRIGHTNESS, 50.0F);
  });
  EXPECT_FLOAT_EQ(rig.state.current_values.get_brightness(), 0.9F) << "the reading did not replace the value";
  rig.state.loop();
  EXPECT_FALSE(rig.hub.writes.empty()) << "the value is written to the heater";

  rig.hub.deliver([](IoDevice &r) {  // the next reading
    test::store_reading(r, DeviceReading::NIGHT_LIGHT, 1);
    test::store_reading(r, DeviceReading::NIGHT_LIGHT_BRIGHTNESS, 50.0F);
  });
  EXPECT_FLOAT_EQ(rig.state.current_values.get_brightness(), 0.5F) << "the heater's level is shown again";
}

TEST(PlatformHeaterNightLight, ATransitionWritesOnlyItsEndAndTheNextReadingIsShownAfterIt) {
  NightLightRig rig;
  rig.state.set_test_output(&rig.light);
  rig.hub.heater_holds(DeviceRegister::NIGHT_LIGHT, 1.0F);
  rig.hub.heater_holds(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, 66.0F);

  rig.state.test_transformer_active_ = true;
  rig.request(true, 0.2F);
  rig.request(true, 0.4F);
  EXPECT_TRUE(rig.hub.writes.empty()) << "a step of a transition is not written";
  rig.hub.deliver([](IoDevice &r) {
    test::store_reading(r, DeviceReading::NIGHT_LIGHT, 0);
    test::store_reading(r, DeviceReading::NIGHT_LIGHT_BRIGHTNESS, 100.0F);
  });
  EXPECT_TRUE(rig.state.current_values.is_on()) << "no reading is shown during the transition";

  rig.state.test_transformer_active_ = false;
  rig.request(true, 0.66F);
  EXPECT_FALSE(rig.hub.writes.empty());
  EXPECT_FLOAT_EQ(rig.state.current_values.get_brightness(), 0.66F) << "the heater's level is shown after the end";
}
