/// @file hub_rain_poll_test.cpp
/// @brief Tests for the opt-in rain sensor poll: scheduling in loop(), the limitation read, and how
///        its outcome reaches the device record.
///
/// The wet reply with originator and time 00 is the one a real window sent under rain; the one with
/// originator 02 and a running timer is constructed.

#include "hub_core.h"
#include "proto_commands.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;

namespace {

constexpr const char *DEVICE_ID = "ABC123";
constexpr uint8_t DEVICE_NODE_ID[3] = {0xAB, 0xC1, 0x23};
constexpr uint32_t POLL_INTERVAL_MS = 15 * 60 * 1000;

class TestableRainPollComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::busy_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::poll_policy_;
  using IOHomeControlComponent::process_pending_operation_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::rain_poll_policy_;
  using IOHomeControlComponent::system_key_;
};

/// One hub with a roller shutter "ABC123" opted into the rain poll, and a mock radio that has
/// nothing queued yet.
class RainPollHub : public ::testing::Test {
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
    add_cover(DEVICE_ID, /*low_power=*/false);
    comp.set_device_rain_poll_interval(DEVICE_ID, POLL_INTERVAL_MS);
    comp.register_device_callback([this](const std::string &, const IoDevice &) { notifications++; });
  }

  void add_cover(const std::string &id, bool low_power) {
    DeviceConfig cfg;
    cfg.type = DeviceType::ROLLER_SHUTTER;
    cfg.low_power = low_power;
    comp.add_device(id, cfg);
    ASSERT_NE(comp.get_device(id), nullptr);
  }

  IoDevice &device() { return *comp.get_device(DEVICE_ID); }

  /// Queue a minimum-limitation reply from the device.
  void queue_limitation_reply(uint16_t value_raw, uint8_t originator = 0, uint8_t time_raw = 0) {
    radio.queue_rx(test::make_rx_packet(
        test::make_limitation_reply(comp.node_id_, DEVICE_NODE_ID, value_raw, originator, time_raw)));
  }

  /// Poll once with a reply queued, expecting the exchange to complete.
  void poll_with_reply(uint16_t value_raw, uint8_t originator = 0, uint8_t time_raw = 0) {
    queue_limitation_reply(value_raw, originator, time_raw);
    ASSERT_TRUE(comp.request_device_limitation(DEVICE_ID));
  }

  TestableRainPollComponent comp;
  MockRadio radio;
  int notifications{0};
};

}  // namespace

// ============================================================================
// Reading the limitation
// ============================================================================

struct ReplyCase {
  const char *name;
  uint16_t value_raw;
  uint8_t originator;
  uint8_t time_raw;
  RainSensorState expected;
};

class RainPollReply : public RainPollHub, public ::testing::WithParamInterface<ReplyCase> {};

TEST_P(RainPollReply, SetsTheRainStateFromTheMinimumLimit) {
  const ReplyCase &c = GetParam();
  poll_with_reply(c.value_raw, c.originator, c.time_raw);
  EXPECT_EQ(device().rain_sensor, c.expected);
}

INSTANTIATE_TEST_SUITE_P(
    HubRainPoll, RainPollReply,
    ::testing::Values(ReplyCase{"dry_reply_heard_from_a_window", 0x0000, 0x00, 0x00, RainSensorState::DRY},
                      ReplyCase{"one_raw_unit_below_the_threshold", 0xB1FF, 0x00, 0x00, RainSensorState::DRY},
                      ReplyCase{"boundary_89_percent", 0xB200, 0x00, 0x00, RainSensorState::RAIN},
                      ReplyCase{"wet_reply_heard_from_a_window", 0xBA00, 0x00, 0x00, RainSensorState::RAIN},
                      // Constructed: originator and timer set, which no real reply has shown.
                      ReplyCase{"synthetic_wet_reply", 0xBA00, 0x02, 0x1D, RainSensorState::RAIN},
                      ReplyCase{"fully_limited", 0xC800, 0x02, 0x1D, RainSensorState::RAIN},
                      ReplyCase{"high_value_is_rain_whoever_set_it", 0xBA00, 0x00, 0x00, RainSensorState::RAIN}),
    [](const ::testing::TestParamInfo<ReplyCase> &info) { return std::string(info.param.name); });

