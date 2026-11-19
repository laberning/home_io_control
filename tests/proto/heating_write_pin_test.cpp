/// @file heating_write_pin_test.cpp
/// @brief Pins what every device value write of a heater encodes to or is refused as: the register
/// writes, the night light's percent scale and the temperature main parameter. The literals are the
/// contract of the write rules; they change only where a decision record changes a rule.

#include "proto_codecs.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace esphome::home_io_control;

namespace {

constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();
constexpr float INF_F = std::numeric_limits<float>::infinity();

/// The payload encode_heating_write() builds; empty when it refuses.
std::vector<uint8_t> write_bytes(HeatingFunction fn, float value) {
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

void expect_cases(HeatingFunction fn, const std::vector<WriteCase> &cases) {
  for (const WriteCase &c : cases)
    EXPECT_EQ(write_bytes(fn, c.value), c.payload) << heating_function_name(fn) << " value " << c.value;
}

}  // namespace

// === Every register of the table ===

TEST(HeatingWritePin, ARegisterThatIsReadOnlyNeverEncodesAWrite) {
  const std::vector<HeatingFunction> read_only = {
      HeatingFunction::POWER_ON,
      HeatingFunction::MIDNIGHT_SYNC,
      HeatingFunction::ROOM_TEMPERATURE,
      HeatingFunction::REGULATOR_STATE,
      HeatingFunction::MAX_SETPOINT,
      HeatingFunction::WINDOW_OPEN,
      HeatingFunction::WORKING_RATE,
      HeatingFunction::TEMPERATURE_IN_FORCE,
      HeatingFunction::ECO_SETBACK,
      HeatingFunction::HEATER_TEMPERATURE_IN_FORCE,
      HeatingFunction::CENTRAL_HEATING_MODE,
      HeatingFunction::ENERGY_TOTAL,
      HeatingFunction::BOOST_REMAINING,
      HeatingFunction::BOOST_DURATION,
      HeatingFunction::DRYING_REMAINING,
      HeatingFunction::DRYING_DURATION,
  };
  for (const HeatingFunction fn : read_only) {
    EXPECT_FALSE(heating_function_is_writable(fn)) << heating_function_name(fn);
    for (const float value : {0.0F, 1.0F, 20.0F, 100.0F})
      EXPECT_TRUE(write_bytes(fn, value).empty()) << heating_function_name(fn) << " value " << value;
  }
}

TEST(HeatingWritePin, TheWritableRegistersAreExactlyTheseTen) {
  const std::vector<HeatingFunction> writable = {
      HeatingFunction::SET_TEMPERATURE,           HeatingFunction::SET_MODE,    HeatingFunction::SET_PRESENCE,
      HeatingFunction::SET_OPEN_WINDOW_DETECTION, HeatingFunction::TOWEL_MODE,  HeatingFunction::TEMPORARY_STATE,
      HeatingFunction::DEROGATED_SETPOINT,        HeatingFunction::NIGHT_LIGHT, HeatingFunction::NIGHT_LIGHT_BRIGHTNESS,
      HeatingFunction::OCCUPANCY_DETECTION,
  };
  size_t count = 0;
  for (uint8_t i = 0; i < static_cast<uint8_t>(HeatingFunction::COUNT); i++) {
    if (heating_function_is_writable(static_cast<HeatingFunction>(i)))
      count++;
  }
  EXPECT_EQ(count, writable.size());
  for (const HeatingFunction fn : writable)
    EXPECT_TRUE(heating_function_is_writable(fn)) << heating_function_name(fn);
}

TEST(HeatingWritePin, ASetpointIsTenthsOfADegreeFromSevenToTwentyEight) {
  expect_cases(HeatingFunction::SET_TEMPERATURE,
               {
                   {7.0F, set_request(0x03, {0x46, 0x00})},
                   {7.04F, set_request(0x03, {0x46, 0x00})},
                   {20.5F, set_request(0x03, {0xCD, 0x00})},
                   {20.55F, set_request(0x03, {0xCE, 0x00})},  // the half rounds away from zero
                   {21.0F, set_request(0x03, {0xD2, 0x00})},
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
  expect_cases(HeatingFunction::DEROGATED_SETPOINT, {
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
  expect_cases(HeatingFunction::SET_MODE, {
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
    HeatingFunction fn;
    uint8_t register_low;
  } flags[] = {
      {HeatingFunction::SET_PRESENCE, 0x10},
      {HeatingFunction::SET_OPEN_WINDOW_DETECTION, 0x06},
      {HeatingFunction::NIGHT_LIGHT, 0x44},
      {HeatingFunction::OCCUPANCY_DETECTION, 0x01},
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
  expect_cases(HeatingFunction::TOWEL_MODE, {
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
  expect_cases(HeatingFunction::TEMPORARY_STATE, {
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
  expect_cases(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, {
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
  EXPECT_TRUE(write_bytes(HeatingFunction::COUNT, 1.0F).empty());
  EXPECT_TRUE(write_bytes(static_cast<HeatingFunction>(200), 1.0F).empty());
}

TEST(HeatingWritePin, TheActionPayloadReadsAReadOnlyRegisterAndWritesTheOthers) {
  uint8_t out[PRIVATE_REGISTER_PAYLOAD_MAX_SIZE] = {0};
  ASSERT_EQ(encode_heating_payload(HeatingFunction::WINDOW_OPEN, 5.0F, out), 4u);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 4), (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x0E}));
  ASSERT_EQ(encode_heating_payload(HeatingFunction::SET_PRESENCE, 1.0F, out), 5u);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 5), (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x10, 0x01}));
  EXPECT_EQ(encode_heating_payload(HeatingFunction::SET_PRESENCE, 2.0F, out), 0u);
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
    EXPECT_EQ(write_bytes(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, static_cast<float>(percent)),
              set_request(0x42, {static_cast<uint8_t>(raw & 0xFF), static_cast<uint8_t>(raw >> 8)}))
        << percent << " %";
  }
}

TEST(NightLightWritePin, AboveOneHundredPercentIsRefused) {
  for (const unsigned percent : {101U, 150U, 255U})
    EXPECT_TRUE(write_bytes(HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, static_cast<float>(percent)).empty())
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
    ASSERT_TRUE(decode_heating_reply(reply, HeatingFunction::NIGHT_LIGHT_BRIGHTNESS, value)) << s.raw;
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
