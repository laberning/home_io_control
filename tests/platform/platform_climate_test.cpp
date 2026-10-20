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
  /// The try cap each read was asked with.
  std::vector<uint8_t> read_tries;
  /// What the heater answers a read with; a register not listed here does not answer.
  std::map<HeatingFunction, float> read_replies;

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

  EXPECT_TRUE(traits.supports_preset(CLIMATE_PRESET_HOME));
  EXPECT_TRUE(traits.supports_preset(CLIMATE_PRESET_AWAY));
}

TEST(PlatformClimate, SetupRegistersProgramCustomPreset) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  ASSERT_EQ(entity.get_supported_custom_presets().size(), 1u);
  EXPECT_EQ(entity.get_supported_custom_presets().front(), "Program");
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

TEST(PlatformClimate, ProgramCustomPresetSendsProgMode) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  auto call = entity.make_call();
  call.set_preset(std::string("Program"));
  call.perform();

  ASSERT_EQ(hub.calls.size(), 1u);
  EXPECT_EQ(hub.calls[0].fn, HeatingFunction::SET_MODE);
  EXPECT_FLOAT_EQ(hub.calls[0].value, static_cast<float>(HeatingMode::PROG));
  EXPECT_EQ(entity.get_custom_preset(), "Program");
  EXPECT_EQ(entity.publish_count_, 1);
}

TEST(PlatformClimate, HomeAndAwayPresetsSendPresence) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);

  auto home = entity.make_call();
  home.set_preset(CLIMATE_PRESET_HOME);
  home.perform();
  ASSERT_EQ(hub.calls.size(), 1u);
  EXPECT_EQ(hub.calls[0].fn, HeatingFunction::SET_PRESENCE);
  EXPECT_FLOAT_EQ(hub.calls[0].value, 1.0F);

  auto away = entity.make_call();
  away.set_preset(CLIMATE_PRESET_AWAY);
  away.perform();
  EXPECT_EQ(hub.calls.back().fn, HeatingFunction::SET_PRESENCE);
  EXPECT_FLOAT_EQ(hub.calls.back().value, 0.0F);
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
  preset.set_preset(CLIMATE_PRESET_AWAY);
  preset.perform();
  EXPECT_FALSE(entity.preset.has_value()) << "preset must not advance past a failed send";

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
  // A heater without the adjustable-setpoint subtype and a heating interface are climate-class
  // types, but the heating payloads fit neither.
  IOHomeClimate plain_heater = make_bound_climate(hub, DeviceType::ELECTRICAL_HEATER, 0);
  plain_heater.setup();
  EXPECT_TRUE(plain_heater.is_failed()) << "an electrical heater without io_subtype: 1 is not driven by this entity";

  IOHomeClimate interface = make_bound_climate(hub, DeviceType::HEATING_TEMPERATURE_INTERFACE, 0);
  interface.setup();
  EXPECT_TRUE(interface.is_failed());
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

TEST(PlatformClimate, HomeAwayPresetClearsProgramCustomPreset) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  // Select the "Program" custom preset (= SET_MODE prog).
  auto program = entity.make_call();
  program.set_preset(std::string("Program"));
  program.perform();
  ASSERT_TRUE(entity.has_custom_preset());
  EXPECT_EQ(entity.get_custom_preset(), "Program");

  // Now select AWAY. Real ESPHome republishes a still-set custom preset alongside the enum
  // preset, so apply_preset_() must clear it — mirroring apply_mode_().
  auto away = entity.make_call();
  away.set_preset(CLIMATE_PRESET_AWAY);
  away.perform();

  EXPECT_FALSE(entity.has_custom_preset()) << "a HOME/AWAY preset supersedes the Program custom preset";
  ASSERT_TRUE(entity.preset.has_value());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_AWAY);
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

TEST(PlatformClimate, ObservedModesMapToHvacModesAndTheProgramPreset) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  deliver_readings(hub, [](HeatingReadings &r) { r.mode = static_cast<int16_t>(HeatingMode::AUTO); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);
  EXPECT_FALSE(entity.has_custom_preset());

  deliver_readings(hub, [](HeatingReadings &r) { r.mode = static_cast<int16_t>(HeatingMode::PROG); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);
  EXPECT_TRUE(entity.has_custom_preset());
  EXPECT_EQ(entity.get_custom_preset(), "Program");

  deliver_readings(hub, [](HeatingReadings &r) { r.mode = static_cast<int16_t>(HeatingMode::MANUAL); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  EXPECT_FALSE(entity.has_custom_preset()) << "leaving program mode drops the Program preset";

  deliver_readings(hub, [](HeatingReadings &r) { r.mode = static_cast<int16_t>(HeatingMode::EXTERNAL); });
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

TEST(PlatformClimate, PresenceStaysLastCommandedAndIsNotReadBack) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  auto away = entity.make_call();
  away.set_preset(CLIMATE_PRESET_AWAY);
  away.perform();

  EXPECT_TRUE(hub.reads.empty()) << "the presence register has no readable value";
  ASSERT_TRUE(entity.preset.has_value());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_AWAY);
  EXPECT_EQ(entity.publish_count_, 1);
}

TEST(PlatformClimate, SetupPollsTheHeaterLikeTheOtherPlatforms) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.set_status_poll_interval(120000);
  entity.setup();
  EXPECT_EQ(hub.poll_policy_.get_interval("ABC123"), 120000u);
}
