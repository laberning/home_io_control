/// @file platform_climate_controls_test.cpp
/// @brief Tests for the generated per-heater companions: the window sensor and the open-window
/// detection switch.

#include "platform_climate_controls.h"

#include "hub_core.h"

#include "test_helpers.h"

#include <map>
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
  std::map<HeatingFunction, float> read_replies;

  bool send_heating_command(const std::string &, HeatingFunction fn, float value) override {
    this->writes.push_back({fn, value});
    return this->write_result;
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
