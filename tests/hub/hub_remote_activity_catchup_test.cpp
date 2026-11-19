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

#include <algorithm>
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
constexpr const char *HEATER_DEVICE = "ABC126";  ///< A register-protocol radiator, written by its climate entity.
constexpr uint8_t HEATER_NODE_ID[3] = {0xAB, 0xC1, 0x26};
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

/// A reply from the heater with command @p cmd and @p payload.
template<size_t N> IoFrame heater_reply(const uint8_t *hub_node_id, uint8_t cmd, const uint8_t (&payload)[N]) {
  IoFrame frame{};
  init_frame(frame, true, false, true, false);
  set_dst(frame, hub_node_id);
  set_src(frame, HEATER_NODE_ID);
  EXPECT_TRUE(set_cmd(frame, cmd, payload, N));
  return frame;
}

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
  EXPECT_TRUE(queued_status_reads().empty()) << "no ladder follows it; its single retry waits the catch-up delay";
}

// ============================================================================
// Retry of an unanswered catch-up read (ADR 0095)
// ============================================================================

TEST_F(RemoteActivityCatchupHub, AnAnsweredCatchupReadIsNotRetried) {
  esphome::test_clock::ManualClock clock;
  comp.tuning_.remote_activity_catchup_poll_ms = CATCHUP_MS;

  press_linked_remote();
  const uint32_t ended = run_answered_first_poll();
  loop_at(ended + CATCHUP_MS);
  ASSERT_EQ(comp.op_queue_.size(), 3u);
  for (const uint8_t *node_id : {PRESSED_DEVICE_NODE_ID, OTHER_DEVICE_NODE_ID, CLASS_DEVICE_NODE_ID}) {
    radio.queue_rx(test::make_rx_packet(status_reply(node_id)));
    comp.process_pending_operation_();
  }

  loop_at(esphome::millis() + 2 * CATCHUP_MS);
  EXPECT_TRUE(queued_status_reads().empty());
}

TEST_F(RemoteActivityCatchupHub, ASilentCatchupReadIsRetriedOnceTheCatchupDelayLater) {
  esphome::test_clock::ManualClock clock;
  comp.tuning_.remote_activity_catchup_poll_ms = CATCHUP_MS;

  press_linked_remote();
  const uint32_t ended = run_answered_first_poll();
  loop_at(ended + CATCHUP_MS);
  ASSERT_EQ(comp.op_queue_.size(), 3u);
  comp.process_pending_operation_();  // PRESSED_DEVICE: nothing queued on the radio, no answer
  const uint32_t first_miss = esphome::millis();
  comp.process_pending_operation_();
  comp.process_pending_operation_();
  const uint32_t last_miss = esphome::millis();

  loop_at(first_miss + CATCHUP_MS - 1);
  EXPECT_TRUE(queued_status_reads().empty()) << "not before the catch-up delay has passed";

  loop_at(last_miss + CATCHUP_MS);
  EXPECT_EQ(queued_status_reads(), (std::vector<std::string>{PRESSED_DEVICE, OTHER_DEVICE, CLASS_DEVICE}))
      << "each silent device is read once more";
}

TEST_F(RemoteActivityCatchupHub, OnlyTheDeviceThatStayedSilentIsRetried) {
  esphome::test_clock::ManualClock clock;
  comp.tuning_.remote_activity_catchup_poll_ms = CATCHUP_MS;

  press_linked_remote();
  const uint32_t ended = run_answered_first_poll();
  loop_at(ended + CATCHUP_MS);
  ASSERT_EQ(comp.op_queue_.size(), 3u);
  radio.queue_rx(test::make_rx_packet(status_reply(PRESSED_DEVICE_NODE_ID)));
  comp.process_pending_operation_();  // answered
  comp.process_pending_operation_();  // OTHER_DEVICE: silent
  radio.queue_rx(test::make_rx_packet(status_reply(CLASS_DEVICE_NODE_ID)));
  comp.process_pending_operation_();  // answered

  loop_at(esphome::millis() + CATCHUP_MS);
  EXPECT_EQ(queued_status_reads(), std::vector<std::string>{OTHER_DEVICE});
}

TEST_F(RemoteActivityCatchupHub, ACatchupReadThatIsSilentTwiceIsNotRetriedAgain) {
  esphome::test_clock::ManualClock clock;
  comp.tuning_.remote_activity_catchup_poll_ms = CATCHUP_MS;

  press_linked_remote();
  const uint32_t ended = run_answered_first_poll();
  loop_at(ended + CATCHUP_MS);
  while (!comp.op_queue_.empty())
    comp.process_pending_operation_();
  loop_at(esphome::millis() + CATCHUP_MS);
  ASSERT_EQ(comp.op_queue_.size(), 3u) << "the retry";
  while (!comp.op_queue_.empty())
    comp.process_pending_operation_();  // silent again

  loop_at(esphome::millis() + 3 * CATCHUP_MS);
  EXPECT_TRUE(queued_status_reads().empty()) << "a second silence ends the catch-up";
}

