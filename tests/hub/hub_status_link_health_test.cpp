/// @file hub_status_link_health_test.cpp
/// @brief Tests for the link health of hub_status.cpp: the RSSI average, the last-seen time and the exchange failures.

#include "hub_core.h"
#include "hub_internal.h"
#include "radio_interface.h"
#include "proto_frame.h"
#include "esphome/core/application.h"
#include "esphome/core/component.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"

using namespace esphome::home_io_control;
using test::make_rx_packet;
using test::RxTestableComponent;
using test::setup_rx_test_component;
using test::TestableHubComponent;

// ========================================================================================
// Link-health tests (RSSI EMA, last-seen, exchange failures)
// ========================================================================================

TEST(HubStatus, RxFromRegisteredDeviceUpdatesLastSeenAndRssiEma) {
  TestableHubComponent comp;
  MockRadio radio;
  comp.radio_ = &radio;
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->last_seen_ms, 0u) << "newly added device should start with no last-seen timestamp";
  EXPECT_EQ(dev->rssi_ema_scaled, RSSI_UNKNOWN_DBM) << "newly added device should start with no RSSI sample";

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_ERROR_RESP);  // Any frame type counts, even one that later fails its own payload guard.

  // First sample: EMA is seeded directly rather than blended from 0 (fixed point: S = -80×8 = -640).
  radio.set_last_capture_rssi(-80);
  comp.update_device_status_(f);
  EXPECT_NE(dev->last_seen_ms, 0u) << "a frame from a registered device should stamp last_seen_ms";
  EXPECT_EQ(dev->last_rssi_dbm, -80) << "the raw RSSI sample should be recorded";
  EXPECT_EQ(device_rssi_ema_dbm(*dev), -80) << "the first sample should seed the EMA directly";

  // Second sample: S = -640 + (-72 - round(-640/8)) = -640 - 72 + 80 = -632 → -79 dBm
  // (matches the real-valued EMA -80 + 8/8 = -79 exactly).
  radio.set_last_capture_rssi(-72);
  comp.update_device_status_(f);
  EXPECT_EQ(dev->last_rssi_dbm, -72);
  EXPECT_EQ(device_rssi_ema_dbm(*dev), -79) << "EMA should blend the new sample by 1/8th";

  // Third sample: S = -632 + (-64 - round(-632/8)) = -632 - 64 + 79 = -617 → round(-77.125) = -77 dBm
  // (the real-valued EMA is -79 + 15/8 = -77.125; a whole-dBm truncating EMA would have said -78).
  radio.set_last_capture_rssi(-64);
  comp.update_device_status_(f);
  EXPECT_EQ(dev->last_rssi_dbm, -64);
  EXPECT_EQ(device_rssi_ema_dbm(*dev), -77) << "EMA should blend by 1/8th with fixed-point precision";
}

TEST(HubStatus, RssiEmaConvergesToStableSignal) {
  TestableHubComponent comp;
  MockRadio radio;
  comp.radio_ = &radio;
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_ERROR_RESP);

  // Seed far away from the eventual level, then feed a long stable signal. A whole-dBm EMA with
  // truncating division would stall as soon as |sample − EMA| < RSSI_EMA_SCALE and report up to
  // 7 dBm off forever; the fixed-point EMA must converge to the true level exactly.
  radio.set_last_capture_rssi(-80);
  comp.update_device_status_(f);
  radio.set_last_capture_rssi(-64);
  for (int i = 0; i < 60; i++)
    comp.update_device_status_(f);

  EXPECT_EQ(device_rssi_ema_dbm(*dev), -64) << "a stable signal must converge exactly, with no truncation dead zone";
}

TEST(HubStatus, RxWithUnknownRssiKeepsTheLastValueAndTheEma) {
  TestableHubComponent comp;
  MockRadio radio;
  comp.radio_ = &radio;
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_ERROR_RESP);

  radio.set_last_capture_rssi(-60);
  comp.update_device_status_(f);
  ASSERT_EQ(dev->last_rssi_dbm, -60);
  const int16_t ema_before = dev->rssi_ema_scaled;

  radio.set_last_capture_rssi_unknown();
  dev->last_seen_ms = 0;
  comp.update_device_status_(f);

  EXPECT_NE(dev->last_seen_ms, 0u) << "the frame still proves the device is alive";
  EXPECT_EQ(dev->last_rssi_dbm, -60) << "an unknown level is no sample";
  EXPECT_EQ(dev->rssi_ema_scaled, ema_before);
}

TEST(HubStatus, FirstSampleUnknownLeavesTheRssiUnknown) {
  TestableHubComponent comp;
  MockRadio radio;
  comp.radio_ = &radio;
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_ERROR_RESP);

  radio.set_last_capture_rssi_unknown();
  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_rssi_dbm, RSSI_UNKNOWN_DBM);
  EXPECT_EQ(dev->rssi_ema_scaled, RSSI_UNKNOWN_DBM);
}

