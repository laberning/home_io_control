/// @file platform_climate_test.cpp
/// @brief Tests for the experimental IO-Homecontrol climate platform (CMD_WRITE_PRIVATE 0x20).

#include "platform_climate.h"
#include "reading_store.h"
#include "hub_port_doubles.h"

#include <gtest/gtest.h>

#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace esphome::home_io_control;
using namespace esphome::climate;

namespace {

/// The heater's hub: the device port holds the device, the heating port records the plans the entity hands
/// over and answers what the test scripted. The double holds no rule of the heater controller: a test scripts
/// what the heater's record holds after a read or an accepted write as record edits, in the order the controller
/// makes its reads, each edit followed by one notification as a stored read is.
class MockHubClimate : public test::DevicePortDouble, public test::HeatingPortDouble {
 public:
  /// A change of the heater's stored record, as a decoded reply makes it.
  using RecordEdit = std::function<void(IoDevice &)>;

  /// What the record holds after read_programme_state(): applied in order, a notification after each. The answer
  /// is `programme_state_answer`, and the edits apply whatever it is (a heater may store a mode and then stay silent).
  std::vector<RecordEdit> on_programme_state;
  /// What the record holds after each accepted plan, in the order the plans are accepted: the edits of one plan
  /// in order, a notification after each. A plan beyond the script changes nothing.
  std::deque<std::vector<RecordEdit>> after_plan;

  bool read_programme_state(const std::string &device_id) override {
    const bool answer = HeatingPortDouble::read_programme_state(device_id);
    this->apply_(device_id, this->on_programme_state);
    return answer;
  }

  bool execute_heating_plan(const std::string &device_id, const HeatingWritePlan &plan,
                            const std::function<void()> &on_accepted) override {
    if (!HeatingPortDouble::execute_heating_plan(device_id, plan, on_accepted))
      return false;
    if (!this->after_plan.empty()) {
      const std::vector<RecordEdit> edits = std::move(this->after_plan.front());
      this->after_plan.pop_front();
      this->apply_(device_id, edits);
    }
    return true;
  }

 private:
  void apply_(const std::string &device_id, const std::vector<RecordEdit> &edits) {
    for (const RecordEdit &edit : edits) {
      IoDevice *dev = this->registry_.get(device_id);
      if (dev == nullptr)
        return;
      edit(*dev);
      this->registry_.notify(device_id);
    }
  }
};

// === Record edits (what the heater's record holds after a read) ===

MockHubClimate::RecordEdit stores(DeviceRegister fn, float value) {
  return [fn, value](IoDevice &dev) { test::store_reading(dev, fn, value); };
}

/// The heater answered the read of its temperature in force: REPORTED with `value_c`, or ABSENT / UNKNOWN.
MockHubClimate::RecordEdit in_force_is(InForceSupport support, float value_c = NAN) {
  return [support, value_c](IoDevice &dev) {
    test::set_in_force_support(dev, support);
    if (support == InForceSupport::REPORTED && !std::isnan(value_c))
      test::store_reading(dev, test::in_force_row_of(dev)->reading, value_c);
  };
}

/// The status read answered `level`, or forgets the stored level when `level` is negative (no level in the answer).
MockHubClimate::RecordEdit level_is(int level) {
  return [level](IoDevice &dev) { test::store_wire(dev, DeviceReading::HEATING_LEVEL, level); };
}

// === What the entity handed over ===

::testing::AssertionResult is_register_write(const HeatingWritePlan &plan, DeviceRegister fn, float value) {
  if (plan.kind != HeatingPlanKind::REGISTER || plan.register_fn != fn || plan.register_value != value)
    return ::testing::AssertionFailure() << "not a write of register " << static_cast<int>(fn) << " = " << value;
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult is_level(const HeatingWritePlan &plan, uint8_t level) {
  if (plan.kind != HeatingPlanKind::LEVEL || plan.level != level)
    return ::testing::AssertionFailure() << "not heating level " << static_cast<int>(level);
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult is_temperature(const HeatingWritePlan &plan, uint16_t main_parameter, HeatingFpKind kind,
                                          uint16_t fp_value) {
  if (plan.kind != HeatingPlanKind::TEMPERATURE || plan.main_parameter != main_parameter || plan.fp.kind != kind ||
      plan.fp.value != fp_value)
    return ::testing::AssertionFailure() << "not temperature " << main_parameter << " with fp kind "
                                         << static_cast<int>(kind) << " value " << fp_value;
  return ::testing::AssertionSuccess();
}

/// The registers the plans write, in order (a plan of another kind adds nothing).
std::vector<DeviceRegister> written(const MockHubClimate &hub) {
  std::vector<DeviceRegister> fns;
  for (const HeatingWritePlan &plan : hub.plans) {
    if (plan.kind == HeatingPlanKind::REGISTER && plan.register_fn)
      fns.push_back(*plan.register_fn);
  }
  return fns;
}

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
  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::MANUAL)));
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);

  perform_mode(entity, CLIMATE_MODE_AUTO);
  EXPECT_TRUE(
      is_register_write(hub.plans.back(), DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::AUTO)));
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);

  perform_mode(entity, CLIMATE_MODE_OFF);
  EXPECT_TRUE(
      is_register_write(hub.plans.back(), DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::OFF)));
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF);

  EXPECT_EQ(entity.publish_count_, 3);
}

TEST(PlatformClimate, TargetTemperatureSendsSetTemperature) {
  MockHubClimate hub;
  // The heater runs heat, then keeps the write. Order of the reads: HeaterController
  // ATargetOnAProgrammeHeaterSendsTheSameExchangesEveryTimeWhateverTheStoredReadingsSay.
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::MANUAL))};
  hub.after_plan = {{stores(DeviceRegister::COMFORT_SETPOINT, 20.5F)}};
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  auto call = entity.make_call();
  call.set_target_temperature(20.5F);
  call.perform();

  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::COMFORT_SETPOINT, 20.5F));
  EXPECT_FLOAT_EQ(entity.target_temperature, 20.5F);
  EXPECT_EQ(entity.publish_count_, 2) << "the mode the read found, then the read-back";
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

    ASSERT_EQ(hub.plans.size(), 1u) << c.name;
    EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::OPERATING_MODE, static_cast<float>(c.mode))) << c.name;
    EXPECT_TRUE(hub.plans[0].read_back) << c.name << ": confirm read";
    // Nothing has been read yet, so the command stands in: both modes show as HVAC auto.
    EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO) << c.name;
    ASSERT_TRUE(entity.has_custom_preset()) << c.name;
    EXPECT_EQ(entity.get_custom_preset(), c.name);
    EXPECT_EQ(entity.publish_count_, 1) << c.name;
  }
}

TEST(PlatformClimate, FailedSendDoesNotPublishOrUpdateState) {
  MockHubClimate hub;
  hub.write_result = false;
  IOHomeClimate entity = make_bound_climate(hub);

  perform_mode(entity, CLIMATE_MODE_HEAT);

  ASSERT_EQ(hub.plans.size(), 1u) << "the send is still attempted";
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF) << "state must not advance past a failed send";
  EXPECT_EQ(entity.publish_count_, 0) << "no publish on a failed send";
}

TEST(PlatformClimate, FailedSendDoesNotAdvanceTemperatureOrPreset) {
  MockHubClimate hub;
  hub.write_result = false;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();  // the device exists; only the send fails
  // The read before the target finds heat (HeaterController
  // ATargetOnAProgrammeHeaterSendsTheSameExchangesEveryTimeWhateverTheStoredReadingsSay).
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::MANUAL))};

  auto temp = entity.make_call();
  temp.set_target_temperature(21.0F);
  temp.perform();
  ASSERT_EQ(hub.plans.size(), 1u) << "the target was planned and the send attempted";
  EXPECT_TRUE(std::isnan(entity.target_temperature)) << "target must not advance past a failed send";

  auto preset = entity.make_call();
  preset.set_preset(std::string("Auto"));
  preset.perform();
  EXPECT_FALSE(entity.has_custom_preset()) << "preset must not advance past a failed send";
  EXPECT_EQ(hub.plans.size(), 2u);

  // The mode the read found is the only publish: nothing was published for a send that failed.
  EXPECT_EQ(entity.publish_count_, 1) << "no publish on any failed send";
}

TEST(PlatformClimate, UnsupportedHvacModeSendsNothing) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);

  perform_mode(entity, CLIMATE_MODE_COOL);  // no mapping to a HeatingMode

  EXPECT_TRUE(hub.plans.empty()) << "a mode with no HeatingMode mapping must not transmit";
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
void deliver_readings(MockHubClimate &hub, const std::function<void(IoDevice &)> &edit) {
  IoDevice dev = *hub.get_device("ABC123");
  edit(dev);
  hub.trigger_device_update("ABC123", dev, /*cache_device=*/true);
}

/// Publish `edit` applied to readings that carry the heater's answer to the read of its temperature in force,
/// as the hub does after the poll that read it: the heater is marked InForceSupport::REPORTED.
void deliver_programme_readings(MockHubClimate &hub, const std::function<void(IoDevice &)> &edit) {
  IoDevice dev = *hub.get_device("ABC123");
  edit(dev);
  test::set_in_force_support(dev, InForceSupport::REPORTED);
  hub.trigger_device_update("ABC123", dev, /*cache_device=*/true);
}

}  // namespace