TEST_F(RainPollHub, AnAnsweredPollStampsLinkHealthAndNotifiesSubscribers) {
  ASSERT_EQ(device().last_seen_ms, 0u);

  poll_with_reply(0x0000);

  EXPECT_NE(device().last_seen_ms, 0u);
  EXPECT_GE(notifications, 1);
}

TEST_F(RainPollHub, SendsExactlyTheMinimumLimitationRequest) {
  poll_with_reply(0x0000);

  ASSERT_EQ(radio.get_sent_data().size(), 1u) << "one start frame, no challenge, no maximum-limit read";
  const auto &tx = radio.get_sent_data().front();
  ASSERT_GE(tx.size(), 12u);
  EXPECT_EQ(tx[8], CMD_LIMITATION_STATUS_REQ);
  EXPECT_EQ(tx[9], 0x80) << "minimum limit selector";
  EXPECT_EQ(tx[10], 0x00);
  EXPECT_EQ(tx[11], 0x00);
  EXPECT_EQ(tx[1] & CTRL1_LOW_POWER, 0) << "an always-alive device is not woken with the low-power flag";
}

TEST_F(RainPollHub, LowPowerDeviceGetsTheLowPowerFlag) {
  add_cover("ABC124", /*low_power=*/true);
  comp.set_device_rain_poll_interval("ABC124", POLL_INTERVAL_MS);

  comp.request_device_limitation("ABC124");

  ASSERT_FALSE(radio.get_sent_data().empty());
  EXPECT_NE(radio.get_sent_data().front()[1] & CTRL1_LOW_POWER, 0);
}

TEST_F(RainPollHub, ARainPollNeverMovesPositionTargetOrMotionState) {
  device().position = 40.0f;
  device().target = 40.0f;
  device().is_stopped = true;

  poll_with_reply(0xBA00, 0x02, 0x1D);

  EXPECT_EQ(device().position, 40.0f);
  EXPECT_EQ(device().target, 40.0f);
  EXPECT_TRUE(device().is_stopped);
}

TEST_F(RainPollHub, ThePollIsIndependentOfTheDerivedRainFlag) {
  poll_with_reply(0xBA00, 0x02, 0x1D);
  EXPECT_FALSE(device().limited_by_rain) << "a rain reading does not set the status-derived flag";

  device().limited_by_rain = true;
  poll_with_reply(0x0000);
  EXPECT_TRUE(device().limited_by_rain) << "a dry reading does not clear it";
}

TEST_F(RainPollHub, UnknownDeviceAndUninitializedHubReadNothing) {
  EXPECT_FALSE(comp.request_device_limitation("000000"));
  comp.initialized_ = false;
  EXPECT_FALSE(comp.request_device_limitation(DEVICE_ID));
  EXPECT_TRUE(radio.get_sent_data().empty());
}

// ============================================================================
// Misses
// ============================================================================

TEST_F(RainPollHub, OneMissKeepsTheLastReading) {
  poll_with_reply(0xBA00, 0x02, 0x1D);
  ASSERT_EQ(device().rain_sensor, RainSensorState::RAIN);

  EXPECT_FALSE(comp.request_device_limitation(DEVICE_ID)) << "nothing queued: the device stays silent";

  EXPECT_EQ(device().rain_sensor, RainSensorState::RAIN);
  EXPECT_EQ(comp.rain_poll_policy_.get_failures(DEVICE_ID), 1u);
}

TEST_F(RainPollHub, ThreeMissesInARowMakeTheReadingUnknown) {
  poll_with_reply(0xBA00, 0x02, 0x1D);

  for (int i = 0; i < RAIN_POLL_FAILURES_BEFORE_UNKNOWN - 1; ++i) {
    EXPECT_FALSE(comp.request_device_limitation(DEVICE_ID));
    EXPECT_EQ(device().rain_sensor, RainSensorState::RAIN) << "miss " << i + 1;
  }
  EXPECT_FALSE(comp.request_device_limitation(DEVICE_ID));

  EXPECT_EQ(device().rain_sensor, RainSensorState::UNKNOWN);
}

