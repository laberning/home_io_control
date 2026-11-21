/// @file hub_oneway_follow_test.cpp
/// @brief Tests that the hub follows its own one-way command on the devices its identity drives
///        (`drives:`, ADR 0116): the shown state at once, a status read after the remote-activity delay.
///
/// A queued one-way operation is run by `loop()` on a `MockRadio` behind the real exchange engine and the
/// real transmitter, whose report reaches the hub through `setup()`'s wiring, so the whole path from the
/// action to the device registry is the production one.
/// @ingroup hioc_hub

#include "hub_core.h"
#include "oneway_controller.h"
#include "proto_commands.h"
#include "proto_frame.h"

#include "esphome/components/api/custom_api_device.h"
#include "esphome/core/application.h"
#include "esphome/core/preferences.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;

namespace {

constexpr uint8_t IDENTITY_NODE[NODE_ID_SIZE] = {0x9D, 0x60, 0x85};
constexpr const char *IDENTITY_NODE_STR = "9D6085";
constexpr const char *IDENTITY = "remote";
constexpr const char *DRIVEN = "ABC123";
constexpr const char *ALSO_DRIVEN = "ABC124";
constexpr const char *NOT_DRIVEN = "ABC125";
constexpr const char *CLASS_LINKED = "ABC126";

class FollowHub : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::radio_;
  /// setup() stops at the unknown radio type and marks the hub failed, and a failed component's timers do not
  /// run; the follow-up's status reads are timers, so the test lifts the mark once the wiring is done.
  void lift_setup_failure() { this->failed_ = false; }
};

class OwnOneWayFollow : public ::testing::Test {
 protected:
  void SetUp() override {
    esphome::test_preferences::wipe();
    esphome::test_hal::reset_delays();
    api_server.reset();
    radio.set_rssi_default(-100);  // clear channel
    hub.set_node_id("C0FFEE");
    hub.set_system_key("00112233445566778899AABBCCDDEEFF");
    hub.set_radio_type("not_a_chip");  // setup() wires the transmitter's report, then stops at the radio
  }
  void TearDown() override { esphome::test_preferences::wipe(); }

  /// Register the identity as a controller of `type`, then run setup() and hand the hub the mock radio.
  void start(DeviceType type, std::optional<bool> invert = std::nullopt, bool broadcast_all = false) {
    OneWayControllerIdentity identity{};
    identity.id = IDENTITY;
    std::memcpy(identity.node_id, IDENTITY_NODE, NODE_ID_SIZE);
    std::memset(identity.system_key, 0x11, AES_KEY_SIZE);
    identity.io_device_type = type;
    identity.power_class_override = OneWayPowerClass::ALWAYS_ALIVE;
    identity.invert_override = invert;
    identity.execute_broadcast_all = broadcast_all;
    hub.add_oneway_controller(identity);
    hub.setup();
    hub.lift_setup_failure();
    hub.initialized_ = true;
    hub.radio_ = &radio;
  }

  void add_device(const char *id, DeviceType type) {
    DeviceConfig config;
    config.type = type;
    hub.add_device(id, config);
  }

  /// One status-read operation per driven device, once the delay has passed.
  size_t polls_queued_after(uint32_t delay_ms) {
    esphome::test_scheduler::run_until(esphome::millis() + delay_ms);
    return hub.op_queue_.size();
  }

  const IoDevice &device(const char *id) { return *hub.get_device(id); }

  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server{api_server};
  esphome::test_clock::ManualClock clock;
  FollowHub hub;
  MockRadio radio;
};

}  // namespace

TEST_F(OwnOneWayFollow, ACloseShowsTheDrivenDeviceMovingAtOnceAndReadsItAfterTheRemoteActivityDelay) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING);
  add_device(DRIVEN, DeviceType::VERTICAL_EXTERIOR_AWNING);
  hub.add_oneway_driven_device(IDENTITY_NODE_STR, DRIVEN);

  hub.send_oneway_position(IDENTITY, ONEWAY_POSITION_FULLY_CLOSED);
  hub.loop();

  EXPECT_EQ(device(DRIVEN).optimistic.target, 100.0f) << "close is the fully closed IO position";
  EXPECT_EQ(device(DRIVEN).optimistic.motion, OptimisticState::Motion::MOVING);
  EXPECT_TRUE(hub.op_queue_.empty()) << "the read waits for the delay";
  ASSERT_EQ(polls_queued_after(REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS), 1u);
  EXPECT_EQ(hub.op_queue_.front().type, PendingOperationType::REQUEST_STATUS);
  EXPECT_EQ(hub.op_queue_.front().device_id, DRIVEN);
}

