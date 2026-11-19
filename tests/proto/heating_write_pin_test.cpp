/// @file heating_write_pin_test.cpp
/// @brief Pins what every device value write of a heater encodes to or is refused as: the register
/// writes, the night light's percent scale and the temperature main parameter. The literals are the
/// contract of the write rules; they change only where a decision record changes a rule.

#include "proto_codecs.h"
#include "proto_device_readings.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <vector>

using namespace esphome::home_io_control;

namespace {

constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();
constexpr float INF_F = std::numeric_limits<float>::infinity();

/// The payload encode_heating_write() builds; empty when it refuses.
std::vector<uint8_t> write_bytes(DeviceRegister fn, float value) {
  uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE] = {0};
  const size_t n = encode_heating_write(fn, value, out);
  return std::vector<uint8_t>(out, out + n);
}

/// The set request of one register: `0C 61 01 <register>` and the value bytes.
std::vector<uint8_t> set_request(uint8_t register_low, std::vector<uint8_t> value) {
  std::vector<uint8_t> payload = {0x0C, 0x61, 0x01, register_low};
  payload.insert(payload.end(), value.begin(), value.end());
  return payload;
}

/// One write and what it becomes: the payload, or an empty one for a refusal.
struct WriteCase {
  float value;
  std::vector<uint8_t> payload;
};

void expect_cases(DeviceRegister fn, const std::vector<WriteCase> &cases) {
  for (const WriteCase &c : cases)
    EXPECT_EQ(write_bytes(fn, c.value), c.payload) << device_register_name(fn) << " value " << c.value;
}

}  // namespace

// === Every register of the table ===

TEST(HeatingWritePin, ARegisterThatIsReadOnlyNeverEncodesAWrite) {
  const std::vector<DeviceRegister> read_only = {
      DeviceRegister::PAIRED_DEVICES,       DeviceRegister::SETPOINT_BLOCK,
      DeviceRegister::ROOM_TEMPERATURE,     DeviceRegister::REGULATOR_STATE,
      DeviceRegister::MAX_SETPOINT,         DeviceRegister::WINDOW_OPEN,
      DeviceRegister::WORKING_RATE,         DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE,
      DeviceRegister::ECO_SETBACK,          DeviceRegister::TEMPERATURE_IN_FORCE,
      DeviceRegister::CENTRAL_HEATING_MODE, DeviceRegister::ENERGY_TOTAL,
      DeviceRegister::BOOST_REMAINING,      DeviceRegister::BOOST_DURATION,
      DeviceRegister::DRYING_REMAINING,     DeviceRegister::DRYING_DURATION,
  };
  for (const DeviceRegister fn : read_only) {
    EXPECT_FALSE(device_register_is_writable(fn)) << device_register_name(fn);
    for (const float value : {0.0F, 1.0F, 20.0F, 100.0F})
      EXPECT_TRUE(write_bytes(fn, value).empty()) << device_register_name(fn) << " value " << value;
  }
}

TEST(HeatingWritePin, TheWritableRegistersAreExactlyTheListedOnes) {
  const std::vector<DeviceRegister> writable = {
      DeviceRegister::COMFORT_SETPOINT,      DeviceRegister::OPERATING_MODE, DeviceRegister::PRESENCE,
      DeviceRegister::OPEN_WINDOW_DETECTION, DeviceRegister::TOWEL_MODE,     DeviceRegister::TEMPORARY_STATE,
      DeviceRegister::DEROGATED_SETPOINT,    DeviceRegister::NIGHT_LIGHT,    DeviceRegister::NIGHT_LIGHT_BRIGHTNESS,
      DeviceRegister::OCCUPANCY_DETECTION,   DeviceRegister::DHW_MODE,       DeviceRegister::DHW_BOOST_DURATION,
      DeviceRegister::DHW_AWAY_DURATION,
  };
  size_t count = 0;
  for (uint8_t i = 0; i < static_cast<uint8_t>(DeviceRegister::COUNT); i++) {
    if (device_register_is_writable(static_cast<DeviceRegister>(i)))
      count++;
  }
  EXPECT_EQ(count, writable.size());
  for (const DeviceRegister fn : writable)
    EXPECT_TRUE(device_register_is_writable(fn)) << device_register_name(fn);
}

