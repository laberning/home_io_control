/// @file hub_oneway_dispatch_test.cpp
/// @brief Tests of what the hub puts on the air when a queued one-way operation is carried out, and of how
///        its own burst holds back background polls (hub_operations.cpp, hub_core.h, oneway_receiver.h).
///
/// The operation is queued through the public `send_oneway_*` calls and run by `loop()`, on a `MockRadio`
/// behind the real exchange engine, so the dispatch, the busy flag, the transmitter and the send path are
/// all the production ones. The expected bytes come from the frame builders, the expected wake level and
/// low-power flag per copy are written out here.
/// @ingroup hioc_hub

#include "hub_core.h"
#include "oneway_controller.h"
#include "proto_commands.h"
#include "proto_frame.h"
#include "proto_timing.h"
#include "oneway_receiver.h"

#include "esphome/core/application.h"
#include "esphome/core/preferences.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <vector>

using namespace esphome::home_io_control;

namespace {

constexpr uint8_t CONTROLLER_NODE[NODE_ID_SIZE] = {0x9D, 0x60, 0x85};
constexpr uint16_t FIRST_SEQUENCE = 700;
constexpr const char *CONTROLLER = "awning";
constexpr const char *POLLED_DEVICE = "ABC123";
/// The start preamble the tests tune the hub to, in programmed bytes on the mock radio.
constexpr uint16_t TUNED_START_PREAMBLE = 40;

class OneWayDispatchHub : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::busy_;
  using IOHomeControlComponent::defer_background_poll_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::op_queue_;
  using IOHomeControlComponent::radio_;
  using IOHomeControlComponent::system_key_;
};

/// A hub with one controller identity, wired to a MockRadio on a clear channel.
class OneWayDispatch : public ::testing::Test {
 protected:
  void SetUp() override {
    esphome::test_preferences::wipe();
    esphome::test_hal::reset_delays();
    std::memcpy(hub.node_id_, test::OWN_ID, NODE_ID_SIZE);
    std::memcpy(hub.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
    hub.initialized_ = true;
    hub.radio_ = &radio;
    radio.set_rssi_default(-100);  // clear channel: no busy check interferes with the count

    TuningConfig tuning{};
    tuning.start_preamble_ms = mock_preamble_ms(TUNED_START_PREAMBLE);
    hub.set_tuning_config(tuning);
  }
  void TearDown() override { esphome::test_preferences::wipe(); }

  /// Register the identity (its power class as given) and open its sequence counter.
  void add_identity(std::optional<OneWayPowerClass> power_class) {
    OneWayControllerIdentity identity{};
    identity.id = CONTROLLER;
    std::memcpy(identity.node_id, CONTROLLER_NODE, NODE_ID_SIZE);
    std::memset(identity.system_key, 0x11, AES_KEY_SIZE);
    identity.io_device_type = DeviceType::VERTICAL_EXTERIOR_AWNING;
    identity.initial_sequence = FIRST_SEQUENCE;
    identity.power_class_override = power_class;
    hub.add_oneway_controller(identity);
    hub.oneway_transmitter().setup();
  }

  /// The identity's key, as add_identity() gave it.
  static const uint8_t *key() {
    static uint8_t bytes[AES_KEY_SIZE];
    std::memset(bytes, 0x11, AES_KEY_SIZE);
    return bytes;
  }

  /// The wire bytes of one copy of a burst of `frame`, with the low-power flag as given.
  static std::vector<uint8_t> wire_bytes(IoFrame frame, bool low_power) {
    if (low_power)
      frame.ctrl1 |= CTRL1_LOW_POWER;
    uint8_t buf[FRAME_MAX_WIRE_SIZE] = {0};
    const uint8_t len = serialize(frame, buf, sizeof(buf));
    return std::vector<uint8_t>(buf, buf + len);
  }

  /// Assert that the burst starting at `first_send` is four copies of `frame` on channel 2 in the shape of
  /// `power_class`: wake level, preamble and low-power flag per copy.
  void expect_burst(size_t first_send, const IoFrame &frame, OneWayPowerClass power_class) {
    const auto &configs = radio.get_tx_configs();
    const auto &sent = radio.get_sent_data();
    ASSERT_GE(configs.size(), first_send + ONEWAY_BURST_REPEATS);
    for (uint8_t copy = 0; copy < ONEWAY_BURST_REPEATS; copy++) {
      const size_t index = first_send + copy;
      const bool wake_copy =
          power_class == OneWayPowerClass::LEGACY_LONG || (power_class == OneWayPowerClass::LOW_POWER && copy == 0);
      const bool flagged = power_class == OneWayPowerClass::LOW_POWER && copy == 0;
      SCOPED_TRACE("copy " + std::to_string(copy) + " of the burst at send " + std::to_string(first_send));
      EXPECT_EQ(configs[index].freq_hz, FREQ_CH2) << "one-way commands go out on channel 2";
      EXPECT_EQ(configs[index].wake, wake_copy ? TxWake::LONG : TxWake::SHORT);
      EXPECT_EQ(configs[index].preamble_len, wake_copy ? LONG_PREAMBLE : TUNED_START_PREAMBLE);
      EXPECT_EQ(sent[index], wire_bytes(frame, flagged));
    }
  }

  OneWayDispatchHub hub;
  MockRadio radio;
};

}  // namespace