TEST_F(OwnOneWayFollow, AStopShowsTheDrivenDeviceStoppedAndReadsItAtOnce) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING);
  add_device(DRIVEN, DeviceType::VERTICAL_EXTERIOR_AWNING);
  hub.add_oneway_driven_device(IDENTITY_NODE_STR, DRIVEN);
  hub.send_oneway_position(IDENTITY, ONEWAY_POSITION_FULLY_CLOSED);
  hub.loop();
  ASSERT_EQ(device(DRIVEN).optimistic.motion, OptimisticState::Motion::MOVING) << "travelling";

  hub.send_oneway_command(IDENTITY, CoverCommand::STOP);
  hub.loop();

  EXPECT_EQ(device(DRIVEN).optimistic.motion, OptimisticState::Motion::STOPPED);
  EXPECT_EQ(device(DRIVEN).optimistic.target, UNKNOWN_POSITION);
  EXPECT_EQ(polls_queued_after(0), 1u) << "a stop is read without the delay";
  EXPECT_EQ(hub.op_queue_.front().device_id, DRIVEN);
}

TEST_F(OwnOneWayFollow, APositionIsShownAsTheTargetOfEveryDrivenDeviceAndNoOtherDevice) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING);
  for (const char *id : {DRIVEN, ALSO_DRIVEN, NOT_DRIVEN, CLASS_LINKED})
    add_device(id, DeviceType::VERTICAL_EXTERIOR_AWNING);
  hub.add_oneway_driven_device(IDENTITY_NODE_STR, DRIVEN);
  hub.add_oneway_driven_device(IDENTITY_NODE_STR, ALSO_DRIVEN);
  hub.add_linked_remote_class(DeviceType::VERTICAL_EXTERIOR_AWNING, CLASS_LINKED);

  hub.send_oneway_position(IDENTITY, 40);
  hub.loop();

  EXPECT_EQ(device(DRIVEN).optimistic.target, 40.0f);
  EXPECT_EQ(device(ALSO_DRIVEN).optimistic.target, 40.0f);
  EXPECT_EQ(device(NOT_DRIVEN).optimistic.target, UNKNOWN_POSITION);
  EXPECT_EQ(device(CLASS_LINKED).optimistic.target, UNKNOWN_POSITION)
      << "a device linked to the class by a real remote is not this identity's to move";
  EXPECT_EQ(polls_queued_after(REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS), 2u);
}

TEST_F(OwnOneWayFollow, AnInvertedKindShowsTheSameTargetAsItsOwnTwoWayCloseWouldSet) {
  // A horizontal awning is closed at IO 0. The identity mirrors CLOSE to wire 0, and the follow-up reads
  // the wire value, so the shown target matches the device's own two-way close.
  start(DeviceType::HORIZONTAL_AWNING);
  add_device(DRIVEN, DeviceType::HORIZONTAL_AWNING);
  hub.add_oneway_driven_device(IDENTITY_NODE_STR, DRIVEN);

  hub.send_oneway_position(IDENTITY, ONEWAY_POSITION_FULLY_CLOSED);
  hub.loop();

  EXPECT_EQ(device(DRIVEN).optimistic.target, 0.0f);
}

TEST_F(OwnOneWayFollow, InvertPositionTrueMirrorsAKindThatIsNotInverted) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING, /*invert=*/true);
  add_device(DRIVEN, DeviceType::VERTICAL_EXTERIOR_AWNING);
  hub.add_oneway_driven_device(IDENTITY_NODE_STR, DRIVEN);

  hub.send_oneway_position(IDENTITY, ONEWAY_POSITION_FULLY_CLOSED);
  hub.loop();

  EXPECT_EQ(device(DRIVEN).optimistic.target, 0.0f) << "invert_position: true mirrors a kind that is not inverted";
}

TEST_F(OwnOneWayFollow, InvertPositionFalseOnAnInvertedKindSendsAndShowsTheRawValue) {
  start(DeviceType::HORIZONTAL_AWNING, /*invert=*/false);
  add_device(DRIVEN, DeviceType::HORIZONTAL_AWNING);
  hub.add_oneway_driven_device(IDENTITY_NODE_STR, DRIVEN);

  hub.send_oneway_position(IDENTITY, ONEWAY_POSITION_FULLY_CLOSED);
  hub.loop();

  EXPECT_EQ(device(DRIVEN).optimistic.target, 100.0f);
}

