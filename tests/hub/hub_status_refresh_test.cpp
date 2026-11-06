/// @file hub_status_refresh_test.cpp
/// @brief Tests for the opt-in status refresh at rest: scheduling in loop(), and which reports move
///        the refresh out.
///
/// The refresh exists for a command the hub never heard: another controller's whole exchange with
/// the device can fall into a moment the hub was transmitting or listening on another channel, and
/// then nothing tells the hub the device moved.

#include "hub_core.h"
#include "proto_commands.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;

namespace {

constexpr const char *DEVICE_ID = "ABC123";
constexpr uint8_t DEVICE_NODE_ID[3] = {0xAB, 0xC1, 0x23};
constexpr uint32_t INTERVAL_MS = 900000;  ///< 15 minutes.
constexpr uint32_t FIRST_MS = 15000;      ///< The first refresh after the opt-in, at random 0.
constexpr uint32_t SHORTEST_MS = 810000;  ///< INTERVAL_MS less 10 %, the delay at random 0.
constexpr uint32_t LONGEST_MS = 990000;   ///< INTERVAL_MS plus 10 %, the delay at random UINT32_MAX.

class TestableStatusRefreshComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::busy_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::process_pending_operation_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::status_refresh_policy_;
  using IOHomeControlComponent::system_key_;
  using IOHomeControlComponent::update_device_status_;
};

/// One hub with a roller shutter "ABC123" and a mock radio that has nothing queued yet. A test opts
/// the shutter into the refresh itself, after starting its manual clock.
class StatusRefreshHub : public ::testing::Test {
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
    add_cover(DEVICE_ID);
  }

  void add_cover(const std::string &id) {
    DeviceConfig cfg;
    cfg.type = DeviceType::ROLLER_SHUTTER;
    comp.add_device(id, cfg);
    ASSERT_NE(comp.get_device(id), nullptr);
  }

  /// A 0x04 reply from the shutter: at rest, target and current 50 %.
  IoFrame status_reply() const {
    IoFrame frame{};
    init_frame(frame, true, false, true, false);
    set_dst(frame, comp.node_id_);
    set_src(frame, DEVICE_NODE_ID);
    const uint8_t payload[8] = {STATUS_STOPPED, 0x00, 0x64, 0x00, 0x64, 0x00, 0x00, 0x00};
    EXPECT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));
    return frame;
  }

  /// A 0x71 the shutter sends by itself: at rest, target and current 50 %.
  IoFrame status_update() const {
    IoFrame frame{};
    init_frame(frame, true, false, true, false);
    set_dst(frame, comp.node_id_);
    set_src(frame, DEVICE_NODE_ID);
    const uint8_t payload[11] = {STATUS_STOPPED, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x64, 0x00, 0x00, 0x00};
    EXPECT_TRUE(set_cmd(frame, CMD_STATUS_UPDATE, payload, sizeof(payload)));
    return frame;
  }

  TestableStatusRefreshComponent comp;
  MockRadio radio;
};

}  // namespace

// ============================================================================
// Scheduling in loop()
// ============================================================================

TEST_F(StatusRefreshHub, LoopQueuesAStatusReadWhenTheRefreshIsDue) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_status_refresh_interval(DEVICE_ID, INTERVAL_MS);
  esphome::test_clock::advance_ms(FIRST_MS);

  comp.loop();

  ASSERT_EQ(comp.op_queue_.size(), 1u);
  EXPECT_EQ(comp.op_queue_.front().type, PendingOperationType::REQUEST_STATUS);
  EXPECT_EQ(comp.op_queue_.front().device_id, DEVICE_ID);
}

TEST_F(StatusRefreshHub, LoopQueuesNothingBeforeTheRefreshIsDue) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_status_refresh_interval(DEVICE_ID, INTERVAL_MS);
  esphome::test_clock::advance_ms(FIRST_MS - 1);

  comp.loop();

  EXPECT_TRUE(comp.op_queue_.empty());
}

TEST_F(StatusRefreshHub, ACoverWithoutAnIntervalIsNeverRefreshed) {
  esphome::test_clock::ManualClock clock;
  add_cover("ABC124");
  comp.set_device_status_refresh_interval(DEVICE_ID, 0);
  esphome::test_clock::advance_ms(3 * INTERVAL_MS);

  comp.loop();

  EXPECT_TRUE(comp.op_queue_.empty());
  EXPECT_TRUE(radio.get_sent_data().empty());
}

TEST_F(StatusRefreshHub, AnUnknownDeviceIsNotScheduled) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_status_refresh_interval("ABC999", INTERVAL_MS);

  EXPECT_EQ(comp.status_refresh_policy_.get_interval("ABC999"), 0u);
}