TEST(PlatformClimate, PollReadingsDriveTheCard) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::ROOM_TEMPERATURE, 20.75F);
    test::store_reading(r, DeviceReading::SETPOINT, 19.5F);
    test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::MANUAL));
    test::store_reading(r, DeviceReading::REGULATOR_STATE, static_cast<int16_t>(HeatingRegulatorState::HEATING));
    test::store_reading(r, DeviceReading::MAX_SETPOINT, 26.0F);
  });

  EXPECT_FLOAT_EQ(entity.current_temperature, 20.75F);
  EXPECT_FLOAT_EQ(entity.target_temperature, 19.5F);
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  EXPECT_EQ(entity.action, CLIMATE_ACTION_HEATING);
  EXPECT_FLOAT_EQ(entity.traits().get_visual_max_temperature(), 26.0F) << "the heater's own maximum";
  EXPECT_EQ(entity.publish_count_, 1);

  // The same readings again change nothing and publish nothing.
  deliver_readings(hub, [](IoDevice &) {});
  EXPECT_EQ(entity.publish_count_, 1);
}

TEST(PlatformClimate, MaximumSetpointIsNeverPastWhatCanBeEncoded) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_readings(hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::MAX_SETPOINT, 30.0F); });
  EXPECT_FLOAT_EQ(entity.traits().get_visual_max_temperature(), HEATING_TEMP_MAX_C);
}

TEST(PlatformClimate, ObservedModesMapToHvacModesAndTheAutomaticPresets) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::AUTO)); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);
  EXPECT_TRUE(entity.has_custom_preset());
  EXPECT_EQ(entity.get_custom_preset(), "Auto");

  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::PROG)); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);
  EXPECT_TRUE(entity.has_custom_preset());
  EXPECT_EQ(entity.get_custom_preset(), "Program");

  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::MANUAL)); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  EXPECT_FALSE(entity.has_custom_preset()) << "leaving program mode drops the Program preset";

  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::EXTERNAL_SETPOINT));
  });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT) << "an external setpoint is shown as heat";

  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::OFF)); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF);

  const int published = entity.publish_count_;
  deliver_readings(hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::MODE, 9); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF) << "an unknown mode value leaves the mode alone";
  EXPECT_EQ(entity.publish_count_, published);
}

TEST(PlatformClimate, ActionFollowsTheRegulatorAndAnOffHeaterIsNotIdle) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::MANUAL));
    test::store_reading(r, DeviceReading::REGULATOR_STATE, static_cast<int16_t>(HeatingRegulatorState::HEATING));
  });
  EXPECT_EQ(entity.action, CLIMATE_ACTION_HEATING);

  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::REGULATOR_STATE, static_cast<int16_t>(HeatingRegulatorState::STANDBY));
  });
  EXPECT_EQ(entity.action, CLIMATE_ACTION_IDLE);

  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::REGULATOR_STATE, static_cast<int16_t>(HeatingRegulatorState::NONE));
  });
  EXPECT_EQ(entity.action, CLIMATE_ACTION_IDLE);

  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::OFF)); });
  EXPECT_EQ(entity.action, CLIMATE_ACTION_OFF);
}

TEST(PlatformClimate, ASensorFaultShowsNoCurrentTemperature) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  deliver_readings(hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::ROOM_TEMPERATURE, 18.0F); });
  ASSERT_FLOAT_EQ(entity.current_temperature, 18.0F);
  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::ROOM_TEMPERATURE, NAN); });  // raw -1 decodes as NaN
  EXPECT_TRUE(std::isnan(entity.current_temperature));
}

TEST(PlatformClimate, UpdatesForOtherDevicesAreIgnored) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  DeviceConfig config{};
  hub.add_device("DEF456", config);
  IoDevice other = *hub.get_device("DEF456");
  test::store_reading(other, DeviceReading::ROOM_TEMPERATURE, 25.0F);
  hub.trigger_device_update("DEF456", other, /*cache_device=*/true);

  EXPECT_TRUE(std::isnan(entity.current_temperature));
  EXPECT_EQ(entity.publish_count_, 0);
}

TEST(PlatformClimate, AcceptedSetpointWriteIsReadBackAndTheHeatersValueIsShown) {
  MockHubClimate hub;
  // Order of the reads: HeaterController
  // ATargetOnAProgrammeHeaterSendsTheSameExchangesEveryTimeWhateverTheStoredReadingsSay.
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::MANUAL))};
  hub.after_plan = {{stores(DeviceRegister::COMFORT_SETPOINT, 19.0F)}};  // the heater held the request to 19.0
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::MANUAL)); });
  const int published = entity.publish_count_;  // the read before the target finds the mode it showed

  auto call = entity.make_call();
  call.set_target_temperature(20.5F);
  call.perform();

  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::COMFORT_SETPOINT, 20.5F));
  EXPECT_TRUE(hub.plans[0].read_back) << "the entity asks for the read-back";
  EXPECT_FLOAT_EQ(entity.target_temperature, 19.0F) << "what the heater reports, not what was commanded";
  EXPECT_EQ(entity.publish_count_, published + 1) << "one publish: the read-back's, not a second for the command";
}

TEST(PlatformClimate, AcceptedModeWriteIsReadBack) {
  MockHubClimate hub;
  // The heater answers the read-back with auto (HeaterController
  // APlannedRegisterWriteSendsItsBytesAppliesTheHookThenReadsBack).
  hub.after_plan = {{stores(DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::AUTO))}};
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  perform_mode(entity, CLIMATE_MODE_HEAT);

  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::MANUAL)));
  EXPECT_TRUE(hub.plans[0].read_back);
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO) << "the heater says auto";
  EXPECT_EQ(entity.publish_count_, 1);
}

TEST(PlatformClimate, AfterTheFirstReadAWriteWhoseReadBackFailsShowsNothingNew) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::MANUAL));
    test::store_reading(r, DeviceReading::SETPOINT, 19.5F);
  });
  const int published = entity.publish_count_;
  // The read before the target finds heat (HeaterController
  // ATargetOnAProgrammeHeaterSendsTheSameExchangesEveryTimeWhateverTheStoredReadingsSay).
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::MANUAL))};

  // Both writes are acknowledged; neither read-back draws an answer (no script after the plans).
  auto call = entity.make_call();
  call.set_target_temperature(22.0F);
  call.perform();
  perform_mode(entity, CLIMATE_MODE_AUTO);

  EXPECT_EQ(hub.plans.size(), 2u) << "both writes were sent";
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT) << "no prediction: the entity keeps the last observed mode";
  EXPECT_FLOAT_EQ(entity.target_temperature, 19.5F);
  EXPECT_EQ(entity.publish_count_, published);
}

TEST(PlatformClimate, ObservedProgramThenAutoPresetWaitsForTheReadBack) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::PROG)); });
  const int published = entity.publish_count_;

  auto call = entity.make_call();
  call.set_preset(std::string("Auto"));
  call.perform();
  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::AUTO)));
  EXPECT_EQ(entity.get_custom_preset(), "Program") << "a read has succeeded: the card waits for the read-back";
  EXPECT_EQ(entity.publish_count_, published);

  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::AUTO)); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);
  EXPECT_EQ(entity.get_custom_preset(), "Auto");
}

TEST(PlatformClimate, ReadBackReplacesTheAutoPresetWithProgram) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::AUTO)); });
  ASSERT_EQ(entity.get_custom_preset(), "Auto");

  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::PROG)); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);
  EXPECT_EQ(entity.get_custom_preset(), "Program");
}

TEST(PlatformClimate, ObservedProgramThenHeatDropsThePresetOnTheReadBack) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::PROG)); });

  perform_mode(entity, CLIMATE_MODE_HEAT);
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO) << "the card waits for the read-back";
  EXPECT_EQ(entity.get_custom_preset(), "Program");

  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::MANUAL)); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  EXPECT_FALSE(entity.has_custom_preset());
}

TEST(PlatformClimate, SetupPollsTheHeaterLikeTheOtherPlatforms) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.set_status_poll_interval(120000);
  entity.setup();
  EXPECT_EQ(hub.status_poll_interval("ABC123"), 120000u);
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
  EXPECT_EQ(entity.get_supported_custom_presets(), (std::vector<std::string>{"Comfort -1", "Comfort -2"}));
  EXPECT_FALSE(traits.get_supports_current_temperature()) << "a level heater reports no temperature";
  EXPECT_FALSE(traits.get_supports_action());
}

TEST(PlatformClimate, ATypeNotKnownYetOffersOnlyOff) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub, DeviceType::UNKNOWN, 0);
  entity.setup();
  EXPECT_EQ(supported_modes(entity.traits()), (std::vector<ClimateMode>{CLIMATE_MODE_OFF}));

  perform_mode(entity, CLIMATE_MODE_HEAT);
  EXPECT_TRUE(hub.plans.empty());
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

  ASSERT_EQ(hub.plans.size(), 2u);
  EXPECT_TRUE(is_level(hub.plans[0], heating_level::OFF));
  EXPECT_TRUE(is_level(hub.plans[1], heating_level::COMFORT));
  for (const HeatingWritePlan &plan : hub.plans)
    EXPECT_FALSE(plan.read_back) << "the hub's settle poll reads the level, not the entity";
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT) << "the commanded mode stands in until a level has been read";
}

