/// @file hub_remote_activity_catchup_test.cpp
/// @brief Tests for the polls after a linked-remote press (ADR 0080): the configurable delay before
///        the first poll, and the opt-in catch-up read of every linked device after the burst.
///
/// The radio is deaf while the hub polls, so a press on another remote during those polls is
/// missed and its device keeps showing the old state (issue #209). The catch-up read corrects it.

#include "hub_core.h"
#include "proto_commands.h"
#include "esphome/core/application.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;

namespace {

constexpr const char *PRESSED_DEVICE = "054E17";  ///< Linked to the remote that is heard.
constexpr uint8_t PRESSED_DEVICE_NODE_ID[3] = {0x05, 0x4E, 0x17};
constexpr uint8_t PRESSED_REMOTE[3] = {0x9D, 0x60, 0x85};
constexpr const char *OTHER_DEVICE = "ABC123";  ///< Linked to a remote that is never heard.
constexpr uint8_t OTHER_DEVICE_NODE_ID[3] = {0xAB, 0xC1, 0x23};
constexpr const char *CLASS_DEVICE = "ABC124";  ///< Linked by `class:` only.
constexpr uint8_t CLASS_DEVICE_NODE_ID[3] = {0xAB, 0xC1, 0x24};
constexpr const char *UNLINKED_DEVICE = "ABC125";
constexpr uint32_t CATCHUP_MS = 30000;

class TestableCatchupComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::poll_policy_;
  using IOHomeControlComponent::process_pending_operation_;
  using IOHomeControlComponent::process_received_packet_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::system_key_;
  using IOHomeControlComponent::tuning_;
};

/// One hub with four roller shutters: one linked to the remote the test presses, one linked to
/// another remote, one linked by class, and one not linked at all.
class RemoteActivityCatchupHub : public ::testing::Test {
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
    for (const char *id : {PRESSED_DEVICE, OTHER_DEVICE, CLASS_DEVICE, UNLINKED_DEVICE}) {
      DeviceConfig cfg;
      cfg.type = DeviceType::ROLLER_SHUTTER;
      comp.add_device(id, cfg);
    }
    comp.add_linked_remote("9D6085", PRESSED_DEVICE);
    comp.add_linked_remote("AABBCC", OTHER_DEVICE);
    comp.add_linked_remote_class(DeviceType::HORIZONTAL_AWNING, CLASS_DEVICE);
  }

  /// A CLOSE press on the remote linked to PRESSED_DEVICE, as the hub receives it.
  void press_linked_remote() {
    IoFrame frame{};
    init_frame(frame, false, true, true, false);  // 1W
    const uint8_t dst[3] = {0x00, 0x01, 0xBF};
    set_src(frame, PRESSED_REMOTE);
    set_dst(frame, dst);
    const uint8_t close[3] = {0x00, 0x00, 0xC8};
    ASSERT_TRUE(set_cmd(frame, CMD_EXECUTE, close, sizeof(close)));
    comp.process_received_packet_(test::make_rx_packet(frame));
  }

  /// A 0x04 reply from @p node_id (PRESSED_DEVICE by default): at rest, fully closed.
  IoFrame status_reply(const uint8_t *node_id = PRESSED_DEVICE_NODE_ID) const {
    const uint8_t payload[8] = {STATUS_STOPPED, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
    return reply_with(node_id, payload);
  }

  /// A 0x04 reply from @p node_id: executing (state 4), closing from open, 20 s left.
  IoFrame moving_reply(const uint8_t *node_id = PRESSED_DEVICE_NODE_ID) const {
    const uint8_t payload[8] = {0x04, 0x00, 0xC8, 0x00, 0x00, 0x00, 0x00, 0x14};
    return reply_with(node_id, payload);
  }

  IoFrame reply_with(const uint8_t *node_id, const uint8_t (&payload)[8]) const {
    IoFrame frame{};
    init_frame(frame, true, false, true, false);
    set_dst(frame, comp.node_id_);
    set_src(frame, node_id);
    EXPECT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));
    return frame;
  }

  /// Run the press's first poll, answered at once; returns when that exchange ended.
  uint32_t run_answered_first_poll() {
    esphome::test_scheduler::run_until(esphome::millis() + REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS);
    EXPECT_EQ(comp.op_queue_.size(), 1u) << "the press queues one poll of its device";
    radio.queue_rx(test::make_rx_packet(status_reply()));
    comp.process_pending_operation_();
    return esphome::millis();
  }

  /// The devices with a status read queued, in queue order.
  std::vector<std::string> queued_status_reads() {
    std::vector<std::string> ids;
    while (const auto op = comp.op_queue_.pop()) {
      if (op->type == PendingOperationType::REQUEST_STATUS)
        ids.push_back(op->device_id);
    }
    return ids;
  }

  /// One loop() pass at @p now, plus the timers it set to fire at once.
  void loop_at(uint32_t now) {
    esphome::test_clock::advance_ms(now - esphome::millis());
    comp.loop();
    esphome::test_scheduler::run_until(esphome::millis());
  }

  TestableCatchupComponent comp;
  MockRadio radio;
};

}  // namespace