TEST(HeatingWritePin, ASetpointIsTenthsOfADegreeFromSevenToTwentyEight) {
  expect_cases(DeviceRegister::COMFORT_SETPOINT,
               {
                   {7.0F, set_request(0x03, {0x46, 0x00})},
                   {7.04F, set_request(0x03, {0x46, 0x00})},
                   {20.5F, set_request(0x03, {0xCD, 0x00})},
                   {20.55F, set_request(0x03, {0xCE, 0x00})},  // the half rounds away from zero
                   {21.0F, set_request(0x03, {0xD2, 0x00})},
                   {25.5F, set_request(0x03, {0xFF, 0x00})},  // the last value that fits the low byte
                   {26.0F, set_request(0x03, {0x04, 0x01})},  // the first value that carries into the high byte
                   {27.96F, set_request(0x03, {0x18, 0x01})},
                   {28.0F, set_request(0x03, {0x18, 0x01})},
                   {6.95F, {}},
                   {6.9F, {}},
                   {28.05F, {}},
                   {28.1F, {}},
                   {0.0F, {}},
                   {-0.0F, {}},
                   {-5.0F, {}},
                   {100.0F, {}},
                   {NAN_F, {}},
                   {INF_F, {}},
                   {-INF_F, {}},
               });
}

TEST(HeatingWritePin, AnOverrideIsASetpointOrZeroWhichCancelsIt) {
  expect_cases(DeviceRegister::DEROGATED_SETPOINT, {
                                                       {0.0F, set_request(0x1D, {0x00, 0x00})},
                                                       {-0.0F, set_request(0x1D, {0x00, 0x00})},
                                                       {7.0F, set_request(0x1D, {0x46, 0x00})},
                                                       {17.5F, set_request(0x1D, {0xAF, 0x00})},
                                                       {28.0F, set_request(0x1D, {0x18, 0x01})},
                                                       {0.1F, {}},
                                                       {6.9F, {}},
                                                       {28.1F, {}},
                                                       {-1.0F, {}},
                                                       {NAN_F, {}},
                                                       {INF_F, {}},
                                                       {-INF_F, {}},
                                                   });
}

TEST(HeatingWritePin, TheModeRegisterTakesEveryModeButTheExternalSetpoint) {
  expect_cases(DeviceRegister::OPERATING_MODE, {
                                                   {0.0F, set_request(0x00, {0x00})},
                                                   {1.0F, set_request(0x00, {0x01})},
                                                   {2.0F, set_request(0x00, {0x02})},
                                                   {3.0F, {}},  // the external setpoint: readable, never written
                                                   {4.0F, set_request(0x00, {0x04})},
                                                   {5.0F, {}},
                                                   {255.0F, {}},
                                                   {256.0F, {}},
                                                   {-1.0F, {}},
                                                   {1.5F, {}},
                                                   {3.5F, {}},
                                                   {NAN_F, {}},
                                                   {INF_F, {}},
                                                   {-INF_F, {}},
                                               });
}

TEST(HeatingWritePin, AFlagRegisterTakesZeroOrOne) {
  const struct {
    DeviceRegister fn;
    uint8_t register_low;
  } flags[] = {
      {DeviceRegister::PRESENCE, 0x10},
      {DeviceRegister::OPEN_WINDOW_DETECTION, 0x06},
      {DeviceRegister::NIGHT_LIGHT, 0x44},
      {DeviceRegister::OCCUPANCY_DETECTION, 0x01},
  };
  for (const auto &flag : flags) {
    expect_cases(flag.fn, {
                              {0.0F, set_request(flag.register_low, {0x00})},
                              {1.0F, set_request(flag.register_low, {0x01})},
                              {2.0F, {}},
                              {-1.0F, {}},
                              {0.5F, {}},
                              {NAN_F, {}},
                              {INF_F, {}},
                          });
  }
}

TEST(HeatingWritePin, ATowelDryerModeAndTemporaryStateTakeTheirOwnRanges) {
  expect_cases(DeviceRegister::TOWEL_MODE, {
                                               {0.0F, set_request(0x36, {0x00})},
                                               {1.0F, set_request(0x36, {0x01})},
                                               {2.0F, set_request(0x36, {0x02})},
                                               {3.0F, set_request(0x36, {0x03})},
                                               {4.0F, {}},
                                               {-1.0F, {}},
                                               {1.5F, {}},
                                               {NAN_F, {}},
                                               {INF_F, {}},
                                           });
  expect_cases(DeviceRegister::TEMPORARY_STATE, {
                                                    {0.0F, set_request(0x3E, {0x00})},
                                                    {1.0F, set_request(0x3E, {0x01})},
                                                    {2.0F, set_request(0x3E, {0x02})},
                                                    {3.0F, {}},
                                                    {-1.0F, {}},
                                                    {0.5F, {}},
                                                    {NAN_F, {}},
                                                    {INF_F, {}},
                                                });
}

