/// @file hub_event_poll_test.cpp
/// @brief Tests for the opt-in sun level and battery level readings (ADR 0064): the subscription they
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
constexpr float CAPTURED_SUN_LUX = 74400.0F;     ///< daylight capture, frame 1 (13:41): raw 744 at 100 lx per step
constexpr uint8_t CAPTURED_BATTERY_LEVEL = 100;  ///< battery capture, frame 0

class TestableEventPollComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::device_reading_key;
  using IOHomeControlComponent::device_reading_poll_policy_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::poll_policy_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::system_key_;
};

/// The key under which the hub schedules the sun level reading or the battery level reading of the test device.
std::string sun_key() { return TestableEventPollComponent::device_reading_key(DEVICE_ID, DeviceReading::SUN_LEVEL); }
std::string battery_key() {
  return TestableEventPollComponent::device_reading_key(DEVICE_ID, DeviceReading::BATTERY_LEVEL);
}

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
    comp.set_device_reading_poll_interval(DEVICE_ID, DeviceReading::SUN_LEVEL, SUN_INTERVAL_MS);
    comp.set_device_reading_poll_interval(DEVICE_ID, DeviceReading::BATTERY_LEVEL, BATTERY_INTERVAL_MS);
    comp.register_device_callback([this](const std::string &, const IoDevice &) { notifications++; });
  }

  IoDevice &device() { return *comp.get_device(DEVICE_ID); }
  std::optional<float> sun() { return reading_value(device(), DeviceReading::SUN_LEVEL); }
  std::optional<float> battery() { return reading_value(device(), DeviceReading::BATTERY_LEVEL); }

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

  /// The same for the battery level poll.
  void miss_battery_poll_and_retry() {
    ASSERT_TRUE(comp.device_reading_poll_policy_
                    .pop_due_device(comp.device_reading_poll_policy_.get_next_poll(battery_key()), 0)
                    .has_value());
    EXPECT_FALSE(comp.request_device_reading(DEVICE_ID, DeviceReading::BATTERY_LEVEL));
    ASSERT_TRUE(comp.device_reading_poll_policy_.retry_armed(battery_key()));
    ASSERT_TRUE(comp.device_reading_poll_policy_
                    .pop_due_device(comp.device_reading_poll_policy_.get_next_poll(battery_key()), 0)
                    .has_value());
    EXPECT_FALSE(comp.request_device_reading(DEVICE_ID, DeviceReading::BATTERY_LEVEL));
  }

  /// One scheduled sun poll the device leaves unanswered, then its retry, unanswered too: one
  /// counted miss. Both are popped from the schedule the way loop() would.
  void miss_sun_poll_and_retry() {
    pop_due();
    EXPECT_FALSE(comp.request_device_reading(DEVICE_ID, DeviceReading::SUN_LEVEL));
    ASSERT_TRUE(comp.device_reading_poll_policy_.retry_armed(sun_key()));
    pop_due();
    EXPECT_FALSE(comp.request_device_reading(DEVICE_ID, DeviceReading::SUN_LEVEL));
  }

  /// Take the device's next poll off the schedule, as loop() does before it queues the poll.
  void pop_due() {
    ASSERT_TRUE(
        comp.device_reading_poll_policy_.pop_due_device(comp.device_reading_poll_policy_.get_next_poll(sun_key()), 0)
            .has_value());
  }

  TestableEventPollComponent comp;
  MockRadio radio;
  int notifications{0};
};

}  // namespace

// ============================================================================
// The request and its answer
// ============================================================================

TEST_F(EventPollHub, TheNightAnswerIsAReadingOfZeroNotNoReading) {
  radio.queue_rx(test::make_rx_packet(
      corpus_test::sensor_event_report(DEVICE_NODE_ID, comp.node_id_)));  // the capture taken at night: value 0

  ASSERT_TRUE(comp.request_device_reading(DEVICE_ID, DeviceReading::SUN_LEVEL));

  ASSERT_TRUE(sun().has_value());
  EXPECT_EQ(*sun(), 0.0F);
}

TEST_F(EventPollHub, SunPollSendsTheSubscriptionAndPublishesTheAnsweredValue) {
  queue_sun_answer();

  ASSERT_TRUE(comp.request_device_reading(DEVICE_ID, DeviceReading::SUN_LEVEL));

  ASSERT_FALSE(radio.get_sent_data().empty());
  const auto &tx = radio.get_sent_data().front();
  ASSERT_GE(tx.size(), 12u);
  EXPECT_EQ(tx[8], CMD_SUBSCRIBE_EVENT);
  EXPECT_EQ(tx[9], EVENT_SUBSCRIPTION_SUBSCRIBE);
  EXPECT_EQ(tx[10], 0x20);
  EXPECT_EQ(tx[11], 0x05);
  EXPECT_NE(tx[1] & CTRL1_LOW_POWER, 0) << "a low_power cover is woken as for any other request";
  ASSERT_TRUE(sun().has_value());
  EXPECT_EQ(*sun(), CAPTURED_SUN_LUX);
  EXPECT_FALSE(battery().has_value()) << "the other reading is untouched";
  EXPECT_NE(device().last_seen_ms, 0u);
  EXPECT_GE(notifications, 1);
}

