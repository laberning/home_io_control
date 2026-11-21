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
  void TearDown() override { esphome::test_preferences::wipe(); }

  /// Register the identity as a controller of `type`, run setup() and wire the cover to the real hub.
  void start(DeviceType type, std::optional<bool> invert = std::nullopt) {
    OneWayControllerIdentity identity{};
    identity.id = IDENTITY;
    std::memcpy(identity.node_id, IDENTITY_NODE, NODE_ID_SIZE);
    std::memset(identity.system_key, 0x11, AES_KEY_SIZE);
    identity.io_device_type = type;
    identity.power_class_override = OneWayPowerClass::ALWAYS_ALIVE;
    identity.invert_override = invert;
    hub.add_oneway_controller(identity);
    hub.setup();
    hub.lift_setup_failure();
    hub.initialized_ = true;
    hub.radio_ = &radio;
    cover.set_parent(&hub);
    cover.set_controller_id(IDENTITY);
    cover.add_follow(REMOTE);
    cover.setup();
  }

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