TEST(HubStatus, RxWithInvalidCaptureUpdatesLastSeenButNotRssi) {
  TestableHubComponent comp;
  MockRadio radio;
  comp.radio_ = &radio;  // Never staged with set_last_capture_rssi(): get_last_capture().valid stays false.
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_ERROR_RESP);

  comp.update_device_status_(f);

  EXPECT_NE(dev->last_seen_ms, 0u) << "last_seen_ms should not depend on a valid radio capture";
  EXPECT_EQ(dev->rssi_ema_scaled, RSSI_UNKNOWN_DBM) << "RSSI must not be fabricated from an invalid capture";
  EXPECT_EQ(dev->last_rssi_dbm, RSSI_UNKNOWN_DBM);
}

TEST(HubStatus, RxWithoutRadioDoesNotCrash) {
  TestableHubComponent comp;
  ASSERT_EQ(comp.radio_, nullptr) << "test relies on radio_ defaulting to nullptr";
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_ERROR_RESP);

  comp.update_device_status_(f);  // Must not crash despite radio_ == nullptr.

  EXPECT_NE(dev->last_seen_ms, 0u) << "last_seen_ms should still update without a radio";
  EXPECT_EQ(dev->rssi_ema_scaled, RSSI_UNKNOWN_DBM);
}

TEST(HubStatus, RxFromUnregisteredDeviceUpdatesNothing) {
  TestableHubComponent comp;
  MockRadio radio;
  comp.radio_ = &radio;
  radio.set_last_capture_rssi(-80);
  comp.add_device("054E17");

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0xAB, 0xCD, 0xEF};  // Not a registered device.
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_ERROR_RESP);

  comp.update_device_status_(f);  // Must not crash and must not register a new device.

  EXPECT_EQ(comp.get_device("ABCDEF"), nullptr) << "an unregistered source must not be implicitly added";
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->last_seen_ms, 0u) << "an unrelated registered device must not be touched";
}

TEST(HubStatus, OwnControllerStatusUpdateSchedulesPoll) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  // A CMD_PRIVATE_RESP addressed to our registered device from a remote controller
  IoFrame f{};
  init_frame(f, true, true, false, false);
  uint8_t src[3] = {0x43, 0x44, 0xE3};  // some remote
  uint8_t dst[3] = {0x05, 0x4E, 0x17};  // our registered device
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_EXECUTE);

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "execute frame should serialize";

  comp.process_received_packet_(pkt);

  // Remote commanding our device → schedule 2s poll
  EXPECT_EQ(comp.last_timeout_ms_, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS) << "remote activity should schedule 2s poll";
  EXPECT_EQ(comp.last_timeout_id_, decisions::remote_poll_timer_id(dst)) << "timeout id should reference our device";
}

TEST(HubStatus, StatusUpdateWithShortPayloadIgnored) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  // CMD_STATUS_UPDATE with payload < 11 bytes — should be logged as unsupported
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[5] = {0, 0, 0, 0, 0};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "short status update should serialize";

  comp.process_received_packet_(pkt);

  // Should not crash — short payload silently handled
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->position, UNKNOWN_POSITION) << "short status update should not change position";
}

// ADR 0022: an unauthenticated CMD_PRIVATE_RESP/foreign-dst CMD_STATUS_UPDATE is never applied
// to device state, regardless of what it claims — see hub_status.cpp's process_received_packet_.
// Both payloads below claim the device is *moving* to 100% (STATUS_STOPPED bit clear) — the
// opposite of a fresh device's default (is_stopped=true, position=UNKNOWN_POSITION) — the same
// shape the equivalent authenticated-path tests elsewhere in this file (e.g.
// StatusUpdateMovingFrameHandling) prove really does decode when trusted, so a regression that
// started trusting this path again would flip these assertions rather than pass vacuously.

TEST(HubStatus, UnauthenticatedForeignPrivateResponseDoesNotUpdateDeviceState) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  comp.add_device("054E17");

  // CMD_PRIVATE_RESP from one of our own registered devices, addressed to some other controller
  // (not us) — as if we're passively overhearing a reply to a different controller's own poll.
  IoFrame f{};
  init_frame(f, true, true, false, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};  // our registered device
  uint8_t dst[3] = {0x43, 0x44, 0xE3};  // some other controller, not us
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[8] = {
      test::STATUS_BYTE_MOVING, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};  // moving, target/current=100%
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "private response frame should serialize";

  comp.process_received_packet_(pkt);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->position, UNKNOWN_POSITION) << "unauthenticated foreign reply must not set position";
  EXPECT_TRUE(dev->is_stopped) << "unauthenticated foreign reply must not change is_stopped from its default";
}

TEST(HubStatus, UnauthenticatedForeignStatusUpdateDoesNotUpdateDeviceState) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  comp.add_device("054E17");

  // CMD_STATUS_UPDATE from one of our own registered devices, addressed to some other controller
  // — the device-initiated equivalent of the CMD_PRIVATE_RESP case above.
  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};  // our registered device
  uint8_t dst[3] = {0x43, 0x44, 0xE3};  // some other controller, not us
  set_src(f, src);
  set_dst(f, dst);
  // moving, target at [5..6]=0xC800 (100%), current at [7..8]=0xC800 (100%)
  uint8_t payload[11] = {test::STATUS_BYTE_MOVING, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "status update frame should serialize";

  comp.process_received_packet_(pkt);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->position, UNKNOWN_POSITION) << "unauthenticated foreign status update must not set position";
  EXPECT_TRUE(dev->is_stopped) << "unauthenticated foreign status update must not change is_stopped from its default";
}