TEST_F(EventPollHub, BatteryPollSendsTheSubscriptionAndPublishesTheAnsweredLevel) {
  queue_battery_answer();

  ASSERT_TRUE(comp.request_device_reading(DEVICE_ID, DeviceReading::BATTERY_LEVEL));

  const auto &tx = radio.get_sent_data().front();
  ASSERT_GE(tx.size(), 12u);
  EXPECT_EQ(tx[8], CMD_SUBSCRIBE_EVENT);
  EXPECT_EQ(tx[10], 0x20);
  EXPECT_EQ(tx[11], 0x01);
  ASSERT_TRUE(battery().has_value());
  EXPECT_EQ(*battery(), CAPTURED_BATTERY_LEVEL);
  EXPECT_FALSE(sun().has_value());
}

TEST_F(EventPollHub, AChallengedSubscriptionIsAnsweredAfterOurChallengeResponse) {
  queue_challenge();
  queue_sun_answer();

  ASSERT_TRUE(comp.request_device_reading(DEVICE_ID, DeviceReading::SUN_LEVEL));

  EXPECT_EQ(radio.get_send_count(), 2) << "the subscription and our 0x3D";
  EXPECT_EQ(*sun(), CAPTURED_SUN_LUX);
}

TEST_F(EventPollHub, TheAnswerIsNeverAppliedAsStatusNorRecordedAsAReport) {
  device().position = 40.0f;
  device().target = 40.0f;
  device().is_stopped = true;
  queue_sun_answer();

  ASSERT_TRUE(comp.request_device_reading(DEVICE_ID, DeviceReading::SUN_LEVEL));

  EXPECT_EQ(device().position, 40.0f);
  EXPECT_EQ(device().target, 40.0f);
  EXPECT_TRUE(device().is_stopped);
  EXPECT_EQ(comp.device_event_count(), 0u) << "the poll's own answer is its reading, not an unhandled report";
  EXPECT_EQ(radio.get_send_count(), 1) << "no 0x72 acknowledgement and no second request";
}

TEST_F(EventPollHub, AReportOfTheOtherEventIsRecordedAndNotTakenAsTheAnswer) {
  queue_sun_answer();  // answers the battery subscription with the wrong event, twice
  queue_sun_answer();

  EXPECT_FALSE(comp.request_device_reading(DEVICE_ID, DeviceReading::BATTERY_LEVEL));

  EXPECT_FALSE(battery().has_value());
  EXPECT_FALSE(sun().has_value()) << "an unsolicited report is never applied";
  EXPECT_EQ(comp.device_event_count(), 2u);
  EXPECT_EQ(comp.device_reading_poll_policy_.get_failures(battery_key()), 0u);
  EXPECT_TRUE(comp.device_reading_poll_policy_.retry_armed(battery_key())) << "the request itself went unanswered";
}

TEST_F(EventPollHub, UnknownDeviceAndUninitializedHubSendNothing) {
  EXPECT_FALSE(comp.request_device_reading("000000", DeviceReading::SUN_LEVEL));
  comp.initialized_ = false;
  EXPECT_FALSE(comp.request_device_reading(DEVICE_ID, DeviceReading::BATTERY_LEVEL));
  EXPECT_TRUE(radio.get_sent_data().empty());
}

// ============================================================================
// Misses
// ============================================================================

TEST_F(EventPollHub, AnUnansweredPollIsAskedAgainAfterAMinuteBeforeItCounts) {
  pop_due();
  const uint32_t before = esphome::millis();
  EXPECT_FALSE(comp.request_device_reading(DEVICE_ID, DeviceReading::SUN_LEVEL));

  EXPECT_EQ(comp.device_reading_poll_policy_.get_failures(sun_key()), 0u);
  EXPECT_TRUE(comp.device_reading_poll_policy_.retry_armed(sun_key()));
  const uint32_t retry_at = comp.device_reading_poll_policy_.get_next_poll(sun_key());
  EXPECT_GE(retry_at, before + PERIODIC_POLL_MISS_RETRY_MS);
  EXPECT_LE(retry_at, esphome::millis() + PERIODIC_POLL_MISS_RETRY_MS) << "a minute after the miss, not an interval";
}

