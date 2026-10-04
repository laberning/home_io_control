/// @file platform_climate_test.cpp
/// @brief Tests for the experimental IO-Homecontrol climate platform (CMD_WRITE_PRIVATE 0x20).

#include "platform_climate.h"
#include "hub_core.h"

#include "test_helpers.h"

#include <cmath>
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

  bool send_heating_command(const std::string &device_id, HeatingFunction fn, float value) override {
    this->calls.push_back({device_id, fn, value});
    return this->send_result;
  }
};

IOHomeClimate make_bound_climate(MockHubClimate &hub, DeviceType type = DeviceType::HEATING_TEMPERATURE_INTERFACE) {
  IOHomeClimate entity;
  entity.set_parent(&hub);
  entity.set_device_id("ABC123");
  entity.set_device_type(type);
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
  EXPECT_FALSE(traits.get_supports_current_temperature()) << "no current-temperature stream in the protocol";

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
