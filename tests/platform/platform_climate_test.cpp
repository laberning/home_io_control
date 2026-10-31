/// @file platform_climate_test.cpp
/// @brief Tests for the experimental IO-Homecontrol climate platform (CMD_WRITE_PRIVATE 0x20).

#include "platform_climate.h"
#include "hub_core.h"

#include "test_helpers.h"

#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace esphome::home_io_control;
using namespace esphome::climate;

namespace {

class MockHubClimate : public test::MockPlatformHubBase {
 public:
  struct HeatingCall {
    std::string device_id;
    HeatingFunction fn;
    float value;
  };

  std::vector<HeatingCall> calls;
  bool send_result{true};
  /// Registers read through read_heating_register(), in order.
  std::vector<HeatingFunction> reads;
  /// One set_heating_temperature() call.
  struct TemperatureCall {
    uint16_t main_parameter;
    HeatingFp fp;
  };
  std::vector<TemperatureCall> temperature_calls;
  bool temperature_result{true};
  /// Heating levels sent through set_heating_level(), in order.
  std::vector<uint8_t> levels;
  bool level_result{true};
  /// The try cap each read was asked with.
  std::vector<uint8_t> read_tries;
  /// What the heater answers a read with; a register not listed here does not answer.
  std::map<HeatingFunction, float> read_replies;

  bool set_heating_temperature(const std::string & /*device_id*/, uint16_t main_parameter,
                               const HeatingFp &fp) override {
    this->temperature_calls.push_back({main_parameter, fp});
    return this->temperature_result;
  }

  bool set_heating_level(const std::string & /*device_id*/, uint8_t level) override {
    this->levels.push_back(level);
    return this->level_result;
  }

  bool read_heating_register(const std::string &device_id, HeatingFunction fn, float *value_out,
                             uint8_t max_tries) override {
    this->reads.push_back(fn);
    this->read_tries.push_back(max_tries);
    const auto reply = this->read_replies.find(fn);
    if (reply == this->read_replies.end())
      return false;
    // What the real hub does with a decoded reply: store it on the device record and notify.
    if (IoDevice *dev = this->registry_.get(device_id); dev != nullptr) {
      dev->heating.store(fn, reply->second);
      this->registry_.notify(device_id);
    }
    if (value_out != nullptr)
      *value_out = reply->second;
    return true;
  }

  bool send_heating_command(const std::string &device_id, HeatingFunction fn, float value) override {
    this->calls.push_back({device_id, fn, value});
    return this->send_result;
  }
};

IOHomeClimate make_bound_climate(MockHubClimate &hub, DeviceType type = DeviceType::ELECTRICAL_HEATER,
                                 uint8_t subtype = 1) {
  IOHomeClimate entity;
  entity.set_parent(&hub);
  entity.set_device_id("ABC123");
  entity.set_device_type(type);
  entity.set_subtype(subtype);
  return entity;
}

void perform_mode(IOHomeClimate &entity, ClimateMode mode) {
  auto call = entity.make_call();
  call.set_mode(mode);
  call.perform();
}

}  // namespace

TEST(PlatformClimate, TraitsAdvertiseRangeModesAndPresets) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  const ClimateTraits traits = entity.traits();

  EXPECT_FLOAT_EQ(traits.get_visual_min_temperature(), 7.0F);
  EXPECT_FLOAT_EQ(traits.get_visual_max_temperature(), 28.0F);
  EXPECT_FLOAT_EQ(traits.get_visual_target_temperature_step(), 0.5F);
  EXPECT_TRUE(traits.get_supports_current_temperature());
  EXPECT_TRUE(traits.get_supports_action());

  EXPECT_TRUE(traits.supports_mode(CLIMATE_MODE_OFF));
  EXPECT_TRUE(traits.supports_mode(CLIMATE_MODE_HEAT));
  EXPECT_TRUE(traits.supports_mode(CLIMATE_MODE_AUTO));
  EXPECT_FALSE(traits.supports_mode(CLIMATE_MODE_COOL));
}

TEST(PlatformClimate, SetupRegistersTheProgramAndAutoCustomPresets) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  EXPECT_EQ(entity.get_supported_custom_presets(), (std::vector<std::string>{"Program", "Auto"}));
  EXPECT_FALSE(entity.is_failed());
}

TEST(PlatformClimate, HvacModesMapToHeatingModesAndPublish) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);

  perform_mode(entity, CLIMATE_MODE_HEAT);
  ASSERT_EQ(hub.calls.size(), 1u);
  EXPECT_EQ(hub.calls[0].device_id, "ABC123");
  EXPECT_EQ(hub.calls[0].fn, HeatingFunction::SET_MODE);
  EXPECT_FLOAT_EQ(hub.calls[0].value, static_cast<float>(HeatingMode::MANUAL));
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);

  perform_mode(entity, CLIMATE_MODE_AUTO);
  EXPECT_EQ(hub.calls.back().fn, HeatingFunction::SET_MODE);
  EXPECT_FLOAT_EQ(hub.calls.back().value, static_cast<float>(HeatingMode::AUTO));
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);

  perform_mode(entity, CLIMATE_MODE_OFF);
  EXPECT_FLOAT_EQ(hub.calls.back().value, static_cast<float>(HeatingMode::OFF));
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF);

  EXPECT_EQ(entity.publish_count_, 3);
}

