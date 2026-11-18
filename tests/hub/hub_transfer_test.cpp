/// @file hub_transfer_test.cpp
/// @brief The hub's side of the queued transfer reader (ADR 0099): the one predicate for background radio work,
/// the foreign-transfer feed from the receive and overheard paths, and the dispatcher keeping a device's background
/// operations queued while its transfer is open.

#include "hub_core.h"
#include "proto_commands.h"
#include "proto_frame.h"
#include "proto_timing.h"

#include "esphome/core/application.h"

#include "stubs/radio_test_common.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <vector>

using namespace esphome::home_io_control;
using test::make_rx_packet;
using test::TestableHubComponent;

namespace {

const uint8_t DEVICE_ID[NODE_ID_SIZE] = {0xAB, 0xC1, 0x23};
const uint8_t OTHER_CONTROLLER[NODE_ID_SIZE] = {0x12, 0x34, 0x56};

/// A hub with two registered devices on a MockRadio, initialised, with the clock under the test's control.
struct HubRig {
  esphome::test_clock::ManualClock clock{200000};
  TestableHubComponent comp;
  MockRadio radio;

  HubRig() {
    comp.node_id_[0] = 0xC0;
    comp.node_id_[1] = 0xFF;
    comp.node_id_[2] = 0xEE;
    static const uint8_t key[] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                  0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};
    std::memcpy(comp.system_key_, key, AES_KEY_SIZE);
    comp.initialized_ = true;
    comp.radio_ = &radio;
    comp.add_device("ABC123");
    comp.add_device("DEF456");
    esphome::test_scheduler::clear();
  }
  ~HubRig() { esphome::test_scheduler::clear(); }

  void run_for(uint32_t ms) { esphome::test_scheduler::run_until(esphome::millis() + ms); }
};

IoFrame transfer_frame(uint8_t cmd, const uint8_t src[NODE_ID_SIZE], const uint8_t dst[NODE_ID_SIZE]) {
  IoFrame f{};
  init_frame(f, true, false, true, false);
  set_src(f, src);
  set_dst(f, dst);
  const uint8_t payload[] = {0x08, 0x00, 0x00, 0x80, 0x03, 0x00, 0x00, 0x00, 0x00};
  EXPECT_TRUE(set_cmd(f, cmd, payload, sizeof(payload)));
  return f;
}

}  // namespace

// === One predicate for background radio work ===

TEST(HubTransfer, BackgroundRadioIsAllowedWhenNothingHoldsTheRadio) {
  HubRig rig;
  EXPECT_TRUE(rig.comp.background_radio_allowed_());
}

TEST(HubTransfer, EachConditionThatHoldsTheRadioStopsBackgroundWorkAndTheLoopsGateAgrees) {
  struct Case {
    const char *name;
    void (*hold)(TestableHubComponent &);
  };
  const Case cases[] = {
      {"an exchange in progress", [](TestableHubComponent &c) { c.busy_ = true; }},
      {"radio test mode", [](TestableHubComponent &c) { c.set_radio_test_mode(true); }},
      {"a key-extraction attempt waiting for its peer",
       [](TestableHubComponent &c) {
         c.key_extraction_.key_extraction_ctx_.state = pairing_responder::ResponderState::SENT_CHALLENGE;
         c.key_extraction_.key_extraction_hold_deadline_ms_ = esphome::millis() + 5000;
       }},
      {"a linked 1W remote transmitting",
       [](TestableHubComponent &c) { c.oneway_receiver_.note_activity(esphome::millis()); }},
  };
  for (const Case &c : cases) {
    SCOPED_TRACE(c.name);
    HubRig rig;
    ASSERT_TRUE(rig.comp.op_queue_.enqueue_request_status("ABC123"));
    ASSERT_TRUE(rig.comp.background_radio_allowed_()) << "precondition";
    EXPECT_FALSE(rig.comp.defer_background_poll_());

    c.hold(rig.comp);

    EXPECT_FALSE(rig.comp.background_radio_allowed_());
    // The loop's gate for a queued background operation is the same predicate; a user command never waits on it.
    EXPECT_TRUE(rig.comp.defer_background_poll_())
        << "a queued poll yields exactly when background work is not allowed";
    rig.comp.op_queue_.enqueue_device_command("ABC123", CoverCommand::STOP);
    EXPECT_FALSE(rig.comp.defer_background_poll_()) << "a control operation is never deferred";
  }
}

TEST(HubTransfer, TheTransferReaderWaitsOnThePredicateTheOperationQueueUses) {
  HubRig rig;
  rig.comp.set_radio_test_mode(true);
  TransferOutcome outcome = TransferOutcome::READ;
  bool ended = false;
  rig.comp.transfer_reader_.read("ABC123", 0, 0, [&](TransferOutcome o, const std::vector<uint8_t> &) {
    outcome = o;
    ended = true;
  });
  rig.run_for(3000);
  EXPECT_EQ(rig.radio.get_send_count(), 0) << "no transmission while the predicate says no";
  EXPECT_FALSE(ended);

  rig.comp.set_radio_test_mode(false);
  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    rig.radio.queue_tx_result(false);
  rig.run_for(1000);
  EXPECT_GT(rig.radio.get_send_count(), 0) << "the open goes out once the radio is free";
  EXPECT_TRUE(ended);
  EXPECT_EQ(outcome, TransferOutcome::NO_ANSWER);
}

