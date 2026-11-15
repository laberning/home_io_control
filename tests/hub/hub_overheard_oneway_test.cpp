/// @file hub_overheard_oneway_test.cpp
/// @brief Tests that a 1W remote press heard during the hub's own exchange is handled once the exchange
///        has ended, exactly like a press heard while idle (hub_status.cpp, oneway_receiver.h).
///
/// A MockRadio queues the press inside the wait of a status poll: the poll's exchange ignores it, the
/// hub keeps it, and the next loop() pass runs the duplicate filter, burst window, sender event and
/// linked-remote follow on it.

#include "hub_core.h"
#include "proto_commands.h"
#include "esphome/components/api/custom_api_device.h"
#include "esphome/core/application.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;

namespace {

constexpr const char *POLLED_DEVICE = "ABC123";  ///< The shutter the hub polls while the press is heard.
constexpr uint8_t POLLED_NODE_ID[3] = {0xAB, 0xC1, 0x23};
constexpr const char *LINKED_DEVICE = "054E17";  ///< Linked to the remote that is heard.
constexpr const char *REMOTE_ID_STR = "9D6085";
constexpr uint8_t REMOTE_ID[3] = {0x9D, 0x60, 0x85};

class TestableOverheardHub : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::on_overheard_frame;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::oneway_receiver_;
  using IOHomeControlComponent::process_pending_operation_;
  using IOHomeControlComponent::process_received_packet_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::system_key_;
};

class OverheardOneWayHub : public ::testing::Test {
 protected:
  void SetUp() override {
    api_server.reset();
    comp.node_id_[0] = 0xC0;
    comp.node_id_[1] = 0xFF;
    comp.node_id_[2] = 0xEE;
    static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                  0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
    std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
    comp.initialized_ = true;
    comp.radio_ = &radio;
    for (const char *id : {POLLED_DEVICE, LINKED_DEVICE}) {
      DeviceConfig cfg;
      cfg.type = DeviceType::ROLLER_SHUTTER;
      comp.add_device(id, cfg);
    }
    comp.add_linked_remote(REMOTE_ID_STR, LINKED_DEVICE);
    comp.add_exposed_sender(REMOTE_ID_STR);
  }

  /// One copy of a CLOSE press on the remote linked to LINKED_DEVICE.
  static IoFrame press() {
    IoFrame frame{};
    init_frame(frame, false, true, true, false);  // 1W
    const uint8_t dst[3] = {0x00, 0x01, 0xBF};
    set_src(frame, REMOTE_ID);
    set_dst(frame, dst);
    const uint8_t close[4] = {ORIGINATOR_USER_REMOTE, 0x41, 0xC8, 0x00};
    EXPECT_TRUE(set_cmd(frame, CMD_EXECUTE, close, sizeof(close)));
    return frame;
  }

  /// POLLED_DEVICE's answer to a status poll: at rest, fully closed.
  IoFrame status_reply() const {
    const uint8_t payload[8] = {STATUS_STOPPED, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
    IoFrame frame{};
    init_frame(frame, true, false, true, false);
    set_dst(frame, comp.node_id_);
    set_src(frame, POLLED_NODE_ID);
    EXPECT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));
    return frame;
  }

  /// Run one status poll of POLLED_DEVICE whose wait hears `copies` copies of the press first.
  void poll_hearing_press(int copies) {
    ASSERT_TRUE(comp.op_queue_.enqueue_request_status(POLLED_DEVICE));
    for (int i = 0; i < copies; i++)
      radio.queue_rx(test::make_rx_packet(press()));
    radio.queue_rx(test::make_rx_packet(status_reply()));
    comp.process_pending_operation_();
  }

  /// One loop() pass, plus the timers it set to fire at once.
  void loop_pass() {
    comp.loop();
    esphome::test_scheduler::run_until(esphome::millis());
  }

  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server{api_server};
  esphome::test_clock::ManualClock clock;
  TestableOverheardHub comp;
  MockRadio radio;
};

}  // namespace

TEST_F(OverheardOneWayHub, APressHeardDuringAPollIsHandledAfterTheExchange) {
  poll_hearing_press(1);
  ASSERT_TRUE(api_server.events_.empty()) << "nothing is handled while the exchange runs";
  ASSERT_TRUE(comp.oneway_receiver_.has_kept());

  loop_pass();

  ASSERT_EQ(api_server.events_.size(), 1u);
  EXPECT_EQ(api_server.events_.front().data.at("intent"), "CLOSE");
  EXPECT_EQ(api_server.events_.front().data.at("linked"), "true");
  EXPECT_FALSE(comp.oneway_receiver_.has_kept());
  // The linked-remote follow: the linked shutter is polled once the configured delay has passed.
  esphome::test_scheduler::run_until(esphome::millis() + REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS);
  ASSERT_EQ(comp.op_queue_.size(), 1u);
  EXPECT_EQ(comp.op_queue_.front().device_id, LINKED_DEVICE);
}

TEST_F(OverheardOneWayHub, CopiesHeardDuringAndAfterTheExchangeGiveOneEvent) {
  poll_hearing_press(2);
  loop_pass();
  // A later copy of the same burst, heard by the idle receiver.
  comp.process_received_packet_(test::make_rx_packet(press()));

  EXPECT_EQ(api_server.events_.size(), 1u);
}

TEST_F(OverheardOneWayHub, ATwoWayFrameFromAnotherDeviceIsNotKept) {
  IoFrame foreign = status_reply();
  const uint8_t other[3] = {0x11, 0x22, 0x33};
  set_src(foreign, other);

  comp.on_overheard_frame(foreign, test::make_rx_packet(foreign), esphome::millis());

  EXPECT_FALSE(comp.oneway_receiver_.has_kept());
}

TEST_F(OverheardOneWayHub, ThePressHeardInsideAnExchangeDefersTheNextBackgroundPoll) {
  poll_hearing_press(1);
  ASSERT_TRUE(comp.op_queue_.enqueue_request_status(POLLED_DEVICE));
  const size_t sent = radio.get_send_count();

  loop_pass();
  EXPECT_EQ(radio.get_send_count(), sent) << "the burst window holds the background poll back";

  esphome::test_clock::advance_ms(ONEWAY_QUIET_PERIOD_MS);
  loop_pass();
  EXPECT_GT(radio.get_send_count(), sent) << "and releases it once the burst is over";
}

TEST_F(OverheardOneWayHub, AKeptPressIsHandledAtItsArrivalTimeNotAtTheDrain) {
  // The burst window measures from the press's arrival: once a quiet period has passed between the
  // exchange and the next loop() pass, the burst is over and the background poll goes out. Handled at
  // the drain's own time instead, the press would hold the poll back for another quiet period.
  poll_hearing_press(1);
  ASSERT_TRUE(comp.op_queue_.enqueue_request_status(POLLED_DEVICE));
  const size_t sent = radio.get_send_count();
  esphome::test_clock::advance_ms(ONEWAY_QUIET_PERIOD_MS);

  loop_pass();

  EXPECT_EQ(api_server.events_.size(), 1u);
  EXPECT_GT(radio.get_send_count(), sent);
}

TEST_F(OverheardOneWayHub, AKeptPressCarriesTheRssiOfItsOwnCapture) {
  radio.set_last_capture_rssi(-71);  // What the wait's reception left in the radio's capture.
  const IoFrame frame = press();

  comp.on_overheard_frame(frame, test::make_rx_packet(frame), esphome::millis());

  const auto kept = comp.oneway_receiver_.take_kept();
  ASSERT_EQ(kept.size(), 1u);
  EXPECT_EQ(kept.front().rssi_dbm, -71);
}