TEST(PlatformClimate, PilotWireInterfaceOffersTheSixOrdersAndNoBoost) {
  MockHubClimate hub;
  IOHomeClimate entity = make_level_climate(hub);
  entity.set_device_model(DeviceModel::PILOT_WIRE_INTERFACE);
  entity.setup();
  ASSERT_FALSE(entity.is_failed());
  const ClimateTraits traits = entity.traits();

  EXPECT_EQ(supported_modes(traits), (std::vector<ClimateMode>{CLIMATE_MODE_OFF, CLIMATE_MODE_HEAT}));
  EXPECT_EQ(supported_presets(traits),
            (std::vector<ClimatePreset>{CLIMATE_PRESET_AWAY, CLIMATE_PRESET_COMFORT, CLIMATE_PRESET_ECO}));
  EXPECT_EQ(entity.get_supported_custom_presets(), (std::vector<std::string>{"Comfort -1", "Comfort -2"}));
  EXPECT_EQ(hub.get_device("ABC123")->model, DeviceModel::PILOT_WIRE_INTERFACE);
}

TEST(PlatformClimate, PilotWireInterfacePresetsSendTheSameLevelsAsAPlainHeater) {
  MockHubClimate hub;
  IOHomeClimate entity = make_level_climate(hub);
  entity.set_device_model(DeviceModel::PILOT_WIRE_INTERFACE);
  entity.setup();

  for (const ClimatePreset preset : {CLIMATE_PRESET_AWAY, CLIMATE_PRESET_ECO, CLIMATE_PRESET_COMFORT}) {
    auto call = entity.make_call();
    call.set_preset(preset);
    call.perform();
  }
  for (const char *name : {"Comfort -1", "Comfort -2"}) {
    auto call = entity.make_call();
    call.set_preset(std::string(name));
    call.perform();
  }
  perform_mode(entity, CLIMATE_MODE_OFF);

  ASSERT_EQ(hub.plans.size(), 6u);
  const uint8_t expected_levels[] = {heating_level::FROST_PROTECTION, heating_level::ECO,
                                     heating_level::COMFORT,          heating_level::COMFORT_MINUS_1,
                                     heating_level::COMFORT_MINUS_2,  heating_level::OFF};
  for (size_t i = 0; i < hub.plans.size(); i++)
    EXPECT_TRUE(is_level(hub.plans[i], expected_levels[i])) << "plan " << i;
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

  ASSERT_EQ(hub.plans.size(), 6u);
  const uint8_t expected_levels[] = {heating_level::FROST_PROTECTION, heating_level::ECO,
                                     heating_level::COMFORT,          heating_level::BOOST,
                                     heating_level::COMFORT_MINUS_1,  heating_level::COMFORT_MINUS_2};
  for (size_t i = 0; i < hub.plans.size(); i++)
    EXPECT_TRUE(is_level(hub.plans[i], expected_levels[i])) << "plan " << i;
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

TEST(PlatformClimate, TheSecuredLevelIsShownAsHeatWithNoPresetAndCannotBeSelected) {
  MockHubClimate hub;
  IOHomeClimate entity = make_level_climate(hub);
  entity.setup();

  auto call = entity.make_call();
  call.set_preset(std::string("Secured"));
  call.perform();
  EXPECT_TRUE(hub.plans.empty());
  EXPECT_FALSE(entity.has_custom_preset());

  deliver_readings(hub,
                   [](IoDevice &r) { test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_level::SECURED); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  EXPECT_FALSE(entity.has_custom_preset()) << "the card offers no Secured: the level sensor shows it";
  EXPECT_FALSE(entity.preset.has_value());
}

TEST(PlatformClimate, ALevelHeaterIgnoresATargetTemperature) {
  MockHubClimate hub;
  IOHomeClimate entity = make_level_climate(hub);
  entity.setup();
  auto call = entity.make_call();
  call.set_target_temperature(21.0F);
  call.perform();
  EXPECT_TRUE(hub.plans.empty());
  EXPECT_EQ(entity.publish_count_, 0);
}

TEST(PlatformClimate, AFailedLevelWriteShowsNothing) {
  MockHubClimate hub;
  hub.write_result = false;
  IOHomeClimate entity = make_level_climate(hub);
  entity.setup();
  perform_mode(entity, CLIMATE_MODE_HEAT);
  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_TRUE(is_level(hub.plans[0], heating_level::COMFORT));
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF);
  EXPECT_EQ(entity.publish_count_, 0);
}

TEST(PlatformClimate, ObservedLevelsDriveModeAndPreset) {
  MockHubClimate hub;
  IOHomeClimate entity = make_level_climate(hub);
  entity.setup();

  deliver_readings(hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_level::ECO); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  ASSERT_TRUE(entity.preset.has_value());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_ECO);

  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_level::COMFORT_MINUS_1); });
  EXPECT_EQ(entity.get_custom_preset(), "Comfort -1");
  EXPECT_FALSE(entity.preset.has_value()) << "a custom preset replaces the standard one";

  deliver_readings(hub,
                   [](IoDevice &r) { test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_level::BOOST); });
  EXPECT_FALSE(entity.has_custom_preset());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_BOOST);

  deliver_readings(hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_level::OFF); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF);
  EXPECT_FALSE(entity.preset.has_value());
  EXPECT_FALSE(entity.has_custom_preset());

  const int published = entity.publish_count_;
  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::HEATING_LEVEL, 0x20); });  // a level nobody defined
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF);
  EXPECT_EQ(entity.publish_count_, published);
}

TEST(PlatformClimate, ALevelHeaterNeverShowsAnActionOrTemperature) {
  MockHubClimate hub;
  IOHomeClimate entity = make_level_climate(hub);
  entity.setup();
  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_level::COMFORT);
    test::store_reading(r, DeviceReading::ROOM_TEMPERATURE,
                        20.0F);  // not part of this profile; must not leak onto the card
    test::store_reading(r, DeviceReading::REGULATOR_STATE, 2);
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
  EXPECT_FLOAT_EQ(traits.get_visual_min_temperature(), 5.0F)
      << "the default encoding range holds the whole visual range";
  EXPECT_FLOAT_EQ(traits.get_visual_max_temperature(), 30.0F);
}

TEST(PlatformClimate, ThermostatCardNarrowsToANarrowerYamlEncodingRange) {
  MockHubClimate hub;
  IOHomeClimate entity = make_thermostat(hub);
  entity.set_temperature_range(10.0F, 25.0F);
  entity.setup();
  const ClimateTraits traits = entity.traits();
  EXPECT_FLOAT_EQ(traits.get_visual_min_temperature(), 10.0F);
  EXPECT_FLOAT_EQ(traits.get_visual_max_temperature(), 25.0F);
}

TEST(PlatformClimate, ThermostatTargetOutsideTheYamlRangeIsNotSent) {
  MockHubClimate hub;
  IOHomeClimate entity = make_thermostat(hub);
  entity.set_temperature_range(10.0F, 25.0F);
  entity.setup();
  // The hub applies the YAML range to the device's record (the double only records the call).
  ASSERT_EQ(hub.temperature_ranges.size(), 1u);
  IoDevice *dev = hub.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  dev->temperature_range_min_c = 10.0F;
  dev->temperature_range_max_c = 25.0F;
  auto call = entity.make_call();
  call.set_target_temperature(28.0F);
  call.perform();
  EXPECT_TRUE(hub.plans.empty()) << "refused, never sent as 25";
  EXPECT_TRUE(std::isnan(entity.target_temperature)) << "the card keeps what it showed";

  auto inside = entity.make_call();
  inside.set_target_temperature(25.0F);
  inside.perform();
  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_EQ(hub.plans[0].kind, HeatingPlanKind::TEMPERATURE);
  EXPECT_EQ(hub.plans[0].main_parameter, 51200);
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
  EXPECT_FLOAT_EQ(traits.get_visual_min_temperature(), 5.0F);
  EXPECT_FLOAT_EQ(traits.get_visual_max_temperature(), 30.0F);
}

TEST(PlatformClimate, ThermostatSetpointIsTheTemperatureWithTheManualFlag) {
  MockHubClimate hub;
  IOHomeClimate entity = make_thermostat(hub);
  entity.setup();
  auto call = entity.make_call();
  call.set_target_temperature(20.0F);
  call.perform();

  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_TRUE(is_temperature(hub.plans[0], 25600, HeatingFpKind::FP9, heating_mode_flags::MANUAL))
      << "no register write";
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
  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_EQ(hub.plans[0].main_parameter, 25600) << "the middle of 10..30";

  MockHubClimate pump_hub;
  IOHomeClimate pump = make_pump(pump_hub);
  pump.set_temperature_range(10.0F, 30.0F);  // not reachable from YAML; the profile still ignores it
  pump.setup();
  auto pump_call = pump.make_call();
  pump_call.set_target_temperature(20.0F);
  pump_call.perform();
  ASSERT_EQ(pump_hub.plans.size(), 1u);
  EXPECT_EQ(pump_hub.plans[0].main_parameter, 25600) << "20 C over the fixed -40..80 is also half way";
  auto top = pump.make_call();
  top.set_target_temperature(30.0F);
  top.perform();
  EXPECT_EQ(pump_hub.plans[1].main_parameter, 29867) << "round(70 x 51200 / 120), not the YAML range's top";
}

