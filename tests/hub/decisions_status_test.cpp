#include "decisions_status.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;
using namespace test;

// ============================================================================
// Status rules: execution state, rain limitation, stopped reports, overheard commands, heater model check and EXECUTE
// result codes.
// ============================================================================
// Pure helpers in decisions_status.h, tested without radio or timing.

// ========================================================================================
// Execution state (byte 0 of a status reply)
// ========================================================================================

TEST(Decisions, WaitingForItsTurnWaitingForPowerAndExecutingAreMoving) {
  EXPECT_TRUE(decisions::is_executing(ExecutionState::WAITING_FOR_EXECUTING));
  EXPECT_TRUE(decisions::is_executing(ExecutionState::WAITING_FOR_POWER));
  EXPECT_TRUE(decisions::is_executing(ExecutionState::EXECUTING));
  EXPECT_FALSE(decisions::is_executing(ExecutionState::NON_EXECUTING));
  EXPECT_FALSE(decisions::is_executing(ExecutionState::EXECUTION_ERROR));
  EXPECT_FALSE(decisions::is_executing(ExecutionState::DONE));
  EXPECT_FALSE(decisions::is_executing(ExecutionState::UNDEFINED));
}

TEST(Decisions, StatusByteReadsAsStoppedByItsStateForEveryOfTheEightValues) {
  // Result code 0 and result code 5 (as an SML answers) over each state.
  for (const uint8_t code_bits : {0x00, 0x28}) {
    EXPECT_TRUE(decisions::reads_stopped_by_status_byte(code_bits | 0)) << "state 0: unable to execute, at rest";
    EXPECT_TRUE(decisions::reads_stopped_by_status_byte(code_bits | 1)) << "state 1: error, at rest";
    EXPECT_FALSE(decisions::reads_stopped_by_status_byte(code_bits | 2)) << "state 2: waiting for its turn";
    EXPECT_FALSE(decisions::reads_stopped_by_status_byte(code_bits | 3)) << "state 3: waiting for power";
    EXPECT_FALSE(decisions::reads_stopped_by_status_byte(code_bits | 4)) << "state 4: executing";
    EXPECT_TRUE(decisions::reads_stopped_by_status_byte(code_bits | 5)) << "state 5: done";
    EXPECT_FALSE(decisions::reads_stopped_by_status_byte(code_bits | 6)) << "state 6: undefined, bit 0 clear";
    EXPECT_TRUE(decisions::reads_stopped_by_status_byte(code_bits | 7)) << "state 7: undefined, bit 0 set";
  }
}

TEST(Decisions, StateThreeDisagreesWithTheStoppedBitItReplaced) {
  // Why the state is read: waiting for power has bit 0 set, which read as at rest.
  EXPECT_NE(decisions::reads_stopped_by_status_byte(0x03), (0x03 & STATUS_STOPPED) != 0);
  EXPECT_NE(decisions::reads_stopped_by_status_byte(0x00), (0x00 & STATUS_STOPPED) != 0)
      << "and state 0 has it clear, which read as moving";
  EXPECT_EQ(decisions::reads_stopped_by_status_byte(0x04), (0x04 & STATUS_STOPPED) != 0) << "4 and 5 agree";
  EXPECT_EQ(decisions::reads_stopped_by_status_byte(0x05), (0x05 & STATUS_STOPPED) != 0);
}

// ========================================================================================
// Rain limitation (derived from observed status)
// ========================================================================================

namespace {
decisions::RainLimitationInput rain_input() {
  return {/*has_last_command=*/true,
          /*originator=*/ORIGINATOR_USER_REMOTE,
          /*rain_evidence_recent=*/false,
          /*predicted_target=*/0.0F,
          /*observed_target=*/93.0F,
          /*stopped=*/true};
}
}  // namespace

