/// @file corpus_execution_state_test.cpp
/// @brief The execution-state decode applied to the status replies of the corpus.
///
/// The hub reads byte 0 of a status reply as an execution state (decisions::reads_stopped_by_status_byte()).
/// Every captured ordinary status reply carries a state that the stopped bit it replaced reads the same
/// way (4 or 5 under a result code). The main-parameter replies of a VELUX SML roller shutter that waits
/// for power on a shared supply are the one place the two disagree: state 3 has the bit set.

#include "corpus_generated.h"
#include "corpus_test_helpers.h"
#include "hub_decisions.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_device_model.h"
#include "proto_frame.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

using namespace esphome::home_io_control;

namespace {

/// Captures that hold main-parameter status reads (CMD_PRIVATE function 0x01). Their replies never
/// reach the hub's status decoder, and a shutter that waits for power answers them with state 3, which
/// the stopped bit would read as at rest. A capture of an ordinary status reply whose byte 0 the two
/// readings disagree on is not listed here on purpose: it changes a decision and needs its own test.
constexpr const char *MAIN_PARAMETER_READ_CAPTURES[] = {
    "velux_sml_probe_status_mp_fp_waiting_for_power",
    "velux_sml_probe_execution_state_waiting_then_executing_sx1262",
};

bool is_main_parameter_read_capture(const char *id) {
  for (const char *listed : MAIN_PARAMETER_READ_CAPTURES) {
    if (std::strcmp(listed, id) == 0)
      return true;
  }
  return false;
}

/// Parse a capture frame and read its execution state from byte 0 of the payload.
ExecutionState execution_state_of_frame(const corpus::CorpusCapture *capture, uint8_t index) {
  EXPECT_NE(capture, nullptr);
  if (capture == nullptr || index >= capture->frame_count)
    return ExecutionState::UNDEFINED;
  const IoFrame frame = corpus_test::parse_capture_frame(capture->frames[index]);
  EXPECT_EQ(frame.cmd, CMD_PRIVATE_RESP);
  EXPECT_GE(frame.data_len, 1);
  return execution_state_of(frame.data[0]);
}

}  // namespace

TEST(CorpusExecutionState, EveryCapturedStatusReplyReadsAsItDidBeforeTheStateWasDecoded) {
  size_t replies = 0;
  for (const corpus::CorpusCapture *capture : corpus_test::all_captures()) {
    if (is_main_parameter_read_capture(capture->id))
      continue;
    for (uint8_t i = 0; i < capture->frame_count; i++) {
      const corpus::CorpusFrame &cf = capture->frames[i];
      if (cf.tx || !cf.has_cmd || (cf.cmd != CMD_PRIVATE_RESP && cf.cmd != CMD_STATUS_UPDATE))
        continue;
      const IoFrame frame = corpus_test::parse_capture_frame(cf);
      if (frame.data_len == 0)
        continue;
      replies++;
      const uint8_t status_byte = frame.data[0];
      const bool stopped_bit = (status_byte & STATUS_STOPPED) != 0;
      EXPECT_EQ(decisions::reads_stopped_by_status_byte(status_byte), stopped_bit)
          << capture->id << " frame " << static_cast<unsigned>(i) << ": byte 0 = 0x" << std::hex
          << static_cast<unsigned>(status_byte);
    }
  }
  EXPECT_GT(replies, 50u) << "the sweep found too few status replies to mean anything";
}

TEST(CorpusExecutionState, AShutterAtRestReadsDoneAndAQueuedOneWaitingForPower) {
  const corpus::CorpusCapture *capture = corpus_test::capture_by_id("velux_sml_probe_status_mp_fp_waiting_for_power");
  ASSERT_NE(capture, nullptr);
  EXPECT_EQ(execution_state_of_frame(capture, 0), ExecutionState::DONE);
  EXPECT_EQ(execution_state_of_frame(capture, 1), ExecutionState::WAITING_FOR_POWER);
  const IoFrame at_rest = corpus_test::parse_capture_frame(capture->frames[0]);
  const IoFrame queued = corpus_test::parse_capture_frame(capture->frames[1]);
  EXPECT_TRUE(decisions::reads_stopped_by_status_byte(at_rest.data[0]));
  EXPECT_FALSE(decisions::reads_stopped_by_status_byte(queued.data[0]))
      << "a waiting shutter is followed as moving although the stopped bit of 0x2B is set";
}

TEST(CorpusExecutionState, AQueuedShutterReadsWaitingForPowerThenExecuting) {
  const corpus::CorpusCapture *capture =
      corpus_test::capture_by_id("velux_sml_probe_execution_state_waiting_then_executing_sx1262");
  ASSERT_NE(capture, nullptr);
  EXPECT_EQ(execution_state_of_frame(capture, 0), ExecutionState::WAITING_FOR_POWER);
  EXPECT_EQ(execution_state_of_frame(capture, 1), ExecutionState::EXECUTING);
}

