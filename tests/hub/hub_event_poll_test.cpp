/// @file hub_event_poll_test.cpp
/// @brief Tests for the opt-in sun level and battery level polls (ADR 0064): the subscription they
///        send, how its answer reaches the device record, misses, and scheduling in loop().
///
/// The answers are the RS100 Solar io's own (issue #156), re-addressed to the test hub:
/// somfy_rs100_probe_subscribe_event_daylight_sx1262 (sun value 744) and
/// somfy_rs100_probe_subscribe_event_battery_sx1262 (battery level 100).

#include "hub_core.h"
#include "proto_commands.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;

namespace {

constexpr const char *DEVICE_ID = "ABC123";
constexpr uint8_t DEVICE_NODE_ID[3] = {0xAB, 0xC1, 0x23};
constexpr uint32_t SUN_INTERVAL_MS = 30 * 60 * 1000;
constexpr uint32_t BATTERY_INTERVAL_MS = 6 * 60 * 60 * 1000;
constexpr const char *DAYLIGHT_CAPTURE = "somfy_rs100_probe_subscribe_event_daylight_sx1262";
constexpr const char *BATTERY_CAPTURE = "somfy_rs100_probe_subscribe_event_battery_sx1262";
constexpr uint16_t CAPTURED_SUN_VALUE = 744;     ///< daylight capture, frame 1 (13:41)
constexpr uint8_t CAPTURED_BATTERY_LEVEL = 100;  ///< battery capture, frame 0

class TestableEventPollComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::battery_level_poll_policy_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::poll_policy_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::sun_poll_policy_;
  using IOHomeControlComponent::system_key_;
};

/// One hub with a low-power roller shutter "ABC123" opted into both polls, and a mock radio that
/// has nothing queued yet.
class EventPollHub : public ::testing::Test {
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
    DeviceConfig cfg;
    cfg.type = DeviceType::ROLLER_SHUTTER;
    cfg.low_power = true;
    comp.add_device(DEVICE_ID, cfg);
    ASSERT_NE(comp.get_device(DEVICE_ID), nullptr);
    comp.set_device_sun_poll_interval(DEVICE_ID, SUN_INTERVAL_MS);
    comp.set_device_battery_level_poll_interval(DEVICE_ID, BATTERY_INTERVAL_MS);
    comp.register_device_callback([this](const std::string &, const IoDevice &) { notifications++; });
  }

  IoDevice &device() { return *comp.get_device(DEVICE_ID); }

  IoFrame captured(const char *capture_id, uint8_t index) {
    return corpus_test::readdressed_capture_frame(capture_id, index, DEVICE_NODE_ID, comp.node_id_);
  }
  void queue_sun_answer() { radio.queue_rx(test::make_rx_packet(captured(DAYLIGHT_CAPTURE, 1))); }
  void queue_battery_answer() { radio.queue_rx(test::make_rx_packet(captured(BATTERY_CAPTURE, 0))); }
  void queue_challenge() {
    const uint8_t challenge[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    IoFrame chal{};
    init_frame(chal, true, false, false, false);
    set_dst(chal, comp.node_id_);
    set_src(chal, DEVICE_NODE_ID);
    set_cmd(chal, CMD_CHALLENGE_REQ, challenge, sizeof(challenge));
    radio.queue_rx(test::make_rx_packet(chal));
  }

  /// One scheduled sun poll the device leaves unanswered, then its retry, unanswered too: one
  /// counted miss. Both are popped from the schedule the way loop() would.
  void miss_sun_poll_and_retry() {
    pop_due();
    EXPECT_FALSE(comp.request_device_sun_level(DEVICE_ID));
    ASSERT_TRUE(comp.sun_poll_policy_.retry_armed(DEVICE_ID));
    pop_due();
    EXPECT_FALSE(comp.request_device_sun_level(DEVICE_ID));
  }

  /// Take the device's next poll off the schedule, as loop() does before it queues the poll.
  void pop_due() {
    ASSERT_TRUE(comp.sun_poll_policy_.pop_due_device(comp.sun_poll_policy_.get_next_poll(DEVICE_ID), 0).has_value());
  }

  TestableEventPollComponent comp;
  MockRadio radio;
  int notifications{0};
};

}  // namespace

