/// @file platform_climate_controls_test.cpp
/// @brief Tests for the generated per-heater companions: the window sensor and the open-window
/// detection switch.

#include "platform_climate_controls.h"

#include "hub_core.h"

#include "test_helpers.h"

#include <map>
#include <set>
#include <vector>

using namespace esphome::home_io_control;

namespace {

/// Serves a heater, records the register traffic and answers reads from a script.
class ClimateControlsHub : public test::MockPlatformHubBase {
 public:
  struct Write {
    HeatingFunction fn;
    float value;
  };
  std::vector<Write> writes;
  std::vector<HeatingFunction> reads;
  std::vector<uint8_t> read_tries;
  bool write_result{true};
  std::set<HeatingFunction> refused_writes;  ///< Writes the heater does not acknowledge, on top of write_result.
  std::map<HeatingFunction, float> read_replies;

  bool send_heating_command(const std::string &, HeatingFunction fn, float value) override {
    this->writes.push_back({fn, value});
    return this->write_result && this->refused_writes.count(fn) == 0;
  }

  bool read_heating_register(const std::string &device_id, HeatingFunction fn, float *, uint8_t max_tries) override {
    this->reads.push_back(fn);
    this->read_tries.push_back(max_tries);
    const auto reply = this->read_replies.find(fn);
    if (reply == this->read_replies.end())
      return false;
    if (IoDevice *dev = this->registry_.get(device_id); dev != nullptr) {
      dev->heating.store(fn, reply->second);
      this->registry_.notify(device_id);
    }
    return true;
  }

  void add_heater() {
    DeviceConfig config{};
    config.type = DeviceType::ELECTRICAL_HEATER;
    config.subtype = 1;
    this->add_device("ABC123", config);
  }

  void deliver(const std::function<void(HeatingReadings &)> &edit) {
    IoDevice dev = *this->get_device("ABC123");
    edit(dev.heating);
    this->trigger_device_update("ABC123", dev, /*cache_device=*/true);
  }
};

class TestableDetectionSwitch : public IOHomeOpenWindowDetectionSwitch {
 public:
  void trigger_write(bool state) { this->write_state(state); }
};

}  // namespace

TEST(PlatformWindowSensor, HasNoStateUntilTheSensorHasBeenRead) {
  ClimateControlsHub hub;
  hub.add_heater();
  IOHomeWindowBinarySensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  EXPECT_FALSE(sensor.has_state()) << "closed must mean the heater said so";
}

TEST(PlatformWindowSensor, FollowsTheHeatersReadings) {
  ClimateControlsHub hub;
  hub.add_heater();
  IOHomeWindowBinarySensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();

  hub.deliver([](HeatingReadings &r) { r.window_open = 1; });
  ASSERT_TRUE(sensor.has_state());
  EXPECT_TRUE(sensor.state);

  hub.deliver([](HeatingReadings &r) { r.window_open = 0; });
  EXPECT_TRUE(sensor.has_state());
  EXPECT_FALSE(sensor.state);
}

TEST(PlatformWindowSensor, PublishesWhatIsAlreadyKnownAtSetup) {
  ClimateControlsHub hub;
  hub.add_heater();
  hub.deliver([](HeatingReadings &r) { r.window_open = 1; });
  IOHomeWindowBinarySensor sensor;
  sensor.set_parent(&hub);
  sensor.set_device_id("ABC123");
  sensor.setup();
  ASSERT_TRUE(sensor.has_state());
  EXPECT_TRUE(sensor.state);
}

TEST(PlatformOpenWindowDetectionSwitch, WritesThenReadsTheRegisterBackWithTheFullBudget) {
  ClimateControlsHub hub;
  hub.add_heater();
  hub.read_replies[HeatingFunction::SET_OPEN_WINDOW_DETECTION] = 1.0F;
  TestableDetectionSwitch sw;
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");
  sw.setup();

  sw.trigger_write(true);

  ASSERT_EQ(hub.writes.size(), 1u);
  EXPECT_EQ(hub.writes[0].fn, HeatingFunction::SET_OPEN_WINDOW_DETECTION);
  EXPECT_FLOAT_EQ(hub.writes[0].value, 1.0F);
  ASSERT_EQ(hub.reads.size(), 1u);
  EXPECT_EQ(hub.reads[0], HeatingFunction::SET_OPEN_WINDOW_DETECTION);
  EXPECT_EQ(hub.read_tries[0], EXCHANGE_RETRY_COUNT);
  EXPECT_TRUE(sw.get_state()) << "shown from the read-back";
}