TEST(HeatingWritePin, TheNightLightBrightnessIsAPercentageSentAsTwoRegisterBytes) {
  expect_cases(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, {
                                                           {0.0F, set_request(0x42, {0x00, 0x00})},
                                                           {3.3F, set_request(0x42, {0x64, 0x00})},
                                                           {33.0F, set_request(0x42, {0xE8, 0x03})},
                                                           {66.0F, set_request(0x42, {0xD0, 0x07})},
                                                           {100.0F, set_request(0x42, {0x10, 0x27})},
                                                           {100.5F, {}},
                                                           {101.0F, {}},
                                                           {65535.0F, {}},
                                                           {-1.0F, {}},
                                                           {NAN_F, {}},
                                                           {INF_F, {}},
                                                           {-INF_F, {}},
                                                       });
}

TEST(HeatingWritePin, AnOutOfRangeFunctionIsNeverWritten) {
  EXPECT_TRUE(write_bytes(DeviceRegister::COUNT, 1.0F).empty());
  EXPECT_TRUE(write_bytes(static_cast<DeviceRegister>(200), 1.0F).empty());
}

TEST(HeatingWritePin, ARefusedFunctionLeavesTheOutputBufferUntouched) {
  uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE];
  std::fill(std::begin(out), std::end(out), uint8_t{0xEE});
  const auto bogus = static_cast<DeviceRegister>(0x7F);
  EXPECT_EQ(encode_heating_payload(bogus, 20.0F, out), 0u);
  EXPECT_EQ(encode_heating_read(bogus, out), 0u);
  EXPECT_EQ(encode_heating_write(bogus, 20.0F, out), 0u);
  for (const uint8_t b : out)
    EXPECT_EQ(b, 0xEE) << "a refused write must not write the output buffer";
}

TEST(HeatingWritePin, TheActionPayloadReadsAReadOnlyRegisterAndWritesTheOthers) {
  uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE] = {0};
  ASSERT_EQ(encode_heating_payload(DeviceRegister::WINDOW_OPEN, 5.0F, out), 4u);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 4), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x0E}));
  ASSERT_EQ(encode_heating_payload(DeviceRegister::PRESENCE, 1.0F, out), 5u);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 5), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x10, 0x01}));
  EXPECT_EQ(encode_heating_payload(DeviceRegister::PRESENCE, 2.0F, out), 0u);
}

// === The night light's percent scale ===

namespace {

/// The register value of each whole percentage 0-100 on the heater's own app's scale.
constexpr uint16_t NIGHT_LIGHT_REGISTER_PER_PERCENT[] = {
    0,    30,   61,   91,   121,  152,  182,  212,  242,  273,  303,  333,  364,  394,  424,  455,   485,
    515,  545,  576,  606,  636,  667,  697,  727,  758,  788,  818,  848,  879,  909,  939,  970,   1000,
    1030, 1061, 1091, 1121, 1152, 1182, 1212, 1242, 1273, 1303, 1333, 1364, 1394, 1424, 1455, 1485,  1515,
    1545, 1576, 1606, 1636, 1667, 1697, 1727, 1758, 1788, 1818, 1848, 1879, 1909, 1939, 1970, 2000,  2235,
    2471, 2706, 2941, 3176, 3412, 3647, 3882, 4118, 4353, 4588, 4824, 5059, 5294, 5529, 5765, 6000,  6235,
    6471, 6706, 6941, 7176, 7412, 7647, 7882, 8118, 8353, 8588, 8824, 9059, 9294, 9529, 9765, 10000,
};

}  // namespace

