/// @file heating_control_pin_test.cpp
/// @brief Pins the `heating_control` action: every function it offers with good and bad values, the
/// spellings it accepts, the messages it answers with, the order those messages come in and the exact
/// frames it sends. The literals are the contract of the action; they change only where a decision record
/// changes a rule.

#include "hub_core.h"
#include "proto_commands.h"
#include "proto_frame.h"

#include "management_test_fixture.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace esphome::home_io_control;
using namespace management_test;

namespace {

/// A heater reply that echoes the register of `payload` (`0C 6x 01 <reg> [value]`).
IoFrame register_ack(const uint8_t dst[3], const std::vector<uint8_t> &payload) {
  IoFrame frame{};
  init_frame(frame, true, false, true, false);
  const uint8_t device_node_id[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, dst);
  set_src(frame, device_node_id);
  set_cmd(frame, CMD_WRITE_PRIVATE_ACK, payload.data(), static_cast<uint8_t>(payload.size()));
  return frame;
}

/// What one run of the action did.
struct ActionRun {
  ManagementActionResult result;
  size_t frames_sent{0};
  uint8_t command{0};
  std::vector<uint8_t> payload;  ///< The data of the one frame sent; empty when none was.
};

/// How the device the action addresses is set up.
enum class Rig : uint8_t {
  RADIATOR,     ///< An electric heater with an adjustable setpoint (private registers, a programme).
  TOWEL_DRYER,  ///< The same type, as a towel dryer.
  LEVELS,       ///< An electric heater commanded with heating levels.
  SHUTTER,      ///< Not a climate device.
};

/// Run the action on a rig; the heater acknowledges every register request with `reply_value` bytes.
ActionRun run_action(const std::string &function, const std::string &value, Rig rig = Rig::RADIATOR,
                     const std::vector<uint8_t> &read_value = {0x01}) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  IoDevice *dev = component.get_device("ABC123");
  EXPECT_NE(dev, nullptr);
  switch (rig) {
    case Rig::RADIATOR:
      test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 1);
      break;
    case Rig::TOWEL_DRYER:
      test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 1);
      dev->model = DeviceModel::TOWEL_DRYER;
      break;
    case Rig::LEVELS:
      test::set_kind(*dev, DeviceType::ELECTRICAL_HEATER, 0);
      break;
    case Rig::SHUTTER:
      test::set_kind(*dev, DeviceType::ROLLER_SHUTTER);
      break;
  }
  // One reply for the one request: a set is acknowledged, a read answers the sensor's value.
  std::vector<uint8_t> reply = {0x0C, 0x60, 0x01, 0x0E};
  reply.insert(reply.end(), read_value.begin(), read_value.end());
  radio.queue_rx(frame_to_packet(register_ack(component.node_id_, reply)));

  ActionRun run;
  run.result = component.management_actions_.heating_control("ABC123", function, value);
  run.frames_sent = radio.get_sent_data().size();
  if (run.frames_sent > 0) {
    const auto &tx = radio.get_sent_data().front();
    IoFrame sent{};
    EXPECT_TRUE(parse(tx.data(), static_cast<uint8_t>(tx.size()), sent));
    run.command = sent.cmd;
    run.payload.assign(sent.data, sent.data + sent.data_len);
  }
  return run;
}

/// A value the action accepts and the frame it sends for it.
struct Accepted {
  const char *function;
  const char *value;
  std::vector<uint8_t> payload;
};

/// A value the action refuses with a message, sending nothing.
struct Refused {
  const char *function;
  const char *value;
  const char *message;
};

constexpr const char *NOT_A_NUMBER = "temperature must be a number in degrees Celsius";
constexpr const char *OUT_OF_RANGE = "temperature must be between 7.0 and 28.0";
constexpr const char *MODE_MESSAGE = "mode must be one of auto, manual, prog, off";
constexpr const char *PRESENCE_MESSAGE = "presence must be 'on' or 'off'";
constexpr const char *DETECTION_MESSAGE = "open-window detection must be 'on' or 'off'";

}  // namespace