TEST(PlatformClimate, TargetTemperatureSendsSetTemperature) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);

  auto call = entity.make_call();
  call.set_target_temperature(20.5F);
  call.perform();

  ASSERT_EQ(hub.calls.size(), 1u);
  EXPECT_EQ(hub.calls[0].fn, HeatingFunction::SET_TEMPERATURE);
  EXPECT_FLOAT_EQ(hub.calls[0].value, 20.5F);
  EXPECT_FLOAT_EQ(entity.target_temperature, 20.5F);
  EXPECT_EQ(entity.publish_count_, 1);
}

TEST(PlatformClimate, AutomaticModePresetsSendTheirModeAndShowAtOnce) {
  const struct {
    const char *name;
    HeatingMode mode;
  } cases[] = {{"Program", HeatingMode::PROG}, {"Auto", HeatingMode::AUTO}};
  for (const auto &c : cases) {
    MockHubClimate hub;
    IOHomeClimate entity = make_bound_climate(hub);
    entity.setup();

    auto call = entity.make_call();
    call.set_preset(std::string(c.name));
    call.perform();

    ASSERT_EQ(hub.calls.size(), 1u) << c.name;
    EXPECT_EQ(hub.calls[0].fn, HeatingFunction::SET_MODE) << c.name;
    EXPECT_FLOAT_EQ(hub.calls[0].value, static_cast<float>(c.mode)) << c.name;
    EXPECT_EQ(hub.reads, (std::vector<HeatingFunction>{HeatingFunction::SET_MODE})) << c.name << ": confirm read";
    // Nothing has been read yet, so the command stands in: both modes show as HVAC auto.
    EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO) << c.name;
    ASSERT_TRUE(entity.has_custom_preset()) << c.name;
    EXPECT_EQ(entity.get_custom_preset(), c.name);
    EXPECT_EQ(entity.publish_count_, 1) << c.name;
  }
}

TEST(PlatformClimate, FailedSendDoesNotPublishOrUpdateState) {
  MockHubClimate hub;
  hub.send_result = false;
  IOHomeClimate entity = make_bound_climate(hub);

  perform_mode(entity, CLIMATE_MODE_HEAT);

  ASSERT_EQ(hub.calls.size(), 1u) << "the send is still attempted";
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF) << "state must not advance past a failed send";
  EXPECT_EQ(entity.publish_count_, 0) << "no publish on a failed send";
}

TEST(PlatformClimate, FailedSendDoesNotAdvanceTemperatureOrPreset) {
  MockHubClimate hub;
  hub.send_result = false;
  IOHomeClimate entity = make_bound_climate(hub);

  auto temp = entity.make_call();
  temp.set_target_temperature(21.0F);
  temp.perform();
  EXPECT_TRUE(std::isnan(entity.target_temperature)) << "target must not advance past a failed send";

  auto preset = entity.make_call();
  preset.set_preset(std::string("Auto"));
  preset.perform();
  EXPECT_FALSE(entity.has_custom_preset()) << "preset must not advance past a failed send";

  EXPECT_EQ(entity.publish_count_, 0) << "no publish on any failed send";
}

TEST(PlatformClimate, UnsupportedHvacModeSendsNothing) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);

  perform_mode(entity, CLIMATE_MODE_COOL);  // no mapping to a HeatingMode

  EXPECT_TRUE(hub.calls.empty()) << "a mode with no HeatingMode mapping must not transmit";
  EXPECT_EQ(entity.publish_count_, 0);
}

TEST(PlatformClimate, NonClimateDeviceBindingIsRejectedInSetup) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub, DeviceType::ROLLER_SHUTTER);
  entity.setup();
  EXPECT_TRUE(entity.is_failed()) << "a known non-climate io_device_type must fail the entity";
}

TEST(PlatformClimate, ClimateTypeWithoutAPlatformIsRejectedInSetup) {
  MockHubClimate hub;
  // The Atlantic heat pump and exterior heating have no heating profile (the latter is a fan).
  IOHomeClimate atlantic = make_bound_climate(hub, DeviceType::HEAT_PUMP, 1);
  atlantic.setup();
  EXPECT_TRUE(atlantic.is_failed()) << "the Atlantic heat pump has no heating profile";

  IOHomeClimate exterior = make_bound_climate(hub, DeviceType::EXTERIOR_HEATING, 0);
  exterior.setup();
  EXPECT_TRUE(exterior.is_failed());

  IOHomeClimate thermostat = make_bound_climate(hub, DeviceType::HEATING_TEMPERATURE_INTERFACE, 0);
  thermostat.setup();
  EXPECT_FALSE(thermostat.is_failed());
  IOHomeClimate pump = make_bound_climate(hub, DeviceType::HEAT_PUMP, 0);
  pump.setup();
  EXPECT_FALSE(pump.is_failed());

  IOHomeClimate plain_heater = make_bound_climate(hub, DeviceType::ELECTRICAL_HEATER, 0);
  plain_heater.setup();
  EXPECT_FALSE(plain_heater.is_failed()) << "the plain electrical heater is driven with heating levels";
}

TEST(PlatformClimate, SystemKeyOptionReachesTheRegisteredDevice) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.set_system_key("00112233445566778899AABBCCDDEEFF");
  entity.setup();

  const IoDevice *dev = hub.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_TRUE(dev->has_system_key);
  const uint8_t expected[AES_KEY_SIZE] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                          0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  EXPECT_EQ(0, memcmp(dev->system_key, expected, AES_KEY_SIZE));
}