TEST_F(EventPollHub, OneMissKeepsTheLastBatteryLevelAndThreeMakeItUnknown) {
  comp.set_device_reading_poll_interval(DEVICE_ID, DeviceReading::SUN_LEVEL, 0);  // the helper pops what is due
  queue_battery_answer();
  ASSERT_TRUE(comp.request_device_reading(DEVICE_ID, DeviceReading::BATTERY_LEVEL));

  for (int i = 0; i < PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN - 1; ++i) {
    miss_battery_poll_and_retry();
    EXPECT_TRUE(battery().has_value()) << "miss " << i + 1;
  }
  miss_battery_poll_and_retry();

  EXPECT_FALSE(battery().has_value());
  EXPECT_EQ(comp.device_reading_poll_policy_.get_failures(battery_key()), PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN);
}

TEST_F(EventPollHub, AnAnsweredRetryPublishesTheReadingWithNoMiss) {
  EXPECT_FALSE(comp.request_device_reading(DEVICE_ID, DeviceReading::SUN_LEVEL));
  ASSERT_TRUE(
      comp.device_reading_poll_policy_.pop_due_device(comp.device_reading_poll_policy_.get_next_poll(sun_key()), 0)
          .has_value());

  queue_sun_answer();
  ASSERT_TRUE(comp.request_device_reading(DEVICE_ID, DeviceReading::SUN_LEVEL));

  EXPECT_EQ(*sun(), CAPTURED_SUN_LUX);
  EXPECT_EQ(comp.device_reading_poll_policy_.get_failures(sun_key()), 0u);
}

TEST_F(EventPollHub, OneMissKeepsTheLastReadingAndThreeMakeItUnknown) {
  queue_sun_answer();
  ASSERT_TRUE(comp.request_device_reading(DEVICE_ID, DeviceReading::SUN_LEVEL));

  for (int i = 0; i < PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN - 1; ++i) {
    miss_sun_poll_and_retry();
    EXPECT_TRUE(sun().has_value()) << "miss " << i + 1;
  }
  miss_sun_poll_and_retry();

  EXPECT_FALSE(sun().has_value());
  EXPECT_EQ(comp.device_reading_poll_policy_.get_failures(sun_key()), PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN);
}

TEST_F(EventPollHub, AnAnswerAfterMissesClearsTheCount) {
  miss_sun_poll_and_retry();
  EXPECT_EQ(comp.device_reading_poll_policy_.get_failures(sun_key()), 1u);

  queue_sun_answer();
  ASSERT_TRUE(comp.request_device_reading(DEVICE_ID, DeviceReading::SUN_LEVEL));

  EXPECT_EQ(comp.device_reading_poll_policy_.get_failures(sun_key()), 0u);
}

TEST_F(EventPollHub, AMissLeavesTheStatusPollLadderAloneAndCountsAsAnExchangeFailure) {
  comp.set_device_status_poll_interval(DEVICE_ID, 2000);
  const uint32_t next_update = comp.poll_policy_.get_next_update(DEVICE_ID);

  EXPECT_FALSE(comp.request_device_reading(DEVICE_ID, DeviceReading::SUN_LEVEL));

  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures(DEVICE_ID), 0u);
  EXPECT_EQ(comp.poll_policy_.get_next_update(DEVICE_ID), next_update);
  EXPECT_EQ(device().exchange_timeout_count, 1u);
}

TEST_F(EventPollHub, AnAnswerOfAnotherShapeIsAMissAndWarnsOnce) {
  IoFrame odd = captured(DAYLIGHT_CAPTURE, 1);
  odd.data[STATUS_UPDATE_EVENT_DATA_OFFSET] = 0x05;  // another sensor type byte
  radio.queue_rx(test::make_rx_packet(odd));

  EXPECT_FALSE(comp.request_device_reading(DEVICE_ID, DeviceReading::SUN_LEVEL));

  EXPECT_FALSE(sun().has_value());
  EXPECT_EQ(comp.device_reading_poll_policy_.get_failures(sun_key()), 1u)
      << "counted at once: asking again changes nothing";
  EXPECT_FALSE(comp.device_reading_poll_policy_.retry_armed(sun_key()));
  EXPECT_FALSE(comp.device_reading_poll_policy_.first_error_reply(sun_key())) << "the warning was spent on this answer";
}

TEST_F(EventPollHub, AnErrorReplyIsAMiss) {
  IoFrame error{};
  init_frame(error, true, false, true, false);
  set_dst(error, comp.node_id_);
  set_src(error, DEVICE_NODE_ID);
  const uint8_t payload[1] = {RESULT_COMMAND_UNKNOWN};
  set_cmd(error, CMD_ERROR_RESP, payload, sizeof(payload));
  radio.queue_rx(test::make_rx_packet(error));

  EXPECT_FALSE(comp.request_device_reading(DEVICE_ID, DeviceReading::BATTERY_LEVEL));

  EXPECT_FALSE(battery().has_value());
  EXPECT_EQ(comp.device_reading_poll_policy_.get_failures(battery_key()), 1u);
  EXPECT_FALSE(comp.device_reading_poll_policy_.retry_armed(battery_key())) << "a refusal is not retried";
  EXPECT_FALSE(comp.device_reading_poll_policy_.first_error_reply(battery_key()));
}

