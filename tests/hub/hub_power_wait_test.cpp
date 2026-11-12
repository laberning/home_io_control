/// @file hub_power_wait_test.cpp
/// @brief Tests for the opt-in `follow_waiting_for_power`: a cover queued on a shared power supply
///        shows as waiting and is followed until it moves.
///
/// Two VELUX SML shutters on one KUX 110 supply run one at a time: the second accepts its command
/// and reports "stopped" off its target until the first has finished. The execution-state replies
/// 2D (done) and 2B (waiting for power) are the ones measured on that shutter
/// (tests/corpus/captures/probe/velux_sml_probe_status_mp_fp_waiting_for_power.yaml); 2C
/// (executing) and 29 (error while executing) are built from the same packing and the state
/// numbers of the KLF 200 API specification.

#include "hub_core.h"
#include "platform_companion_sensors.h"
#include "proto_commands.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <initializer_list>

using namespace esphome::home_io_control;

namespace {

constexpr const char *DEVICE_ID = "ABC123";
constexpr uint8_t DEVICE_NODE_ID[3] = {0xAB, 0xC1, 0x23};
constexpr uint16_t RAW_25 = 0x3200;        ///< 25 % in the status position coding.
constexpr uint16_t RAW_60 = 0x7800;        ///< 60 %.
constexpr uint16_t RAW_75 = 0x9600;        ///< 75 %.
constexpr uint8_t STATE_DONE = 0x2D;       ///< (5 << 3) | 5, measured at rest.
constexpr uint8_t STATE_WAITING = 0x2B;    ///< (5 << 3) | 3, measured while queued.
constexpr uint8_t STATE_EXECUTING = 0x2C;  ///< (5 << 3) | 4, from the specification.
constexpr uint8_t STATE_ERROR = 0x29;      ///< (5 << 3) | 1, from the specification.
constexpr uint8_t FLAGS_MOVING = 0x04;     ///< An ordinary status reply without STATUS_STOPPED.
constexpr uint8_t FLAGS_STOPPED = 0x05;    ///< An ordinary status reply with STATUS_STOPPED.

class TestablePowerWaitComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::poll_policy_;
  using IOHomeControlComponent::process_pending_operation_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::schedule_device_polls_;
  using IOHomeControlComponent::system_key_;
};

/// One hub with a roller shutter "ABC123" resting at 75 %, its Active Issue sensor, and a mock
/// radio. A test opts the shutter in itself, so the fixture also serves the cover without the option.
class PowerWaitHub : public ::testing::Test {
 protected:
  void SetUp() override {
    comp.node_id_[0] = 0xC0;
    comp.node_id_[1] = 0xFF;
    comp.node_id_[2] = 0xEE;
    static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                  0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
    std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
    comp.initialized_ = true;
    comp.radio_ = &radio;
    esphome::test_random::set(0);
    DeviceConfig cfg;
    cfg.type = DeviceType::ROLLER_SHUTTER;
    comp.add_device(DEVICE_ID, cfg);
    ASSERT_NE(comp.get_device(DEVICE_ID), nullptr);
    issue.set_parent(&comp);
    issue.set_device_id(DEVICE_ID);
    issue.setup();
  }

  IoDevice &device() { return *comp.get_device(DEVICE_ID); }