TEST(PlatformClimate, DeclaredReadingEntitiesAskTheHubToReadTheirReadings) {
  MockHubClimate asked;
  IOHomeClimate with_entities = make_thermostat(asked);
  with_entities.add_device_reading(DeviceReading::CENTRAL_HEATING_MODE);
  with_entities.add_device_reading(DeviceReading::OCCUPANCY_DETECTION);
  with_entities.setup();
  EXPECT_EQ(asked.device_readings,
            (std::vector<std::pair<std::string, DeviceReading>>{{"ABC123", DeviceReading::CENTRAL_HEATING_MODE},
                                                                {"ABC123", DeviceReading::OCCUPANCY_DETECTION}}));

  MockHubClimate not_asked;
  IOHomeClimate without_entities = make_thermostat(not_asked);
  without_entities.setup();
  EXPECT_TRUE(not_asked.device_readings.empty()) << "no entity, no extra read";
}

TEST(PlatformClimate, HeatPumpSetpointCarriesNoFunctionalParameter) {
  MockHubClimate hub;
  IOHomeClimate entity = make_pump(hub);
  entity.setup();
  auto call = entity.make_call();
  call.set_target_temperature(21.0F);
  call.perform();
  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_EQ(hub.plans[0].fp.kind, HeatingFpKind::NONE);
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
  ASSERT_EQ(hub.plans.size(), 3u);
  EXPECT_TRUE(is_temperature(hub.plans[0], 0xD80F, HeatingFpKind::NONE, 0));
  EXPECT_TRUE(is_temperature(hub.plans[1], 0xD80E, HeatingFpKind::NONE, 0));
  EXPECT_TRUE(is_temperature(hub.plans[2], 0xD80A, HeatingFpKind::NONE, 0));
  EXPECT_EQ(entity.get_custom_preset(), "Secured");
}

TEST(PlatformClimate, ThermostatModesAreTheDevicesOwnCommands) {
  MockHubClimate hub;
  IOHomeClimate entity = make_thermostat(hub);
  entity.setup();

  perform_mode(entity, CLIMATE_MODE_AUTO);
  perform_mode(entity, CLIMATE_MODE_OFF);
  ASSERT_EQ(hub.plans.size(), 2u);
  EXPECT_TRUE(is_temperature(hub.plans[0], 0xD80F, HeatingFpKind::FP9, heating_mode_flags::AUTO));
  EXPECT_TRUE(is_temperature(hub.plans[1], 0xD400, HeatingFpKind::FP9, heating_mode_flags::OFF));
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF);

  // Heat is the current target temperature with the manual flag; with no target known, nothing is sent.
  perform_mode(entity, CLIMATE_MODE_HEAT);
  EXPECT_EQ(hub.plans.size(), 2u);
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF) << "a refused mode is not shown";
  auto target = entity.make_call();
  target.set_target_temperature(22.0F);
  target.perform();
  perform_mode(entity, CLIMATE_MODE_HEAT);
  ASSERT_EQ(hub.plans.size(), 4u);
  EXPECT_TRUE(
      is_temperature(hub.plans[3], hub.plans[2].main_parameter, HeatingFpKind::FP9, heating_mode_flags::MANUAL));
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
}

TEST(PlatformClimate, ThermostatHeatUsesTheReportedSetpointWhenNoTargetWasSet) {
  MockHubClimate hub;
  IOHomeClimate entity = make_thermostat(hub);
  entity.setup();
  deliver_readings(hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::STATUS_SETPOINT, 20.0F); });
  perform_mode(entity, CLIMATE_MODE_HEAT);
  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_TRUE(is_temperature(hub.plans[0], 25600, HeatingFpKind::FP9, heating_mode_flags::MANUAL));
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
  ASSERT_EQ(hub.plans.size(), 5u);
  EXPECT_TRUE(is_temperature(hub.plans[0], 0xD400, HeatingFpKind::FP16, 0x8000));
  EXPECT_TRUE(is_temperature(hub.plans[1], 0xD80F, HeatingFpKind::FP16, 0x4000));
  EXPECT_TRUE(is_temperature(hub.plans[2], 0xD812, HeatingFpKind::FP16, 0x4000));
  EXPECT_TRUE(is_temperature(hub.plans[3], 0xD80E, HeatingFpKind::FP16, 0x4000));
  EXPECT_TRUE(is_temperature(hub.plans[4], 0xD813, HeatingFpKind::FP16, 0x4000));
}

TEST(PlatformClimate, AFailedTemperatureSendShowsNothing) {
  MockHubClimate hub;
  hub.write_result = false;
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

  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_alias::THERMOSTAT_ECO); });
  ASSERT_TRUE(entity.preset.has_value());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_ECO);
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO) << "the mode is the last command: nothing reads it back";

  deliver_readings(hub,
                   [](IoDevice &r) { test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_alias::SECURED); });
  EXPECT_EQ(entity.get_custom_preset(), "Secured");

  deliver_readings(hub, [](IoDevice &r) {
    clear_reading(r, DeviceReading::HEATING_LEVEL);
    test::store_reading(r, DeviceReading::STATUS_SETPOINT, 21.5F);
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
  deliver_readings(
      hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_alias::HEAT_PUMP_ECO); });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_ECO);

  perform_mode(entity, CLIMATE_MODE_OFF);
  deliver_readings(hub,
                   [](IoDevice &r) { test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_alias::COMFORT); });
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
    for (const HeatingWritePlan &plan : hub.plans) {
      EXPECT_EQ(plan.kind, HeatingPlanKind::TEMPERATURE) << "neither a register write nor a level";
      EXPECT_FALSE(plan.read_back);
    }
  }
}

// ========================================================================================
// Towel dryer (52/1, io_model: towel_dryer): its own mode register and a separate preset register
// ========================================================================================

namespace {

IOHomeClimate make_towel_climate(MockHubClimate &hub) {
  IOHomeClimate entity = make_bound_climate(hub);
  entity.set_device_model(DeviceModel::TOWEL_DRYER);
  return entity;
}

void perform_preset(IOHomeClimate &entity, ClimatePreset preset) {
  auto call = entity.make_call();
  call.set_preset(preset);
  call.perform();
}

void perform_custom_preset(IOHomeClimate &entity, const char *name) {
  auto call = entity.make_call();
  call.set_preset(std::string(name));
  call.perform();
}

}  // namespace

TEST(PlatformClimate, TowelDryerTraitsAreExactlyItsProfileWithNoRadiatorPresets) {
  MockHubClimate hub;
  IOHomeClimate entity = make_towel_climate(hub);
  entity.setup();
  ASSERT_FALSE(entity.is_failed());
  const ClimateTraits traits = entity.traits();

  EXPECT_EQ(supported_modes(traits),
            (std::vector<ClimateMode>{CLIMATE_MODE_OFF, CLIMATE_MODE_HEAT, CLIMATE_MODE_AUTO}));
  EXPECT_EQ(supported_presets(traits), (std::vector<ClimatePreset>{CLIMATE_PRESET_NONE, CLIMATE_PRESET_BOOST}))
      << "every preset sends a command: comfort and eco are the heating_level sensor's, not the card's";
  EXPECT_EQ(entity.get_supported_custom_presets(), (std::vector<std::string>{"Drying"}))
      << "no Program / Auto: PROG is the dryer's auto mode";
  EXPECT_TRUE(traits.get_supports_current_temperature());
  EXPECT_TRUE(traits.get_supports_action());
  EXPECT_EQ(hub.get_device("ABC123")->model, DeviceModel::TOWEL_DRYER) << "the model reaches the device record";
}

TEST(PlatformClimate, TowelDryerHvacModesWriteItsModeRegisterAndReadItBack) {
  MockHubClimate hub;
  IOHomeClimate entity = make_towel_climate(hub);
  entity.setup();

  perform_mode(entity, CLIMATE_MODE_HEAT);
  perform_mode(entity, CLIMATE_MODE_AUTO);
  perform_mode(entity, CLIMATE_MODE_OFF);

  // Never the radiator's mode register.
  ASSERT_EQ(hub.plans.size(), 3u);
  EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::TOWEL_MODE, static_cast<float>(TowelDryerMode::MANUAL)));
  EXPECT_TRUE(is_register_write(hub.plans[1], DeviceRegister::TOWEL_MODE, static_cast<float>(TowelDryerMode::PROG)));
  EXPECT_TRUE(is_register_write(hub.plans[2], DeviceRegister::TOWEL_MODE, static_cast<float>(TowelDryerMode::OFF)));
  for (const HeatingWritePlan &plan : hub.plans)
    EXPECT_TRUE(plan.read_back) << "each write is read back";
}

