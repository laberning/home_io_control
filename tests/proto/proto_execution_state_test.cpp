/// @file proto_execution_state_test.cpp
/// @brief The execution state in byte 0 of a status reply: decode and names.

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