  IoFrame reply(std::initializer_list<uint8_t> payload) const {
    IoFrame frame{};
    init_frame(frame, true, false, true, false);
    set_dst(frame, comp.node_id_);
    set_src(frame, DEVICE_NODE_ID);
    EXPECT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload.begin(), static_cast<uint8_t>(payload.size())));
    return frame;
  }

  /// The reply to the ordinary status read: flags, target, current position.
  IoFrame status_reply(uint8_t flags, uint16_t target, uint16_t current) const {
    return reply({flags, 0x00, static_cast<uint8_t>(target >> 8), static_cast<uint8_t>(target),
                  static_cast<uint8_t>(current >> 8), static_cast<uint8_t>(current), 0x00, 0x00});
  }

  /// The reply to the execution-state read, in the measured layout: status, 00, main parameter
  /// (the current position), FPI1 echo 80, FP1 00 00, FPI2 echo 00.
  IoFrame state_reply(uint8_t status, uint16_t current = RAW_75) const {
    return reply(
        {status, 0x00, static_cast<uint8_t>(current >> 8), static_cast<uint8_t>(current), 0x80, 0x00, 0x00, 0x00});
  }

  void queue(const IoFrame &frame) { radio.queue_rx(test::make_rx_packet(frame)); }

  /// An untracked read that finds the shutter at rest at 75 %.
  void read_at_rest() {
    queue(status_reply(FLAGS_STOPPED, RAW_75, RAW_75));
    ASSERT_TRUE(comp.request_device_status(DEVICE_ID));
    ASSERT_TRUE(device().is_stopped);
    ASSERT_FLOAT_EQ(device().position, 75.0F);
  }

  /// Command the shutter to 25 %; it acknowledges and the follow-up window opens.
  void command_to_25() {
    queue(status_reply(STATE_EXECUTING, RAW_75, RAW_75));
    ASSERT_TRUE(comp.set_device_position(DEVICE_ID, 25));
  }

  /// One follow-up poll answered "stopped at 75 %, target 25 %", then whatever @p then holds.
  bool poll_stopped_off_target(std::initializer_list<IoFrame> then = {}) {
    queue(status_reply(FLAGS_STOPPED, RAW_25, RAW_75));
    for (const IoFrame &frame : then)
      queue(frame);
    return comp.request_device_status(DEVICE_ID);
  }

  /// Rest, command, and the first poll that reports the stop off the target: the shutter settles.
  void command_and_settle() {
    read_at_rest();
    command_to_25();
    ASSERT_TRUE(poll_stopped_off_target());
    ASSERT_FALSE(device().is_stopped) << "the first stop off the target is confirmed by one more poll";
    ASSERT_EQ(state_reads(), 0u);
  }

  /// As command_and_settle(), then the confirming poll and a state read answered "waiting for power".
  void command_and_wait() {
    command_and_settle();
    ASSERT_TRUE(poll_stopped_off_target({state_reply(STATE_WAITING)}));
    ASSERT_EQ(device().power_wait, PowerWait::WAITING);
  }

  /// Let the scheduled poll come due and answer it with @p frames.
  void run_due_poll(uint32_t after_ms, std::initializer_list<IoFrame> frames) {
    esphome::test_clock::advance_ms(after_ms + 1);
    comp.loop();
    ASSERT_EQ(comp.op_queue_.size(), 1u) << "a poll is due";
    for (const IoFrame &frame : frames)
      queue(frame);
    comp.process_pending_operation_();
  }

  /// Execution-state reads sent so far: CMD_PRIVATE function 0x01 with the FP1-FP7 mask.
  size_t state_reads() {
    size_t count = 0;
    for (const auto &sent : radio.get_sent_data()) {
      if (sent.size() > FRAME_CMD_OFFSET + 2 && sent[FRAME_CMD_OFFSET] == CMD_PRIVATE &&
          sent[FRAME_CMD_OFFSET + 1] == 0x01 && sent[FRAME_CMD_OFFSET + 2] == 0xFE)
        count++;
    }
    return count;
  }

  TestablePowerWaitComponent comp;
  MockRadio radio;
  IOHomeActiveIssueTextSensor issue;
};

}  // namespace

// ============================================================================
// Waiting, then moving
// ============================================================================

TEST_F(PowerWaitHub, AQueuedCoverShowsWaitingAndKeepsItsDirection) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_follow_waiting_for_power(DEVICE_ID, true);

  command_and_wait();

  EXPECT_EQ(state_reads(), 1u) << "read once, right after the confirming poll";
  EXPECT_FALSE(effective_is_stopped(device())) << "still handled as moving, so the cover shows its direction";
  EXPECT_FLOAT_EQ(device().position, 75.0F) << "the state read moves neither position";
  EXPECT_FLOAT_EQ(device().target, 25.0F) << "nor target";
  EXPECT_EQ(issue.state, "NODE_WAITING_FOR_POWER");
  EXPECT_EQ(comp.poll_policy_.get_next_update(DEVICE_ID), esphome::millis() + POWER_WAIT_POLL_INTERVAL_MS);
}