TEST(PlatformClimate, WithoutASystemKeyOptionTheDeviceUsesTheHubKey) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  const IoDevice *dev = hub.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FALSE(dev->has_system_key);
}

TEST(PlatformClimate, UnknownDeviceTypeIsToleratedInSetup) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub, DeviceType::UNKNOWN);
  entity.setup();
  EXPECT_FALSE(entity.is_failed()) << "an unknown type is left for discovery to resolve";
}

// ========================================================================================
// State comes from the heater's replies
// ========================================================================================

namespace {

/// Publish `edit` applied to the device's stored record, as the hub does after a decoded reply.
void deliver_readings(MockHubClimate &hub, const std::function<void(HeatingReadings &)> &edit) {
  IoDevice dev = *hub.get_device("ABC123");
  edit(dev.heating);
  hub.trigger_device_update("ABC123", dev, /*cache_device=*/true);
}

}  // namespace

TEST(PlatformClimate, PollReadingsDriveTheCard) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  deliver_readings(hub, [](HeatingReadings &r) {
    r.room_temperature_c = 20.75F;
    r.setpoint_c = 19.5F;
    r.mode = static_cast<int16_t>(HeatingMode::MANUAL);
    r.regulator = static_cast<int16_t>(HeatingRegulatorState::HEATING);
    r.max_setpoint_c = 26.0F;
  });

  EXPECT_FLOAT_EQ(entity.current_temperature, 20.75F);
  EXPECT_FLOAT_EQ(entity.target_temperature, 19.5F);
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  EXPECT_EQ(entity.action, CLIMATE_ACTION_HEATING);
  EXPECT_FLOAT_EQ(entity.traits().get_visual_max_temperature(), 26.0F) << "the heater's own maximum";
  EXPECT_EQ(entity.publish_count_, 1);

  // The same readings again change nothing and publish nothing.
  deliver_readings(hub, [](HeatingReadings &) {});
  EXPECT_EQ(entity.publish_count_, 1);
}

TEST(PlatformClimate, MaximumSetpointIsNeverPastWhatCanBeEncoded) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_readings(hub, [](HeatingReadings &r) { r.max_setpoint_c = 30.0F; });
  EXPECT_FLOAT_EQ(entity.traits().get_visual_max_temperature(), HEATING_TEMP_MAX_C);
}

TEST(PlatformClimate, ObservedModesMapToHvacModesAndTheAutomaticPresets) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  deliver_readings(hub, [](HeatingReadings &r) { r.mode = static_cast<int16_t>(HeatingMode::AUTO); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);
  EXPECT_TRUE(entity.has_custom_preset());
  EXPECT_EQ(entity.get_custom_preset(), "Auto");

  deliver_readings(hub, [](HeatingReadings &r) { r.mode = static_cast<int16_t>(HeatingMode::PROG); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);
  EXPECT_TRUE(entity.has_custom_preset());
  EXPECT_EQ(entity.get_custom_preset(), "Program");

  deliver_readings(hub, [](HeatingReadings &r) { r.mode = static_cast<int16_t>(HeatingMode::MANUAL); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  EXPECT_FALSE(entity.has_custom_preset()) << "leaving program mode drops the Program preset";

  deliver_readings(hub, [](HeatingReadings &r) { r.mode = static_cast<int16_t>(HeatingMode::EXTERNAL_SETPOINT); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT) << "an external setpoint is shown as heat";

  deliver_readings(hub, [](HeatingReadings &r) { r.mode = static_cast<int16_t>(HeatingMode::OFF); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF);

  const int published = entity.publish_count_;
  deliver_readings(hub, [](HeatingReadings &r) { r.mode = 9; });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF) << "an unknown mode value leaves the mode alone";
  EXPECT_EQ(entity.publish_count_, published);
}

TEST(PlatformClimate, ActionFollowsTheRegulatorAndAnOffHeaterIsNotIdle) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  deliver_readings(hub, [](HeatingReadings &r) {
    r.mode = static_cast<int16_t>(HeatingMode::MANUAL);
    r.regulator = static_cast<int16_t>(HeatingRegulatorState::HEATING);
  });
  EXPECT_EQ(entity.action, CLIMATE_ACTION_HEATING);

  deliver_readings(hub, [](HeatingReadings &r) { r.regulator = static_cast<int16_t>(HeatingRegulatorState::STANDBY); });
  EXPECT_EQ(entity.action, CLIMATE_ACTION_IDLE);

  deliver_readings(hub, [](HeatingReadings &r) { r.regulator = static_cast<int16_t>(HeatingRegulatorState::NONE); });
  EXPECT_EQ(entity.action, CLIMATE_ACTION_IDLE);

  deliver_readings(hub, [](HeatingReadings &r) { r.mode = static_cast<int16_t>(HeatingMode::OFF); });
  EXPECT_EQ(entity.action, CLIMATE_ACTION_OFF);
}

TEST(PlatformClimate, ASensorFaultShowsNoCurrentTemperature) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  deliver_readings(hub, [](HeatingReadings &r) { r.room_temperature_c = 18.0F; });
  ASSERT_FLOAT_EQ(entity.current_temperature, 18.0F);
  deliver_readings(hub, [](HeatingReadings &r) { r.room_temperature_c = NAN; });  // raw -1 decodes as NaN
  EXPECT_TRUE(std::isnan(entity.current_temperature));
}

TEST(PlatformClimate, UpdatesForOtherDevicesAreIgnored) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  DeviceConfig config{};
  hub.add_device("DEF456", config);
  IoDevice other = *hub.get_device("DEF456");
  other.heating.room_temperature_c = 25.0F;
  hub.trigger_device_update("DEF456", other, /*cache_device=*/true);

  EXPECT_TRUE(std::isnan(entity.current_temperature));
  EXPECT_EQ(entity.publish_count_, 0);
}

TEST(PlatformClimate, AcceptedSetpointWriteIsReadBackAndTheHeatersValueIsShown) {
  MockHubClimate hub;
  hub.read_replies[HeatingFunction::SET_TEMPERATURE] = 19.0F;  // the heater held the request to 19.0
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  auto call = entity.make_call();
  call.set_target_temperature(20.5F);
  call.perform();

  ASSERT_EQ(hub.reads.size(), 1u);
  EXPECT_EQ(hub.reads[0], HeatingFunction::SET_TEMPERATURE);
  EXPECT_EQ(hub.read_tries[0], EXCHANGE_RETRY_COUNT) << "the user waits on a read-back: full budget";
  EXPECT_FLOAT_EQ(entity.target_temperature, 19.0F) << "what the heater reports, not what was commanded";
  EXPECT_EQ(entity.publish_count_, 1) << "one publish: the read-back's, not a second for the command";
}

TEST(PlatformClimate, AcceptedModeWriteIsReadBack) {
  MockHubClimate hub;
  hub.read_replies[HeatingFunction::SET_MODE] = static_cast<float>(HeatingMode::AUTO);
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  perform_mode(entity, CLIMATE_MODE_HEAT);

  ASSERT_EQ(hub.reads.size(), 1u);
  EXPECT_EQ(hub.reads[0], HeatingFunction::SET_MODE);
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO) << "the heater says auto";
  EXPECT_EQ(entity.publish_count_, 1);
}