TEST_F(RainPollHub, AnAnswerAfterMissesSetsTheStateAndClearsTheCount) {
  EXPECT_FALSE(comp.request_device_limitation(DEVICE_ID));
  EXPECT_FALSE(comp.request_device_limitation(DEVICE_ID));

  poll_with_reply(0x0000);

  EXPECT_EQ(device().rain_sensor, RainSensorState::DRY);
  EXPECT_EQ(comp.rain_poll_policy_.get_failures(DEVICE_ID), 0u);
}

TEST_F(RainPollHub, AMissLeavesTheStatusPollLadderAlone) {
  comp.set_device_status_poll_interval(DEVICE_ID, 2000);
  const uint32_t next_update = comp.poll_policy_.get_next_update(DEVICE_ID);

  EXPECT_FALSE(comp.request_device_limitation(DEVICE_ID));

  EXPECT_EQ(comp.poll_policy_.get_status_poll_failures(DEVICE_ID), 0u);
  EXPECT_EQ(comp.poll_policy_.get_auth_poll_failures(DEVICE_ID), 0u);
  EXPECT_EQ(comp.poll_policy_.get_next_update(DEVICE_ID), next_update);
}

TEST_F(RainPollHub, AMissCountsInTheExchangeFailureDiagnostics) {
  ASSERT_EQ(device().exchange_timeout_count, 0u);

  EXPECT_FALSE(comp.request_device_limitation(DEVICE_ID));

  EXPECT_EQ(device().exchange_timeout_count, 1u) << "a missed rain poll is an unanswered request like any other";
}

TEST_F(RainPollHub, AnErrorReplyIsAMissAndIsRecorded) {
  IoFrame error{};
  init_frame(error, true, false, true, false);
  set_dst(error, comp.node_id_);
  set_src(error, DEVICE_NODE_ID);
  const uint8_t payload[1] = {RESULT_ERROR_DURING_EXECUTION};
  set_cmd(error, CMD_ERROR_RESP, payload, sizeof(payload));
  radio.queue_rx(test::make_rx_packet(error));

  EXPECT_FALSE(comp.request_device_limitation(DEVICE_ID));

  EXPECT_EQ(device().rain_sensor, RainSensorState::UNKNOWN);
  EXPECT_EQ(comp.rain_poll_policy_.get_failures(DEVICE_ID), 1u);
  EXPECT_EQ(device().last_result_code, RESULT_ERROR_DURING_EXECUTION);
  EXPECT_FALSE(comp.rain_poll_policy_.first_error_reply(DEVICE_ID)) << "the warning was spent on this reply";
}

TEST_F(RainPollHub, AnotherCommandAnswersAsAMissAndIsNeverAppliedAsStatus) {
  device().position = 40.0f;
  IoFrame status{};
  init_frame(status, true, false, true, false);
  set_dst(status, comp.node_id_);
  set_src(status, DEVICE_NODE_ID);
  const uint8_t payload[8] = {STATUS_STOPPED, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  set_cmd(status, CMD_PRIVATE_RESP, payload, sizeof(payload));
  radio.queue_rx(test::make_rx_packet(status));

  EXPECT_FALSE(comp.request_device_limitation(DEVICE_ID));

  EXPECT_EQ(comp.rain_poll_policy_.get_failures(DEVICE_ID), 1u);
  EXPECT_EQ(device().position, 40.0f) << "a limitation read's reply is never parsed as a status";
}

TEST_F(RainPollHub, ALimitationReplyForAnotherParameterIsAMiss) {
  radio.queue_rx(test::make_rx_packet(
      test::make_limitation_reply(comp.node_id_, DEVICE_NODE_ID, 0xBA00, 0x02, 0x1D, LIMITATION_PARAM_FP_FIRST)));

  EXPECT_FALSE(comp.request_device_limitation(DEVICE_ID));

  EXPECT_EQ(device().rain_sensor, RainSensorState::UNKNOWN);
  EXPECT_EQ(comp.rain_poll_policy_.get_failures(DEVICE_ID), 1u);
}

// ============================================================================
// Moving device
// ============================================================================

TEST_F(RainPollHub, AMovingDeviceIsNotAskedAndTheNextPollMovesForward) {
  esphome::test_clock::ManualClock clock;
  esphome::test_random::set(UINT32_MAX);
  comp.set_device_rain_poll_interval(DEVICE_ID, POLL_INTERVAL_MS);  // first poll at the far end: 45 s away
  device().is_stopped = false;
  const uint32_t now = esphome::millis();

  EXPECT_FALSE(comp.request_device_limitation(DEVICE_ID));

  EXPECT_TRUE(radio.get_sent_data().empty()) << "nothing is transmitted to a moving device";
  EXPECT_EQ(comp.rain_poll_policy_.get_next_poll(DEVICE_ID), now + RAIN_POLL_MOVING_RETRY_MS);
  EXPECT_EQ(comp.rain_poll_policy_.get_failures(DEVICE_ID), 0u) << "waiting for a manoeuvre is not a miss";
}

// ============================================================================
// Scheduling in loop()
// ============================================================================

TEST_F(RainPollHub, LoopQueuesALimitationReadWhenThePollIsDue) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_rain_poll_interval(DEVICE_ID, POLL_INTERVAL_MS);
  esphome::test_clock::advance_ms(RAIN_POLL_INITIAL_DELAY_MS + RAIN_POLL_INITIAL_SPREAD_MS);

  comp.loop();

  ASSERT_EQ(comp.op_queue_.size(), 1u);
  EXPECT_EQ(comp.op_queue_.front().type, PendingOperationType::REQUEST_LIMITATION);
  EXPECT_EQ(comp.op_queue_.front().device_id, DEVICE_ID);
}