TEST_F(PowerWaitHub, AWaitingCoverIsReadSlowlyAndFollowedOnceItMoves) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_follow_waiting_for_power(DEVICE_ID, true);
  command_and_wait();

  run_due_poll(POWER_WAIT_POLL_INTERVAL_MS, {state_reply(STATE_WAITING)});
  EXPECT_EQ(device().power_wait, PowerWait::WAITING);
  EXPECT_EQ(state_reads(), 2u);
  EXPECT_EQ(comp.poll_policy_.get_next_update(DEVICE_ID), esphome::millis() + POWER_WAIT_POLL_INTERVAL_MS);

  run_due_poll(POWER_WAIT_POLL_INTERVAL_MS, {state_reply(STATE_EXECUTING)});
  EXPECT_EQ(device().power_wait, PowerWait::NONE);
  EXPECT_FALSE(effective_is_stopped(device()));
  EXPECT_EQ(issue.state, "");
  EXPECT_EQ(comp.poll_policy_.get_next_update(DEVICE_ID), esphome::millis() + DEFAULT_SETTLE_POLL_DELAY_MS)
      << "the ordinary follow-up polls take over";

  run_due_poll(DEFAULT_SETTLE_POLL_DELAY_MS, {status_reply(FLAGS_MOVING, RAW_25, RAW_60)});
  EXPECT_FALSE(device().is_stopped);
  EXPECT_FLOAT_EQ(device().position, 60.0F);

  run_due_poll(DEFAULT_SETTLE_POLL_DELAY_MS, {status_reply(FLAGS_STOPPED, RAW_25, RAW_25)});
  EXPECT_TRUE(device().is_stopped);
  EXPECT_FLOAT_EQ(device().position, 25.0F);
  EXPECT_EQ(state_reads(), 3u) << "no state read once the cover moves";
}

TEST_F(PowerWaitHub, AnUnansweredReadDuringAWaitKeepsWaiting) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_follow_waiting_for_power(DEVICE_ID, true);
  command_and_wait();

  run_due_poll(POWER_WAIT_POLL_INTERVAL_MS, {});

  EXPECT_EQ(device().power_wait, PowerWait::WAITING);
  EXPECT_FALSE(effective_is_stopped(device()));
  EXPECT_EQ(comp.poll_policy_.get_next_update(DEVICE_ID), esphome::millis() + POWER_WAIT_POLL_INTERVAL_MS);
}

TEST_F(PowerWaitHub, AWaitThatEndedUnseenIsSettledByAnOrdinaryRead) {
  // A short travel can start and finish between two state reads.
  esphome::test_clock::ManualClock clock;
  comp.set_device_follow_waiting_for_power(DEVICE_ID, true);
  command_and_wait();

  run_due_poll(POWER_WAIT_POLL_INTERVAL_MS,
               {state_reply(STATE_DONE, RAW_25), status_reply(FLAGS_STOPPED, RAW_25, RAW_25)});

  EXPECT_EQ(device().power_wait, PowerWait::NONE);
  EXPECT_TRUE(device().is_stopped);
  EXPECT_FLOAT_EQ(device().position, 25.0F) << "the position comes from the ordinary read";
  EXPECT_EQ(issue.state, "");
}

// ============================================================================
// Waiting, then the safety bound
// ============================================================================