TEST(NightLightWritePin, EveryWholePercentageWritesItsRegisterValueInTwoBytes) {
  static_assert(sizeof(NIGHT_LIGHT_REGISTER_PER_PERCENT) / sizeof(uint16_t) == 101);
  for (unsigned percent = 0; percent <= 100; percent++) {
    const uint16_t raw = NIGHT_LIGHT_REGISTER_PER_PERCENT[percent];
    EXPECT_EQ(write_bytes(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, static_cast<float>(percent)),
              set_request(0x42, {static_cast<uint8_t>(raw & 0xFF), static_cast<uint8_t>(raw >> 8)}))
        << percent << " %";
  }
}

TEST(NightLightWritePin, APercentageBetweenTwoWholeOnesIsInterpolated) {
  EXPECT_EQ(write_bytes(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, 50.5F), set_request(0x42, {0xFA, 0x05}))
      << "50.5 % -> 1530 = 0x05FA";
}

TEST(NightLightWritePin, AboveOneHundredPercentIsRefused) {
  for (const unsigned percent : {101U, 150U, 255U})
    EXPECT_TRUE(write_bytes(DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, static_cast<float>(percent)).empty())
        << percent << " %";
}

TEST(NightLightWritePin, ARegisterValueShowsAsItsPercentageOnTheScale) {
  const struct {
    uint16_t raw;
    float percent;
  } shown[] = {
      {0, 0.0F},     {1, 0.033F},   {30, 0.99F},    {100, 3.3F},    {500, 16.5F},     {1000, 33.0F},
      {1500, 49.5F}, {2000, 66.0F}, {3000, 70.25F}, {5000, 78.75F}, {9999, 99.9958F}, {10000, 100.0F},
  };
  for (const auto &s : shown) {
    float value = NAN_F;
    IoFrame reply{};
    reply.cmd = CMD_WRITE_PRIVATE_ACK;
    const uint8_t data[] = {
        0x0C, 0x60, 0x01, 0x42, static_cast<uint8_t>(s.raw & 0xFF), static_cast<uint8_t>(s.raw >> 8)};
    reply.data_len = sizeof(data);
    std::copy(data, data + sizeof(data), reply.data);
    ASSERT_TRUE(decode_heating_reply(reply, DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, value)) << s.raw;
    EXPECT_NEAR(value, s.percent, 1e-4F) << s.raw;
  }
}

// === The temperature main parameter ===

TEST(MainParameterTemperaturePin, AnInRangeTemperatureEncodesOverTheDeviceRange) {
  const struct {
    float celsius;
    float min_c;
    float max_c;
    uint16_t main_parameter;
  } cases[] = {
      {-40.0F, -40.0F, 80.0F, 0},   {80.0F, -40.0F, 80.0F, 51200}, {20.0F, -40.0F, 80.0F, 25600},
      {0.0F, -40.0F, 80.0F, 17067}, {10.0F, 10.0F, 25.0F, 0},      {25.0F, 10.0F, 25.0F, 51200},
      {17.5F, 10.0F, 25.0F, 25600}, {21.0F, 7.0F, 30.0F, 31165},
  };
  for (const auto &c : cases) {
    uint16_t main_parameter = 0xFFFF;
    ASSERT_TRUE(encode_temperature_main_parameter(c.celsius, c.min_c, c.max_c, main_parameter)) << c.celsius;
    EXPECT_EQ(main_parameter, c.main_parameter) << c.celsius << " over " << c.min_c << ".." << c.max_c;
  }
}

TEST(MainParameterTemperaturePin, ABadTemperatureOrRangeIsRefusedAndLeavesTheOutputUntouched) {
  uint16_t main_parameter = 0x1234;
  EXPECT_FALSE(encode_temperature_main_parameter(NAN_F, -40.0F, 80.0F, main_parameter));
  EXPECT_FALSE(encode_temperature_main_parameter(INF_F, -40.0F, 80.0F, main_parameter));
  EXPECT_FALSE(encode_temperature_main_parameter(20.0F, 80.0F, -40.0F, main_parameter));
  EXPECT_FALSE(encode_temperature_main_parameter(20.0F, 10.0F, 10.0F, main_parameter));
  EXPECT_FALSE(encode_temperature_main_parameter(20.0F, NAN_F, 30.0F, main_parameter));
  EXPECT_EQ(main_parameter, 0x1234);
}