TEST_F(RainPollHub, LoopQueuesNothingBeforeThePollIsDue) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_rain_poll_interval(DEVICE_ID, POLL_INTERVAL_MS);
  esphome::test_clock::advance_ms(RAIN_POLL_INITIAL_DELAY_MS - 1);

  comp.loop();

  EXPECT_TRUE(comp.op_queue_.empty());
}

TEST_F(RainPollHub, ACoverWithoutAnIntervalIsNeverPolled) {
  esphome::test_clock::ManualClock clock;
  add_cover("ABC124", /*low_power=*/false);
  comp.set_device_rain_poll_interval(DEVICE_ID, 0);
  esphome::test_clock::advance_ms(3 * POLL_INTERVAL_MS);

  comp.loop();

  EXPECT_TRUE(comp.op_queue_.empty());
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST_F(RainPollHub, TwoCoversWithTheSameIntervalStartAndRepeatAtDifferentTimes) {
  esphome::test_clock::ManualClock clock;
  add_cover("ABC124", /*low_power=*/false);
  esphome::test_random::set(0);
  comp.set_device_rain_poll_interval(DEVICE_ID, POLL_INTERVAL_MS);
  esphome::test_random::set(UINT32_MAX);
  comp.set_device_rain_poll_interval("ABC124", POLL_INTERVAL_MS);
  EXPECT_NE(comp.rain_poll_policy_.get_next_poll(DEVICE_ID), comp.rain_poll_policy_.get_next_poll("ABC124"));

  esphome::test_clock::advance_ms(RAIN_POLL_INITIAL_DELAY_MS + RAIN_POLL_INITIAL_SPREAD_MS);
  esphome::test_random::set(0);
  comp.loop();
  esphome::test_random::set(UINT32_MAX);
  comp.loop();

  // loop() also dispatches a queued read, so the proof that both came due is that both re-armed.
  const uint32_t now = esphome::millis();
  EXPECT_GT(comp.rain_poll_policy_.get_next_poll(DEVICE_ID), now);
  EXPECT_GT(comp.rain_poll_policy_.get_next_poll("ABC124"), now);
  EXPECT_NE(comp.rain_poll_policy_.get_next_poll(DEVICE_ID), comp.rain_poll_policy_.get_next_poll("ABC124"));
}

TEST_F(RainPollHub, DispatchRunsTheLimitationRead) {
  comp.op_queue_.enqueue_request_limitation(DEVICE_ID);
  queue_limitation_reply(0xBA00, 0x02, 0x1D);

  comp.process_pending_operation_();

  EXPECT_EQ(device().rain_sensor, RainSensorState::RAIN);
}