TEST(Decisions, RainLimitedByRainOriginatorWhetherMovingOrStopped) {
  auto in = rain_input();
  in.originator = ORIGINATOR_RAIN_SENSOR;
  in.stopped = false;
  EXPECT_EQ(decisions::rain_limitation_rule(in), decisions::RainLimitationRule::RAIN_ORIGINATOR);
  in.stopped = true;
  EXPECT_TRUE(decisions::is_rain_limited(in));
}

TEST(Decisions, RainOriginatorNeedsAWellFormedRecord) {
  auto in = rain_input();
  in.originator = ORIGINATOR_RAIN_SENSOR;
  in.has_last_command = false;
  EXPECT_FALSE(decisions::is_rain_limited(in));
}

TEST(Decisions, RainClampedCommandWithFreshEvidenceIsLimited) {
  auto in = rain_input();
  in.rain_evidence_recent = true;
  EXPECT_EQ(decisions::rain_limitation_rule(in), decisions::RainLimitationRule::CLAMPED_COMMAND);
}

TEST(Decisions, RainClampNotLabelledWithoutRecentEvidence) {
  auto in = rain_input();
  in.rain_evidence_recent = false;
  EXPECT_FALSE(decisions::is_rain_limited(in));
}

TEST(Decisions, RainClampNotLabelledWhileMoving) {
  auto in = rain_input();
  in.rain_evidence_recent = true;
  in.stopped = false;
  EXPECT_FALSE(decisions::is_rain_limited(in));
}

TEST(Decisions, RainClampNeedsBothTargetsKnown) {
  auto in = rain_input();
  in.rain_evidence_recent = true;
  in.predicted_target = UNKNOWN_POSITION;
  EXPECT_FALSE(decisions::is_rain_limited(in));
  in = rain_input();
  in.rain_evidence_recent = true;
  in.observed_target = UNKNOWN_POSITION;
  EXPECT_FALSE(decisions::is_rain_limited(in));
}

TEST(Decisions, RainClampWithinToleranceIsNotAClamp) {
  auto in = rain_input();
  in.rain_evidence_recent = true;
  in.observed_target = in.predicted_target;  // reached exactly what was asked
  EXPECT_FALSE(decisions::is_rain_limited(in));
}

TEST(Decisions, RainLimitationOtherOriginatorsAreNotRain) {
  auto in = rain_input();
  for (uint8_t originator : {ORIGINATOR_USER_REMOTE, ORIGINATOR_WIND_SENSOR}) {
    in.originator = originator;
    EXPECT_FALSE(decisions::is_rain_limited(in)) << "originator " << static_cast<int>(originator);
  }
}

TEST(Decisions, RainLimitationRuleNames) {
  EXPECT_STREQ(decisions::rain_limitation_rule_name(decisions::RainLimitationRule::NONE), "none");
  EXPECT_STREQ(decisions::rain_limitation_rule_name(decisions::RainLimitationRule::RAIN_ORIGINATOR), "rain_originator");
  EXPECT_STREQ(decisions::rain_limitation_rule_name(decisions::RainLimitationRule::CLAMPED_COMMAND), "clamped_command");
}

// ========================================================================================
// Rain state from the minimum limitation
// ========================================================================================

TEST(Decisions, RainThresholdIsEightyNinePercentInRawUnits) {
  EXPECT_EQ(decisions::RAIN_MINIMUM_LIMIT_RAW, 0xB200);
  EXPECT_EQ(decisions::RAIN_MINIMUM_LIMIT_RAW * 100 / STATUS_POS_MAX, decisions::RAIN_MINIMUM_LIMIT_PERCENT);
}

TEST(Decisions, RainStateFromMinimumLimitValue) {
  struct Case {
    uint16_t value_raw;
    RainSensorState expected;
  };
  const Case cases[] = {
      {0x0000, RainSensorState::DRY},      // dry reply heard from a real window
      {0xB1FF, RainSensorState::DRY},      // one raw unit below the threshold
      {0xB200, RainSensorState::RAIN},     // 89 %, the boundary
      {0xBA00, RainSensorState::RAIN},     // 91 %
      {0xC800, RainSensorState::RAIN},     // 100 %
      {0xC801, RainSensorState::UNKNOWN},  // not a percentage
      {0xD400, RainSensorState::UNKNOWN},  // "unknown position" selector
      {0xF7FF, RainSensorState::UNKNOWN},
  };
  for (const auto &c : cases) {
    EXPECT_EQ(decisions::rain_state_from_minimum_limit(c.value_raw), c.expected)
        << "value 0x" << std::hex << c.value_raw;
  }
}