// ============================================================================
// The request and its answer
// ============================================================================

TEST_F(EventPollHub, SunPollSendsTheSubscriptionAndPublishesTheAnsweredValue) {
  queue_sun_answer();

  ASSERT_TRUE(comp.request_device_sun_level(DEVICE_ID));

  ASSERT_FALSE(radio.get_sent_data().empty());
  const auto &tx = radio.get_sent_data().front();
  ASSERT_GE(tx.size(), 12u);
  EXPECT_EQ(tx[8], CMD_SUBSCRIBE_EVENT);
  EXPECT_EQ(tx[9], EVENT_SUBSCRIPTION_SUBSCRIBE);
  EXPECT_EQ(tx[10], 0x20);
  EXPECT_EQ(tx[11], 0x05);
  EXPECT_NE(tx[1] & CTRL1_LOW_POWER, 0) << "a low_power cover is woken as for any other request";
  ASSERT_TRUE(device().sun_level.has_value());
  EXPECT_EQ(*device().sun_level, CAPTURED_SUN_VALUE);
  EXPECT_FALSE(device().battery_level.has_value()) << "the other reading is untouched";
  EXPECT_NE(device().last_seen_ms, 0u);
  EXPECT_GE(notifications, 1);
}

TEST_F(EventPollHub, BatteryPollSendsTheSubscriptionAndPublishesTheAnsweredLevel) {
  queue_battery_answer();

  ASSERT_TRUE(comp.request_device_battery_level(DEVICE_ID));

  const auto &tx = radio.get_sent_data().front();
  ASSERT_GE(tx.size(), 12u);
  EXPECT_EQ(tx[8], CMD_SUBSCRIBE_EVENT);
  EXPECT_EQ(tx[10], 0x20);
  EXPECT_EQ(tx[11], 0x01);
  ASSERT_TRUE(device().battery_level.has_value());
  EXPECT_EQ(*device().battery_level, CAPTURED_BATTERY_LEVEL);
  EXPECT_FALSE(device().sun_level.has_value());
}

TEST_F(EventPollHub, AChallengedSubscriptionIsAnsweredAfterOurChallengeResponse) {
  queue_challenge();
  queue_sun_answer();

  ASSERT_TRUE(comp.request_device_sun_level(DEVICE_ID));

  EXPECT_EQ(radio.get_send_count(), 2) << "the subscription and our 0x3D";
  EXPECT_EQ(*device().sun_level, CAPTURED_SUN_VALUE);
}

TEST_F(EventPollHub, TheAnswerIsNeverAppliedAsStatusNorRecordedAsAReport) {
  device().position = 40.0f;
  device().target = 40.0f;
  device().is_stopped = true;
  queue_sun_answer();

  ASSERT_TRUE(comp.request_device_sun_level(DEVICE_ID));

  EXPECT_EQ(device().position, 40.0f);
  EXPECT_EQ(device().target, 40.0f);
  EXPECT_TRUE(device().is_stopped);
  EXPECT_EQ(comp.device_event_count(), 0u) << "the poll's own answer is its reading, not an unhandled report";
  EXPECT_EQ(radio.get_send_count(), 1) << "no 0x72 acknowledgement and no second request";
}

TEST_F(EventPollHub, AReportOfTheOtherEventIsRecordedAndNotTakenAsTheAnswer) {
  queue_sun_answer();  // answers the battery subscription with the wrong event, twice
  queue_sun_answer();

  EXPECT_FALSE(comp.request_device_battery_level(DEVICE_ID));

  EXPECT_FALSE(device().battery_level.has_value());
  EXPECT_FALSE(device().sun_level.has_value()) << "an unsolicited report is never applied";
  EXPECT_EQ(comp.device_event_count(), 2u);
  EXPECT_EQ(comp.battery_level_poll_policy_.get_failures(DEVICE_ID), 0u);
  EXPECT_TRUE(comp.battery_level_poll_policy_.retry_armed(DEVICE_ID)) << "the request itself went unanswered";
}