TEST(PlatformClimate, AfterTheFirstReadAWriteWhoseReadBackFailsShowsNothingNew) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_readings(hub, [](HeatingReadings &r) {
    r.mode = static_cast<int16_t>(HeatingMode::MANUAL);
    r.setpoint_c = 19.5F;
  });
  const int published = entity.publish_count_;

  perform_mode(entity, CLIMATE_MODE_AUTO);  // acknowledged, but the register read draws no answer
  auto call = entity.make_call();
  call.set_target_temperature(22.0F);
  call.perform();

  EXPECT_EQ(hub.calls.size(), 2u) << "both writes were sent";
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT) << "no prediction: the entity keeps the last observed mode";
  EXPECT_FLOAT_EQ(entity.target_temperature, 19.5F);
  EXPECT_EQ(entity.publish_count_, published);
}

TEST(PlatformClimate, NothingIsReadBackAfterAFailedWrite) {
  MockHubClimate hub;
  hub.send_result = false;
  IOHomeClimate entity = make_bound_climate(hub);
  perform_mode(entity, CLIMATE_MODE_HEAT);
  EXPECT_TRUE(hub.reads.empty());
}

TEST(PlatformClimate, ObservedProgramThenAutoPresetWaitsForTheReadBack) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_readings(hub, [](HeatingReadings &r) { r.mode = static_cast<int16_t>(HeatingMode::PROG); });
  const int published = entity.publish_count_;

  auto call = entity.make_call();
  call.set_preset(std::string("Auto"));
  call.perform();
  ASSERT_EQ(hub.calls.size(), 1u);
  EXPECT_FLOAT_EQ(hub.calls[0].value, static_cast<float>(HeatingMode::AUTO));
  EXPECT_EQ(entity.get_custom_preset(), "Program") << "a read has succeeded: the card waits for the read-back";
  EXPECT_EQ(entity.publish_count_, published);

  deliver_readings(hub, [](HeatingReadings &r) { r.mode = static_cast<int16_t>(HeatingMode::AUTO); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);
  EXPECT_EQ(entity.get_custom_preset(), "Auto");
}

TEST(PlatformClimate, ReadBackReplacesTheAutoPresetWithProgram) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_readings(hub, [](HeatingReadings &r) { r.mode = static_cast<int16_t>(HeatingMode::AUTO); });
  ASSERT_EQ(entity.get_custom_preset(), "Auto");

  deliver_readings(hub, [](HeatingReadings &r) { r.mode = static_cast<int16_t>(HeatingMode::PROG); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);
  EXPECT_EQ(entity.get_custom_preset(), "Program");
}

TEST(PlatformClimate, ObservedProgramThenHeatDropsThePresetOnTheReadBack) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_readings(hub, [](HeatingReadings &r) { r.mode = static_cast<int16_t>(HeatingMode::PROG); });

  perform_mode(entity, CLIMATE_MODE_HEAT);
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO) << "the card waits for the read-back";
  EXPECT_EQ(entity.get_custom_preset(), "Program");

  deliver_readings(hub, [](HeatingReadings &r) { r.mode = static_cast<int16_t>(HeatingMode::MANUAL); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  EXPECT_FALSE(entity.has_custom_preset());
}

TEST(PlatformClimate, SetupPollsTheHeaterLikeTheOtherPlatforms) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.set_status_poll_interval(120000);
  entity.setup();
  EXPECT_EQ(hub.poll_policy_.get_interval("ABC123"), 120000u);
}

// ========================================================================================
// Traits come from the heating profile
// ========================================================================================

