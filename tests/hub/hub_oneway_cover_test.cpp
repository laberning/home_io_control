/// @file hub_oneway_cover_test.cpp
/// @brief Tests the hub side of the assumed-state cover of a one-way controller (ADR 0117): the press
///        subscription behind the duplicate filter, the identity's inversion, and the whole path from the
///        cover's command to the shown state.
///
/// A real hub runs behind a `MockRadio`: its transmitter reports the identity's own commands, and
/// `process_received_packet_()` hears a remote's frames through the real duplicate filter, so the cover is
/// driven by the production wiring and not by a double.
/// @ingroup hioc_hub

#include "hub_core.h"
#include "oneway_controller.h"
#include "platform_oneway_entities.h"
#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_frame.h"

#include "esphome/components/api/custom_api_device.h"
#include "esphome/core/application.h"
#include "esphome/core/preferences.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;
using namespace esphome::cover;

namespace {

constexpr uint8_t IDENTITY_NODE[NODE_ID_SIZE] = {0xA1, 0xB2, 0xC3};
constexpr const char *IDENTITY = "awning_remote";
constexpr uint8_t REMOTE_NODE[NODE_ID_SIZE] = {0x9D, 0x60, 0x85};
constexpr const char *REMOTE = "9D6085";
constexpr uint8_t OTHER_NODE[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
constexpr uint8_t SENDER_KEY[AES_KEY_SIZE] = {0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
                                              0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22};

constexpr uint32_t OPEN_MS = 20000;
constexpr uint32_t CLOSE_MS = 10000;

class CoverHub : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::process_received_packet_;
  using IOHomeControlComponent::radio_;
  void lift_setup_failure() { this->failed_ = false; }
};

class TestableCover : public IOHomeOneWayCover {
 public:
  using IOHomeOneWayCover::control;
};

class HubOneWayCover : public ::testing::Test {
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
  void TearDown() override {
    esphome::test_scheduler::clear();
    esphome::test_preferences::wipe();
  }

  /// Register the identity as a controller of `type`, run setup() and wire the cover to the real hub. With
  /// @p way the cover has travel times (20 s to open, 10 s to close) and that slider way.
  void start(DeviceType type, std::optional<bool> invert = std::nullopt,
             std::optional<decisions::OneWaySliderWay> way = std::nullopt) {
    OneWayControllerIdentity identity{};
    identity.id = IDENTITY;
    std::memcpy(identity.node_id, IDENTITY_NODE, NODE_ID_SIZE);
    std::memset(identity.system_key, 0x11, AES_KEY_SIZE);
    identity.io_device_type = type;
    identity.power_class = OneWayPowerClass::ALWAYS_ALIVE;
    identity.invert_override = invert;
    hub.add_oneway_controller(identity);
    hub.setup();
    hub.lift_setup_failure();
    hub.initialized_ = true;
    hub.radio_ = &radio;
    cover.set_parent(&hub);
    cover.set_controller_id(IDENTITY);
    cover.add_follow(REMOTE);
    if (way.has_value()) {
      cover.set_travel_durations(OPEN_MS, CLOSE_MS);
      cover.set_slider_way(*way);
    }
    cover.setup();
  }

  /// Let @p ms pass: timers fire, the cover publishes, and a stop that was queued goes out.
  void pass(uint32_t ms) {
    esphome::test_scheduler::run_until(esphome::millis() + ms);
    cover.loop();
    hub.loop();
  }

  /// The decoded frame of the first copy of burst number @p burst.
  OneWayFrameInfo burst_frame(size_t burst) const {
    const auto &sent = radio.get_sent_data();
    const size_t index = burst * ONEWAY_BURST_REPEATS;
    EXPECT_GT(sent.size(), index);
    IoFrame frame{};
    EXPECT_TRUE(parse(sent.at(index).data(), static_cast<uint8_t>(sent.at(index).size()), frame));
    return decode_1w_frame(frame);
  }

  [[nodiscard]] size_t bursts() const { return radio.get_send_count() / ONEWAY_BURST_REPEATS; }

  /// One press of the remote at @p src: a 1W execute carrying the wire position @p wire_percent, as the hub hears it.
  void hear_press(const uint8_t src[NODE_ID_SIZE], uint8_t wire_percent, uint16_t sequence) {
    IoFrame frame{};
    ASSERT_TRUE(create_1w_execute_position(frame, src, DeviceType::VERTICAL_EXTERIOR_AWNING, wire_percent, sequence,
                                           SENDER_KEY, ONEWAY_EXECUTE_ACEI, /*broadcast_all=*/false));
    hub.process_received_packet_(test::make_rx_packet(frame));
  }

