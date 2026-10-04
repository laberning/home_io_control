#include "oneway_controller.h"
#include "platform_oneway_entities.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;

// ============================================================================
// 1W control surface
// ============================================================================
// The button-action mapping and the diagnostic wording. Both are pure on purpose: the host
// ESP_LOG stub discards its arguments, so anything formatted inside a publish or log call could
// not be asserted on at all.

TEST(OneWayControlSurface, OpenAndCloseAreSentAsPositions) {
  // There is no open/close opcode. They are positions 0 and 100, and only look like named
  // commands to a user — pinning that here keeps a future "tidy-up" from inventing a command for
  // them and silently changing what goes on air.
  const OneWayActionEncoding open = encode_oneway_action(OneWayButtonAction::OPEN);
  EXPECT_TRUE(open.is_position) << "OPEN is a position on the wire";
  EXPECT_EQ(open.position, 0) << "0 is fully open";

  const OneWayActionEncoding close = encode_oneway_action(OneWayButtonAction::CLOSE);
  EXPECT_TRUE(close.is_position);
  EXPECT_EQ(close.position, 100) << "100 is fully closed";
}

TEST(OneWayControlSurface, NamedActionsMapToTheirCoverCommands) {
  struct Case {
    OneWayButtonAction action;
    CoverCommand expected;
    const char *name;
  };
  const Case cases[] = {
      {OneWayButtonAction::STOP, CoverCommand::STOP, "STOP"},
      {OneWayButtonAction::VENT, CoverCommand::VENT, "VENT"},
      {OneWayButtonAction::FAVORITE, CoverCommand::FAVORITE, "FAVORITE"},
  };

  for (const auto &c : cases) {
    const OneWayActionEncoding encoding = encode_oneway_action(c.action);
    EXPECT_FALSE(encoding.is_position) << c.name << " is a named command, not a position";
    EXPECT_EQ(encoding.command, c.expected) << c.name << " maps to the wrong CoverCommand";
    EXPECT_STREQ(oneway_button_action_name(c.action), c.name);
  }
}

TEST(OneWayControlSurface, EveryActionNameIsDistinct) {
  // The names reach the diagnostic sensor and the boot log; two actions sharing one would make a
  // misconfigured button indistinguishable from a correct one in the only output that exists.
  const OneWayButtonAction actions[] = {OneWayButtonAction::OPEN, OneWayButtonAction::CLOSE, OneWayButtonAction::STOP,
                                        OneWayButtonAction::VENT, OneWayButtonAction::FAVORITE};
  for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
    for (size_t j = i + 1; j < sizeof(actions) / sizeof(actions[0]); j++) {
      EXPECT_STRNE(oneway_button_action_name(actions[i]), oneway_button_action_name(actions[j]))
          << "actions " << i << " and " << j << " share a name";
    }
  }
}

// ========================================================================================
// Diagnostic wording
// ========================================================================================

TEST(OneWayControlSurface, ReportNamesTheIntentClassAndSequence) {
  // The sequence is in there because it is the one number a stuck user needs: it is what tells
  // them whether initial_sequence: is worth bumping.
  OneWayCommandReport report{};
  report.controller_id = "awning_remote";
  report.intent = "STOP";
  report.target_type = DeviceType::AWNING;
  report.sequence = 1234;
  report.sequence_reserved = true;
  report.transmitted = true;

  const std::string summary = format_oneway_command_report(report);
  EXPECT_NE(summary.find("STOP"), std::string::npos) << "the intent must be visible";
  EXPECT_NE(summary.find("1234"), std::string::npos) << "the sequence used must be visible";
  EXPECT_NE(summary.find(device_type_name(DeviceType::AWNING)), std::string::npos)
      << "the addressed class must be visible; 1W has no device to name instead";
}

TEST(OneWayControlSurface, ReportNeverClaimsTheDeviceActed) {
  // 1W has no reply. Wording that implied success would be a claim the hub cannot support, and
  // it is exactly what would send a user looking at the wrong thing.
  OneWayCommandReport report{};
  report.intent = "CLOSE";
  report.target_type = DeviceType::AWNING;
  report.sequence = 9;
  report.sequence_reserved = true;
  report.transmitted = true;

  const std::string summary = format_oneway_command_report(report);
  for (const char *forbidden : {"success", "applied", "acknowledged", "confirmed", "obeyed"})
    EXPECT_EQ(summary.find(forbidden), std::string::npos) << "the summary must not contain '" << forbidden << "'";
}

TEST(OneWayControlSurface, ReportDistinguishesNotTransmittedFromTransmitted) {
  OneWayCommandReport report{};
  report.intent = "STOP";
  report.target_type = DeviceType::AWNING;
  report.sequence = 5;
  report.sequence_reserved = true;
  report.transmitted = false;

  const std::string sent_nothing = format_oneway_command_report(report);
  report.transmitted = true;
  const std::string sent = format_oneway_command_report(report);

  EXPECT_NE(sent_nothing, sent) << "a command that never left the radio must read differently";
  EXPECT_NE(sent_nothing.find("not sent"), std::string::npos);
}

TEST(OneWayControlSurface, ReportForAnUnreservedSequenceSaysSo) {
  // The failure a user is most likely to hit and least likely to diagnose: the identity resolved
  // but no sequence could be reserved, so nothing was ever built.
  OneWayCommandReport report{};
  report.controller_id = "awning_remote";

  EXPECT_NE(format_oneway_command_report(report).find("no sequence"), std::string::npos)
      << "the report must name the reason, not just report emptiness";
}

TEST(OneWayControlSurface, ReportForAGenuineSequenceZeroDoesNotSayNoSequence) {
  // Regression for the sentinel collision: sequence 0 is a legitimate value (the first command an
  // identity with no configured initial_sequence: ever sends), distinct from "no sequence was
  // reserved at all". A report that reserved sequence 0 but failed to transmit must say so as a
  // real command, not fall back to the "no sequence reserved" wording meant for the other case.
  OneWayCommandReport report{};
  report.controller_id = "awning_remote";
  report.intent = "STOP";
  report.target_type = DeviceType::AWNING;
  report.sequence = 0;
  report.sequence_reserved = true;
  report.transmitted = false;

  const std::string summary = format_oneway_command_report(report);
  EXPECT_EQ(summary.find("no sequence"), std::string::npos)
      << "sequence 0 was genuinely reserved and must not be reported as unreserved";
  EXPECT_NE(summary.find("seq 0"), std::string::npos) << "the reserved sequence must be visible";
  EXPECT_NE(summary.find("not sent"), std::string::npos) << "it did not transmit, and that must still be visible";
}