TEST_F(EventPollHub, UnknownDeviceAndUninitializedHubSendNothing) {
  EXPECT_FALSE(comp.request_device_sun_level("000000"));
  comp.initialized_ = false;
  EXPECT_FALSE(comp.request_device_battery_level(DEVICE_ID));
  EXPECT_TRUE(radio.get_sent_data().empty());
}

// ============================================================================
// Misses
// ============================================================================

TEST_F(EventPollHub, AnUnansweredPollIsAskedAgainAfterAMinuteBeforeItCounts) {
  pop_due();
  const uint32_t before = esphome::millis();
  EXPECT_FALSE(comp.request_device_sun_level(DEVICE_ID));

  EXPECT_EQ(comp.sun_poll_policy_.get_failures(DEVICE_ID), 0u);
  EXPECT_TRUE(comp.sun_poll_policy_.retry_armed(DEVICE_ID));
  const uint32_t retry_at = comp.sun_poll_policy_.get_next_poll(DEVICE_ID);
  EXPECT_GE(retry_at, before + PERIODIC_POLL_MISS_RETRY_MS);
  EXPECT_LE(retry_at, esphome::millis() + PERIODIC_POLL_MISS_RETRY_MS) << "a minute after the miss, not an interval";
}

TEST_F(EventPollHub, AnAnsweredRetryPublishesTheReadingWithNoMiss) {
  EXPECT_FALSE(comp.request_device_sun_level(DEVICE_ID));
  ASSERT_TRUE(comp.sun_poll_policy_.pop_due_device(comp.sun_poll_policy_.get_next_poll(DEVICE_ID), 0).has_value());

  queue_sun_answer();
  ASSERT_TRUE(comp.request_device_sun_level(DEVICE_ID));

  EXPECT_EQ(*device().sun_level, CAPTURED_SUN_VALUE);
  EXPECT_EQ(comp.sun_poll_policy_.get_failures(DEVICE_ID), 0u);
}

TEST_F(EventPollHub, OneMissKeepsTheLastReadingAndThreeMakeItUnknown) {
  queue_sun_answer();
  ASSERT_TRUE(comp.request_device_sun_level(DEVICE_ID));

  for (int i = 0; i < PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN - 1; ++i) {
    miss_sun_poll_and_retry();
    EXPECT_TRUE(device().sun_level.has_value()) << "miss " << i + 1;
  }
  miss_sun_poll_and_retry();

  EXPECT_FALSE(device().sun_level.has_value());
  EXPECT_EQ(comp.sun_poll_policy_.get_failures(DEVICE_ID), PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN);
}

TEST_F(EventPollHub, AnAnswerAfterMissesClearsTheCount) {
  miss_sun_poll_and_retry();
  EXPECT_EQ(comp.sun_poll_policy_.get_failures(DEVICE_ID), 1u);

  queue_sun_answer();
  ASSERT_TRUE(comp.request_device_sun_level(DEVICE_ID));

  EXPECT_EQ(comp.sun_poll_policy_.get_failures(DEVICE_ID), 0u);
}

TEST_F(EventPollHub, AMissLeavesTheStatusPollLadderAloneAndCountsAsAnExchangeFailure) {
  comp.set_device_status_poll_interval(DEVICE_ID, 2000);
  const uint32_t next_update = comp.poll_policy_.get_next_update(DEVICE_ID);

  EXPECT_FALSE(comp.request_device_sun_level(DEVICE_ID));

  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures(DEVICE_ID), 0u);
  EXPECT_EQ(comp.poll_policy_.get_next_update(DEVICE_ID), next_update);
  EXPECT_EQ(device().exchange_timeout_count, 1u);
}