TEST_F(PowerWaitHub, AWaitEndsWithTheFollowUpWindow) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_follow_waiting_for_power(DEVICE_ID, true);
  command_and_wait();
  const uint32_t deadline = comp.poll_policy_.get_poll_deadline(DEVICE_ID);
  ASSERT_NE(deadline, 0u);

  // The device answers "waiting for power" for as long as it is asked.
  for (int i = 0; i < 100 && !device().is_stopped; i++) {
    esphome::test_clock::advance_ms(POWER_WAIT_POLL_INTERVAL_MS + 1);
    comp.loop();
    if (comp.op_queue_.empty())
      continue;
    queue(state_reply(STATE_WAITING));
    comp.process_pending_operation_();
  }

  EXPECT_TRUE(device().is_stopped) << "shown at rest once the window has run out";
  EXPECT_EQ(device().power_wait, PowerWait::NONE);
  EXPECT_EQ(issue.state, "");
  EXPECT_GT(esphome::millis(), deadline);
  EXPECT_LE(state_reads(), MAX_TRACKED_STATUS_POLL_WINDOW_MS / POWER_WAIT_POLL_INTERVAL_MS + 1);

  const size_t sent = radio.get_sent_data().size();
  for (int i = 0; i < 10; i++) {
    esphome::test_clock::advance_ms(POWER_WAIT_POLL_INTERVAL_MS + 1);
    comp.loop();
  }
  EXPECT_TRUE(comp.op_queue_.empty());
  EXPECT_EQ(radio.get_sent_data().size(), sent) << "nothing is read after the wait has ended";
}

// ============================================================================
// States that conclude at once
// ============================================================================

TEST_F(PowerWaitHub, AStopOffTheTargetWithStateDoneIsAtRestAtOnce) {
  // A frozen or obstructed shutter: stopped off its target, and not waiting for anything.
  esphome::test_clock::ManualClock clock;
  comp.set_device_follow_waiting_for_power(DEVICE_ID, true);
  command_and_settle();

  ASSERT_TRUE(poll_stopped_off_target({state_reply(STATE_DONE)}));

  EXPECT_TRUE(device().is_stopped);
  EXPECT_EQ(device().power_wait, PowerWait::NONE);
  EXPECT_EQ(state_reads(), 1u);
  EXPECT_EQ(comp.poll_policy_.get_next_update(DEVICE_ID), 0u) << "no further poll";
  EXPECT_EQ(issue.state, "");
}

TEST_F(PowerWaitHub, AnErrorStateIsAtRestAtOnce) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_follow_waiting_for_power(DEVICE_ID, true);
  command_and_settle();

  ASSERT_TRUE(poll_stopped_off_target({state_reply(STATE_ERROR)}));

  EXPECT_TRUE(device().is_stopped);
  EXPECT_EQ(device().power_wait, PowerWait::NONE);
  EXPECT_EQ(comp.poll_policy_.get_next_update(DEVICE_ID), 0u);
  EXPECT_EQ(issue.state, "");
}

TEST_F(PowerWaitHub, AStatusByteOfAnotherShapeIsNotReadAsWaiting) {
  // State 3 in the low bits, but not under the code every measured reply carries.
  esphome::test_clock::ManualClock clock;
  comp.set_device_follow_waiting_for_power(DEVICE_ID, true);
  command_and_settle();

  ASSERT_TRUE(poll_stopped_off_target({state_reply(0x03)}));

  EXPECT_TRUE(device().is_stopped);
  EXPECT_EQ(device().power_wait, PowerWait::NONE);
}

TEST_F(PowerWaitHub, AnUnansweredFirstReadIsAtRest) {
  // Two ordinary reports agreed on the stop; a device that does not answer the state read keeps
  // that verdict.
  esphome::test_clock::ManualClock clock;
  comp.set_device_follow_waiting_for_power(DEVICE_ID, true);
  command_and_settle();

  ASSERT_TRUE(poll_stopped_off_target());

  EXPECT_EQ(state_reads(), static_cast<size_t>(PERIODIC_POLL_MAX_TRIES));
  EXPECT_TRUE(device().is_stopped);
  EXPECT_EQ(device().power_wait, PowerWait::NONE);
  EXPECT_EQ(comp.poll_policy_.get_next_update(DEVICE_ID), 0u);
}

// ============================================================================
// What ends a wait
// ============================================================================

