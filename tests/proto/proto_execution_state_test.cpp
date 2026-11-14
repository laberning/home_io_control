/// @file proto_execution_state_test.cpp
/// @brief The execution state and the result code in byte 0 of a status reply: decode and names.

#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_device_model.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

TEST(ExecutionState, BitsTwoToZeroOfByteZeroAreTheStateInKlf200Numbering) {
  EXPECT_EQ(execution_state_of(0x00), ExecutionState::NON_EXECUTING);
  EXPECT_EQ(execution_state_of(0x01), ExecutionState::EXECUTION_ERROR);
  EXPECT_EQ(execution_state_of(0x02), ExecutionState::WAITING_FOR_EXECUTING);
  EXPECT_EQ(execution_state_of(0x03), ExecutionState::WAITING_FOR_POWER);
  EXPECT_EQ(execution_state_of(0x04), ExecutionState::EXECUTING);
  EXPECT_EQ(execution_state_of(0x05), ExecutionState::DONE);
}

TEST(ExecutionState, StatesSixAndSevenAreUndefined) {
  EXPECT_EQ(execution_state_of(0x06), ExecutionState::UNDEFINED);
  EXPECT_EQ(execution_state_of(0x07), ExecutionState::UNDEFINED);
}

TEST(ExecutionState, TheResultCodeAboveTheStateIsIgnored) {
  // The SML answers with result code 5: 0x2B, 0x2C and 0x2D.
  EXPECT_EQ(execution_state_of(0x2B), ExecutionState::WAITING_FOR_POWER);
  EXPECT_EQ(execution_state_of(0x2C), ExecutionState::EXECUTING);
  EXPECT_EQ(execution_state_of(0x2D), ExecutionState::DONE);
  EXPECT_EQ(execution_state_of(0xFF), ExecutionState::UNDEFINED);
  EXPECT_EQ(execution_state_of(0xF8), ExecutionState::NON_EXECUTING);
}

TEST(ExecutionState, ItUsesTheSameMaskAsTheErrorResponseState) {
  for (unsigned byte = 0; byte <= 0xFF; byte++) {
    const auto state = execution_state_of(static_cast<uint8_t>(byte));
    const uint8_t masked = error_resp_state(static_cast<uint8_t>(byte));
    if (state == ExecutionState::UNDEFINED)
      EXPECT_GT(masked, 5u) << byte;
    else
      EXPECT_EQ(static_cast<uint8_t>(state), masked) << byte;
  }
}

TEST(ExecutionState, EveryStateHasADistinctName) {
  EXPECT_STREQ(execution_state_name(ExecutionState::NON_EXECUTING), "non_executing");
  EXPECT_STREQ(execution_state_name(ExecutionState::EXECUTION_ERROR), "error");
  EXPECT_STREQ(execution_state_name(ExecutionState::WAITING_FOR_EXECUTING), "waiting_for_executing");
  EXPECT_STREQ(execution_state_name(ExecutionState::WAITING_FOR_POWER), "waiting_for_power");
  EXPECT_STREQ(execution_state_name(ExecutionState::EXECUTING), "executing");
  EXPECT_STREQ(execution_state_name(ExecutionState::DONE), "done");
  EXPECT_STREQ(execution_state_name(ExecutionState::UNDEFINED), "undefined");
}

TEST(ResultCode, BitsSevenToThreeOfByteZeroAreTheResultCode) {
  EXPECT_EQ(result_code_of(0x00), 0);
  EXPECT_EQ(result_code_of(0x04), 0) << "an executing reply with no complaint";
  EXPECT_EQ(result_code_of(0x05), 0) << "a done reply with no complaint";
  EXPECT_EQ(result_code_of(0x2C), 5) << "the answer to the 8-byte EXECUTE: parameter unknown, still executing";
  EXPECT_EQ(result_code_of(0x2D), 5);
  EXPECT_EQ(result_code_of(0x48), 9);
  EXPECT_EQ(result_code_of(0x50), 10);
  EXPECT_EQ(result_code_of(0xFF), 31);
}

TEST(ResultCode, TheNamesAreThoseOfTheSplitByteNotOfTheWholeByte) {
  EXPECT_STREQ(result_code_name(5), "PARAMETER_UNKNOWN");
  EXPECT_STREQ(result_code_name(9), "PARAMETER_LIMITATION_ADJUSTMENT");
  EXPECT_STREQ(result_code_name(10), "PARAMETER_INCOHERENCE_ADJUSTMENT");
  EXPECT_STREQ(result_code_name(0), "UNKNOWN_RESULT_CODE");
  EXPECT_STREQ(result_code_name(ERROR_RESP_CODE_MAX + 1), "UNKNOWN_RESULT_CODE");
  // The whole-byte values 0x05 and 0x09 are other results; they must not be reached through a code.
  EXPECT_STRNE(command_result_name(0x05), result_code_name(5));
  EXPECT_STRNE(command_result_name(0x09), result_code_name(9));
}

TEST(ResultCode, OnlyTheEightByteExecuteCarriesTheSpeedBlock) {
  const uint8_t own[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
  const uint8_t dst[NODE_ID_SIZE] = {0xAB, 0xC1, 0x23};
  IoFrame frame{};
  ASSERT_TRUE(create_execute_position(frame, own, dst, false, 50, false));
  EXPECT_TRUE(execute_carries_speed_block(frame));
  ASSERT_TRUE(create_execute_position(frame, own, dst, false, 50, true));
  EXPECT_TRUE(execute_carries_speed_block(frame)) << "the silent profile is a speed block too";
  ASSERT_TRUE(create_force_open(frame, own, dst, false, 0));
  EXPECT_TRUE(execute_carries_speed_block(frame));

  ASSERT_TRUE(create_execute_main_parameter(frame, own, dst, false, 50));
  EXPECT_FALSE(execute_carries_speed_block(frame));
  ASSERT_TRUE(create_execute_command(frame, own, dst, false, CoverCommand::STOP, false));
  EXPECT_FALSE(execute_carries_speed_block(frame));
  ASSERT_TRUE(create_execute_tilt(frame, own, dst, false, 40));
  EXPECT_FALSE(execute_carries_speed_block(frame)) << "a tilt frame names functional parameter 3, not 1";
  ASSERT_TRUE(create_execute_position_and_tilt(frame, own, dst, false, 50, 40));
  EXPECT_FALSE(execute_carries_speed_block(frame));
  ASSERT_TRUE(create_get_status(frame, own, dst, false));
  EXPECT_FALSE(execute_carries_speed_block(frame)) << "not an EXECUTE";
}