TEST(MainParameterTemperaturePin, ATemperatureOutsideTheRangeIsRefused) {
  uint16_t main_parameter = 0x1234;
  EXPECT_FALSE(encode_temperature_main_parameter(-100.0F, -40.0F, 80.0F, main_parameter));
  EXPECT_FALSE(encode_temperature_main_parameter(500.0F, -40.0F, 80.0F, main_parameter));
  EXPECT_FALSE(encode_temperature_main_parameter(80.1F, -40.0F, 80.0F, main_parameter)) << "just above the top";
  EXPECT_FALSE(encode_temperature_main_parameter(28.0F, 10.0F, 25.0F, main_parameter))
      << "a target above the YAML range is refused, not sent as its top";
  EXPECT_FALSE(encode_temperature_main_parameter(5.0F, 10.0F, 25.0F, main_parameter));
  EXPECT_EQ(main_parameter, 0x1234);
}

TEST(MainParameterTemperaturePin, AThermostatTargetOutsideItsYamlRangeIsRefused) {
  const HeatingProfile *thermostat =
      find_heating_profile(DeviceType::HEATING_TEMPERATURE_INTERFACE, 0, DeviceModel::NONE);
  ASSERT_NE(thermostat, nullptr);
  const HeatingDeviceFacts facts = {
      .has_record = true, .range_min_c = 10.0F, .range_max_c = 25.0F, .programme_state_answered = true};

  const HeatingWritePlan inside =
      plan_heating_write(*thermostat, {.kind = HeatingWriteKind::TARGET, .target_c = 17.5F}, facts);
  ASSERT_EQ(inside.kind, HeatingPlanKind::TEMPERATURE);
  EXPECT_EQ(inside.main_parameter, 25600);

  const HeatingWritePlan above =
      plan_heating_write(*thermostat, {.kind = HeatingWriteKind::TARGET, .target_c = 28.0F}, facts);
  EXPECT_EQ(above.kind, HeatingPlanKind::NONE);
  EXPECT_EQ(above.refusal, HeatingRefusal::OUT_OF_RANGE);

  const HeatingWritePlan heat_above = plan_heating_write(
      *thermostat, {.kind = HeatingWriteKind::MODE, .hvac = HeatingHvacMode::HEAT, .shown_target_c = 28.0F}, facts);
  EXPECT_EQ(heat_above.kind, HeatingPlanKind::NONE);
  EXPECT_EQ(heat_above.refusal, HeatingRefusal::OUT_OF_RANGE);
}

TEST(MainParameterTemperaturePin, AFixedRangeProfileEncodesOverTheProfileRange) {
  const HeatingProfile *pump = find_heating_profile(DeviceType::HEAT_PUMP, 0, DeviceModel::NONE);
  ASSERT_NE(pump, nullptr);
  const HeatingDeviceFacts facts = {.has_record = true, .range_min_c = 10.0F, .range_max_c = 25.0F};
  const HeatingWritePlan plan = plan_heating_write(*pump, {.kind = HeatingWriteKind::TARGET, .target_c = 20.0F}, facts);
  ASSERT_EQ(plan.kind, HeatingPlanKind::TEMPERATURE);
  EXPECT_EQ(plan.main_parameter, 25600);
}

// === The register table ===

namespace {

/// What one register is: where it lives, its log label and the name the `heating_control` action offers it under.
struct RegisterFact {
  DeviceRegister fn;
  uint8_t family;
  uint8_t low;
  const char *name;        ///< The log label.
  bool offered_by_action;  ///< Whether `heating_control` takes the name.
};

}  // namespace