TEST(PlatformClimate, TowelDryerModeComesFromItsModeRegisterNotTheRadiators) {
  MockHubClimate hub;
  IOHomeClimate entity = make_towel_climate(hub);
  entity.setup();

  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::MODE,
                        static_cast<int16_t>(HeatingMode::OFF));  // register 0x0100 is not the dryer's mode
    test::store_reading(r, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::MANUAL));
  });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);

  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::PROG));
  });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);
  EXPECT_FALSE(entity.has_custom_preset()) << "PROG is auto, with no Program preset";

  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::OFF));
  });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF);

  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::AUTO));
  });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_OFF) << "value 1 has never been seen and maps to nothing";
}

TEST(PlatformClimate, TowelDryerBoostAndDryingAreShownFromTheTemporaryStateAndSurviveEveryPoll) {
  MockHubClimate hub;
  IOHomeClimate entity = make_towel_climate(hub);
  entity.setup();

  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::MANUAL));
    test::store_reading(r, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::BOOST));
  });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  ASSERT_TRUE(entity.preset.has_value());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_BOOST);

  // The next poll reads the same mode: a mode entry without a preset must not wipe the boost.
  const int published = entity.publish_count_;
  deliver_readings(hub, [](IoDevice &) {});
  ASSERT_TRUE(entity.preset.has_value());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_BOOST);
  EXPECT_EQ(entity.publish_count_, published) << "nothing changed, nothing published";

  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::DRYING));
  });
  EXPECT_TRUE(entity.has_custom_preset());
  EXPECT_EQ(entity.get_custom_preset(), "Drying");
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT) << "drying does not change the base mode";

  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::NONE));
  });
  EXPECT_FALSE(entity.has_custom_preset());
  ASSERT_TRUE(entity.preset.has_value());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_NONE);
}

TEST(PlatformClimate, TowelDryerPresetsWriteTheTemporaryStateAndShowAsPresetsNotModes) {
  MockHubClimate hub;
  IOHomeClimate entity = make_towel_climate(hub);
  entity.setup();
  const ClimateMode before = entity.mode;

  perform_preset(entity, CLIMATE_PRESET_BOOST);
  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::TEMPORARY_STATE,
                                static_cast<float>(HeatingTemporaryState::BOOST)));
  ASSERT_TRUE(entity.preset.has_value());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_BOOST) << "shown until a read lands";
  EXPECT_EQ(entity.mode, before) << "boost's value 1 is not a mode value";
  EXPECT_TRUE(hub.plans.back().read_back) << "read back";

  perform_custom_preset(entity, "Drying");
  EXPECT_TRUE(is_register_write(hub.plans.back(), DeviceRegister::TEMPORARY_STATE,
                                static_cast<float>(HeatingTemporaryState::DRYING)));
  EXPECT_EQ(entity.get_custom_preset(), "Drying");
  EXPECT_NE(entity.mode, CLIMATE_MODE_AUTO) << "drying's value 2 is not PROG";
}

TEST(PlatformClimate, TowelDryerShowsTheTemperatureInForceAndNoPresetForThePeriodWhileItsProgrammeRuns) {
  MockHubClimate hub;
  IOHomeClimate entity = make_towel_climate(hub);
  entity.setup();

  deliver_readings(hub, [](IoDevice &r) {
    test::store_programme_readings(r, TowelDryerMode::PROG, HeatingTemporaryState::NONE, 20.0F, heating_level::COMFORT);
  });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);
  ASSERT_TRUE(entity.preset.has_value());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_NONE) << "comfort is a level, shown by the sensor";
  EXPECT_FLOAT_EQ(entity.target_temperature, 20.0F);

  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE, 16.5F);
    test::store_reading(r, DeviceReading::HEATING_LEVEL, heating_level::ECO);
  });
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_NONE);
  EXPECT_FLOAT_EQ(entity.target_temperature, 16.5F) << "the temperature the dryer heats to now";

  // A frost or holiday value: the temperature, and no preset.
  deliver_readings(hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::TOWEL_TEMPERATURE_IN_FORCE, 7.0F); });
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_NONE);
  EXPECT_FLOAT_EQ(entity.target_temperature, 7.0F);
}

TEST(PlatformClimate, TowelDryerTargetFollowsComfortEcoBoostComfortWithoutAFlickerOrAStaleValue) {
  MockHubClimate hub;
  IOHomeClimate entity = make_towel_climate(hub);
  entity.setup();
  const auto in_period = [&](float in_force) {
    deliver_readings(hub, [in_force](IoDevice &r) {
      test::store_programme_readings(r, TowelDryerMode::PROG, HeatingTemporaryState::NONE, in_force,
                                     heating_level::COMFORT);
    });
  };

  in_period(20.0F);
  EXPECT_FLOAT_EQ(entity.target_temperature, 20.0F);
  in_period(16.5F);
  EXPECT_FLOAT_EQ(entity.target_temperature, 16.5F);

  // Boost on top: the card shows boost and the comfort setpoint again, as without a programme period.
  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::BOOST));
    clear_reading(r,
                  DeviceReading::TOWEL_TEMPERATURE_IN_FORCE);  // what the hub does the moment the programme stops
  });
  ASSERT_TRUE(entity.preset.has_value());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_BOOST);
  EXPECT_FLOAT_EQ(entity.target_temperature, 20.0F);

  // Back in the programme, before 0x0137 is read again: today's target, no value of the old period.
  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::NONE));
  });
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_NONE);
  EXPECT_FLOAT_EQ(entity.target_temperature, 20.0F);

  const int published = entity.publish_count_;
  in_period(20.0F);
  EXPECT_EQ(entity.publish_count_, published) << "the same card as before: nothing to publish";
}

TEST(PlatformClimate, TowelDryerOutsideTheProgrammeNeverShowsTheTemperatureInForce) {
  MockHubClimate hub;
  IOHomeClimate entity = make_towel_climate(hub);
  entity.setup();
  // 0x0137 reads 1 in boost and 2 in drying; a stale reading must not reach the card even if one slips in.
  for (const auto state : {HeatingTemporaryState::BOOST, HeatingTemporaryState::DRYING}) {
    deliver_readings(hub, [state](IoDevice &r) {
      test::store_programme_readings(r, TowelDryerMode::PROG, state, 0.1F, heating_level::COMFORT);
    });
    EXPECT_FLOAT_EQ(entity.target_temperature, 20.0F);
  }
  deliver_readings(hub, [](IoDevice &r) {
    test::store_programme_readings(r, TowelDryerMode::MANUAL, HeatingTemporaryState::NONE, 16.5F,
                                   heating_level::COMFORT);
  });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  EXPECT_FLOAT_EQ(entity.target_temperature, 20.0F) << "MANUEL holds the comfort setpoint";
  ASSERT_TRUE(entity.preset.has_value());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_NONE);
}

namespace {

constexpr int16_t RADIATOR_HEAT = static_cast<int16_t>(HeatingMode::MANUAL);
constexpr int16_t RADIATOR_PROGRAMME = static_cast<int16_t>(HeatingMode::PROG);
constexpr int16_t DRYER_HEAT = static_cast<int16_t>(TowelDryerMode::MANUAL);
constexpr int16_t DRYER_PROGRAMME = static_cast<int16_t>(TowelDryerMode::PROG);
constexpr int16_t NO_TIMED_RUN = static_cast<int16_t>(HeatingTemporaryState::NONE);
constexpr int16_t TIMED_BOOST = static_cast<int16_t>(HeatingTemporaryState::BOOST);

// Scripts of what the heater's record holds after a read. They follow the order the real controller reads in, one
// notification after each stored read: before a target the mode, then (on a dryer in its programme) the timed run,
// then the temperature in force while the heater has not answered it, then (on a dryer in an eco-only period) the
// level (HeaterController ATargetOnAProgrammeHeaterSendsTheSameExchangesEveryTimeWhateverTheStoredReadingsSay and
// ADryerTargetInAnEcoPeriodAsksForTheLevelThenWritesTheOverride); after a mode write the read-back, then one read of
// the temperature in force (AModeWriteIntoTheProgrammeIsReadBackThenTheTemperatureInForceIsReadOnce); after an
// override the read-back, which carries the new value in force with it
// (AnOverrideInAnEcoOrAComfortPeriodSendsTheOverrideRegisterAndShowsTheNewTargetAtTheReadBack).

/// The read-back of an override that the heater took: the override register and the temperature in force it brings.
MockHubClimate::RecordEdit override_taken(float value_c) {
  return [value_c](IoDevice &dev) {
    test::store_reading(dev, DeviceRegister::DEROGATED_SETPOINT, value_c);
    test::store_reading(dev, test::in_force_row_of(dev)->reading, value_c);
  };
}

void perform_target(IOHomeClimate &entity, float target_c) {
  auto call = entity.make_call();
  call.set_target_temperature(target_c);
  call.perform();
}

/// A radiator the hub last saw in heat (comfort 19.0 C) that has reported a temperature in force before.
void deliver_radiator_in_heat(MockHubClimate &hub) {
  deliver_programme_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::MODE, RADIATOR_HEAT);
    test::store_reading(r, DeviceReading::SETPOINT, 19.0F);
  });
}

}  // namespace