// ============================================================================
// Moving device and scheduling
// ============================================================================

TEST_F(EventPollHub, AMovingDeviceIsNotAskedAndThePollMovesForward) {
  esphome::test_clock::ManualClock clock;
  esphome::test_random::set(UINT32_MAX);
  comp.set_device_reading_poll_interval(DEVICE_ID, DeviceReading::SUN_LEVEL, SUN_INTERVAL_MS);
  device().is_stopped = false;
  const uint32_t now = esphome::millis();

  EXPECT_FALSE(comp.request_device_reading(DEVICE_ID, DeviceReading::SUN_LEVEL));

  EXPECT_TRUE(radio.get_sent_data().empty());
  EXPECT_EQ(comp.device_reading_poll_policy_.get_next_poll(sun_key()), now + PERIODIC_POLL_MOVING_RETRY_MS);
  EXPECT_EQ(comp.device_reading_poll_policy_.get_failures(sun_key()), 0u);
}

TEST_F(EventPollHub, AMovingDeviceIsNotAskedItsBatteryLevelEitherAndThePollMovesForward) {
  esphome::test_clock::ManualClock clock;
  esphome::test_random::set(UINT32_MAX);
  comp.set_device_reading_poll_interval(DEVICE_ID, DeviceReading::BATTERY_LEVEL, BATTERY_INTERVAL_MS);
  device().is_stopped = false;
  const uint32_t now = esphome::millis();

  EXPECT_FALSE(comp.request_device_reading(DEVICE_ID, DeviceReading::BATTERY_LEVEL));

  EXPECT_TRUE(radio.get_sent_data().empty());
  EXPECT_EQ(comp.device_reading_poll_policy_.get_next_poll(battery_key()), now + PERIODIC_POLL_MOVING_RETRY_MS);
  EXPECT_EQ(comp.device_reading_poll_policy_.get_failures(battery_key()), 0u);
}

TEST_F(EventPollHub, LoopQueuesEachReadingWhenItIsDueOneForEachPass) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_reading_poll_interval(DEVICE_ID, DeviceReading::SUN_LEVEL, SUN_INTERVAL_MS);
  comp.set_device_reading_poll_interval(DEVICE_ID, DeviceReading::BATTERY_LEVEL, BATTERY_INTERVAL_MS);
  esphome::test_clock::advance_ms(PERIODIC_POLL_INITIAL_DELAY_MS + PERIODIC_POLL_INITIAL_SPREAD_MS);

  comp.loop();
  ASSERT_EQ(comp.op_queue_.size(), 1u) << "the scheduled readings share one schedule, which hands out one per pass";
  const PendingOperation first = comp.op_queue_.front();
  comp.loop();
  ASSERT_FALSE(comp.op_queue_.empty());
  const PendingOperation second = comp.op_queue_.back();

  for (const PendingOperation &queued : {first, second}) {
    EXPECT_EQ(queued.type, PendingOperationType::REQUEST_DEVICE_READING);
    EXPECT_EQ(queued.device_id, DEVICE_ID);
  }
  const uint8_t sun = static_cast<uint8_t>(DeviceReading::SUN_LEVEL);
  const uint8_t battery = static_cast<uint8_t>(DeviceReading::BATTERY_LEVEL);
  EXPECT_TRUE((first.reading == sun && second.reading == battery) ||
              (first.reading == battery && second.reading == sun))
      << "each of the two readings was queued, once";
}

TEST_F(EventPollHub, AnEventReadingIsReadAtTheIntervalTheYamlGivesNotRaisedToADataObjectsMinimum) {
  constexpr uint32_t ONE_MINUTE_MS = 60 * 1000;
  static_assert(ONE_MINUTE_MS < DEVICE_READING_MIN_INTERVAL_MS,
                "the test needs an interval below a data object's floor");
  comp.set_device_reading_poll_interval(DEVICE_ID, DeviceReading::SUN_LEVEL, ONE_MINUTE_MS);

  EXPECT_EQ(comp.device_reading_poll_policy_.get_interval(sun_key()), ONE_MINUTE_MS);
}

TEST_F(EventPollHub, ACoverWithoutIntervalsIsNeverPolled) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_reading_poll_interval(DEVICE_ID, DeviceReading::SUN_LEVEL, 0);
  comp.set_device_reading_poll_interval(DEVICE_ID, DeviceReading::BATTERY_LEVEL, 0);
  esphome::test_clock::advance_ms(2 * BATTERY_INTERVAL_MS);

  comp.loop();

  EXPECT_TRUE(comp.op_queue_.empty());
}