// ============================================================================
// Delay before the first poll
// ============================================================================

TEST_F(RemoteActivityCatchupHub, TheFirstPollWaitsTheConfiguredDelay) {
  esphome::test_clock::ManualClock clock;
  const uint32_t t0 = esphome::millis();
  comp.tuning_.remote_activity_poll_delay_ms = 40000;

  press_linked_remote();

  EXPECT_EQ(esphome::test_scheduler::run_until(t0 + 39999), 0u) << "no poll before the configured delay";
  EXPECT_EQ(esphome::test_scheduler::run_until(t0 + 40000), 1u);
  EXPECT_EQ(queued_status_reads(), std::vector<std::string>{PRESSED_DEVICE});
}

// ============================================================================
// Catch-up read
// ============================================================================

TEST_F(RemoteActivityCatchupHub, EveryLinkedDeviceIsReadTheCatchupDelayAfterTheLastExchange) {
  esphome::test_clock::ManualClock clock;
  comp.tuning_.remote_activity_catchup_poll_ms = CATCHUP_MS;

  press_linked_remote();
  const uint32_t ended = run_answered_first_poll();

  loop_at(ended + CATCHUP_MS - 1);
  EXPECT_TRUE(queued_status_reads().empty()) << "nothing before the catch-up is due";

  loop_at(ended + CATCHUP_MS);
  EXPECT_EQ(queued_status_reads(), (std::vector<std::string>{PRESSED_DEVICE, OTHER_DEVICE, CLASS_DEVICE}))
      << "every linked device, by id or class, once; the unlinked one is not read";
}

TEST_F(RemoteActivityCatchupHub, TheCatchupIsOffByDefault) {
  esphome::test_clock::ManualClock clock;

  press_linked_remote();
  const uint32_t ended = run_answered_first_poll();

  loop_at(ended + MAX_TRACKED_STATUS_POLL_WINDOW_MS);
  EXPECT_TRUE(queued_status_reads().empty());
}

TEST_F(RemoteActivityCatchupHub, AnUnansweredCatchupReadOpensNoFollowUpWindow) {
  esphome::test_clock::ManualClock clock;
  comp.tuning_.remote_activity_catchup_poll_ms = CATCHUP_MS;

  press_linked_remote();
  const uint32_t ended = run_answered_first_poll();
  loop_at(ended + CATCHUP_MS);
  ASSERT_EQ(comp.op_queue_.size(), 3u);
  // Nothing queued on the radio: none of the three reads is answered.
  for (int i = 0; i < 3; i++)
    comp.process_pending_operation_();
  const uint32_t missed = esphome::millis();

  for (const char *id : {PRESSED_DEVICE, OTHER_DEVICE, CLASS_DEVICE})
    EXPECT_FALSE(comp.poll_policy_.is_tracking_active(id, missed)) << id << ": a catch-up read is a one-off read";
  loop_at(missed + STATUS_RETRY_AFTER_FAIL_MS);
  EXPECT_TRUE(queued_status_reads().empty()) << "its attempts are its retry; no ladder follows it";
}