  void press(CoverCall call) {
    cover.control(call);
    hub.loop();  // runs the queued burst on the mock radio
  }

  CoverCall close_call() {
    CoverCall call(&cover);
    call.set_position(COVER_CLOSED);
    return call;
  }

  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server{api_server};
  esphome::test_clock::ManualClock clock;
  CoverHub hub;
  MockRadio radio;
  TestableCover cover;
};

}  // namespace

TEST_F(HubOneWayCover, AFollowedRemotesBurstIsOnePressForTheCover) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING);
  int presses = 0;
  hub.add_oneway_press_callback({REMOTE}, [&](const OneWayFrameInfo &) { ++presses; });

  for (int copy = 0; copy < 4; ++copy)  // a remote repeats each press, every copy with the same sequence
    hear_press(REMOTE_NODE, 100, 0x1F44);

  EXPECT_EQ(presses, 1) << "the repeats of one press are filtered before a subscriber sees them";
  EXPECT_EQ(cover.position, COVER_CLOSED);
}

TEST_F(HubOneWayCover, TwoQuickPressesOfOneButtonAreTwoPresses) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING);
  int presses = 0;
  hub.add_oneway_press_callback({REMOTE}, [&](const OneWayFrameInfo &) { ++presses; });

  hear_press(REMOTE_NODE, 100, 0x1F44);
  hear_press(REMOTE_NODE, 100, 0x1F45);

  EXPECT_EQ(presses, 2) << "the sequence tells a new press from a repeat";
}

TEST_F(HubOneWayCover, ARemoteThatIsNotListedIsNeverDelivered) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING);

  hear_press(OTHER_NODE, 100, 0x0001);

  EXPECT_EQ(cover.position, COVER_OPEN) << "a neighbour's remote must not move the cover";
}

TEST_F(HubOneWayCover, AFrameWithoutACommandIsNotDeliveredAsAPress) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING);
  int presses = 0;
  hub.add_oneway_press_callback({REMOTE}, [&](const OneWayFrameInfo &) { ++presses; });

  IoFrame frame{};
  init_frame(frame, false, true, true, false);  // 1W
  set_src(frame, REMOTE_NODE);
  set_dst(frame, BROADCAST_DISCOVER_ALT);
  const uint8_t payload[1] = {0x00};
  ASSERT_TRUE(set_cmd(frame, CMD_WRITE_PRIVATE, payload, sizeof(payload)));
  hub.process_received_packet_(test::make_rx_packet(frame));

  EXPECT_EQ(presses, 0);
}

TEST_F(HubOneWayCover, TheCoversOwnCloseShowsClosedOnceItWentOutAndIsTransmitted) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING);

  press(close_call());

  EXPECT_EQ(cover.position, COVER_CLOSED);
}

TEST_F(HubOneWayCover, AnInvertedKindShowsTheUsersCloseNotTheMirroredWireValue) {
  start(DeviceType::HORIZONTAL_AWNING);
  EXPECT_TRUE(hub.oneway_identity_inverted(IDENTITY)) << "a horizontal awning is inverted by its kind";

  press(close_call());  // goes out as wire 0
  EXPECT_EQ(cover.position, COVER_CLOSED);

  hear_press(REMOTE_NODE, 0, 0x2000);  // the remote's own close is wire 0 as well
  EXPECT_EQ(cover.position, COVER_CLOSED);
  hear_press(REMOTE_NODE, 100, 0x2001);
  EXPECT_EQ(cover.position, COVER_OPEN);
}

TEST_F(HubOneWayCover, AnIdentityThatIsNotInvertedOrUnknownReportsNoInversion) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING);

  EXPECT_FALSE(hub.oneway_identity_inverted(IDENTITY));
  EXPECT_FALSE(hub.oneway_identity_inverted("no_such_identity"));
}

TEST_F(HubOneWayCover, ATimedCoverEstimatesItsPositionFromItsOwnCommandAndAFollowedPress) {
  cover.set_travel_durations(20000, 10000);
  start(DeviceType::VERTICAL_EXTERIOR_AWNING);

  press(close_call());  // the real transmitter reports the burst
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_CLOSING);
  esphome::test_clock::advance_ms(4000);
  cover.loop();
  EXPECT_FLOAT_EQ(cover.position, 0.6F);

  hear_press(REMOTE_NODE, 0, 0x3000);  // the wall remote opens it again
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_OPENING);
  esphome::test_clock::advance_ms(2000);
  cover.loop();
  EXPECT_FLOAT_EQ(cover.position, 0.7F);
}