TEST_F(OwnOneWayFollow, ALightFollowsAPositionOfItsIdentity) {
  start(DeviceType::LIGHT);
  add_device(DRIVEN, DeviceType::LIGHT);
  hub.add_oneway_driven_device(IDENTITY_NODE_STR, DRIVEN);

  hub.send_oneway_position(IDENTITY, 0);  // IO 0 is full brightness
  hub.loop();

  EXPECT_EQ(device(DRIVEN).optimistic.target, 0.0f);
  EXPECT_EQ(polls_queued_after(REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS), 1u);
}

TEST_F(OwnOneWayFollow, ABurstThatDidNotReachTheRadioChangesNothing) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING);
  add_device(DRIVEN, DeviceType::VERTICAL_EXTERIOR_AWNING);
  hub.add_oneway_driven_device(IDENTITY_NODE_STR, DRIVEN);
  for (uint8_t copy = 0; copy < ONEWAY_BURST_REPEATS; copy++)
    radio.queue_tx_result(false);

  hub.send_oneway_position(IDENTITY, ONEWAY_POSITION_FULLY_CLOSED);
  hub.loop();

  EXPECT_EQ(device(DRIVEN).optimistic.target, UNKNOWN_POSITION);
  EXPECT_EQ(device(DRIVEN).optimistic.motion, OptimisticState::Motion::NONE);
  EXPECT_EQ(polls_queued_after(REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS), 0u);
}

TEST_F(OwnOneWayFollow, AnEnrollmentChangesNoDeviceState) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING);
  add_device(DRIVEN, DeviceType::VERTICAL_EXTERIOR_AWNING);
  hub.add_oneway_driven_device(IDENTITY_NODE_STR, DRIVEN);

  hub.send_oneway_enroll(IDENTITY);
  hub.loop();

  ASSERT_GT(radio.get_send_count(), 0) << "the enrollment bursts went out";
  EXPECT_EQ(device(DRIVEN).optimistic.target, UNKNOWN_POSITION);
  EXPECT_EQ(device(DRIVEN).optimistic.motion, OptimisticState::Motion::NONE);
  EXPECT_EQ(polls_queued_after(REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS), 0u);
}

TEST_F(OwnOneWayFollow, AnIdentityWithoutDrivesLeavesEveryDeviceAlone) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING);
  add_device(DRIVEN, DeviceType::VERTICAL_EXTERIOR_AWNING);

  hub.send_oneway_position(IDENTITY, ONEWAY_POSITION_FULLY_CLOSED);
  hub.loop();

  EXPECT_EQ(device(DRIVEN).optimistic.target, UNKNOWN_POSITION);
  EXPECT_EQ(polls_queued_after(REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS), 0u);
}

TEST_F(OwnOneWayFollow, TheHubsOwnCommandFiresNoSenderEvent) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING);
  add_device(DRIVEN, DeviceType::VERTICAL_EXTERIOR_AWNING);
  hub.add_oneway_driven_device(IDENTITY_NODE_STR, DRIVEN);
  hub.add_exposed_sender(IDENTITY_NODE_STR);  // would fire for a remote of this address heard on the air

  hub.send_oneway_position(IDENTITY, ONEWAY_POSITION_FULLY_CLOSED);
  hub.loop();

  EXPECT_TRUE(api_server.events_.empty());
  EXPECT_EQ(device(DRIVEN).optimistic.target, 100.0f) << "the follow-up did run";
}

TEST_F(OwnOneWayFollow, ATypedCommandReadsADrivenDeviceOfAnotherClassWithoutMovingIt) {
  // The typed command addresses the identity's class; a device of another known class is not its addressee.
  start(DeviceType::VERTICAL_EXTERIOR_AWNING);
  add_device(DRIVEN, DeviceType::ROLLER_SHUTTER);
  hub.add_oneway_driven_device(IDENTITY_NODE_STR, DRIVEN);

  hub.send_oneway_position(IDENTITY, ONEWAY_POSITION_FULLY_CLOSED);
  hub.loop();

  EXPECT_EQ(device(DRIVEN).optimistic.target, UNKNOWN_POSITION);
  EXPECT_EQ(polls_queued_after(REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS), 1u) << "the read still confirms";
}

TEST_F(OwnOneWayFollow, AnAllDevicesCommandMovesADrivenDeviceOfAnotherClass) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING, std::nullopt, /*broadcast_all=*/true);
  add_device(DRIVEN, DeviceType::ROLLER_SHUTTER);
  hub.add_oneway_driven_device(IDENTITY_NODE_STR, DRIVEN);

  hub.send_oneway_position(IDENTITY, ONEWAY_POSITION_FULLY_CLOSED);
  hub.loop();

  EXPECT_EQ(device(DRIVEN).optimistic.target, 100.0f);
}