namespace {

std::vector<ClimateMode> supported_modes(const ClimateTraits &traits) {
  std::vector<ClimateMode> modes;
  for (const ClimateMode mode : {CLIMATE_MODE_OFF, CLIMATE_MODE_HEAT_COOL, CLIMATE_MODE_COOL, CLIMATE_MODE_HEAT,
                                 CLIMATE_MODE_FAN_ONLY, CLIMATE_MODE_DRY, CLIMATE_MODE_AUTO}) {
    if (traits.supports_mode(mode))
      modes.push_back(mode);
  }
  return modes;
}

std::vector<ClimatePreset> supported_presets(const ClimateTraits &traits) {
  std::vector<ClimatePreset> presets;
  for (const ClimatePreset preset :
       {CLIMATE_PRESET_NONE, CLIMATE_PRESET_HOME, CLIMATE_PRESET_AWAY, CLIMATE_PRESET_BOOST, CLIMATE_PRESET_COMFORT,
        CLIMATE_PRESET_ECO, CLIMATE_PRESET_SLEEP, CLIMATE_PRESET_ACTIVITY}) {
    if (traits.supports_preset(preset))
      presets.push_back(preset);
  }
  return presets;
}

IOHomeClimate make_level_climate(MockHubClimate &hub) {
  return make_bound_climate(hub, DeviceType::ELECTRICAL_HEATER, 0);
}

}  // namespace

TEST(PlatformClimate, SetpointHeaterTraitsAreExactlyTheProfile) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  const ClimateTraits traits = entity.traits();

  EXPECT_EQ(supported_modes(traits),
            (std::vector<ClimateMode>{CLIMATE_MODE_OFF, CLIMATE_MODE_HEAT, CLIMATE_MODE_AUTO}));
  EXPECT_TRUE(supported_presets(traits).empty());
  EXPECT_EQ(entity.get_supported_custom_presets(), (std::vector<std::string>{"Program", "Auto"}));
  EXPECT_TRUE(traits.get_supports_current_temperature());
  EXPECT_TRUE(traits.get_supports_action());
}

TEST(PlatformClimate, LevelHeaterTraitsAreExactlyTheProfile) {
  MockHubClimate hub;
  IOHomeClimate entity = make_level_climate(hub);
  entity.setup();
  ASSERT_FALSE(entity.is_failed());
  const ClimateTraits traits = entity.traits();

  EXPECT_EQ(supported_modes(traits), (std::vector<ClimateMode>{CLIMATE_MODE_OFF, CLIMATE_MODE_HEAT}));
  EXPECT_EQ(supported_presets(traits), (std::vector<ClimatePreset>{CLIMATE_PRESET_AWAY, CLIMATE_PRESET_BOOST,
                                                                   CLIMATE_PRESET_COMFORT, CLIMATE_PRESET_ECO}));
  EXPECT_EQ(entity.get_supported_custom_presets(), (std::vector<std::string>{"Comfort -1", "Comfort -2", "Secured"}));
  EXPECT_FALSE(traits.get_supports_current_temperature()) << "a level heater reports no temperature";
  EXPECT_FALSE(traits.get_supports_action());
}

TEST(PlatformClimate, ATypeNotKnownYetOffersOnlyOff) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub, DeviceType::UNKNOWN, 0);
  entity.setup();
  EXPECT_EQ(supported_modes(entity.traits()), (std::vector<ClimateMode>{CLIMATE_MODE_OFF}));

  perform_mode(entity, CLIMATE_MODE_HEAT);
  EXPECT_TRUE(hub.calls.empty());
  EXPECT_TRUE(hub.levels.empty());
}

// ========================================================================================
// Heating-level control path
// ========================================================================================

TEST(PlatformClimate, LevelHeaterModesSendLevelsAndNeverRegisters) {
  MockHubClimate hub;
  IOHomeClimate entity = make_level_climate(hub);
  entity.setup();

  perform_mode(entity, CLIMATE_MODE_OFF);
  perform_mode(entity, CLIMATE_MODE_HEAT);

  EXPECT_EQ(hub.levels, (std::vector<uint8_t>{heating_level::OFF, heating_level::COMFORT}));
  EXPECT_TRUE(hub.calls.empty()) << "no register write for a heater commanded with levels";
  EXPECT_TRUE(hub.reads.empty()) << "the hub's settle poll reads the level, not the entity";
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT) << "the commanded mode stands in until a level has been read";
}

TEST(PlatformClimate, LevelHeaterPresetsSendTheirLevels) {
  MockHubClimate hub;
  IOHomeClimate entity = make_level_climate(hub);
  entity.setup();

  for (const ClimatePreset preset :
       {CLIMATE_PRESET_AWAY, CLIMATE_PRESET_ECO, CLIMATE_PRESET_COMFORT, CLIMATE_PRESET_BOOST}) {
    auto call = entity.make_call();
    call.set_preset(preset);
    call.perform();
  }
  for (const char *name : {"Comfort -1", "Comfort -2"}) {
    auto call = entity.make_call();
    call.set_preset(std::string(name));
    call.perform();
  }

  EXPECT_EQ(hub.levels, (std::vector<uint8_t>{heating_level::FROST_PROTECTION, heating_level::ECO,
                                              heating_level::COMFORT, heating_level::BOOST,
                                              heating_level::COMFORT_MINUS_1, heating_level::COMFORT_MINUS_2}));
  EXPECT_EQ(entity.get_custom_preset(), "Comfort -2");
}