TEST_F(PowerWaitHub, AStopCancelsTheWait) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_follow_waiting_for_power(DEVICE_ID, true);
  command_and_wait();
  const size_t reads = state_reads();

  queue(status_reply(FLAGS_STOPPED, RAW_75, RAW_75));
  ASSERT_TRUE(comp.queue_device_command(DEVICE_ID, CoverCommand::STOP));
  comp.process_pending_operation_();
  ASSERT_EQ(radio.get_sent_data().back()[FRAME_CMD_OFFSET], CMD_EXECUTE) << "the STOP went out";

  EXPECT_EQ(device().power_wait, PowerWait::NONE);
  EXPECT_EQ(issue.state, "");
  EXPECT_TRUE(effective_is_stopped(device()));

  // The poll that confirms the STOP is an ordinary read again.
  queue(status_reply(FLAGS_STOPPED, RAW_75, RAW_75));
  ASSERT_TRUE(comp.request_device_status(DEVICE_ID));
  EXPECT_TRUE(device().is_stopped);
  EXPECT_EQ(state_reads(), reads);
}

TEST_F(PowerWaitHub, ANewCommandCancelsTheWait) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_follow_waiting_for_power(DEVICE_ID, true);
  command_and_wait();

  queue(status_reply(STATE_EXECUTING, RAW_75, RAW_75));
  ASSERT_TRUE(comp.set_device_position(DEVICE_ID, 60));

  EXPECT_EQ(device().power_wait, PowerWait::NONE);
  EXPECT_EQ(issue.state, "");
}

TEST_F(PowerWaitHub, ALinkedRemotePressCancelsTheWaitAndIsFollowedAfresh) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_follow_waiting_for_power(DEVICE_ID, true);
  command_and_wait();

  comp.schedule_device_polls_({DEVICE_ID}, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS);

  EXPECT_EQ(device().power_wait, PowerWait::NONE);
  EXPECT_EQ(issue.state, "");

  // The remembered stop still matches, so the next ordinary report confirms and the state is read again.
  ASSERT_TRUE(poll_stopped_off_target({state_reply(STATE_WAITING)}));
  EXPECT_EQ(device().power_wait, PowerWait::WAITING);
  EXPECT_EQ(issue.state, "NODE_WAITING_FOR_POWER");
}

TEST_F(PowerWaitHub, ACoverQueuedByALinkedRemoteIsFollowedToo) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_follow_waiting_for_power(DEVICE_ID, true);
  read_at_rest();

  comp.schedule_device_polls_({DEVICE_ID}, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS);
  ASSERT_TRUE(poll_stopped_off_target());
  ASSERT_TRUE(poll_stopped_off_target({state_reply(STATE_WAITING)}));

  EXPECT_EQ(device().power_wait, PowerWait::WAITING);
  EXPECT_FALSE(effective_is_stopped(device()));
}

// ============================================================================
// Without the option, and outside a follow-up window
// ============================================================================

TEST_F(PowerWaitHub, ACoverWithoutTheOptionSettlesAsBefore) {
  esphome::test_clock::ManualClock clock;
  command_and_settle();

  ASSERT_TRUE(poll_stopped_off_target());

  EXPECT_TRUE(device().is_stopped) << "two agreeing reports: at rest off the target";
  EXPECT_EQ(device().power_wait, PowerWait::NONE);
  EXPECT_EQ(state_reads(), 0u) << "no execution state read is ever sent";
  EXPECT_EQ(comp.poll_policy_.get_next_update(DEVICE_ID), 0u);
  EXPECT_EQ(issue.state, "");
}

TEST_F(PowerWaitHub, AnUntrackedReadOfACoverRestingOffItsTargetSendsNoStateRead) {
  // A status refresh, or a read at boot, of a cover that rests off its target for good.
  esphome::test_clock::ManualClock clock;
  comp.set_device_follow_waiting_for_power(DEVICE_ID, true);

  ASSERT_TRUE(poll_stopped_off_target());
  ASSERT_TRUE(poll_stopped_off_target());

  EXPECT_TRUE(device().is_stopped);
  EXPECT_EQ(device().power_wait, PowerWait::NONE);
  EXPECT_EQ(state_reads(), 0u);
}

TEST_F(PowerWaitHub, OptingOutEndsARunningWait) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_follow_waiting_for_power(DEVICE_ID, true);
  command_and_wait();

  comp.set_device_follow_waiting_for_power(DEVICE_ID, false);

  EXPECT_EQ(device().power_wait, PowerWait::NONE);
  EXPECT_FALSE(device().follows_waiting_for_power);
}