TEST(HeatingControlPin, EveryFunctionSendsItsExactFrameForAGoodValue) {
  const std::vector<Accepted> cases = {
      {"power_on", "", {0x0C, 0x60, 0x01, 0x2C}},
      {"power_on", "anything", {0x0C, 0x60, 0x01, 0x2C}},
      {"midnight_sync", "", {0x0C, 0x60, 0x01, 0x30}},
      {"midnight_sync", "ignored", {0x0C, 0x60, 0x01, 0x30}},
      {"set_temperature", "7", {0x0C, 0x61, 0x01, 0x03, 0x46, 0x00}},
      {"set_temperature", "7.0", {0x0C, 0x61, 0x01, 0x03, 0x46, 0x00}},
      {"set_temperature", "20.5", {0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00}},
      {"set_temperature", "20.55", {0x0C, 0x61, 0x01, 0x03, 0xCE, 0x00}},
      {"set_temperature", "28", {0x0C, 0x61, 0x01, 0x03, 0x18, 0x01}},
      {"set_derogation", "17.5", {0x0C, 0x61, 0x01, 0x1D, 0xAF, 0x00}},
      {"set_derogation", "7", {0x0C, 0x61, 0x01, 0x1D, 0x46, 0x00}},
      {"set_derogation", "28", {0x0C, 0x61, 0x01, 0x1D, 0x18, 0x01}},
      {"set_derogation", "0", {0x0C, 0x61, 0x01, 0x1D, 0x00, 0x00}},
      {"set_mode", "auto", {0x0C, 0x61, 0x01, 0x00, 0x00}},
      {"set_mode", "manual", {0x0C, 0x61, 0x01, 0x00, 0x01}},
      {"set_mode", "prog", {0x0C, 0x61, 0x01, 0x00, 0x02}},
      {"set_mode", "off", {0x0C, 0x61, 0x01, 0x00, 0x04}},
      {"set_presence", "on", {0x0C, 0x61, 0x01, 0x10, 0x01}},
      {"set_presence", "off", {0x0C, 0x61, 0x01, 0x10, 0x00}},
      {"set_open_window_detection", "on", {0x0C, 0x61, 0x01, 0x06, 0x01}},
      {"set_open_window_detection", "off", {0x0C, 0x61, 0x01, 0x06, 0x00}},
  };
  for (const Accepted &c : cases) {
    const ActionRun run = run_action(c.function, c.value);
    EXPECT_TRUE(run.result.success) << c.function << " " << c.value << ": " << run.result.message;
    EXPECT_FALSE(run.result.verified) << c.function << " is an ack, not a read-back";
    EXPECT_EQ(run.result.message, std::string("heating ") + c.function + " acknowledged by device");
    EXPECT_EQ(run.frames_sent, 1u) << c.function << " " << c.value;
    EXPECT_EQ(run.command, CMD_WRITE_PRIVATE) << c.function << " " << c.value;
    EXPECT_EQ(run.payload, c.payload) << c.function << " " << c.value;
  }
}

TEST(HeatingControlPin, TheWindowSensorIsReadAndReported) {
  for (const auto &[reply_value, text] : {std::pair<uint8_t, const char *>{0x01, "open"}, {0x00, "closed"}}) {
    const ActionRun run = run_action("window_open", "", Rig::RADIATOR, {reply_value});
    EXPECT_TRUE(run.result.success);
    EXPECT_FALSE(run.result.verified);
    EXPECT_EQ(run.result.message, std::string("heating window_open: window ") + text);
    ASSERT_EQ(run.frames_sent, 1u);
    EXPECT_EQ(run.payload, (std::vector<uint8_t>{0x0C, 0x60, 0x01, 0x0E}));
  }
  const ActionRun ignored = run_action("window_open", "whatever");
  EXPECT_TRUE(ignored.result.success) << "the value of a read is ignored";
}

TEST(HeatingControlPin, ATemperatureThatIsNotANumberIsRefusedBeforeAnyDeviceCheck) {
  for (const char *function : {"set_temperature", "set_derogation"}) {
    for (const char *value :
         {"", " ", "warm", "nan", "NaN", "inf", "-inf", "infinity", "21abc", "21,5", "1e999", "--5", "5 5"}) {
      const ActionRun run = run_action(function, value);
      EXPECT_FALSE(run.result.success) << function << " '" << value << "'";
      EXPECT_EQ(run.result.message, NOT_A_NUMBER) << function << " '" << value << "'";
      EXPECT_EQ(run.frames_sent, 0u) << function << " '" << value << "'";
    }
  }
}