// === Another controller's transfers ===

TEST(HubTransfer, AForeignTransferFrameTheHubReceivesHoldsTheReadsOfThatDevice) {
  HubRig rig;
  const IoDevice *dev = rig.comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  rig.comp.process_received_packet_(make_rx_packet(transfer_frame(CMD_TRANSFER_OPEN, OTHER_CONTROLLER, dev->node_id)));

  rig.comp.transfer_reader_.read("ABC123", 0, 0, nullptr);
  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    rig.radio.queue_tx_result(false);
  rig.run_for(TRANSFER_FOREIGN_QUIET_MS - 1000);
  EXPECT_EQ(rig.radio.get_send_count(), 0);
  rig.run_for(2000);
  EXPECT_GT(rig.radio.get_send_count(), 0);
}

TEST(HubTransfer, AForeignTransferFrameAnExchangeWaitOverheardHoldsTheReadsToo) {
  HubRig rig;
  const IoDevice *dev = rig.comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  const IoFrame frame = transfer_frame(CMD_UNKNOWN4A_REQ, OTHER_CONTROLLER, dev->node_id);
  rig.comp.on_overheard_frame(frame, make_rx_packet(frame), -80, esphome::millis());

  rig.comp.transfer_reader_.read("ABC123", 0, 0, nullptr);
  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    rig.radio.queue_tx_result(false);
  rig.run_for(TRANSFER_FOREIGN_QUIET_MS - 1000);
  EXPECT_EQ(rig.radio.get_send_count(), 0);
}

TEST(HubTransfer, ATransferFrameToTheHubItselfDoesNotHoldTheReads) {
  HubRig rig;
  const IoDevice *dev = rig.comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  rig.comp.process_received_packet_(
      make_rx_packet(transfer_frame(CMD_UNKNOWN4A_RESP, dev->node_id, rig.comp.node_id_)));

  rig.comp.transfer_reader_.read("ABC123", 0, 0, nullptr);
  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    rig.radio.queue_tx_result(false);
  rig.run_for(1000);
  EXPECT_GT(rig.radio.get_send_count(), 0);
}

// === Queued background operations wait while a device's transfer is open ===

TEST(HubTransfer, ADevicesQueuedBackgroundOperationsWaitWhileItsTransferIsOpen) {
  HubRig rig;
  // The device accepts the open of object row 2 (4 bytes), so the transfer stays open between steps.
  IoFrame open_answer{};
  init_frame(open_answer, true, false, true, false);
  set_src(open_answer, DEVICE_ID);
  set_dst(open_answer, rig.comp.node_id_);
  const uint8_t payload[] = {0x10, 0x00, 0x00, 0x04};
  ASSERT_TRUE(set_cmd(open_answer, CMD_TRANSFER_OPEN_RESP, payload, sizeof(payload)));
  rig.radio.queue_rx(make_rx_packet(open_answer));

  rig.comp.transfer_reader_.read("ABC123", 2, 0, nullptr);
  rig.run_for(10);
  ASSERT_TRUE(rig.comp.transfer_reader_.transfer_open_to("ABC123")) << "precondition: the open was answered";

  ASSERT_TRUE(rig.comp.op_queue_.enqueue_request_status("ABC123"));
  const int sent_before = rig.radio.get_send_count();
  rig.comp.process_pending_operation_();
  EXPECT_EQ(rig.comp.op_queue_.size(), 1u) << "the status poll of the device in transfer stays queued";
  EXPECT_EQ(rig.radio.get_send_count(), sent_before) << "and nothing is sent to it";

  // A poll of another device behind it is not held: it is dispatched past the held one.
  ASSERT_TRUE(rig.comp.op_queue_.enqueue_request_status("DEF456"));
  for (size_t i = 0; i < EXCHANGE_MAX_ATTEMPTS; ++i)
    rig.radio.queue_tx_result(false);
  rig.comp.process_pending_operation_();
  ASSERT_EQ(rig.comp.op_queue_.size(), 1u);
  EXPECT_EQ(rig.comp.op_queue_.front().device_id, "ABC123") << "the held poll keeps its place";
  EXPECT_GT(rig.radio.get_send_count(), sent_before) << "the other device's poll went out";

  // Once the read has ended (its next block goes unanswered and it closes), the held poll is dispatched.
  for (size_t i = 0; i < 3 * EXCHANGE_MAX_ATTEMPTS; ++i)
    rig.radio.queue_tx_result(false);
  rig.run_for(10000);
  ASSERT_FALSE(rig.comp.transfer_reader_.transfer_open_to("ABC123"));
  rig.comp.process_pending_operation_();
  EXPECT_TRUE(rig.comp.op_queue_.empty());
}