// ============================================================================
// The four queued operations
// ============================================================================

TEST_F(OneWayDispatch, ACommandIsBurstFourTimesOnChannelTwoAndLeavesTheHubIdle) {
  add_identity(OneWayPowerClass::LOW_POWER);
  hub.send_oneway_command(CONTROLLER, CoverCommand::STOP);
  ASSERT_FALSE(hub.op_queue_.empty());

  hub.loop();

  ASSERT_EQ(radio.get_send_count(), ONEWAY_BURST_REPEATS);
  IoFrame expected{};
  ASSERT_TRUE(create_1w_execute_command(expected, CONTROLLER_NODE, DeviceType::VERTICAL_EXTERIOR_AWNING,
                                        CoverCommand::STOP, FIRST_SEQUENCE, key()));
  expect_burst(0, expected, OneWayPowerClass::LOW_POWER);
  EXPECT_FALSE(hub.busy_) << "the hub is released once the burst is out";
  EXPECT_TRUE(hub.op_queue_.empty());
}

TEST_F(OneWayDispatch, APositionIsBurstFourTimesOnChannelTwoAndLeavesTheHubIdle) {
  add_identity(OneWayPowerClass::LOW_POWER);
  hub.send_oneway_position(CONTROLLER, 30);

  hub.loop();

  ASSERT_EQ(radio.get_send_count(), ONEWAY_BURST_REPEATS);
  IoFrame expected{};
  ASSERT_TRUE(create_1w_execute_position(expected, CONTROLLER_NODE, DeviceType::VERTICAL_EXTERIOR_AWNING, 30,
                                         FIRST_SEQUENCE, key()));
  expect_burst(0, expected, OneWayPowerClass::LOW_POWER);
  EXPECT_FALSE(hub.busy_);
}

TEST_F(OneWayDispatch, AnEnrollmentIsAnUnenrollBurstThenAnAddBurstUnderTheNextSequence) {
  add_identity(OneWayPowerClass::LOW_POWER);
  hub.send_oneway_enroll(CONTROLLER);

  hub.loop();

  ASSERT_EQ(radio.get_send_count(), 2 * ONEWAY_BURST_REPEATS);
  IoFrame remove{};
  ASSERT_TRUE(create_1w_remove_controller(remove, CONTROLLER_NODE, DeviceType::UNKNOWN, FIRST_SEQUENCE, key()));
  IoFrame add{};
  ASSERT_TRUE(create_1w_add_controller(add, CONTROLLER_NODE, DeviceType::UNKNOWN, 0, FIRST_SEQUENCE + 1, key(),
                                       /*with_mac=*/false));
  expect_burst(0, remove, OneWayPowerClass::LOW_POWER);
  expect_burst(ONEWAY_BURST_REPEATS, add, OneWayPowerClass::LOW_POWER);
  EXPECT_FALSE(hub.busy_);
}

TEST_F(OneWayDispatch, AnUnenrollmentIsOneRemoveBurst) {
  add_identity(OneWayPowerClass::LOW_POWER);
  hub.send_oneway_unenroll(CONTROLLER);

  hub.loop();

  ASSERT_EQ(radio.get_send_count(), ONEWAY_BURST_REPEATS);
  IoFrame remove{};
  ASSERT_TRUE(create_1w_remove_controller(remove, CONTROLLER_NODE, DeviceType::UNKNOWN, FIRST_SEQUENCE, key()));
  expect_burst(0, remove, OneWayPowerClass::LOW_POWER);
  EXPECT_FALSE(hub.busy_);
}