// === A slider value with travel times (ADR 0128) ===

TEST_F(HubOneWayCover, ATimedValueGoesOutAsCloseThenOneStopAtTheValueAndThePositionFreezesThere) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING, std::nullopt, decisions::OneWaySliderWay::TIMED);
  CoverCall call(&cover);
  call.set_position(0.5F);

  press(call);

  ASSERT_EQ(bursts(), 1u);
  EXPECT_EQ(burst_frame(0).main0, 0xC8) << "close is the position frame at 100 % closed (wire 200)";
  pass(CLOSE_MS / 2 - 1);
  EXPECT_EQ(bursts(), 1u) << "no stop before half of the 10 s fall has run";
  pass(1);

  ASSERT_EQ(bursts(), 2u) << "exactly one stop";
  EXPECT_EQ(burst_frame(1).main0, POS_STOP);
  EXPECT_NEAR(cover.position, 0.5F, 0.1F) << "frozen near the value; the burst itself takes a little of the fall";
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_IDLE);
  pass(60000);
  EXPECT_EQ(bursts(), 2u);
}

TEST_F(HubOneWayCover, AFrameValueGoesOutAsOnePositionFrameAndTheCoverTravelsToIt) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING, std::nullopt, decisions::OneWaySliderWay::FRAME);
  CoverCall call(&cover);
  call.set_position(0.25F);

  press(call);

  ASSERT_EQ(bursts(), 1u);
  EXPECT_EQ(burst_frame(0).main0, 0x96) << "75 % closed on the wire, doubled into the main byte";
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_CLOSING) << "a cover travels to a position frame";
  pass(CLOSE_MS);
  EXPECT_EQ(bursts(), 1u) << "no stop follows a position frame";
  EXPECT_NEAR(cover.position, 0.25F, 0.01F);
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_IDLE);
}

TEST_F(HubOneWayCover, AFollowedRemotesPressDuringATimedRunTakesOverAndNoStaleStopGoesOut) {
  start(DeviceType::VERTICAL_EXTERIOR_AWNING, std::nullopt, decisions::OneWaySliderWay::TIMED);
  CoverCall call(&cover);
  call.set_position(0.2F);
  press(call);
  ASSERT_EQ(bursts(), 1u);
  pass(2000);

  hear_press(REMOTE_NODE, 0, 0x3000);  // the wall remote opens it again
  pass(120000);

  EXPECT_EQ(bursts(), 1u) << "the stop planned for 0.2 would be stale";
  EXPECT_FLOAT_EQ(cover.position, COVER_OPEN);
}

TEST_F(HubOneWayCover, AnInvertedKindsSliderValueIsMirroredOnTheWireAndShownAsTheUsersValue) {
  start(DeviceType::HORIZONTAL_AWNING, std::nullopt, decisions::OneWaySliderWay::FRAME);
  ASSERT_TRUE(hub.oneway_identity_inverted(IDENTITY));
  CoverCall call(&cover);
  call.set_position(0.25F);

  press(call);

  ASSERT_EQ(bursts(), 1u);
  EXPECT_EQ(burst_frame(0).main0, 0x32) << "75 % closed for the user is wire 25 on an inverted kind, doubled";
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_CLOSING);
  pass(CLOSE_MS);
  EXPECT_NEAR(cover.position, 0.25F, 0.01F);
}

TEST_F(HubOneWayCover, AnInvertedKindsTimedValueClosesOnTheMirroredWireValueAndStopsOnce) {
  start(DeviceType::HORIZONTAL_AWNING, std::nullopt, decisions::OneWaySliderWay::TIMED);
  CoverCall call(&cover);
  call.set_position(0.5F);

  press(call);

  ASSERT_EQ(bursts(), 1u);
  EXPECT_EQ(burst_frame(0).main0, 0x00) << "the user's close is wire 0 on an inverted kind";
  EXPECT_EQ(cover.current_operation, COVER_OPERATION_CLOSING);
  pass(CLOSE_MS / 2);

  ASSERT_EQ(bursts(), 2u) << "exactly one stop, half of the fall later";
  EXPECT_EQ(burst_frame(1).main0, POS_STOP);
  EXPECT_NEAR(cover.position, 0.5F, 0.1F);
  pass(60000);
  EXPECT_EQ(bursts(), 2u);
}