TEST_F(EventPollHub, AnAnswerOfAnotherShapeIsAMissAndWarnsOnce) {
  IoFrame odd = captured(DAYLIGHT_CAPTURE, 1);
  odd.data[STATUS_UPDATE_EVENT_DATA_OFFSET] = 0x05;  // another sensor type byte
  radio.queue_rx(test::make_rx_packet(odd));

  EXPECT_FALSE(comp.request_device_sun_level(DEVICE_ID));

  EXPECT_FALSE(device().sun_level.has_value());
  EXPECT_EQ(comp.sun_poll_policy_.get_failures(DEVICE_ID), 1u) << "counted at once: asking again changes nothing";
  EXPECT_FALSE(comp.sun_poll_policy_.retry_armed(DEVICE_ID));
  EXPECT_FALSE(comp.sun_poll_policy_.first_error_reply(DEVICE_ID)) << "the warning was spent on this answer";
}

TEST_F(EventPollHub, AnErrorReplyIsAMiss) {
  IoFrame error{};
  init_frame(error, true, false, true, false);
  set_dst(error, comp.node_id_);
  set_src(error, DEVICE_NODE_ID);
  const uint8_t payload[1] = {RESULT_COMMAND_UNKNOWN};
  set_cmd(error, CMD_ERROR_RESP, payload, sizeof(payload));
  radio.queue_rx(test::make_rx_packet(error));

  EXPECT_FALSE(comp.request_device_battery_level(DEVICE_ID));

  EXPECT_FALSE(device().battery_level.has_value());
  EXPECT_EQ(comp.battery_level_poll_policy_.get_failures(DEVICE_ID), 1u);
  EXPECT_FALSE(comp.battery_level_poll_policy_.retry_armed(DEVICE_ID)) << "a refusal is not retried";
  EXPECT_FALSE(comp.battery_level_poll_policy_.first_error_reply(DEVICE_ID));
}

// ============================================================================
// Moving device and scheduling
// ============================================================================

TEST_F(EventPollHub, AMovingDeviceIsNotAskedAndThePollMovesForward) {
  esphome::test_clock::ManualClock clock;
  esphome::test_random::set(UINT32_MAX);
  comp.set_device_sun_poll_interval(DEVICE_ID, SUN_INTERVAL_MS);
  device().is_stopped = false;
  const uint32_t now = esphome::millis();

  EXPECT_FALSE(comp.request_device_sun_level(DEVICE_ID));

  EXPECT_TRUE(radio.get_sent_data().empty());
  EXPECT_EQ(comp.sun_poll_policy_.get_next_poll(DEVICE_ID), now + PERIODIC_POLL_MOVING_RETRY_MS);
  EXPECT_EQ(comp.sun_poll_policy_.get_failures(DEVICE_ID), 0u);
}

TEST_F(EventPollHub, LoopQueuesEachPollWhenItIsDue) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_sun_poll_interval(DEVICE_ID, SUN_INTERVAL_MS);
  comp.set_device_battery_level_poll_interval(DEVICE_ID, BATTERY_INTERVAL_MS);
  esphome::test_clock::advance_ms(PERIODIC_POLL_INITIAL_DELAY_MS + PERIODIC_POLL_INITIAL_SPREAD_MS);

  comp.loop();

  ASSERT_EQ(comp.op_queue_.size(), 2u);
  EXPECT_EQ(comp.op_queue_[0].type, PendingOperationType::REQUEST_SUN_LEVEL);
  EXPECT_EQ(comp.op_queue_[1].type, PendingOperationType::REQUEST_BATTERY_LEVEL);
  EXPECT_EQ(comp.op_queue_[0].device_id, DEVICE_ID);
}

TEST_F(EventPollHub, ACoverWithoutIntervalsIsNeverPolled) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_sun_poll_interval(DEVICE_ID, 0);
  comp.set_device_battery_level_poll_interval(DEVICE_ID, 0);
  esphome::test_clock::advance_ms(2 * BATTERY_INTERVAL_MS);

  comp.loop();

  EXPECT_TRUE(comp.op_queue_.empty());
}