TEST_F(RemoteActivityCatchupHub, ARetryThatIsAnsweredEndsTheCatchupAndAMovingAnswerIsFollowed) {
  esphome::test_clock::ManualClock clock;
  comp.tuning_.remote_activity_catchup_poll_ms = CATCHUP_MS;

  press_linked_remote();
  const uint32_t ended = run_answered_first_poll();
  loop_at(ended + CATCHUP_MS);
  while (!comp.op_queue_.empty())
    comp.process_pending_operation_();
  loop_at(esphome::millis() + CATCHUP_MS);
  ASSERT_EQ(comp.op_queue_.size(), 3u);
  for (const uint8_t *node_id : {PRESSED_DEVICE_NODE_ID, OTHER_DEVICE_NODE_ID, CLASS_DEVICE_NODE_ID}) {
    const bool moving = node_id == OTHER_DEVICE_NODE_ID;
    radio.queue_rx(test::make_rx_packet(moving ? moving_reply(node_id) : status_reply(node_id)));
    comp.process_pending_operation_();
  }

  EXPECT_TRUE(comp.poll_policy_.is_tracking_active(OTHER_DEVICE, esphome::millis()))
      << "an answer to the retry is followed like any catch-up answer";
  loop_at(esphome::millis() + 3 * CATCHUP_MS);
  const std::vector<std::string> later = queued_status_reads();
  EXPECT_EQ(std::count(later.begin(), later.end(), PRESSED_DEVICE), 0) << "no retry of an answered read";
  EXPECT_EQ(std::count(later.begin(), later.end(), CLASS_DEVICE), 0) << "no retry of an answered read";
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
  EXPECT_EQ(radio.get_tx_configs()[sent_before].preamble_len, mock_preamble_bytes(comp.tuning_.start_preamble_ms))
      << "the short lead a moving receiver hears";
  EXPECT_EQ(radio.get_tx_configs()[sent_before + 1].preamble_len, mock_preamble_bytes(comp.tuning_.low_power_wake_ms))
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

TEST_F(RemoteActivityCatchupHub, AHeaterWriteDuringTheBurstMovesTheCatchupOut) {
  esphome::test_clock::ManualClock clock;
  comp.tuning_.remote_activity_catchup_poll_ms = CATCHUP_MS;
  DeviceConfig heater;
  heater.type = DeviceType::ELECTRICAL_HEATER;
  heater.subtype = 1;
  comp.add_device(HEATER_DEVICE, heater);

  press_linked_remote();
  const uint32_t ended = run_answered_first_poll();
  esphome::test_clock::advance_ms(CATCHUP_MS / 2);
  const uint8_t ack[] = {0x0C, 0x61, 0x01, 0x03, 0xCD, 0x00};
  radio.queue_rx(test::make_rx_packet(heater_reply(comp.node_id_, CMD_WRITE_PRIVATE_ACK, ack)));
  ASSERT_TRUE(comp.send_heating_command(HEATER_DEVICE, HeatingFunction::SET_TEMPERATURE, 20.5F));
  const uint32_t written = esphome::millis();
  ASSERT_GT(written, ended);

  loop_at(ended + CATCHUP_MS);
  EXPECT_TRUE(queued_status_reads().empty()) << "the write ended later, so the catch-up is not due yet";
  loop_at(written + CATCHUP_MS);
  EXPECT_EQ(queued_status_reads().size(), 3u);
}

TEST_F(RemoteActivityCatchupHub, AWaterHeaterWriteDuringTheBurstMovesTheCatchupOut) {
  esphome::test_clock::ManualClock clock;
  comp.tuning_.remote_activity_catchup_poll_ms = CATCHUP_MS;
  DeviceConfig water_heater;
  water_heater.type = DeviceType::DOMESTIC_HOT_WATER;
  water_heater.entity_class = DeviceCapabilityClass::WATER_HEATER;
  comp.add_device(HEATER_DEVICE, water_heater);

  press_linked_remote();
  const uint32_t ended = run_answered_first_poll();
  esphome::test_clock::advance_ms(CATCHUP_MS / 2);
  // The mode write, its register read-back and the status read that follows it.
  const uint8_t write_ack[] = {0x0C, 0x61, 0x00, 0x06, 0x02};
  const uint8_t read_ack[] = {0x0C, 0x60, 0x00, 0x06, 0x02};
  const uint8_t status[] = {0x2D, 0x00, 0xA1, 0xE8, 0x00, 0x00};
  radio.queue_rx(test::make_rx_packet(heater_reply(comp.node_id_, CMD_WRITE_PRIVATE_ACK, write_ack)));
  radio.queue_rx(test::make_rx_packet(heater_reply(comp.node_id_, CMD_WRITE_PRIVATE_ACK, read_ack)));
  radio.queue_rx(test::make_rx_packet(heater_reply(comp.node_id_, CMD_PRIVATE_RESP, status)));
  ASSERT_TRUE(comp.set_water_heater_mode(HEATER_DEVICE, DhwMode::MANUAL_ECO_OFF));
  const uint32_t written = esphome::millis();
  ASSERT_GT(written, ended);

  loop_at(ended + CATCHUP_MS);
  EXPECT_TRUE(queued_status_reads().empty()) << "the write ended later, so the catch-up is not due yet";
  loop_at(written + CATCHUP_MS);
  EXPECT_EQ(queued_status_reads().size(), 3u);
}