TEST(PlatformClimate, ATargetFirstAsksTheHeaterWhatItRunsThenPlansFromTheAnswer) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_radiator_in_heat(hub);
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, RADIATOR_HEAT)};
  hub.after_plan = {{stores(DeviceRegister::COMFORT_SETPOINT, 21.0F)}};

  perform_target(entity, 21.0F);

  EXPECT_EQ(hub.programme_state_reads, std::vector<std::string>{"ABC123"}) << "asked once, before the plan";
  EXPECT_EQ(written(hub), std::vector<DeviceRegister>{DeviceRegister::COMFORT_SETPOINT});
}

TEST(PlatformClimate, AHeaterThatNowRunsItsProgrammeTakesTheTargetAsAnOverrideWhateverTheCardShowed) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_radiator_in_heat(hub);  // the card shows heat
  ASSERT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  ASSERT_FLOAT_EQ(entity.target_temperature, 19.0F);
  // But the heater's panel switched it to its programme.
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, RADIATOR_PROGRAMME)};
  hub.after_plan = {{override_taken(21.0F)}};
  const int published = entity.publish_count_;

  perform_target(entity, 21.0F);

  EXPECT_EQ(written(hub), std::vector<DeviceRegister>{DeviceRegister::DEROGATED_SETPOINT})
      << "the comfort setpoint is not written";
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO) << "the card shows what the read found";
  EXPECT_FLOAT_EQ(entity.target_temperature, 21.0F);
  EXPECT_EQ(entity.publish_count_, published + 2) << "the read's state and the override's read-back";
  EXPECT_EQ(std::vector<float>(entity.published_targets_.end() - 2, entity.published_targets_.end()),
            (std::vector<float>{19.0F, 21.0F}))
      << "no passing target: the comfort setpoint the card showed, then the override";
}

TEST(PlatformClimate, AHeaterThatNowRunsHeatTakesTheTargetAsTheComfortSetpointWhateverTheCardShowed) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_programme_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::MODE, RADIATOR_PROGRAMME);
    test::store_reading(r, DeviceReading::SETPOINT, 19.0F);
    test::store_reading(r, DeviceReading::TEMPERATURE_IN_FORCE, 17.0F);
  });
  ASSERT_EQ(entity.mode, CLIMATE_MODE_AUTO);
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, RADIATOR_HEAT)};
  hub.after_plan = {{stores(DeviceRegister::COMFORT_SETPOINT, 21.0F)}};

  perform_target(entity, 21.0F);

  EXPECT_EQ(written(hub), std::vector<DeviceRegister>{DeviceRegister::COMFORT_SETPOINT});
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT) << "the card shows what the read found";
  EXPECT_FLOAT_EQ(entity.target_temperature, 21.0F);
}

TEST(PlatformClimate, RadiatorInProgOrAutoShowsTheTemperatureInForceAndATargetIsAnOverride) {
  for (const HeatingMode mode : {HeatingMode::PROG, HeatingMode::AUTO}) {
    MockHubClimate hub;
    IOHomeClimate entity = make_bound_climate(hub);
    entity.setup();
    hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, static_cast<int16_t>(mode))};
    hub.after_plan = {{override_taken(21.5F)}};
    deliver_programme_readings(hub, [mode](IoDevice &r) {
      test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(mode));
      test::store_reading(r, DeviceReading::SETPOINT, 19.0F);
      test::store_reading(r, DeviceReading::TEMPERATURE_IN_FORCE, 17.0F);
    });
    EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);
    EXPECT_FLOAT_EQ(entity.target_temperature, 17.0F) << "the temperature in force, not the comfort setpoint";

    perform_target(entity, 21.5F);

    ASSERT_EQ(hub.plans.size(), 1u);
    EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::DEROGATED_SETPOINT, 21.5F))
        << "the comfort setpoint is not written";
    EXPECT_TRUE(hub.plans[0].temporary_override);
    EXPECT_TRUE(hub.plans[0].read_back) << "the override is read back";
  }
}

TEST(PlatformClimate, TowelDryerTargetInAutoIsTheOverrideInAnEcoPeriodAndTheComfortSetpointInAComfortPeriodOrHeat) {
  MockHubClimate hub;
  IOHomeClimate entity = make_towel_climate(hub);
  entity.setup();

  struct Period {
    float in_force_c;
    int16_t level;
    DeviceRegister written;
  };
  for (const Period &period : {Period{16.5F, heating_level::ECO, DeviceRegister::DEROGATED_SETPOINT},
                               Period{20.0F, heating_level::COMFORT, DeviceRegister::COMFORT_SETPOINT}}) {
    hub.plans.clear();
    deliver_programme_readings(hub, [&period](IoDevice &r) {
      test::store_programme_readings(r, TowelDryerMode::PROG, HeatingTemporaryState::NONE, period.in_force_c,
                                     period.level);
    });
    hub.on_programme_state = {stores(DeviceRegister::TOWEL_MODE, DRYER_PROGRAMME),
                              stores(DeviceRegister::TEMPORARY_STATE, NO_TIMED_RUN), level_is(period.level)};
    hub.after_plan = {
        {period.written == DeviceRegister::DEROGATED_SETPOINT ? override_taken(17.5F) : stores(period.written, 17.5F)}};
    perform_target(entity, 17.5F);
    ASSERT_EQ(hub.plans.size(), 1u) << "level " << period.level;
    EXPECT_TRUE(is_register_write(hub.plans[0], period.written, 17.5F)) << "level " << period.level;
    EXPECT_TRUE(hub.plans[0].read_back) << "the register written is the one read back";
  }

  hub.plans.clear();
  hub.on_programme_state = {stores(DeviceRegister::TOWEL_MODE, DRYER_HEAT)};
  hub.after_plan = {{stores(DeviceRegister::COMFORT_SETPOINT, 21.0F)}};
  perform_target(entity, 21.0F);
  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::COMFORT_SETPOINT, 21.0F))
      << "heat: the target is the comfort temperature";
}

TEST(PlatformClimate, ATowelDryerTargetInAutoThatReportsNoPeriodWritesNothing) {
  MockHubClimate hub;
  IOHomeClimate entity = make_towel_climate(hub);
  entity.setup();
  deliver_programme_readings(hub, [](IoDevice &r) {
    test::store_programme_readings(r, TowelDryerMode::PROG, HeatingTemporaryState::NONE, 16.5F, heating_level::ECO);
  });
  // The status read answers with no level.
  hub.on_programme_state = {stores(DeviceRegister::TOWEL_MODE, DRYER_PROGRAMME),
                            stores(DeviceRegister::TEMPORARY_STATE, NO_TIMED_RUN), level_is(-1)};
  const int published = entity.publish_count_;

  perform_target(entity, 21.0F);

  EXPECT_TRUE(hub.plans.empty()) << "neither the override nor a comfort write is known to be right";
  EXPECT_EQ(entity.publish_count_, published) << "the card stays as it was";
}

TEST(PlatformClimate, ARefusedOverrideShowsTheHeatersOwnTargetAtOnceInsteadOfTheTypedValue) {
  MockHubClimate hub;
  IOHomeClimate entity = make_towel_climate(hub);
  entity.setup();
  deliver_programme_readings(hub, [](IoDevice &r) {
    test::store_programme_readings(r, TowelDryerMode::PROG, HeatingTemporaryState::NONE, 16.5F, heating_level::ECO);
  });
  hub.on_programme_state = {stores(DeviceRegister::TOWEL_MODE, DRYER_PROGRAMME),
                            stores(DeviceRegister::TEMPORARY_STATE, NO_TIMED_RUN), level_is(heating_level::ECO)};
  // Acknowledged, reads back as none: the read-back of 0 leaves the temperature in force alone
  // (HeaterController AnOverrideReadBackOfZeroLeavesTheTemperatureInForceAlone).
  hub.after_plan = {{stores(DeviceRegister::DEROGATED_SETPOINT, 0.0F)}};
  ASSERT_FLOAT_EQ(entity.target_temperature, 16.5F);
  const int published = entity.publish_count_;

  perform_target(entity, 21.0F);

  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::DEROGATED_SETPOINT, 21.0F));
  EXPECT_GT(entity.publish_count_, published) << "Home Assistant is told, so its card drops the typed value";
  EXPECT_FLOAT_EQ(entity.published_targets_.back(), 16.5F) << "the heater's own target";
  EXPECT_FLOAT_EQ(entity.target_temperature, 16.5F);
}