TEST(Decisions, RainStateFromLimitationReplyUsesOnlyTheMainParameterValue) {
  LimitationStatus status;
  status.value_raw = 0xBA00;
  status.originator = 0x02;
  status.time_raw = 0x1D;
  EXPECT_EQ(decisions::rain_state_from_limitation_reply(status), RainSensorState::RAIN);

  status.originator = 0x00;  // originator and remaining time do not change the decision
  status.time_raw = 0x00;
  EXPECT_EQ(decisions::rain_state_from_limitation_reply(status), RainSensorState::RAIN);

  status.value_raw = 0x0000;
  EXPECT_EQ(decisions::rain_state_from_limitation_reply(status), RainSensorState::DRY);

  status.value_raw = 0xBA00;
  status.parameter_id = LIMITATION_PARAM_FP_FIRST;  // a functional parameter is not the window's limit
  EXPECT_EQ(decisions::rain_state_from_limitation_reply(status), RainSensorState::UNKNOWN);
}

// ========================================================================================
// Stopped reports (issue #151)
// ========================================================================================

namespace {

/// A report at 30 % with target 0 %, far enough off to need confirming.
decisions::StoppedReportInput off_target_report(StopCandidate candidate, bool follow_up_possible = true) {
  return {true, 0.0f, 30.0f, candidate, follow_up_possible};
}

StopCandidate candidate_at(float target, float position) {
  StopCandidate c;
  c.target = target;
  c.position = position;
  return c;
}

}  // namespace

TEST(Decisions, StoppedReportMovingFlagIsMovingWhateverThePositions) {
  EXPECT_EQ(decisions::classify_stopped_report({false, 0.0f, 0.0f, candidate_at(0.0f, 0.0f), true}),
            decisions::StoppedReport::MOVING);
}

TEST(Decisions, StoppedReportOnTargetIsStopped) {
  EXPECT_EQ(decisions::classify_stopped_report({true, 50.0f, 50.1f, {}, true}), decisions::StoppedReport::STOPPED)
      << "within STATUS_POS_TOLERANCE_RAW counts as on target";
}

TEST(Decisions, StoppedReportWithoutPositionsToCompareIsStopped) {
  EXPECT_EQ(decisions::classify_stopped_report({true, UNKNOWN_POSITION, 30.0f, {}, true}),
            decisions::StoppedReport::STOPPED);
  EXPECT_EQ(decisions::classify_stopped_report({true, 30.0f, UNKNOWN_POSITION, {}, true}),
            decisions::StoppedReport::STOPPED);
}

TEST(Decisions, FirstOffTargetStoppedReportSettles) {
  EXPECT_EQ(decisions::classify_stopped_report(off_target_report({})), decisions::StoppedReport::SETTLING);
}

TEST(Decisions, RepeatedOffTargetStoppedReportIsConfirmed) {
  EXPECT_EQ(decisions::classify_stopped_report(off_target_report(candidate_at(0.0f, 30.0f))),
            decisions::StoppedReport::STOPPED_CONFIRMED);
}

TEST(Decisions, OffTargetStoppedReportAtAnotherPositionDoesNotConfirm) {
  EXPECT_EQ(decisions::classify_stopped_report(off_target_report(candidate_at(0.0f, 40.0f))),
            decisions::StoppedReport::SETTLING)
      << "a device that moved between the two reports was not at rest";
}