TEST_F(RemoteActivityCatchupHub, AMovingAnswerToACatchupReadIsFollowed) {
  esphome::test_clock::ManualClock clock;
  comp.tuning_.remote_activity_catchup_poll_ms = CATCHUP_MS;

  press_linked_remote();
  const uint32_t ended = run_answered_first_poll();
  loop_at(ended + CATCHUP_MS);
  ASSERT_EQ(comp.op_queue_.size(), 3u);
  // OTHER_DEVICE was set moving by a press the hub missed; it answers "executing".
  for (const uint8_t *node_id : {PRESSED_DEVICE_NODE_ID, OTHER_DEVICE_NODE_ID, CLASS_DEVICE_NODE_ID}) {
    const bool moving = node_id == OTHER_DEVICE_NODE_ID;
    radio.queue_rx(test::make_rx_packet(moving ? moving_reply(node_id) : status_reply(node_id)));
    comp.process_pending_operation_();
  }

  EXPECT_TRUE(comp.poll_policy_.is_tracking_active(OTHER_DEVICE, esphome::millis()))
      << "a moving answer opens the follow-up window by itself";
  EXPECT_FALSE(comp.poll_policy_.is_tracking_active(PRESSED_DEVICE, esphome::millis()));
}

TEST_F(RemoteActivityCatchupHub, ACatchupReadLeadsShortToALowPowerDeviceThatJustReportedStopped) {
  // The #209 case: a solar screen answered "stopped" moments ago, then a press the hub missed set it
  // moving. A moving screen ignores the wake-up preamble, so the catch-up read leads short.
  esphome::test_clock::ManualClock clock;
  comp.tuning_.remote_activity_catchup_poll_ms = CATCHUP_MS;
  comp.get_device(PRESSED_DEVICE)->low_power = true;

  press_linked_remote();
  const uint32_t ended = run_answered_first_poll();  // the "stopped" report, CATCHUP_MS before the read
  loop_at(ended + CATCHUP_MS);
  const size_t sent_before = radio.get_tx_configs().size();
  comp.process_pending_operation_();  // PRESSED_DEVICE sorts first

  ASSERT_GT(radio.get_tx_configs().size(), sent_before + 1);
  EXPECT_EQ(radio.get_tx_configs()[sent_before].preamble_len, comp.tuning_.normal_start_preamble)
      << "the short lead a moving receiver hears";
  EXPECT_EQ(radio.get_tx_configs()[sent_before + 1].preamble_len, comp.tuning_.low_power_wake_preamble)
      << "then the wake-up attempts a resting one hears";
}

TEST_F(RemoteActivityCatchupHub, TheStateUnknownMarkIsConsumedByTheCatchupRead) {
  esphome::test_clock::ManualClock clock;
  comp.tuning_.remote_activity_catchup_poll_ms = CATCHUP_MS;

  press_linked_remote();
  const uint32_t ended = run_answered_first_poll();
  loop_at(ended + CATCHUP_MS);
  while (!comp.op_queue_.empty())
    comp.process_pending_operation_();

  for (const char *id : {PRESSED_DEVICE, OTHER_DEVICE, CLASS_DEVICE})
    EXPECT_FALSE(comp.poll_policy_.take_state_unknown(id)) << id << ": a later read is an ordinary one";
}

TEST_F(RemoteActivityCatchupHub, TheCatchupReadsDoNotStartAnotherCatchup) {
  esphome::test_clock::ManualClock clock;
  comp.tuning_.remote_activity_catchup_poll_ms = CATCHUP_MS;

  press_linked_remote();
  const uint32_t ended = run_answered_first_poll();
  loop_at(ended + CATCHUP_MS);
  ASSERT_EQ(comp.op_queue_.size(), 3u);
  // Queue order is the sorted device order, so each read gets its own device's answer.
  for (const uint8_t *node_id : {PRESSED_DEVICE_NODE_ID, OTHER_DEVICE_NODE_ID, CLASS_DEVICE_NODE_ID}) {
    radio.queue_rx(test::make_rx_packet(status_reply(node_id)));
    comp.process_pending_operation_();
  }
  ASSERT_TRUE(comp.op_queue_.empty());

  loop_at(esphome::millis() + CATCHUP_MS);
  EXPECT_TRUE(queued_status_reads().empty());
}