TEST(PlatformClimate, ADryerWhoseBoostEndedOrBeganSincethePollTakesTheTargetWhereTheHeaterNowSendsIt) {
  MockHubClimate hub;
  IOHomeClimate entity = make_towel_climate(hub);
  entity.setup();

  // The card shows a boost; the heater's timer has ended it.
  deliver_programme_readings(hub, [](IoDevice &r) {
    test::store_programme_readings(r, TowelDryerMode::PROG, HeatingTemporaryState::BOOST, NAN, heating_level::COMFORT);
  });
  ASSERT_TRUE(entity.preset.has_value());
  ASSERT_EQ(*entity.preset, CLIMATE_PRESET_BOOST);
  hub.on_programme_state = {stores(DeviceRegister::TOWEL_MODE, DRYER_PROGRAMME),
                            stores(DeviceRegister::TEMPORARY_STATE, NO_TIMED_RUN), level_is(heating_level::ECO)};
  hub.after_plan = {{override_taken(21.0F)}};
  perform_target(entity, 21.0F);
  EXPECT_EQ(written(hub), std::vector<DeviceRegister>{DeviceRegister::DEROGATED_SETPOINT});

  // The card shows the programme; a boost has begun on the heater's panel.
  hub.plans.clear();
  deliver_programme_readings(hub, [](IoDevice &r) {
    test::store_programme_readings(r, TowelDryerMode::PROG, HeatingTemporaryState::NONE, 17.0F, heating_level::COMFORT);
  });
  hub.on_programme_state = {stores(DeviceRegister::TOWEL_MODE, DRYER_PROGRAMME),
                            stores(DeviceRegister::TEMPORARY_STATE, TIMED_BOOST)};
  hub.after_plan = {{stores(DeviceRegister::COMFORT_SETPOINT, 21.0F)}};
  perform_target(entity, 21.0F);
  EXPECT_EQ(written(hub), std::vector<DeviceRegister>{DeviceRegister::COMFORT_SETPOINT});
}

TEST(PlatformClimate, RadiatorInHeatShowsAndWritesTheComfortSetpoint) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::MODE, RADIATOR_HEAT);
    test::store_reading(r, DeviceReading::SETPOINT, 19.0F);
  });
  EXPECT_FLOAT_EQ(entity.target_temperature, 19.0F);
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, RADIATOR_HEAT)};
  hub.after_plan = {{stores(DeviceRegister::COMFORT_SETPOINT, 21.0F)}};

  perform_target(entity, 21.0F);

  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::COMFORT_SETPOINT, 21.0F));
}

TEST(PlatformClimate, RadiatorWithoutATemperatureInForceTakesTheTargetAsTheComfortSetpointInAuto) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  IoDevice dev = *hub.get_device("ABC123");
  test::store_reading(dev, DeviceReading::MODE, RADIATOR_PROGRAMME);
  test::store_reading(dev, DeviceReading::SETPOINT, 19.0F);  // register 0x012A answered empty: no temperature in force
  test::set_in_force_support(dev, InForceSupport::ABSENT);
  hub.trigger_device_update("ABC123", dev, /*cache_device=*/true);
  EXPECT_FLOAT_EQ(entity.target_temperature, 19.0F);
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, RADIATOR_PROGRAMME)};

  perform_target(entity, 21.0F);

  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::COMFORT_SETPOINT, 21.0F));
}

TEST(PlatformClimate, ATargetInAutoBeforeTheHeaterAnsweredTheTemperatureInForceAsksItAndWritesTheOverride) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, RADIATOR_PROGRAMME),
                            in_force_is(InForceSupport::REPORTED, 17.0F)};
  hub.after_plan = {{override_taken(21.0F)}};

  perform_target(entity, 21.0F);  // nothing has been read since boot

  EXPECT_EQ(written(hub), std::vector<DeviceRegister>{DeviceRegister::DEROGATED_SETPOINT});
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);
}

TEST(PlatformClimate, ATargetInAutoOnAHeaterThatAnswersTheTemperatureInForceEmptyWritesTheComfortSetpoint) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, RADIATOR_PROGRAMME),
                            in_force_is(InForceSupport::ABSENT)};

  perform_target(entity, 21.0F);

  EXPECT_EQ(written(hub), std::vector<DeviceRegister>{DeviceRegister::COMFORT_SETPOINT});
}

TEST(PlatformClimate, ASilentHeaterGetsNoTargetAndTheCardKeepsWhatItShowed) {
  for (const bool towel : {false, true}) {
    MockHubClimate hub;
    IOHomeClimate entity = towel ? make_towel_climate(hub) : make_bound_climate(hub);
    entity.setup();
    if (towel) {
      deliver_programme_readings(hub, [](IoDevice &r) {
        test::store_programme_readings(r, TowelDryerMode::PROG, HeatingTemporaryState::NONE, 17.0F,
                                       heating_level::COMFORT);
      });
    } else {
      deliver_programme_readings(hub, [](IoDevice &r) {
        test::store_reading(r, DeviceReading::MODE, RADIATOR_PROGRAMME);
        test::store_reading(r, DeviceReading::SETPOINT, 19.0F);
        test::store_reading(r, DeviceReading::TEMPERATURE_IN_FORCE, 17.0F);
      });
    }
    const ClimateMode shown_mode = entity.mode;
    const float shown_target = entity.target_temperature;
    const int published = entity.publish_count_;
    hub.programme_state_answer = false;  // the heater does not answer the read of its mode

    perform_target(entity, 21.0F);

    EXPECT_EQ(hub.programme_state_reads.size(), 1u) << "towel " << towel << ": asked once";
    EXPECT_TRUE(hub.plans.empty()) << "no override and no comfort fallback";
    EXPECT_EQ(entity.mode, shown_mode);
    EXPECT_FLOAT_EQ(entity.target_temperature, shown_target);
    EXPECT_EQ(entity.publish_count_, published);
  }
}

TEST(PlatformClimate, AHeaterThatDoesNotAnswerTheTemperatureInForceGetsNoTarget) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  // The mode is answered, the temperature in force is not: it stays unknown.
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, RADIATOR_PROGRAMME)};

  perform_target(entity, 21.0F);

  EXPECT_TRUE(hub.plans.empty());
}

TEST(PlatformClimate, TheSameTargetGoesThroughOnceTheSilentHeaterAnswers) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  hub.programme_state_answer = false;
  perform_target(entity, 21.0F);
  ASSERT_TRUE(hub.plans.empty());

  hub.programme_state_answer = true;
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, RADIATOR_HEAT)};
  hub.after_plan = {{stores(DeviceRegister::COMFORT_SETPOINT, 21.0F)}};
  perform_target(entity, 21.0F);

  EXPECT_EQ(written(hub), std::vector<DeviceRegister>{DeviceRegister::COMFORT_SETPOINT});
}

namespace {

/// Asks for `auto` and a target in one call.
void perform_auto_and_target(IOHomeClimate &entity, float target_c) {
  auto call = entity.make_call();
  call.set_mode(CLIMATE_MODE_AUTO);
  call.set_target_temperature(target_c);
  call.perform();
}

}  // namespace

TEST(PlatformClimate, AModeAndATargetInOneCallWriteTheModeThenAskTheHeaterBeforeTheTarget) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_radiator_in_heat(hub);
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, RADIATOR_PROGRAMME)};
  // The mode write's read-back, then the override's.
  hub.after_plan = {{stores(DeviceRegister::OPERATING_MODE, RADIATOR_PROGRAMME)}, {override_taken(21.0F)}};

  perform_auto_and_target(entity, 21.0F);

  EXPECT_EQ(written(hub),
            (std::vector<DeviceRegister>{DeviceRegister::OPERATING_MODE, DeviceRegister::DEROGATED_SETPOINT}));
  EXPECT_EQ(hub.programme_state_reads.size(), 1u) << "the read is for the target, not for the mode";
}

TEST(PlatformClimate, SwitchingToAutoShowsTheTemperatureInForceAsTheTarget) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_radiator_in_heat(hub);
  ASSERT_FLOAT_EQ(entity.target_temperature, 19.0F);
  // The mode's read-back, then one read of the temperature in force.
  hub.after_plan = {
      {stores(DeviceRegister::OPERATING_MODE, RADIATOR_PROGRAMME), in_force_is(InForceSupport::REPORTED, 17.5F)}};
  const size_t published = entity.published_targets_.size();

  perform_mode(entity, CLIMATE_MODE_AUTO);

  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO);
  EXPECT_FLOAT_EQ(entity.target_temperature, 17.5F) << "the temperature in force, not the comfort setpoint";
  // The read-back's own publish shows the comfort setpoint for one step (the in-force read is a second
  // exchange); the card then moves to the temperature in force. Nothing is published after it.
  EXPECT_EQ(std::vector<float>(entity.published_targets_.begin() + static_cast<std::ptrdiff_t>(published),
                               entity.published_targets_.end()),
            (std::vector<float>{19.0F, 17.5F}));
}

TEST(PlatformClimate, AModeAndATargetInOneCallShowTheOverrideAfterTheTemperatureInForce) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  deliver_radiator_in_heat(hub);
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, RADIATOR_PROGRAMME)};
  hub.after_plan = {
      {stores(DeviceRegister::OPERATING_MODE, RADIATOR_PROGRAMME), in_force_is(InForceSupport::REPORTED, 17.5F)},
      {override_taken(21.0F)}};
  const size_t published = entity.published_targets_.size();

  perform_auto_and_target(entity, 21.0F);

  EXPECT_FLOAT_EQ(entity.target_temperature, 21.0F);
  EXPECT_EQ(std::vector<float>(entity.published_targets_.begin() + static_cast<std::ptrdiff_t>(published),
                               entity.published_targets_.end()),
            (std::vector<float>{19.0F, 17.5F, 21.0F}))
      << "the mode's read-back, the temperature in force, then the override's read-back";
}

