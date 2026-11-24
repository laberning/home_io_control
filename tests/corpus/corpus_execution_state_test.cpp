/// @file corpus_execution_state_test.cpp
/// @brief The execution-state decode applied to the status replies of the corpus.
///
/// The hub reads byte 0 of a status reply as an execution state (decisions::reads_stopped_by_status_byte()).
/// Every captured ordinary status reply carries a state that the stopped bit it replaced reads the same
/// way (4 or 5 under a result code), except the replies of a VELUX SML roller shutter that waits for power
/// on a shared supply: state 3 has the bit set. Those captures are replayed through the hub here.

#include "corpus_generated.h"
#include "corpus_test_helpers.h"
#include "decisions_status.h"
#include "hub_core.h"
#include "hub_internal.h"
#include "platform_companion_sensors.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_device_model.h"
#include "proto_frame.h"
#include "esphome/core/application.h"

#include "test_helpers.h"

#include <gtest/gtest.h>

#include <cstring>
#include <iterator>
#include <string>
#include <vector>

using namespace esphome::home_io_control;

namespace {

/// Captures of a VELUX SML shutter whose byte 0 holds state 3 (waiting for power), which the stopped bit
/// would read as at rest: two of main-parameter status reads (CMD_PRIVATE function 0x01, never decoded
/// by the hub), and the acknowledgement and ordinary status reads replayed through the hub below.
constexpr const char *WAITING_STATE_CAPTURES[] = {
    "velux_sml_probe_status_mp_fp_waiting_for_power",
    "velux_sml_probe_execution_state_waiting_then_executing_sx1262",
    "velux_sml_exchange_execute_ack_waiting_for_power_sx1262",
    "velux_sml_statuspoll_queued_by_hub_sx1262",
    "velux_sml_statuspoll_queued_by_remote_sx1262",
};

bool is_waiting_state_capture(const char *id) {
  for (const char *listed : WAITING_STATE_CAPTURES) {
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
    if (is_waiting_state_capture(capture->id))
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

// === The queued VELUX SML through the hub (issue #230) ===

namespace {

constexpr const char *QUEUED_SHUTTER_ID = "FB4288";

/// A hub with the queued shutter registered and its Active Issue sensor; feeds the device replies of a
/// capture and reports what the hub made of each.
struct QueuedShutterReplay {
  esphome::test_clock::ManualClock clock{50000};
  test::TestableHubComponent comp;
  IOHomeActiveIssueTextSensor sensor;
  IoDevice *dev{nullptr};

  QueuedShutterReplay() {
    comp.add_device(QUEUED_SHUTTER_ID);
    dev = comp.get_device(QUEUED_SHUTTER_ID);
    sensor.set_parent(&comp);
    sensor.set_device_id(QUEUED_SHUTTER_ID);
    sensor.setup();
  }

  /// Feed frame @p index of @p capture, a device reply, at its captured time. @p command_ack is true for the
  /// acknowledgement of the hub's own command, whose position bytes the hub does not trust.
  void feed(const corpus::CorpusCapture &capture, uint8_t index, bool command_ack) {
    const corpus::CorpusFrame &cf = capture.frames[index];
    ASSERT_FALSE(cf.tx);
    const uint32_t at = 50000 + cf.t_ms;
    if (static_cast<int32_t>(at - esphome::millis()) > 0)
      esphome::test_clock::advance_ms(at - esphome::millis());
    comp.poll_policy_.set_next_update(QUEUED_SHUTTER_ID, 0);  // the scheduler has taken the read that was due
    comp.update_device_status_(corpus_test::parse_capture_frame(cf), !command_ack);
    comp.notify_device_update_(QUEUED_SHUTTER_ID);
  }

  /// Delay from now to the next status read the hub planned.
  [[nodiscard]] uint32_t planned_read_in_ms() const {
    return comp.poll_policy_.get_next_update(QUEUED_SHUTTER_ID) - esphome::millis();
  }
};

/// Indices of the device replies (CMD_PRIVATE_RESP) in a capture.
std::vector<uint8_t> reply_indices(const corpus::CorpusCapture &capture) {
  std::vector<uint8_t> indices;
  for (uint8_t i = 0; i < capture.frame_count; i++) {
    if (!capture.frames[i].tx && capture.frames[i].has_cmd && capture.frames[i].cmd == CMD_PRIVATE_RESP)
      indices.push_back(i);
  }
  return indices;
}

}  // namespace

TEST(CorpusQueuedShutter, TheAckOfAHubCommandReadsAsMovingAndRaisesNoIssue) {
  const corpus::CorpusCapture *capture =
      corpus_test::capture_by_id("velux_sml_exchange_execute_ack_waiting_for_power_sx1262");
  ASSERT_NE(capture, nullptr);
  const std::vector<uint8_t> replies = reply_indices(*capture);
  ASSERT_EQ(replies.size(), 7u) << "the acknowledgement and six status reads";
  ASSERT_EQ(execution_state_of_frame(capture, replies[0]), ExecutionState::WAITING_FOR_POWER)
      << "what the shutter said";

  QueuedShutterReplay replay;
  replay.feed(*capture, replies[0], /*command_ack=*/true);
  EXPECT_FALSE(replay.dev->is_stopped) << "the command was accepted: the shutter is followed";
  EXPECT_EQ(replay.dev->execution_state, ExecutionState::EXECUTING);
  EXPECT_EQ(replay.sensor.state, "") << "no waiting issue from an acknowledgement";
  EXPECT_EQ(replay.planned_read_in_ms(), DEFAULT_SETTLE_POLL_DELAY_MS) << "the first read follows at the usual gap";

  for (size_t n = 1; n < replies.size(); n++) {
    SCOPED_TRACE("status read " + std::to_string(n));
    replay.feed(*capture, replies[n], /*command_ack=*/false);
    EXPECT_EQ(replay.sensor.state, "") << "the shutter runs at once: no waiting issue at any point";
    if (n + 1 < replies.size()) {
      EXPECT_EQ(replay.dev->execution_state, ExecutionState::EXECUTING);
      EXPECT_FALSE(replay.dev->is_stopped);
    } else {
      EXPECT_EQ(replay.dev->execution_state, ExecutionState::DONE);
      EXPECT_TRUE(replay.dev->is_stopped);
      EXPECT_FLOAT_EQ(replay.dev->position, 75.0F);
    }
  }
}

TEST(CorpusQueuedShutter, StatusReadsWhileQueuedByAHubCommandShowTheIssueUntilTheShutterIsDone) {
  const corpus::CorpusCapture *capture = corpus_test::capture_by_id("velux_sml_statuspoll_queued_by_hub_sx1262");
  ASSERT_NE(capture, nullptr);
  const std::vector<uint8_t> replies = reply_indices(*capture);
  ASSERT_EQ(replies.size(), 7u) << "the acknowledgement and six status reads";

  QueuedShutterReplay replay;
  replay.feed(*capture, replies[0], /*command_ack=*/true);
  EXPECT_EQ(replay.dev->execution_state, ExecutionState::EXECUTING);
  EXPECT_EQ(replay.sensor.state, "") << "the acknowledgement's state 3 shows nothing";

  for (size_t n = 1; n < replies.size(); n++) {
    SCOPED_TRACE("status read " + std::to_string(n));
    replay.feed(*capture, replies[n], /*command_ack=*/false);
    if (n + 1 < replies.size()) {
      EXPECT_EQ(replay.dev->execution_state, ExecutionState::WAITING_FOR_POWER);
      EXPECT_FALSE(replay.dev->is_stopped);
      EXPECT_EQ(replay.sensor.state, "NODE_WAITING_FOR_POWER") << "from the first status read on";
      EXPECT_EQ(replay.planned_read_in_ms(), DEFAULT_SETTLE_POLL_DELAY_MS)
          << "a count held at 5 s while it waits does not back the reads off";
    } else {
      EXPECT_EQ(replay.dev->execution_state, ExecutionState::DONE);
      EXPECT_TRUE(replay.dev->is_stopped);
      EXPECT_FLOAT_EQ(replay.dev->position, 100.0F);
      EXPECT_EQ(replay.sensor.state, "");
    }
  }
}

TEST(CorpusQueuedShutter, StatusReadsWhileQueuedByARemotePressShowTheIssue) {
  const corpus::CorpusCapture *capture = corpus_test::capture_by_id("velux_sml_statuspoll_queued_by_remote_sx1262");
  ASSERT_NE(capture, nullptr);
  const std::vector<uint8_t> replies = reply_indices(*capture);
  ASSERT_EQ(replies.size(), 5u) << "the log ends while the shutter still waits";

  QueuedShutterReplay replay;
  for (const uint8_t index : replies) {
    replay.feed(*capture, index, /*command_ack=*/false);
    EXPECT_EQ(replay.dev->execution_state, ExecutionState::WAITING_FOR_POWER);
    EXPECT_FALSE(replay.dev->is_stopped) << "queued behind the other shutter, not at rest";
    EXPECT_EQ(replay.sensor.state, "NODE_WAITING_FOR_POWER");
    EXPECT_EQ(replay.planned_read_in_ms(), DEFAULT_SETTLE_POLL_DELAY_MS);
  }
}

// === Result code of the answer to an EXECUTE ===

namespace {

/// Captures of a device that takes the speed block: a VELUX SML roller shutter behind a KUX 110 answers
/// the 8-byte form with result code 0 (issue #230). Every other device in the corpus answers it with
/// code 5.
constexpr const char *ACCEPTS_SPEED_BLOCK_CAPTURES[] = {
    "velux_sml_exchange_execute_ack_waiting_for_power_sx1262",
    "velux_sml_statuspoll_queued_by_hub_sx1262",
};

bool accepts_speed_block(const char *id) {
  for (const char *listed : ACCEPTS_SPEED_BLOCK_CAPTURES) {
    if (std::strcmp(listed, id) == 0)
      return true;
  }
  return false;
}

/// How many answers to an EXECUTE of each shape the corpus holds, by the result code they carry.
struct ExecuteAnswerTally {
  size_t with_speed_block{0};
  size_t with_speed_block_as_captured{0};  ///< Code 0 from a listed device, code 5 from any other.
  size_t with_speed_block_code_zero{0};
  size_t without_speed_block{0};
  size_t without_speed_block_code_zero{0};
};

/// Call @p on_answer(request, answer) for every EXECUTE of @p capture and the CMD_PRIVATE_RESP with a
/// payload that answers it (the next one before the following EXECUTE).
template<class OnAnswer> void for_each_execute_answer(const corpus::CorpusCapture &capture, OnAnswer on_answer) {
  const corpus::CorpusFrame *execute = nullptr;
  for (uint8_t i = 0; i < capture.frame_count; i++) {
    const corpus::CorpusFrame &cf = capture.frames[i];
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
    if (answer.data_len > 0)
      on_answer(request, answer);
  }
}

/// Count the answers to an EXECUTE in every real 868 MHz device capture by frame shape and result code.
/// Synthetic captures are hand-written and say nothing about a device. An answer to the speed block counts
/// as expected when it carries code 0 in a capture of ACCEPTS_SPEED_BLOCK_CAPTURES and code 5 in any other.
/// The 2.4 GHz captures stay out: the skylight blind answers an EXECUTE with a first byte of 0x54 (code 10,
/// state 4), which no 868 MHz device returns and no capture explains, so the 868 MHz result-code split does
/// not describe it; TheSkylightBlindAnswersEveryExecuteOn2g4WithCodeTen pins that exception.
ExecuteAnswerTally tally_execute_answers() {
  ExecuteAnswerTally tally;
  for (const corpus::CorpusCapture *capture : corpus_test::all_captures()) {
    if (std::strncmp(capture->id, "synthetic_", 10) == 0 || capture->phy == corpus::Phy::IO_2G4)
      continue;
    const uint8_t speed_block_code = accepts_speed_block(capture->id) ? 0 : 5;
    for_each_execute_answer(*capture, [&](const IoFrame &request, const IoFrame &answer) {
      const uint8_t code = result_code_of(answer.data[0]);
      if (execute_carries_speed_block(request)) {
        tally.with_speed_block++;
        tally.with_speed_block_as_captured += code == speed_block_code ? 1 : 0;
        tally.with_speed_block_code_zero += code == 0 ? 1 : 0;
      } else {
        tally.without_speed_block++;
        tally.without_speed_block_code_zero += code == 0 ? 1 : 0;
      }
    });
  }
  return tally;
}

}  // namespace

TEST(CorpusResultCode, EveryRealDeviceButTheVeluxSmlAnswersTheSpeedBlockWithCodeFive) {
  const ExecuteAnswerTally tally = tally_execute_answers();
  EXPECT_GE(tally.with_speed_block, 20u) << "the sweep found too few answers to mean anything";
  EXPECT_EQ(tally.with_speed_block_as_captured, tally.with_speed_block)
      << "the awning, the dimmer on three chips, the VELUX SSL and the tilt cover say a parameter is unknown; "
         "the VELUX SML takes the speed block";
  EXPECT_EQ(tally.with_speed_block_code_zero, std::size(ACCEPTS_SPEED_BLOCK_CAPTURES))
      << "one acknowledgement per listed capture";
}

TEST(CorpusResultCode, EveryRealDeviceAnswersTheShortFrameWithCodeZero) {
  const ExecuteAnswerTally tally = tally_execute_answers();
  EXPECT_GE(tally.without_speed_block, 22u)
      << "the awning, pergola and VELUX captures plus the twelve dimmer runs and the four short awning runs";
  EXPECT_EQ(tally.without_speed_block_code_zero, tally.without_speed_block);
}

/// The answers the 868 MHz sweep leaves out: every answer to an EXECUTE on 2.4 GHz carries code 10 with
/// state 4 (executing). A 2.4 GHz capture with another code makes this fail, so the exclusion above cannot
/// hide it.
TEST(CorpusResultCode, TheSkylightBlindAnswersEveryExecuteOn2g4WithCodeTen) {
  size_t answers = 0;
  for (const corpus::CorpusCapture *capture : corpus_test::all_captures()) {
    if (capture->phy != corpus::Phy::IO_2G4)
      continue;
    SCOPED_TRACE(capture->id);
    for_each_execute_answer(*capture, [&](const IoFrame &, const IoFrame &answer) {
      answers++;
      EXPECT_EQ(result_code_of(answer.data[0]), 10u);
      EXPECT_EQ(execution_state_of(answer.data[0]), ExecutionState::EXECUTING);
    });
  }
  EXPECT_GE(answers, 2u) << "the KLR 200 and the hub's own EXECUTE to the skylight blind";
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