TEST(HeatingRegisterPin, EveryRegisterKeepsItsNumberItsLabelAndItsPlaceInTheAction) {
  constexpr uint8_t H = 0x01;  // the heater family
  constexpr uint8_t D = 0x00;  // the water heater family
  const std::vector<RegisterFact> facts = {
      {DeviceRegister::PAIRED_DEVICES, H, 0x2C, "power_on", true},
      {DeviceRegister::COMFORT_SETPOINT, H, 0x03, "set_temperature", true},
      {DeviceRegister::OPERATING_MODE, H, 0x00, "set_mode", true},
      {DeviceRegister::PRESENCE, H, 0x10, "set_presence", true},
      {DeviceRegister::OPEN_WINDOW_DETECTION, H, 0x06, "set_open_window_detection", true},
      {DeviceRegister::SETPOINT_BLOCK, H, 0x30, "midnight_sync", true},
      {DeviceRegister::ROOM_TEMPERATURE, H, 0x15, "room_temperature", false},
      {DeviceRegister::REGULATOR_STATE, H, 0x17, "regulator_state", false},
      {DeviceRegister::MAX_SETPOINT, H, 0x02, "max_setpoint", false},
      {DeviceRegister::WINDOW_OPEN, H, 0x0E, "window_open", true},
      {DeviceRegister::TOWEL_MODE, H, 0x36, "towel_mode", false},
      {DeviceRegister::TEMPORARY_STATE, H, 0x3E, "temporary_state", false},
      {DeviceRegister::WORKING_RATE, H, 0x26, "working_rate", false},
      {DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE, H, 0x37, "temperature_in_force", false},
      {DeviceRegister::ECO_SETBACK, H, 0x04, "eco_setback", false},
      {DeviceRegister::DEROGATED_SETPOINT, H, 0x1D, "set_derogation", true},
      {DeviceRegister::TEMPERATURE_IN_FORCE, H, 0x2A, "heater_temperature_in_force", false},
      {DeviceRegister::NIGHT_LIGHT, H, 0x44, "night_light", false},
      {DeviceRegister::NIGHT_LIGHT_BRIGHTNESS, H, 0x42, "night_light_brightness", false},
      {DeviceRegister::CENTRAL_HEATING_MODE, H, 0x53, "central_heating_mode", false},
      {DeviceRegister::OCCUPANCY_DETECTION, H, 0x01, "occupancy_detection", false},
      {DeviceRegister::ENERGY_TOTAL, H, 0x16, "energy_total", false},
      {DeviceRegister::BOOST_REMAINING, H, 0x3A, "boost_remaining", false},
      {DeviceRegister::BOOST_DURATION, H, 0x3B, "boost_duration", false},
      {DeviceRegister::DRYING_REMAINING, H, 0x3C, "drying_remaining", false},
      {DeviceRegister::DRYING_DURATION, H, 0x3D, "drying_duration", false},
      {DeviceRegister::DHW_WATER_TEMPERATURE, D, 0x17, "dhw_water_temperature", false},
      {DeviceRegister::DHW_MODE, D, 0x06, "dhw_mode", false},
      {DeviceRegister::DHW_HEATING, D, 0x21, "dhw_heating", false},
      {DeviceRegister::DHW_BOOST_DURATION, D, 0x00, "dhw_boost_duration", false},
      {DeviceRegister::DHW_AWAY_DURATION, D, 0x05, "dhw_away_duration", false},
      {DeviceRegister::DHW_ENERGY, D, 0x04, "dhw_energy", false},
      {DeviceRegister::DHW_BRAND, D, 0x07, "dhw_brand", false},
  };
  EXPECT_EQ(facts.size(), static_cast<size_t>(DeviceRegister::COUNT)) << "a register is missing from the pin";
  for (const RegisterFact &f : facts) {
    uint8_t high = 0xEE;
    uint8_t low = 0xEE;
    ASSERT_TRUE(device_register_bytes(f.fn, high, low)) << f.name;
    EXPECT_EQ(high, f.family) << f.name;
    EXPECT_EQ(low, f.low) << f.name;
    EXPECT_STREQ(device_register_name(f.fn), f.name);
    const std::optional<DeviceRegister> offered = heating_action_function(f.name);
    EXPECT_EQ(offered.has_value(), f.offered_by_action) << f.name;
    if (offered.has_value()) {
      EXPECT_EQ(*offered, f.fn) << f.name;
    }
  }
}

TEST(HeatingRegisterPin, TheTemperaturesInForceAreReadFromTheirOwnRegisters) {
  // The same value on two kinds of heater, in two registers: each reading names the register of its kind.
  const auto register_of = [](DeviceReading reading) {
    const DeviceReadingRow *row = find_device_reading(reading);
    EXPECT_NE(row, nullptr);
    const auto *source = row != nullptr ? std::get_if<RegisterSource>(&row->source) : nullptr;
    EXPECT_NE(source, nullptr);
    return source != nullptr ? source->fn : DeviceRegister::COUNT;
  };
  EXPECT_EQ(register_of(DeviceReading::TOWEL_TEMPERATURE_IN_FORCE), DeviceRegister::TOWEL_TEMPERATURE_IN_FORCE);
  EXPECT_EQ(register_of(DeviceReading::TEMPERATURE_IN_FORCE), DeviceRegister::TEMPERATURE_IN_FORCE);
}