// === Result code of the answer to an EXECUTE ===

namespace {

/// How many answers to an EXECUTE of each shape the corpus holds, by the result code they carry.
struct ExecuteAnswerTally {
  size_t with_speed_block{0};
  size_t with_speed_block_code_five{0};
  size_t without_speed_block{0};
  size_t without_speed_block_code_zero{0};
};

/// Pair every EXECUTE of a real device capture with the CMD_PRIVATE_RESP that answers it (the next one
/// before the following EXECUTE) and count them by frame shape and result code. Synthetic captures are
/// hand-written and say nothing about a device.
ExecuteAnswerTally tally_execute_answers() {
  ExecuteAnswerTally tally;
  for (const corpus::CorpusCapture *capture : corpus_test::all_captures()) {
    if (std::strncmp(capture->id, "synthetic_", 10) == 0)
      continue;
    const corpus::CorpusFrame *execute = nullptr;
    for (uint8_t i = 0; i < capture->frame_count; i++) {
      const corpus::CorpusFrame &cf = capture->frames[i];
      if (!cf.has_cmd)
        continue;
      if (cf.tx && cf.cmd == CMD_EXECUTE) {
        execute = &cf;
        continue;
      }
      if (cf.tx || cf.cmd != CMD_PRIVATE_RESP || execute == nullptr)
        continue;
      const IoFrame request = corpus_test::parse_capture_frame(*execute);
      const IoFrame answer = corpus_test::parse_capture_frame(cf);
      execute = nullptr;
      if (answer.data_len == 0)
        continue;
      const uint8_t code = result_code_of(answer.data[0]);
      if (execute_carries_speed_block(request)) {
        tally.with_speed_block++;
        tally.with_speed_block_code_five += code == 5 ? 1 : 0;
      } else {
        tally.without_speed_block++;
        tally.without_speed_block_code_zero += code == 0 ? 1 : 0;
      }
    }
  }
  return tally;
}

}  // namespace

TEST(CorpusResultCode, EveryRealDeviceAnswersTheSpeedBlockWithCodeFive) {
  const ExecuteAnswerTally tally = tally_execute_answers();
  EXPECT_GE(tally.with_speed_block, 20u) << "the sweep found too few answers to mean anything";
  EXPECT_EQ(tally.with_speed_block_code_five, tally.with_speed_block)
      << "the awning, the dimmer on three chips, the roller shutter and the tilt cover all say a parameter is unknown";
}

TEST(CorpusResultCode, EveryRealDeviceAnswersTheShortFrameWithCodeZero) {
  const ExecuteAnswerTally tally = tally_execute_answers();
  EXPECT_GE(tally.without_speed_block, 22u)
      << "the awning, pergola and VELUX captures plus the twelve dimmer runs and the four short awning runs";
  EXPECT_EQ(tally.without_speed_block_code_zero, tally.without_speed_block);
}

namespace {

/// One command of the own-hardware dimmer runs and the position its 6-byte EXECUTE carries: a light's
/// position is 100 minus its brightness.
struct DimmerShortCommand {
  const char *name;
  uint8_t position;
};

constexpr DimmerShortCommand DIMMER_SHORT_COMMANDS[] = {
    {"on", 50},    // light.turn_on without a brightness: the last brightness, 50 %
    {"off", 100},  // brightness 0 %
    {"set_brightness_50", 50},
    {"set_brightness_75", 25},
};

constexpr const char *DIMMER_CHIPS[] = {"sx1276", "sx1262", "lr1121"};

}  // namespace