TEST(HeatingControlPin, ATemperatureOutsideSevenToTwentyEightIsRefusedWithTheRange) {
  for (const char *function : {"set_temperature", "set_derogation"}) {
    for (const char *value : {"6.9", "6.95", "28.05", "28.1", "100", "-5", "1e30"}) {
      const ActionRun run = run_action(function, value);
      EXPECT_FALSE(run.result.success) << function << " '" << value << "'";
      EXPECT_EQ(run.result.message, OUT_OF_RANGE) << function << " '" << value << "'";
      EXPECT_EQ(run.frames_sent, 0u) << function << " '" << value << "'";
    }
  }
  for (const char *value : {"0", "0.0", "-0", "0.5"}) {
    const ActionRun run = run_action("set_temperature", value);
    EXPECT_EQ(run.result.message, OUT_OF_RANGE) << "set_temperature zero is no setpoint: '" << value << "'";
  }
}

TEST(HeatingControlPin, ATemperatureAcceptsTheSpellingsStrtofReads) {
  for (const char *value : {" 21 ", "21.0", "+21", "2.1e1", "21."}) {
    const ActionRun run = run_action("set_temperature", value);
    EXPECT_TRUE(run.result.success) << "'" << value << "'";
    EXPECT_EQ(run.payload, (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x03, 0xD2, 0x00})) << "'" << value << "'";
  }
}

TEST(HeatingControlPin, AnyValueThatEqualsZeroCancelsAnOverride) {
  for (const char *value : {"0", " 0 ", "0.0", "-0", "00", "0e0", "+0"}) {
    const ActionRun run = run_action("set_derogation", value);
    EXPECT_TRUE(run.result.success) << "'" << value << "'";
    EXPECT_EQ(run.payload, (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x1D, 0x00, 0x00})) << "'" << value << "'";
  }
}

TEST(HeatingControlPin, ANamedValueIsCaseFoldedAndTrimmedAndAnythingElseIsRefused) {
  for (const char *value : {"AUTO", " Auto ", "auto\t"})
    EXPECT_EQ(run_action("set_mode", value).payload, (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x00, 0x00})) << value;
  for (const char *value : {"ON", " On "})
    EXPECT_EQ(run_action("set_presence", value).payload, (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x10, 0x01})) << value;
  for (const char *value : {"OFF", " off "})
    EXPECT_EQ(run_action("set_open_window_detection", value).payload,
              (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x06, 0x00}))
        << value;

  const std::vector<Refused> refused = {
      {"set_mode", "", MODE_MESSAGE},
      {"set_mode", "heat", MODE_MESSAGE},
      {"set_mode", "sideways", MODE_MESSAGE},
      {"set_mode", "external", MODE_MESSAGE},
      {"set_mode", "0", MODE_MESSAGE},
      {"set_mode", "1", MODE_MESSAGE},
      {"set_mode", "3", MODE_MESSAGE},
      {"set_mode", "4", MODE_MESSAGE},
      {"set_mode", "au to", MODE_MESSAGE},
      {"set_presence", "", PRESENCE_MESSAGE},
      {"set_presence", "1", PRESENCE_MESSAGE},
      {"set_presence", "0", PRESENCE_MESSAGE},
      {"set_presence", "true", PRESENCE_MESSAGE},
      {"set_presence", "maybe", PRESENCE_MESSAGE},
      {"set_open_window_detection", "", DETECTION_MESSAGE},
      {"set_open_window_detection", "1", DETECTION_MESSAGE},
      {"set_open_window_detection", "ajar", DETECTION_MESSAGE},
      {"set_open_window_detection", "yes", DETECTION_MESSAGE},
  };
  for (const Refused &c : refused) {
    const ActionRun run = run_action(c.function, c.value);
    EXPECT_FALSE(run.result.success) << c.function << " '" << c.value << "'";
    EXPECT_EQ(run.result.message, c.message) << c.function << " '" << c.value << "'";
    EXPECT_EQ(run.frames_sent, 0u) << c.function << " '" << c.value << "'";
  }
}