TEST(PlatformOpenWindowDetectionSwitch, ShowsWhatTheHeaterReportsNotWhatWasCommanded) {
  ClimateControlsHub hub;
  hub.add_heater();
  hub.read_replies[HeatingFunction::SET_OPEN_WINDOW_DETECTION] = 0.0F;  // the heater kept it off
  TestableDetectionSwitch sw;
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");
  sw.setup();

  sw.trigger_write(true);
  EXPECT_FALSE(sw.get_state());
}

TEST(PlatformOpenWindowDetectionSwitch, AnUnacknowledgedWriteIsNotReadBackOrShown) {
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

TEST(PlatformOpenWindowDetectionSwitch, FollowsTheBootPollReading) {
  ClimateControlsHub hub;
  hub.add_heater();
  TestableDetectionSwitch sw;
  sw.set_parent(&hub);
  sw.set_device_id("ABC123");
  sw.setup();

  hub.deliver([](HeatingReadings &r) { r.open_window_detection = 1; });
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
  void deliver(int16_t on, int16_t brightness) {
    const bool was_on = this->state.current_values.is_on();
    const float was_brightness = this->state.current_values.get_brightness();
    this->hub.deliver([on, brightness](HeatingReadings &r) {
      r.night_light = on;
      r.night_light_brightness = brightness;
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

  rig.deliver(1, 40);
  EXPECT_TRUE(rig.state.current_values.is_on());
  EXPECT_FLOAT_EQ(rig.state.current_values.get_brightness(), 0.4F);
  rig.deliver(0, 40);
  EXPECT_FALSE(rig.state.current_values.is_on());
  EXPECT_TRUE(rig.hub.writes.empty()) << "what the heater reports is not echoed back";
}

TEST(PlatformHeaterNightLight, NothingIsShownUntilTheLightHasBeenRead) {
  NightLightRig rig;
  rig.state.set_current_brightness(0.7F);
  rig.hub.deliver([](HeatingReadings &r) { r.night_light_brightness = 100; });
  EXPECT_FALSE(rig.state.current_values.is_on());
  EXPECT_FLOAT_EQ(rig.state.current_values.get_brightness(), 0.7F) << "no on/off reading: nothing published";
}

TEST(PlatformHeaterNightLight, SwitchingOnAtANewBrightnessWritesBrightnessThenOnAndReadsBothBack) {
  NightLightRig rig;
  rig.deliver(0, 100);
  rig.hub.read_replies[HeatingFunction::NIGHT_LIGHT] = 1.0F;
  rig.hub.read_replies[HeatingFunction::NIGHT_LIGHT_BRIGHTNESS] = 50.0F;

  rig.request(true, 0.5F);

  ASSERT_EQ(rig.hub.writes.size(), 2u);
  EXPECT_EQ(rig.hub.writes[0].fn, HeatingFunction::NIGHT_LIGHT_BRIGHTNESS);
  EXPECT_FLOAT_EQ(rig.hub.writes[0].value, 50.0F);
  EXPECT_EQ(rig.hub.writes[1].fn, HeatingFunction::NIGHT_LIGHT);
  EXPECT_FLOAT_EQ(rig.hub.writes[1].value, 1.0F);
  EXPECT_EQ(rig.hub.reads,
            (std::vector<HeatingFunction>{HeatingFunction::NIGHT_LIGHT, HeatingFunction::NIGHT_LIGHT_BRIGHTNESS}));
  EXPECT_EQ(rig.hub.read_tries[0], EXCHANGE_RETRY_COUNT);
  EXPECT_TRUE(rig.state.current_values.is_on());
  EXPECT_FLOAT_EQ(rig.state.current_values.get_brightness(), 0.5F);
}

TEST(PlatformHeaterNightLight, AWriteIsPublishedOnceWithBothReadBacks) {
  // Counts what reaches the LightState while a write runs: the first read-back alone (on, with the
  // old 100 %) must not be shown before the brightness read-back.
  NightLightRig rig;
  rig.deliver(0, 100);
  rig.hub.read_replies[HeatingFunction::NIGHT_LIGHT] = 1.0F;
  rig.hub.read_replies[HeatingFunction::NIGHT_LIGHT_BRIGHTNESS] = 50.0F;
  std::vector<float> shown;
  rig.hub.register_device_callback([&rig, &shown](const std::string &, const IoDevice &) {
    shown.push_back(rig.state.current_values.get_brightness());
  });

  rig.request(true, 0.5F);

  for (const float brightness : shown)
    EXPECT_FLOAT_EQ(brightness, 0.5F) << "no stale brightness while the write runs";
  EXPECT_FLOAT_EQ(rig.state.current_values.get_brightness(), 0.5F);
  rig.request(false, 0.5F);
  EXPECT_EQ(rig.hub.writes.back().fn, HeatingFunction::NIGHT_LIGHT) << "the next request is not swallowed";
  EXPECT_FLOAT_EQ(rig.hub.writes.back().value, 0.0F);
}

TEST(PlatformHeaterNightLight, OnAtTheBrightnessTheHeaterHoldsWritesOnlyOnOff) {
  NightLightRig rig;
  rig.deliver(0, 100);
  rig.hub.read_replies[HeatingFunction::NIGHT_LIGHT] = 1.0F;

  rig.request(true, 1.0F);

  ASSERT_EQ(rig.hub.writes.size(), 1u) << "on/off never depends on the brightness write";
  EXPECT_EQ(rig.hub.writes[0].fn, HeatingFunction::NIGHT_LIGHT);
  EXPECT_EQ(rig.hub.reads, (std::vector<HeatingFunction>{HeatingFunction::NIGHT_LIGHT}));
}

TEST(PlatformHeaterNightLight, OffWritesOnlyOnOff) {
  NightLightRig rig;
  rig.deliver(1, 100);
  rig.hub.read_replies[HeatingFunction::NIGHT_LIGHT] = 0.0F;

  rig.request(false, 0.3F);

  ASSERT_EQ(rig.hub.writes.size(), 1u);
  EXPECT_EQ(rig.hub.writes[0].fn, HeatingFunction::NIGHT_LIGHT);
  EXPECT_FLOAT_EQ(rig.hub.writes[0].value, 0.0F);
  EXPECT_FALSE(rig.state.current_values.is_on());
}

TEST(PlatformHeaterNightLight, ABrightnessThatRoundsToZeroIsSentAsOnePercent) {
  NightLightRig rig;
  rig.deliver(0, 100);
  rig.request(true, 0.004F);
  ASSERT_FALSE(rig.hub.writes.empty());
  EXPECT_EQ(rig.hub.writes[0].fn, HeatingFunction::NIGHT_LIGHT_BRIGHTNESS);
  EXPECT_FLOAT_EQ(rig.hub.writes[0].value, 1.0F);
}

TEST(PlatformHeaterNightLight, ARefusedBrightnessWriteStillSwitchesTheLightAndIsNotReadBack) {
  NightLightRig rig;
  rig.deliver(0, 100);
  rig.hub.refused_writes = {HeatingFunction::NIGHT_LIGHT_BRIGHTNESS};
  rig.hub.read_replies[HeatingFunction::NIGHT_LIGHT] = 1.0F;

  rig.request(true, 0.5F);

  ASSERT_EQ(rig.hub.writes.size(), 2u);
  EXPECT_EQ(rig.hub.writes[1].fn, HeatingFunction::NIGHT_LIGHT);
  EXPECT_EQ(rig.hub.reads, (std::vector<HeatingFunction>{HeatingFunction::NIGHT_LIGHT}));
  EXPECT_TRUE(rig.state.current_values.is_on());
  EXPECT_FLOAT_EQ(rig.state.current_values.get_brightness(), 1.0F) << "the brightness the heater still holds";
}

TEST(PlatformHeaterNightLight, AnUnacknowledgedSwitchIsNotReadBackAndShowsTheHeatersLastReading) {
  NightLightRig rig;
  rig.deliver(0, 100);
  rig.hub.write_result = false;

  rig.request(true, 1.0F);

  ASSERT_EQ(rig.hub.writes.size(), 1u);
  EXPECT_TRUE(rig.hub.reads.empty()) << "a heater that did not answer the write is not asked again";
  EXPECT_FALSE(rig.state.current_values.is_on()) << "never shows a command the heater did not confirm";
}