TEST_F(StatusRefreshHub, ABusyRadioHoldsTheRefreshBack) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_status_refresh_interval(DEVICE_ID, INTERVAL_MS);
  esphome::test_clock::advance_ms(FIRST_MS);
  comp.busy_ = true;

  comp.loop();

  EXPECT_TRUE(comp.op_queue_.empty());
  EXPECT_EQ(comp.status_refresh_policy_.get_next_poll(DEVICE_ID), esphome::millis())
      << "still due once the radio is free";
}

// ============================================================================
// The refresh read
// ============================================================================

TEST_F(StatusRefreshHub, TheRefreshSendsAStatusReadAndItsAnswerMovesTheNextOneOut) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_status_refresh_interval(DEVICE_ID, INTERVAL_MS);
  esphome::test_clock::advance_ms(FIRST_MS);
  comp.loop();
  ASSERT_EQ(comp.op_queue_.size(), 1u);
  radio.queue_rx(test::make_rx_packet(status_reply()));
  esphome::test_random::set(UINT32_MAX);

  comp.process_pending_operation_();

  ASSERT_EQ(radio.get_sent_data().size(), 1u) << "one status read, answered at once";
  EXPECT_EQ(radio.get_sent_data().front()[8], CMD_PRIVATE);
  const IoDevice &dev = *comp.get_device(DEVICE_ID);
  EXPECT_FLOAT_EQ(dev.position, 50.0F);
  EXPECT_EQ(comp.status_refresh_policy_.get_next_poll(DEVICE_ID), dev.last_status + LONGEST_MS)
      << "the next refresh counts from the answer";
}

TEST_F(StatusRefreshHub, AnUnansweredRefreshIsTriedAgainAnIntervalLaterNotOnTheNextLoop) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_status_refresh_interval(DEVICE_ID, INTERVAL_MS);
  esphome::test_clock::advance_ms(FIRST_MS);
  const uint32_t popped_at = esphome::millis();
  comp.loop();
  ASSERT_EQ(comp.op_queue_.size(), 1u);

  comp.process_pending_operation_();  // nothing queued on the radio: no answer
  comp.loop();

  EXPECT_TRUE(comp.op_queue_.empty());
  EXPECT_EQ(comp.status_refresh_policy_.get_next_poll(DEVICE_ID), popped_at + SHORTEST_MS);
}

// ============================================================================
// Reports that move the refresh out
// ============================================================================

TEST_F(StatusRefreshHub, AStatusReplyMovesTheRefreshOut) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_status_refresh_interval(DEVICE_ID, INTERVAL_MS);
  esphome::test_clock::advance_ms(FIRST_MS - 10000);  // 10 s before the refresh

  comp.update_device_status_(status_reply());

  const uint32_t reported_at = esphome::millis();
  EXPECT_EQ(comp.status_refresh_policy_.get_next_poll(DEVICE_ID), reported_at + SHORTEST_MS);
  esphome::test_clock::advance_ms(10000);
  comp.loop();
  EXPECT_TRUE(comp.op_queue_.empty()) << "the device answered 10 s ago, no read is needed";
}

TEST_F(StatusRefreshHub, AStatusUpdateTheDeviceSendsByItselfMovesTheRefreshOut) {
  esphome::test_clock::ManualClock clock;
  comp.set_device_status_refresh_interval(DEVICE_ID, INTERVAL_MS);
  esphome::test_clock::advance_ms(FIRST_MS - 10000);

  comp.update_device_status_(status_update());

  EXPECT_EQ(comp.status_refresh_policy_.get_next_poll(DEVICE_ID), esphome::millis() + SHORTEST_MS);
}

TEST_F(StatusRefreshHub, TheAckToACommandDoesNotMoveTheRefreshOut) {
  // The ack's position bytes describe the request, not the device; the follow-up polls after the
  // command are the reports that count.
  esphome::test_clock::ManualClock clock;
  comp.set_device_status_refresh_interval(DEVICE_ID, INTERVAL_MS);
  const uint32_t next_refresh = comp.status_refresh_policy_.get_next_poll(DEVICE_ID);
  esphome::test_clock::advance_ms(FIRST_MS - 10000);

  comp.update_device_status_(status_reply(), /*trust_position=*/false);

  EXPECT_EQ(comp.status_refresh_policy_.get_next_poll(DEVICE_ID), next_refresh);
}

TEST_F(StatusRefreshHub, AReportFromACoverWithoutTheOptionSchedulesNothing) {
  esphome::test_clock::ManualClock clock;

  comp.update_device_status_(status_reply());

  EXPECT_EQ(comp.status_refresh_policy_.get_next_poll(DEVICE_ID), 0u);
  esphome::test_clock::advance_ms(3 * INTERVAL_MS);
  comp.loop();
  EXPECT_TRUE(comp.op_queue_.empty());
}