TEST(PlatformClimate, ACommandedLevelPresetShowsAsTheReadBackWould) {
  // Before the first read, a preset stands in the way its level is read back: mode heat plus the preset.
  MockHubClimate hub;
  IOHomeClimate entity = make_level_climate(hub);
  entity.setup();

  auto call = entity.make_call();
  call.set_preset(std::string("Comfort -1"));
  call.perform();
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  EXPECT_EQ(entity.get_custom_preset(), "Comfort -1");
  EXPECT_EQ(entity.publish_count_, 1);
}

TEST(PlatformClimate, TheSecuredPresetIsShownButNeverCommanded) {
  MockHubClimate hub;
  IOHomeClimate entity = make_level_climate(hub);
  entity.setup();

  auto call = entity.make_call();
  call.set_preset(std::string("Secured"));
  call.perform();
  EXPECT_TRUE(hub.levels.empty());
  EXPECT_FALSE(entity.has_custom_preset());

  deliver_readings(hub, [](HeatingReadings &r) { r.level = heating_level::SECURED; });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  EXPECT_EQ(entity.get_custom_preset(), "Secured");
}

TEST(PlatformClimate, ALevelHeaterIgnoresATargetTemperature) {
  MockHubClimate hub;
  IOHomeClimate entity = make_level_climate(hub);
  entity.setup();
  auto call = entity.make_call();
  call.set_target_temperature(21.0F);
  call.perform();
  EXPECT_TRUE(hub.calls.empty());
  EXPECT_TRUE(hub.levels.empty());
  EXPECT_EQ(entity.publish_count_, 0);
}

TEST(PlatformClimate, AFailedLevelWriteShowsNothing) {
  MockHubClimate hub;
  hub.level_result = false;
  IOHomeClimate entity = make_level_climate(hub);
  entity.setup();
  perform_mode(entity, CLIMATE_MODE_HEAT);
  EXPECT_EQ(hub.levels.size(), 1u);
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF);
  EXPECT_EQ(entity.publish_count_, 0);
}

TEST(PlatformClimate, ObservedLevelsDriveModeAndPreset) {
  MockHubClimate hub;
  IOHomeClimate entity = make_level_climate(hub);
  entity.setup();

  deliver_readings(hub, [](HeatingReadings &r) { r.level = heating_level::ECO; });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  ASSERT_TRUE(entity.preset.has_value());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_ECO);

  deliver_readings(hub, [](HeatingReadings &r) { r.level = heating_level::COMFORT_MINUS_1; });
  EXPECT_EQ(entity.get_custom_preset(), "Comfort -1");
  EXPECT_FALSE(entity.preset.has_value()) << "a custom preset replaces the standard one";

  deliver_readings(hub, [](HeatingReadings &r) { r.level = heating_level::BOOST; });
  EXPECT_FALSE(entity.has_custom_preset());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_BOOST);

  deliver_readings(hub, [](HeatingReadings &r) { r.level = heating_level::OFF; });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF);
  EXPECT_FALSE(entity.preset.has_value());
  EXPECT_FALSE(entity.has_custom_preset());

  const int published = entity.publish_count_;
  deliver_readings(hub, [](HeatingReadings &r) { r.level = 0x20; });  // a level nobody defined
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF);
  EXPECT_EQ(entity.publish_count_, published);
}

TEST(PlatformClimate, ALevelHeaterNeverShowsAnActionOrTemperature) {
  MockHubClimate hub;
  IOHomeClimate entity = make_level_climate(hub);
  entity.setup();
  deliver_readings(hub, [](HeatingReadings &r) {
    r.level = heating_level::COMFORT;
    r.room_temperature_c = 20.0F;  // not part of this profile; must not leak onto the card
    r.regulator = 2;
  });
  EXPECT_TRUE(std::isnan(entity.current_temperature));
  EXPECT_EQ(entity.action, CLIMATE_ACTION_OFF);
}

// ========================================================================================
// Temperature in the main parameter (heating temperature interface, heat pump)
// ========================================================================================

namespace {

IOHomeClimate make_thermostat(MockHubClimate &hub) {
  return make_bound_climate(hub, DeviceType::HEATING_TEMPERATURE_INTERFACE, 0);
}
IOHomeClimate make_pump(MockHubClimate &hub) { return make_bound_climate(hub, DeviceType::HEAT_PUMP, 0); }

void expect_call(const MockHubClimate::TemperatureCall &call, uint16_t main_parameter, HeatingFpKind kind,
                 uint16_t value) {
  EXPECT_EQ(call.main_parameter, main_parameter);
  EXPECT_EQ(call.fp.kind, kind);
  EXPECT_EQ(call.fp.value, value);
}

}  // namespace

TEST(PlatformClimate, ThermostatTraitsAreExactlyTheProfile) {
  MockHubClimate hub;
  IOHomeClimate entity = make_thermostat(hub);
  entity.setup();
  const ClimateTraits traits = entity.traits();
  EXPECT_EQ(supported_modes(traits),
            (std::vector<ClimateMode>{CLIMATE_MODE_OFF, CLIMATE_MODE_HEAT, CLIMATE_MODE_AUTO}));
  EXPECT_EQ(supported_presets(traits), (std::vector<ClimatePreset>{CLIMATE_PRESET_COMFORT, CLIMATE_PRESET_ECO}));
  EXPECT_EQ(entity.get_supported_custom_presets(), (std::vector<std::string>{"Secured"}));
  EXPECT_FALSE(traits.get_supports_current_temperature()) << "no temperature is invented";
  EXPECT_FALSE(traits.get_supports_action());
  EXPECT_FLOAT_EQ(traits.get_visual_min_temperature(), 5.0F);
  EXPECT_FLOAT_EQ(traits.get_visual_max_temperature(), 30.0F);
}