TEST(CorpusResultCode, TheDimmerRunsSendTheShortFrameAndAreAnsweredWithCodeZero) {
  for (const DimmerShortCommand &command : DIMMER_SHORT_COMMANDS) {
    for (const char *chip : DIMMER_CHIPS) {
      const std::string id = std::string("somfy_izymo_dimmer_exchange_") + command.name + "_short_" + chip;
      SCOPED_TRACE(id);
      const corpus::CorpusCapture *capture = corpus_test::capture_by_id(id.c_str());
      ASSERT_NE(capture, nullptr);

      const IoFrame *request = nullptr;
      IoFrame request_storage{};
      IoFrame answer{};
      bool answered = false;
      for (uint8_t i = 0; i < capture->frame_count; i++) {
        const corpus::CorpusFrame &cf = capture->frames[i];
        if (!cf.has_cmd)
          continue;
        if (cf.tx && cf.cmd == CMD_EXECUTE) {
          request_storage = corpus_test::parse_capture_frame(cf);
          request = &request_storage;
        } else if (!cf.tx && cf.cmd == CMD_PRIVATE_RESP && request != nullptr) {
          answer = corpus_test::parse_capture_frame(cf);
          answered = true;
        }
      }
      ASSERT_NE(request, nullptr) << "no EXECUTE in the capture";
      ASSERT_TRUE(answered) << "no answer in the capture";

      EXPECT_FALSE(execute_carries_speed_block(*request));
      // The hub's own builder for the default form makes the frame the dimmer took, byte for byte.
      IoFrame built{};
      ASSERT_TRUE(create_execute_main_parameter(built, request->src, request->dst, false, command.position));
      uint8_t built_bytes[FRAME_MAX_SIZE];
      uint8_t captured_bytes[FRAME_MAX_SIZE];
      const uint8_t built_len = serialize(built, built_bytes, sizeof(built_bytes));
      const uint8_t captured_len = serialize(*request, captured_bytes, sizeof(captured_bytes));
      ASSERT_GT(built_len, 0u);
      ASSERT_EQ(built_len, captured_len);
      EXPECT_EQ(0, std::memcmp(built_bytes, captured_bytes, built_len));
      ASSERT_GE(answer.data_len, 1u);
      EXPECT_EQ(result_code_of(answer.data[0]), 0)
          << "byte 0 = 0x" << std::hex << static_cast<unsigned>(answer.data[0]);
      EXPECT_TRUE(answer.data[0] == 0x04 || answer.data[0] == 0x05);
    }
  }
}

namespace {

/// One command of the own-hardware awning runs sent in the 6-byte form: the capture id's command name,
/// the IO position its EXECUTE carries (0 = retracted on this awning; stop has none) and the execution
/// state in byte 0 of the answer.
struct AwningShortCommand {
  const char *name;
  bool stop;
  uint8_t position;
  uint8_t answer_byte0;
};

constexpr AwningShortCommand AWNING_SHORT_COMMANDS[] = {
    {"open", false, 100, 0x04},
    {"close", false, 0, 0x04},
    {"set_position_25", false, 25, 0x04},
    {"stop", true, 0, 0x05},
};

}  // namespace

TEST(CorpusResultCode, TheAwningAnswersTheShortFrameWithCodeZeroWhereTheSpeedBlockGotCodeFive) {
  for (const AwningShortCommand &command : AWNING_SHORT_COMMANDS) {
    const std::string id = std::string("somfy_awning_exchange_") + command.name + "_short_sx1262";
    SCOPED_TRACE(id);
    const corpus::CorpusCapture *capture = corpus_test::capture_by_id(id.c_str());
    ASSERT_NE(capture, nullptr);

    IoFrame request{};
    IoFrame answer{};
    bool has_request = false;
    bool answered = false;
    for (uint8_t i = 0; i < capture->frame_count; i++) {
      const corpus::CorpusFrame &cf = capture->frames[i];
      if (!cf.has_cmd)
        continue;
      if (cf.tx && cf.cmd == CMD_EXECUTE) {
        request = corpus_test::parse_capture_frame(cf);
        has_request = true;
      } else if (!cf.tx && cf.cmd == CMD_PRIVATE_RESP && has_request) {
        answer = corpus_test::parse_capture_frame(cf);
        answered = true;
      }
    }
    ASSERT_TRUE(has_request) << "no EXECUTE in the capture";
    ASSERT_TRUE(answered) << "no answer in the capture";

    EXPECT_FALSE(execute_carries_speed_block(request));
    // The hub's own builders for the default form make the frame the awning took, byte for byte.
    IoFrame built{};
    if (command.stop)
      ASSERT_TRUE(create_execute_command(built, request.src, request.dst, false, CoverCommand::STOP));
    else
      ASSERT_TRUE(create_execute_main_parameter(built, request.src, request.dst, false, command.position));
    uint8_t built_bytes[FRAME_MAX_SIZE];
    uint8_t captured_bytes[FRAME_MAX_SIZE];
    const uint8_t built_len = serialize(built, built_bytes, sizeof(built_bytes));
    const uint8_t captured_len = serialize(request, captured_bytes, sizeof(captured_bytes));
    ASSERT_GT(built_len, 0u);
    ASSERT_EQ(built_len, captured_len);
    EXPECT_EQ(0, std::memcmp(built_bytes, captured_bytes, built_len));
    ASSERT_GE(answer.data_len, 1u);
    EXPECT_EQ(answer.data[0], command.answer_byte0);
    EXPECT_EQ(result_code_of(answer.data[0]), 0);
  }
}