TEST_F(OneWayDispatch, AnIdentityWithoutLowPowerSetBurstsTheLongWakeUpOnEveryCopy) {
  add_identity(std::nullopt);  // unset: the manufacturer profile decides; no manufacturer means legacy long
  hub.send_oneway_command(CONTROLLER, CoverCommand::STOP);

  hub.loop();

  IoFrame expected{};
  ASSERT_TRUE(create_1w_execute_command(expected, CONTROLLER_NODE, DeviceType::VERTICAL_EXTERIOR_AWNING,
                                        CoverCommand::STOP, FIRST_SEQUENCE, key()));
  ASSERT_EQ(radio.get_send_count(), ONEWAY_BURST_REPEATS);
  expect_burst(0, expected, OneWayPowerClass::LEGACY_LONG);
}

TEST_F(OneWayDispatch, AnIdentityWithLowPowerFalseBurstsTheStartPreambleOnEveryCopyWithoutTheFlag) {
  add_identity(OneWayPowerClass::ALWAYS_ALIVE);
  hub.send_oneway_command(CONTROLLER, CoverCommand::STOP);

  hub.loop();

  IoFrame expected{};
  ASSERT_TRUE(create_1w_execute_command(expected, CONTROLLER_NODE, DeviceType::VERTICAL_EXTERIOR_AWNING,
                                        CoverCommand::STOP, FIRST_SEQUENCE, key()));
  ASSERT_EQ(radio.get_send_count(), ONEWAY_BURST_REPEATS);
  expect_burst(0, expected, OneWayPowerClass::ALWAYS_ALIVE);
}

TEST_F(OneWayDispatch, AnActionIsQueuedAsItsCommandOrItsPosition) {
  // send_oneway_action() maps a button press to the same two operations the direct calls queue.
  add_identity(OneWayPowerClass::ALWAYS_ALIVE);

  hub.send_oneway_action(CONTROLLER, OneWayButtonAction::STOP);
  hub.loop();
  hub.send_oneway_action(CONTROLLER, OneWayButtonAction::OPEN);
  hub.loop();

  ASSERT_EQ(radio.get_send_count(), 2 * ONEWAY_BURST_REPEATS);
  IoFrame stop{};
  ASSERT_TRUE(create_1w_execute_command(stop, CONTROLLER_NODE, DeviceType::VERTICAL_EXTERIOR_AWNING, CoverCommand::STOP,
                                        FIRST_SEQUENCE, key()));
  IoFrame open{};
  ASSERT_TRUE(create_1w_execute_position(open, CONTROLLER_NODE, DeviceType::VERTICAL_EXTERIOR_AWNING,
                                         ONEWAY_POSITION_FULLY_OPEN, FIRST_SEQUENCE + 1, key()));
  expect_burst(0, stop, OneWayPowerClass::ALWAYS_ALIVE);
  expect_burst(ONEWAY_BURST_REPEATS, open, OneWayPowerClass::ALWAYS_ALIVE);
}

TEST_F(OneWayDispatch, TheHubIsBusyWhileTheBurstGoesOut) {
  add_identity(OneWayPowerClass::ALWAYS_ALIVE);
  std::vector<bool> busy_at_send;
  radio.set_on_send([&] { busy_at_send.push_back(hub.busy_); });
  hub.send_oneway_command(CONTROLLER, CoverCommand::STOP);

  hub.loop();

  ASSERT_EQ(busy_at_send.size(), ONEWAY_BURST_REPEATS);
  for (const bool busy : busy_at_send)
    EXPECT_TRUE(busy) << "no other radio work may start between the copies";
}

// ============================================================================
// The report fan-out
// ============================================================================

TEST_F(OneWayDispatch, EveryCallbackRegisteredWithTheHubGetsTheReportOfACarriedOutCommand) {
  // setup() wires the transmitter's report to the hub's callbacks; it stops at the radio here, after
  // the wiring, which is all this test needs of it.
  hub.set_node_id("C0FFEE");
  hub.set_system_key("00112233445566778899AABBCCDDEEFF");
  hub.set_radio_type("not_a_chip");
  add_identity(OneWayPowerClass::ALWAYS_ALIVE);
  std::vector<OneWayCommandReport> first;
  std::vector<OneWayCommandReport> second;
  hub.add_oneway_command_report_callback([&](const OneWayCommandReport &r) { first.push_back(r); });
  hub.add_oneway_command_report_callback([&](const OneWayCommandReport &r) { second.push_back(r); });
  hub.setup();
  hub.initialized_ = true;
  hub.radio_ = &radio;
  hub.send_oneway_command(CONTROLLER, CoverCommand::STOP);

  hub.loop();

  ASSERT_EQ(first.size(), 1u);
  ASSERT_EQ(second.size(), 1u);
  EXPECT_EQ(first[0].controller_id, CONTROLLER);
  EXPECT_EQ(first[0].intent, "STOP");
  EXPECT_TRUE(first[0].transmitted);
  EXPECT_TRUE(first[0].sequence_reserved);
  EXPECT_EQ(first[0].sequence, FIRST_SEQUENCE);
  EXPECT_EQ(second[0].intent, "STOP");
}