TEST(PlatformClimate, HeatPumpTraitsAreExactlyTheProfile) {
  MockHubClimate hub;
  IOHomeClimate entity = make_pump(hub);
  entity.setup();
  const ClimateTraits traits = entity.traits();
  EXPECT_EQ(supported_modes(traits), (std::vector<ClimateMode>{CLIMATE_MODE_OFF, CLIMATE_MODE_HEAT}))
      << "heat and off; no cooling";
  EXPECT_EQ(supported_presets(traits), (std::vector<ClimatePreset>{CLIMATE_PRESET_COMFORT, CLIMATE_PRESET_ECO}));
  EXPECT_EQ(entity.get_supported_custom_presets(), (std::vector<std::string>{"Setback", "Halted"}));
}

TEST(PlatformClimate, ThermostatSetpointIsTheTemperatureWithTheManualFlag) {
  MockHubClimate hub;
  IOHomeClimate entity = make_thermostat(hub);
  entity.setup();
  auto call = entity.make_call();
  call.set_target_temperature(20.0F);
  call.perform();

  ASSERT_EQ(hub.temperature_calls.size(), 1u);
  expect_call(hub.temperature_calls[0], 25600, HeatingFpKind::FP9, heating_mode_flags::MANUAL);
  EXPECT_TRUE(hub.calls.empty()) << "no register write";
  EXPECT_FLOAT_EQ(entity.target_temperature, 20.0F) << "the commanded value stands in until a reading";
}

TEST(PlatformClimate, ThermostatUsesTheYamlRangeButAHeatPumpNeverDoes) {
  MockHubClimate hub;
  IOHomeClimate thermostat = make_thermostat(hub);
  thermostat.set_temperature_range(10.0F, 30.0F);
  thermostat.setup();
  auto call = thermostat.make_call();
  call.set_target_temperature(20.0F);
  call.perform();
  ASSERT_EQ(hub.temperature_calls.size(), 1u);
  EXPECT_EQ(hub.temperature_calls[0].main_parameter, 25600) << "the middle of 10..30";

  MockHubClimate pump_hub;
  IOHomeClimate pump = make_pump(pump_hub);
  pump.set_temperature_range(10.0F, 30.0F);  // not reachable from YAML; the profile still ignores it
  pump.setup();
  auto pump_call = pump.make_call();
  pump_call.set_target_temperature(20.0F);
  pump_call.perform();
  ASSERT_EQ(pump_hub.temperature_calls.size(), 1u);
  EXPECT_EQ(pump_hub.temperature_calls[0].main_parameter, 25600) << "20 C over the fixed -40..80 is also half way";
  auto top = pump.make_call();
  top.set_target_temperature(30.0F);
  top.perform();
  EXPECT_EQ(pump_hub.temperature_calls[1].main_parameter, 29867) << "round(70 x 51200 / 120), not the YAML range's top";
}

TEST(PlatformClimate, HeatPumpSetpointCarriesNoFunctionalParameter) {
  MockHubClimate hub;
  IOHomeClimate entity = make_pump(hub);
  entity.setup();
  auto call = entity.make_call();
  call.set_target_temperature(21.0F);
  call.perform();
  ASSERT_EQ(hub.temperature_calls.size(), 1u);
  EXPECT_EQ(hub.temperature_calls[0].fp.kind, HeatingFpKind::NONE);
}

TEST(PlatformClimate, ThermostatPresetsAreTheAliasAloneWithNoFunctionalParameter) {
  MockHubClimate hub;
  IOHomeClimate entity = make_thermostat(hub);
  entity.setup();
  auto comfort = entity.make_call();
  comfort.set_preset(CLIMATE_PRESET_COMFORT);
  comfort.perform();
  auto eco = entity.make_call();
  eco.set_preset(CLIMATE_PRESET_ECO);
  eco.perform();
  auto secured = entity.make_call();
  secured.set_preset(std::string("Secured"));
  secured.perform();
  ASSERT_EQ(hub.temperature_calls.size(), 3u);
  expect_call(hub.temperature_calls[0], 0xD80F, HeatingFpKind::NONE, 0);
  expect_call(hub.temperature_calls[1], 0xD80E, HeatingFpKind::NONE, 0);
  expect_call(hub.temperature_calls[2], 0xD80A, HeatingFpKind::NONE, 0);
  EXPECT_EQ(entity.get_custom_preset(), "Secured");
}

TEST(PlatformClimate, ThermostatModesAreTheDevicesOwnCommands) {
  MockHubClimate hub;
  IOHomeClimate entity = make_thermostat(hub);
  entity.setup();

  perform_mode(entity, CLIMATE_MODE_AUTO);
  perform_mode(entity, CLIMATE_MODE_OFF);
  ASSERT_EQ(hub.temperature_calls.size(), 2u);
  expect_call(hub.temperature_calls[0], 0xD80F, HeatingFpKind::FP9, heating_mode_flags::AUTO);
  expect_call(hub.temperature_calls[1], 0xD400, HeatingFpKind::FP9, heating_mode_flags::OFF);
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF);

  // Heat is the current target temperature with the manual flag; with no target known, nothing is sent.
  perform_mode(entity, CLIMATE_MODE_HEAT);
  EXPECT_EQ(hub.temperature_calls.size(), 2u);
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF) << "a refused mode is not shown";
  auto target = entity.make_call();
  target.set_target_temperature(22.0F);
  target.perform();
  perform_mode(entity, CLIMATE_MODE_HEAT);
  ASSERT_EQ(hub.temperature_calls.size(), 4u);
  expect_call(hub.temperature_calls[3], hub.temperature_calls[2].main_parameter, HeatingFpKind::FP9,
              heating_mode_flags::MANUAL);
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
}