TEST(PlatformClimate, AModeAndATargetInOneCallToASilentHeaterWriteOnlyTheMode) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  hub.programme_state_answer = false;

  perform_auto_and_target(entity, 21.0F);

  ASSERT_EQ(hub.plans.size(), 1u) << "the mode write is acknowledged, the target is not written";
  EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::AUTO)));
  EXPECT_EQ(hub.programme_state_reads.size(), 1u);
  EXPECT_TRUE(std::isnan(entity.target_temperature)) << "the refused target is not shown";
}

TEST(PlatformClimate, APresetAndATargetInOneCallAskTheHeaterBeforeTheTarget) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, RADIATOR_PROGRAMME),
                            in_force_is(InForceSupport::REPORTED, 17.0F)};
  hub.after_plan = {{stores(DeviceRegister::OPERATING_MODE, RADIATOR_PROGRAMME)}, {override_taken(21.0F)}};

  auto call = entity.make_call();
  call.set_preset(std::string("Program"));
  call.set_target_temperature(21.0F);
  call.perform();

  EXPECT_EQ(written(hub),
            (std::vector<DeviceRegister>{DeviceRegister::OPERATING_MODE, DeviceRegister::DEROGATED_SETPOINT}));
  EXPECT_EQ(hub.programme_state_reads.size(), 1u);
}

TEST(PlatformClimate, AModeOrAPresetAloneNeverAsksTheHeater) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  perform_mode(entity, CLIMATE_MODE_HEAT);
  auto preset = entity.make_call();
  preset.set_preset(std::string("Auto"));
  preset.perform();

  EXPECT_TRUE(hub.programme_state_reads.empty());
  EXPECT_EQ(hub.plans.size(), 2u);
}

TEST(PlatformClimate, AHeaterWithoutAProgrammeTakesATargetWithoutAnyRead) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub, DeviceType::HEATING_TEMPERATURE_INTERFACE, 0);
  entity.setup();

  perform_target(entity, 21.0F);

  EXPECT_TRUE(hub.programme_state_reads.empty());
  EXPECT_EQ(hub.plans.size(), 1u);
}

TEST(PlatformClimate, TowelDryerComfortAndEcoAreNoPresetAndSendNothing) {
  MockHubClimate hub;
  IOHomeClimate entity = make_towel_climate(hub);
  entity.setup();
  deliver_readings(hub, [](IoDevice &r) {
    test::store_programme_readings(r, TowelDryerMode::PROG, HeatingTemporaryState::NONE, 16.5F, heating_level::ECO);
  });
  const int published = entity.publish_count_;

  perform_preset(entity, CLIMATE_PRESET_COMFORT);
  perform_preset(entity, CLIMATE_PRESET_ECO);

  EXPECT_TRUE(hub.plans.empty()) << "no frame";
  ASSERT_TRUE(entity.preset.has_value());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_NONE);
  EXPECT_EQ(entity.publish_count_, published);
}

TEST(PlatformClimate, TowelDryerPresetNoneEndsTheBoost) {
  MockHubClimate hub;
  IOHomeClimate entity = make_towel_climate(hub);
  entity.setup();
  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::MANUAL));
    test::store_reading(r, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::BOOST));
  });

  perform_preset(entity, CLIMATE_PRESET_NONE);
  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::TEMPORARY_STATE,
                                static_cast<float>(HeatingTemporaryState::NONE)));
}

TEST(PlatformClimate, TowelDryerModeCommandKeepsTheCommandedBoost) {
  MockHubClimate hub;
  IOHomeClimate entity = make_towel_climate(hub);
  entity.setup();

  perform_preset(entity, CLIMATE_PRESET_BOOST);
  perform_mode(entity, CLIMATE_MODE_HEAT);
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT);
  ASSERT_TRUE(entity.preset.has_value());
  EXPECT_EQ(*entity.preset, CLIMATE_PRESET_BOOST) << "the boost lives in its own register";
}

TEST(PlatformClimate, TowelDryerActionFollowsTheWorkingRateNotTheRegulator) {
  MockHubClimate hub;
  IOHomeClimate entity = make_towel_climate(hub);
  entity.setup();

  // Boost as two Adelis units report it: regulator 0, working rate 100 %.
  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::MANUAL));
    test::store_reading(r, DeviceReading::REGULATOR_STATE, static_cast<int16_t>(HeatingRegulatorState::NONE));
    test::store_reading(r, DeviceReading::WORKING_RATE, 100);
  });
  EXPECT_EQ(entity.action, CLIMATE_ACTION_HEATING);

  deliver_readings(hub, [](IoDevice &r) { test::store_reading(r, DeviceReading::WORKING_RATE, 13); });
  EXPECT_EQ(entity.action, CLIMATE_ACTION_HEATING) << "any working rate above 0 heats";

  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::WORKING_RATE, 0);
    test::store_reading(r, DeviceReading::REGULATOR_STATE, static_cast<int16_t>(HeatingRegulatorState::HEATING));
  });
  EXPECT_EQ(entity.action, CLIMATE_ACTION_IDLE) << "the regulator is not the dryer's action source";

  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::OFF));
  });
  EXPECT_EQ(entity.action, CLIMATE_ACTION_OFF);
}

TEST(PlatformClimate, RadiatorIgnoresTheTowelRegistersAndTheNonePreset) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);  // 52/1 without a model: the AGILIA radiator
  entity.setup();

  deliver_readings(hub, [](IoDevice &r) {
    test::store_reading(r, DeviceReading::MODE, static_cast<int16_t>(HeatingMode::MANUAL));
    test::store_reading(r, DeviceReading::REGULATOR_STATE, static_cast<int16_t>(HeatingRegulatorState::STANDBY));
    test::store_reading(r, DeviceReading::TOWEL_MODE, static_cast<int16_t>(TowelDryerMode::OFF));
    test::store_reading(r, DeviceReading::TEMPORARY_STATE, static_cast<int16_t>(HeatingTemporaryState::BOOST));
    test::store_reading(r, DeviceReading::WORKING_RATE, 100);
  });
  EXPECT_EQ(entity.mode, CLIMATE_MODE_HEAT) << "the mode is register 0x0100";
  EXPECT_EQ(entity.action, CLIMATE_ACTION_IDLE) << "the action is the regulator";
  EXPECT_FALSE(entity.preset.has_value());
  EXPECT_FALSE(entity.has_custom_preset());

  perform_preset(entity, CLIMATE_PRESET_NONE);
  perform_preset(entity, CLIMATE_PRESET_BOOST);
  EXPECT_TRUE(hub.plans.empty()) << "the radiator offers neither";

  perform_mode(entity, CLIMATE_MODE_OFF);
  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::OFF)));
}

TEST(PlatformClimate, RadiatorProgramPresetComesFromTheModeRegister) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  perform_custom_preset(entity, "Program");
  ASSERT_EQ(hub.plans.size(), 1u);
  EXPECT_TRUE(is_register_write(hub.plans[0], DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::PROG)));
  EXPECT_EQ(entity.mode, CLIMATE_MODE_AUTO) << "a commanded preset shows through the mode table";
  EXPECT_EQ(entity.get_custom_preset(), "Program");
}

TEST(PlatformClimate, EveryWriteIsOnePlanHandedToTheHubAndARefusedRequestHandsNoneOver) {
  MockHubClimate hub;
  IOHomeClimate entity = make_bound_climate(hub);
  entity.setup();

  // The read before the target finds heat; the first write's read-back too (HeaterController
  // ATargetOnAProgrammeHeaterSendsTheSameExchangesEveryTimeWhateverTheStoredReadingsSay,
  // APlannedRegisterWriteSendsItsBytesAppliesTheHookThenReadsBack).
  hub.on_programme_state = {stores(DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::MANUAL))};
  hub.after_plan = {{stores(DeviceRegister::OPERATING_MODE, static_cast<float>(HeatingMode::MANUAL))}};

  perform_mode(entity, CLIMATE_MODE_HEAT);
  auto target = entity.make_call();
  target.set_target_temperature(20.5F);
  target.perform();
  perform_custom_preset(entity, "Program");

  ASSERT_EQ(hub.plans.size(), 3u) << "each write reaches the hub as one plan";
  EXPECT_EQ(hub.plans[0].register_fn, DeviceRegister::OPERATING_MODE);
  EXPECT_EQ(hub.plans[1].register_fn, DeviceRegister::COMFORT_SETPOINT);
  EXPECT_EQ(hub.plans[2].register_fn, DeviceRegister::OPERATING_MODE);

  perform_mode(entity, CLIMATE_MODE_COOL);
  perform_custom_preset(entity, "Drying");
  EXPECT_EQ(hub.plans.size(), 3u) << "a request the profile does not offer is never handed over";
}