TEST(Decisions, OffTargetStoppedReportWithANewTargetDoesNotConfirm) {
  // At rest off target at 30 % (target 100 %), then commanded to 0 %: a motor that has not started
  // yet answers "stopped, target 0 %, current 30 %". The old report must not confirm it.
  EXPECT_EQ(decisions::classify_stopped_report(off_target_report(candidate_at(100.0f, 30.0f))),
            decisions::StoppedReport::SETTLING);
}

TEST(Decisions, OffTargetStoppedReportWithNoFollowUpPollIsTakenAsAtRest) {
  EXPECT_EQ(decisions::classify_stopped_report(off_target_report({}, /*follow_up_possible=*/false)),
            decisions::StoppedReport::STOPPED_UNCONFIRMABLE);
}

TEST(Decisions, StoppedReportVerdictsMapToRestAndCandidate) {
  using decisions::StoppedReport;
  EXPECT_FALSE(decisions::stopped_report_at_rest(StoppedReport::MOVING));
  EXPECT_FALSE(decisions::stopped_report_at_rest(StoppedReport::SETTLING));
  EXPECT_TRUE(decisions::stopped_report_at_rest(StoppedReport::STOPPED));
  EXPECT_TRUE(decisions::stopped_report_at_rest(StoppedReport::STOPPED_CONFIRMED));
  EXPECT_TRUE(decisions::stopped_report_at_rest(StoppedReport::STOPPED_UNCONFIRMABLE));

  EXPECT_FALSE(decisions::stopped_report_keeps_candidate(StoppedReport::MOVING));
  EXPECT_FALSE(decisions::stopped_report_keeps_candidate(StoppedReport::STOPPED));
  EXPECT_TRUE(decisions::stopped_report_keeps_candidate(StoppedReport::SETTLING));
  EXPECT_TRUE(decisions::stopped_report_keeps_candidate(StoppedReport::STOPPED_CONFIRMED))
      << "kept so a later poll at the same place confirms at once";
  EXPECT_TRUE(decisions::stopped_report_keeps_candidate(StoppedReport::STOPPED_UNCONFIRMABLE));
}

TEST(Decisions, StoppedReportOfADeviceWithoutMotionIsStopped) {
  decisions::StoppedReportInput in = off_target_report({});
  in.has_motion = false;
  EXPECT_EQ(decisions::classify_stopped_report(in), decisions::StoppedReport::STOPPED)
      << "a heater's setpoint/reading gap is no movement";
}

TEST(Decisions, OverheardCommandWritesOnlyForTheWriteCommands) {
  for (int cmd = 0; cmd <= 0xFF; ++cmd) {
    const bool expected = cmd == CMD_EXECUTE || cmd == CMD_ACTIVATE_MODE || cmd == CMD_WRITE_PRIVATE;
    EXPECT_EQ(decisions::overheard_command_writes(static_cast<uint8_t>(cmd)), expected) << "cmd " << cmd;
  }
}

TEST(Decisions, FrameIssueIsAWarningOnlyWhenAddressedToADeviceOrToUs) {
  // dst is the hub: a real problem whoever the source is.
  EXPECT_TRUE(decisions::frame_issue_is_warning(false, true));
  // dst is a registered device.
  EXPECT_TRUE(decisions::frame_issue_is_warning(true, false));
  // dst is neither (a broadcast such as 00003F, or another controller), even from a registered
  // source: overheard traffic, DEBUG.
  EXPECT_FALSE(decisions::frame_issue_is_warning(false, false));
}

TEST(Decisions, StatusUpdateAckChannelOrderPutsTheReceiveChannelFirst) {
  using Order = std::array<uint32_t, 3>;
  EXPECT_EQ(decisions::status_update_ack_channel_order(FREQ_CH3), (Order{FREQ_CH3, FREQ_CH1, FREQ_CH2}));
  EXPECT_EQ(decisions::status_update_ack_channel_order(FREQ_CH2), (Order{FREQ_CH2, FREQ_CH1, FREQ_CH3}));
  EXPECT_EQ(decisions::status_update_ack_channel_order(FREQ_CH1), (Order{FREQ_CH1, FREQ_CH2, FREQ_CH3}));
  EXPECT_EQ(decisions::status_update_ack_channel_order(0), (Order{FREQ_CH1, FREQ_CH2, FREQ_CH3}));
}