TEST(HeatingControlPin, TheFunctionNameIsCaseFoldedAndTrimmed) {
  for (const char *name : {"SET_MODE", "  set_mode  ", "Set_Mode"}) {
    const ActionRun run = run_action(name, "manual");
    EXPECT_TRUE(run.result.success) << name;
    EXPECT_EQ(run.result.message, "heating set_mode acknowledged by device");
    EXPECT_EQ(run.payload, (std::vector<uint8_t>{0x0C, 0x61, 0x01, 0x00, 0x01})) << name;
  }
}

TEST(HeatingControlPin, AnUnknownFunctionIsNamedAsTypedAndSendsNothing) {
  // Registers the component writes or reads elsewhere are not functions of the action.
  for (const char *name : {"", "make_coffee", "set_window", "night_light", "night_light_brightness", "towel_mode",
                           "temporary_state", "occupancy_detection", "room_temperature", "regulator_state",
                           "max_setpoint", "working_rate", "energy_total", "set_temperature2", "set_", "power"}) {
    const ActionRun run = run_action(name, "1");
    EXPECT_FALSE(run.result.success) << name;
    EXPECT_EQ(run.result.message, std::string("unknown heating function '") + name + "'") << name;
    EXPECT_EQ(run.frames_sent, 0u) << name;
  }
  EXPECT_EQ(run_action("  SET_FOO ", "1").result.message, "unknown heating function 'SET_FOO'")
      << "the name is echoed trimmed, in the case it was typed";
}

TEST(HeatingControlPin, MessagesComeInTheOrderOfTheChecks) {
  // The function and the value are judged before the device is looked at.
  EXPECT_EQ(run_action("make_coffee", "1", Rig::SHUTTER).result.message, "unknown heating function 'make_coffee'");
  EXPECT_EQ(run_action("set_temperature", "warm", Rig::SHUTTER).result.message, NOT_A_NUMBER);
  EXPECT_EQ(run_action("set_temperature", "99", Rig::LEVELS).result.message, OUT_OF_RANGE);
  EXPECT_EQ(run_action("set_mode", "sideways", Rig::SHUTTER).result.message, MODE_MESSAGE);
  EXPECT_EQ(run_action("set_presence", "maybe", Rig::LEVELS).result.message, PRESENCE_MESSAGE);
  EXPECT_EQ(run_action("set_open_window_detection", "ajar", Rig::SHUTTER).result.message, DETECTION_MESSAGE);
  // Then the device: not a climate device, then commanded with levels.
  for (const char *function : {"power_on", "set_temperature", "set_mode", "window_open", "set_derogation"}) {
    const char *value = std::string(function) == "set_mode" ? "auto" : "21";
    const ActionRun shutter = run_action(function, value, Rig::SHUTTER);
    EXPECT_EQ(shutter.result.message, "device is not a climate device") << function;
    EXPECT_EQ(shutter.frames_sent, 0u) << function;
    const ActionRun levels = run_action(function, value, Rig::LEVELS);
    EXPECT_EQ(levels.result.message, "device is commanded with heating levels, not private registers") << function;
    EXPECT_EQ(levels.frames_sent, 0u) << function;
  }
}

TEST(HeatingControlPin, AnOverrideNeedsAHeaterWithAProgramme) {
  for (const Rig rig : {Rig::RADIATOR, Rig::TOWEL_DRYER})
    EXPECT_TRUE(run_action("set_derogation", "17.5", rig).result.success);
}

TEST(HeatingControlPin, AnUnansweredRequestIsReportedAsNotAcknowledged) {
  TestableManagementComponent component;
  MockRadio radio;
  setup_component(component, radio);
  test::set_kind(*component.get_device("ABC123"), DeviceType::ELECTRICAL_HEATER, 1);
  const auto set = component.management_actions_.heating_control("ABC123", "set_temperature", "21.0");
  EXPECT_FALSE(set.success);
  EXPECT_EQ(set.message, "device did not acknowledge the set_temperature command");
  const auto read = component.management_actions_.heating_control("ABC123", "window_open", "");
  EXPECT_FALSE(read.success);
  EXPECT_EQ(read.message, "device did not acknowledge the window_open command");
}