TEST(PlatformClimate, ThermostatHeatUsesTheReportedSetpointWhenNoTargetWasSet) {
  MockHubClimate hub;
  IOHomeClimate entity = make_thermostat(hub);
  entity.setup();
  deliver_readings(hub, [](HeatingReadings &r) { r.setpoint_c = 20.0F; });
  perform_mode(entity, CLIMATE_MODE_HEAT);
  ASSERT_EQ(hub.temperature_calls.size(), 1u);
  expect_call(hub.temperature_calls[0], 25600, HeatingFpKind::FP9, heating_mode_flags::MANUAL);
}

TEST(PlatformClimate, HeatPumpModesAndPresetsCarryFp16) {
  MockHubClimate hub;
  IOHomeClimate entity = make_pump(hub);
  entity.setup();

  perform_mode(entity, CLIMATE_MODE_OFF);
  perform_mode(entity, CLIMATE_MODE_HEAT);
  auto eco = entity.make_call();
  eco.set_preset(CLIMATE_PRESET_ECO);
  eco.perform();
  for (const char *name : {"Setback", "Halted"}) {
    auto custom = entity.make_call();
    custom.set_preset(std::string(name));
    custom.perform();
  }
  ASSERT_EQ(hub.temperature_calls.size(), 5u);
  expect_call(hub.temperature_calls[0], 0xD400, HeatingFpKind::FP16, 0x8000);
  expect_call(hub.temperature_calls[1], 0xD80F, HeatingFpKind::FP16, 0x4000);
  expect_call(hub.temperature_calls[2], 0xD812, HeatingFpKind::FP16, 0x4000);
  expect_call(hub.temperature_calls[3], 0xD80E, HeatingFpKind::FP16, 0x4000);
  expect_call(hub.temperature_calls[4], 0xD813, HeatingFpKind::FP16, 0x4000);
}

TEST(PlatformClimate, AFailedTemperatureSendShowsNothing) {
  MockHubClimate hub;
  hub.temperature_result = false;
  IOHomeClimate entity = make_thermostat(hub);
  entity.setup();
  auto call = entity.make_call();
  call.set_target_temperature(20.0F);
  call.perform();
  perform_mode(entity, CLIMATE_MODE_AUTO);
  EXPECT_TRUE(std::isnan(entity.target_temperature));
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF);
  EXPECT_EQ(entity.publish_count_, 0);
}

TEST(PlatformClimate, ObservedAliasAndTemperatureDrivePresetAndTargetNotTheCommandedMode) {
  MockHubClimate hub;
  IOHomeClimate entity = make_thermostat(hub);
  entity.setup();
  perform_mode(entity, CLIMATE_MODE_AUTO);

  deliver_readings(hub, [](HeatingReadings &r) { r.level = heating_alias::THERMOSTAT_ECO; });
  ASSERT_TRUE(entity.preset.has_value());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_ECO);
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO) << "the mode is the last command: nothing reads it back";

  deliver_readings(hub, [](HeatingReadings &r) { r.level = heating_alias::SECURED; });
  EXPECT_EQ(entity.get_custom_preset(), "Secured");

  deliver_readings(hub, [](HeatingReadings &r) {
    r.level = -1;
    r.setpoint_c = 21.5F;
  });
  EXPECT_FLOAT_EQ(entity.target_temperature, 21.5F);
  EXPECT_FALSE(entity.preset.has_value()) << "a plain temperature is on no alias preset";
  EXPECT_FALSE(entity.has_custom_preset());
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);
}

TEST(PlatformClimate, WithoutACommandedModeADeviceThatReportsIsShownAsHeating) {
  MockHubClimate hub;
  IOHomeClimate entity = make_pump(hub);
  entity.setup();
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF);
  deliver_readings(hub, [](HeatingReadings &r) { r.level = heating_alias::HEAT_PUMP_ECO; });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_ECO);

  perform_mode(entity, CLIMATE_MODE_OFF);
  deliver_readings(hub, [](HeatingReadings &r) { r.level = heating_alias::COMFORT; });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF) << "a commanded mode is not overwritten by a reading that cannot tell";
}

TEST(PlatformClimate, TemperatureHeatersNeverGetARegisterWrite) {
  for (const DeviceType type : {DeviceType::HEATING_TEMPERATURE_INTERFACE, DeviceType::HEAT_PUMP}) {
    MockHubClimate hub;
    IOHomeClimate entity = make_bound_climate(hub, type, 0);
    entity.setup();
    perform_mode(entity, CLIMATE_MODE_HEAT);
    auto away = entity.make_call();
    away.set_preset(CLIMATE_PRESET_AWAY);
    away.perform();
    EXPECT_TRUE(hub.calls.empty());
    EXPECT_TRUE(hub.reads.empty());
    EXPECT_TRUE(hub.levels.empty());
  }
}