TEST_F(OneWayDispatch, AnUnknownIdentityIsReportedAsNotTransmittedAndLeavesTheHubIdle) {
  hub.set_node_id("C0FFEE");
  hub.set_system_key("00112233445566778899AABBCCDDEEFF");
  hub.set_radio_type("not_a_chip");
  std::vector<OneWayCommandReport> reports;
  hub.add_oneway_command_report_callback([&](const OneWayCommandReport &r) { reports.push_back(r); });
  hub.setup();
  hub.initialized_ = true;
  hub.radio_ = &radio;
  hub.send_oneway_command("typo", CoverCommand::STOP);

  hub.loop();

  ASSERT_EQ(reports.size(), 1u);
  EXPECT_FALSE(reports[0].transmitted);
  EXPECT_FALSE(reports[0].sequence_reserved);
  EXPECT_EQ(radio.get_send_count(), 0);
  EXPECT_FALSE(hub.busy_);
}

// ============================================================================
// The hub's own burst and background polls (manual clock)
// ============================================================================

class OneWayDispatchPolls : public OneWayDispatch {
 protected:
  void SetUp() override {
    OneWayDispatch::SetUp();
    DeviceConfig config;
    config.type = DeviceType::ROLLER_SHUTTER;
    hub.add_device(POLLED_DEVICE, config);
    add_identity(OneWayPowerClass::ALWAYS_ALIVE);
  }

  esphome::test_clock::ManualClock clock;
};

TEST_F(OneWayDispatchPolls, AQueuedPollWaitsOutTheQuietPeriodAfterTheHubsOwnBurst) {
  ASSERT_TRUE(hub.op_queue_.enqueue_request_status(POLLED_DEVICE));
  hub.send_oneway_command(CONTROLLER, CoverCommand::STOP);

  hub.loop();  // the command goes first, a poll never overtakes it
  ASSERT_EQ(radio.get_send_count(), ONEWAY_BURST_REPEATS);

  hub.loop();
  EXPECT_EQ(radio.get_send_count(), ONEWAY_BURST_REPEATS) << "the poll is held back right after the burst";
  EXPECT_FALSE(hub.op_queue_.empty());
  EXPECT_TRUE(hub.defer_background_poll_());

  esphome::test_clock::advance_ms(ONEWAY_QUIET_PERIOD_MS - 1);
  hub.loop();
  EXPECT_EQ(radio.get_send_count(), ONEWAY_BURST_REPEATS) << "still inside the quiet period";

  esphome::test_clock::advance_ms(1);
  hub.loop();
  EXPECT_GT(radio.get_send_count(), static_cast<int>(ONEWAY_BURST_REPEATS)) << "the poll runs once it has passed";
  EXPECT_TRUE(hub.op_queue_.empty());
}

TEST_F(OneWayDispatchPolls, AUserCommandDoesNotWaitForTheQuietPeriodOfTheHubsOwnBurst) {
  hub.send_oneway_command(CONTROLLER, CoverCommand::STOP);
  hub.loop();
  ASSERT_EQ(radio.get_send_count(), ONEWAY_BURST_REPEATS);

  hub.send_oneway_position(CONTROLLER, 0);
  hub.loop();  // no time has passed since the first burst

  EXPECT_EQ(radio.get_send_count(), 2 * ONEWAY_BURST_REPEATS) << "a control operation never yields to 1W activity";
  EXPECT_TRUE(hub.op_queue_.empty());
}

TEST_F(OneWayDispatchPolls, TheQuietPeriodCountsFromTheEndOfTheHubsOwnBurst) {
  // The activity is stamped when the burst has ended, so the quiet period counts from the last copy.
  hub.send_oneway_command(CONTROLLER, CoverCommand::STOP);
  const uint32_t before = esphome::millis();

  hub.loop();

  const uint32_t burst_ms = esphome::millis() - before;
  EXPECT_GE(burst_ms, (ONEWAY_BURST_REPEATS - 1) * ONEWAY_BURST_INTERVAL_MS) << "the copies are spaced by the gap";
  ASSERT_TRUE(hub.op_queue_.enqueue_request_status(POLLED_DEVICE));
  esphome::test_clock::advance_ms(ONEWAY_QUIET_PERIOD_MS - 1);
  EXPECT_TRUE(hub.defer_background_poll_()) << "measured from the end of the burst, not from its start";
}