// === Heater model check ===

TEST(Decisions, HeaterModelHintFlagsOnlyADeclarationTheTowelModeRegisterContradicts) {
  using decisions::HeaterModelHint;
  EXPECT_EQ(decisions::classify_heater_model_hint(DeviceModel::TOWEL_DRYER, false),
            HeaterModelHint::LOOKS_LIKE_RADIATOR);
  EXPECT_EQ(decisions::classify_heater_model_hint(DeviceModel::NONE, true), HeaterModelHint::LOOKS_LIKE_TOWEL_DRYER);
  EXPECT_EQ(decisions::classify_heater_model_hint(DeviceModel::TOWEL_DRYER, true), HeaterModelHint::NONE);
  EXPECT_EQ(decisions::classify_heater_model_hint(DeviceModel::NONE, false), HeaterModelHint::NONE);
  // A heater with a night light is a radiator as far as the towel-dryer register goes.
  EXPECT_EQ(decisions::classify_heater_model_hint(DeviceModel::HEATER_WITH_LIGHT, true),
            HeaterModelHint::LOOKS_LIKE_TOWEL_DRYER);
  EXPECT_EQ(decisions::classify_heater_model_hint(DeviceModel::HEATER_WITH_LIGHT, false), HeaterModelHint::NONE);
}

TEST(Decisions, HeaterModelNameIsOneReadableLine) {
  const uint8_t adelis[] = {'A', 'D', 'E', 'L', 'I', 'S', ' ', '2', ' ', 'D', 'I', 'G', 'I', 'T'};
  EXPECT_EQ(decisions::format_heater_model_name(adelis, sizeof(adelis)), "ADELIS 2 DIGIT");
  const uint8_t padded[] = {'A', 'B', ' ', ' ', 0x00, 'Z'};
  EXPECT_EQ(decisions::format_heater_model_name(padded, sizeof(padded)), "AB") << "stops at NUL, trims spaces";
  const uint8_t garbled[] = {'A', 0x01, 0xC3, 'B'};
  EXPECT_EQ(decisions::format_heater_model_name(garbled, sizeof(garbled)), "A??B");
  EXPECT_EQ(decisions::format_heater_model_name(adelis, 0), "");
}

// ========================================================================================
// Result code of an EXECUTE reply
// ========================================================================================

TEST(Decisions, AnExecuteReplyWithCodeZeroIsNotReported) {
  for (const uint8_t byte : {0x04, 0x05}) {
    EXPECT_EQ(decisions::classify_execute_result_code(byte, true), decisions::ExecuteResultCodeReport::NONE);
    EXPECT_EQ(decisions::classify_execute_result_code(byte, false), decisions::ExecuteResultCodeReport::NONE);
  }
}

TEST(Decisions, CodeFiveAfterTheSpeedBlockSaysTheDeviceHasNoSpeedSetting) {
  EXPECT_EQ(decisions::classify_execute_result_code(0x2C, true),
            decisions::ExecuteResultCodeReport::SPEED_BLOCK_UNKNOWN);
  EXPECT_EQ(decisions::classify_execute_result_code(0x2D, true),
            decisions::ExecuteResultCodeReport::SPEED_BLOCK_UNKNOWN);
}

TEST(Decisions, CodeFiveAfterTheShortFrameIsNotBlamedOnTheSpeedBlock) {
  EXPECT_EQ(decisions::classify_execute_result_code(0x2C, false), decisions::ExecuteResultCodeReport::OTHER_CODE);
}

TEST(Decisions, EveryOtherNonZeroCodeIsReported) {
  for (const uint8_t byte : {0x48, 0x50, 0x08, 0xFC}) {
    EXPECT_EQ(decisions::classify_execute_result_code(byte, true), decisions::ExecuteResultCodeReport::OTHER_CODE)
        << static_cast<unsigned>(byte);
  }
}
