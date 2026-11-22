/// @file hub_oneway_light_test.cpp
/// @brief Tests the whole path of the assumed-state light of a one-way controller (ADR 0128) through the real hub:
///        a slider value becomes the frames of its way on a `MockRadio`, the timed stop goes out once the estimate
///        reaches the value, and the level follows the identity's own report and a followed remote's press.
///
/// A real hub runs behind a `MockRadio`, like in hub_oneway_cover_test.cpp: its transmitter reports the identity's own
/// commands and `process_received_packet_()` hears the remote through the real duplicate filter. The frames that went
/// out are read back from the bytes the radio was given.
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

namespace {

constexpr uint8_t IDENTITY_NODE[NODE_ID_SIZE] = {0xA1, 0xB2, 0xC3};
constexpr const char *IDENTITY = "lamp_remote";
constexpr uint8_t REMOTE_NODE[NODE_ID_SIZE] = {0x9D, 0x60, 0x85};
constexpr const char *REMOTE = "9D6085";
constexpr uint8_t SENDER_KEY[AES_KEY_SIZE] = {0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
                                              0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22};
constexpr uint32_t ON_MS = 20000;
constexpr uint32_t OFF_MS = 10000;

class LightHub : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::process_received_packet_;
  using IOHomeControlComponent::radio_;
  void lift_setup_failure() { this->failed_ = false; }
};

class TestableLight : public IOHomeOneWayLight {
 public:
  using IOHomeOneWayLight::suppress_write_;
};

class HubOneWayLight : public ::testing::Test {
 protected:
  void SetUp() override {
    esphome::test_preferences::wipe();
    esphome::test_hal::reset_delays();
    esphome::test_scheduler::clear();
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

  /// A dimmable light on a light identity with the fade times of the Izymo-like test device.
  void start(decisions::OneWaySliderWay way) {
    OneWayControllerIdentity identity{};
    identity.id = IDENTITY;
    std::memcpy(identity.node_id, IDENTITY_NODE, NODE_ID_SIZE);
    std::memset(identity.system_key, 0x11, AES_KEY_SIZE);
    identity.io_device_type = DeviceType::LIGHT;
    identity.power_class = OneWayPowerClass::ALWAYS_ALIVE;
    hub.add_oneway_controller(identity);
    hub.setup();
    hub.lift_setup_failure();
    hub.initialized_ = true;
    hub.radio_ = &radio;
    light.set_parent(&hub);
    light.set_controller_id(IDENTITY);
    light.set_dimmable(true);
    light.set_fade_durations(ON_MS, OFF_MS);
    light.set_slider_way(way);
    light.add_follow(REMOTE);
    light.setup();
    light.setup_state(&state);
    light.write_state(&state);  // the boot-time restore write, swallowed
  }

  /// Home Assistant asks for @p brightness; the hub then carries out the queued burst on the mock radio.
  void ask(float brightness) {
    esphome::light::LightState request;
    request.set_current_on(brightness > 0.0F);
    request.set_current_brightness(brightness);
    light.write_state(&request);
    hub.loop();
    echo();
  }

  /// Let @p ms pass: timers fire, the light publishes, and a stop that was queued goes out.
  void pass(uint32_t ms) {
    esphome::test_scheduler::run_until(esphome::millis() + ms);
    light.loop();
    echo();
    hub.loop();
    echo();
  }

  /// ESPHome re-enters write_state() while it applies a published level; the host stub does not.
  void echo() {
    if (light.suppress_write_)
      light.write_state(&state);
  }

  /// One press of the remote: a 1W execute carrying the wire position @p wire_percent, as the hub hears it.
  void hear_press(uint8_t wire_percent, uint16_t sequence) {
    IoFrame frame{};
    ASSERT_TRUE(create_1w_execute_position(frame, REMOTE_NODE, DeviceType::LIGHT, wire_percent, sequence, SENDER_KEY,
                                           ONEWAY_EXECUTE_ACEI, /*broadcast_all=*/false));
    hub.process_received_packet_(test::make_rx_packet(frame));
    echo();
  }

  /// The decoded frame of the first copy of the burst that starts at send number @p burst * 4.
  OneWayFrameInfo burst_frame(size_t burst) const {
    const auto &sent = radio.get_sent_data();
    const size_t index = burst * ONEWAY_BURST_REPEATS;
    EXPECT_GT(sent.size(), index);
    IoFrame frame{};
    EXPECT_TRUE(parse(sent.at(index).data(), static_cast<uint8_t>(sent.at(index).size()), frame));
    return decode_1w_frame(frame);
  }

  [[nodiscard]] size_t bursts() const { return radio.get_send_count() / ONEWAY_BURST_REPEATS; }
  [[nodiscard]] float shown() const {
    return state.current_values.is_on() ? state.current_values.get_brightness() : 0.0F;
  }

  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server{api_server};
  esphome::test_clock::ManualClock clock;
  LightHub hub;
  MockRadio radio;
  TestableLight light;
  esphome::light::LightState state;
};

}  // namespace

TEST_F(HubOneWayLight, ATimedValueGoesOutAsOnThenOneStopAtTheValueAndTheLevelFreezesThere) {
  start(decisions::OneWaySliderWay::TIMED);

  ask(0.5F);
  ASSERT_EQ(bursts(), 1u);
  const OneWayFrameInfo on = burst_frame(0);
  EXPECT_EQ(on.main0, 0x00) << "on is the position frame at full brightness (wire 0)";
  EXPECT_EQ(on.target_type, DeviceType::LIGHT);

  pass(ON_MS / 2 - 1);
  EXPECT_EQ(bursts(), 1u) << "no stop before half of the 20 s rise has run";
  pass(1);

  ASSERT_EQ(bursts(), 2u) << "exactly one stop";
  EXPECT_EQ(burst_frame(1).main0, POS_STOP);
  EXPECT_NEAR(shown(), 0.5F, 0.1F) << "frozen near the value; the burst itself takes a little of the rise";
  const float frozen = shown();
  pass(60000);
  EXPECT_EQ(bursts(), 2u);
  EXPECT_FLOAT_EQ(shown(), frozen);
}

TEST_F(HubOneWayLight, AFrameValueGoesOutAsOnePositionFrameAndTheLevelIsThereAtOnce) {
  start(decisions::OneWaySliderWay::FRAME);

  ask(0.25F);

  ASSERT_EQ(bursts(), 1u);
  EXPECT_EQ(burst_frame(0).main0, 0x96) << "75 % on the wire, doubled into the main byte";
  EXPECT_FLOAT_EQ(shown(), 0.25F);
  pass(60000);
  EXPECT_EQ(bursts(), 1u) << "no stop follows a position frame";
  EXPECT_FLOAT_EQ(shown(), 0.25F);
}

TEST_F(HubOneWayLight, AWallRemotesPressDuringAFadeTakesOverAndNoStaleStopGoesOut) {
  start(decisions::OneWaySliderWay::TIMED);
  ask(0.8F);
  ASSERT_EQ(bursts(), 1u);
  pass(4000);

  hear_press(100, 0x3000);  // the wall remote switches it off
  pass(120000);

  EXPECT_EQ(bursts(), 1u) << "the stop planned for 0.8 would be stale";
  EXPECT_FLOAT_EQ(shown(), 0.0F);
}
