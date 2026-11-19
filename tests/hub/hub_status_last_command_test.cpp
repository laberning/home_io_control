/// @file hub_status_last_command_test.cpp
/// @brief Tests for the last-command record that update_device_status_() decodes, and the prediction text appended to a
/// status log line.

#include "hub_core.h"
#include "hub_internal.h"
#include "radio_interface.h"
#include "proto_frame.h"
#include "esphome/core/application.h"
#include "esphome/core/component.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;
using test::TestableHubComponent;

// ============================================================================
// Last-command record — decoded by update_device_status_() into dev.last_commander /
// last_command_originator / has_last_command (decode_last_command_record(),
// proto_codecs.h). Byte-exact against real captures, not synthetic frames, for the fields that
// matter most: the +3 offset shift between 0x04 and 0x71 was the source plan's own bug (see
// proto_codecs.h's PRIVATE_RESPONSE_LAST_COMMAND_OFFSET/STATUS_UPDATE_LAST_COMMAND_OFFSET doc).
// ============================================================================

TEST(HubStatus, PrivateResponseRecordsLastCommander) {
  // tests/corpus/captures/statuspoll/somfy_rs100_statuspoll_kig300_sx1276.yaml frame 3 (0x04).
  TestableHubComponent comp;
  comp.add_device("E461E9");

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0xE4, 0x61, 0xE9};
  uint8_t dst[3] = {0xA0, 0x25, 0xE1};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[14] = {0x04, 0x60, 0x00, 0x00, 0x77, 0xCE, 0x00, 0x0B, 0xBE, 0xFE, 0xDB, 0x01, 0x00, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("E461E9");
  ASSERT_NE(dev, nullptr);
  EXPECT_TRUE(dev->has_last_command);
  EXPECT_EQ(std::memcmp(dev->last_commander, "\xBE\xFE\xDB", 3), 0);
  EXPECT_EQ(dev->last_command_originator, 0x01);
}

TEST(HubStatus, StatusUpdateRecordsLastCommanderAtTheShiftedOffset) {
  // Same corpus file, frame 2 (0x71) — this is the test that pins the +3 offset shift (C1):
  // implementing the source plan literally (data[8..10] on 0x71 too) would read data[8..10] =
  // "9E 00 0F" here instead of the real commander at data[11..13].
  TestableHubComponent comp;
  comp.add_device("E461E9");

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0xE4, 0x61, 0xE9};
  uint8_t dst[3] = {0xA0, 0x25, 0xE1};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[16] = {0x04, 0x60, 0x10, 0x0A, 0x0B, 0x00, 0x00, 0xAC,
                         0x9E, 0x00, 0x0F, 0xBE, 0xFE, 0xDB, 0x01, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("E461E9");
  ASSERT_NE(dev, nullptr);
  EXPECT_TRUE(dev->has_last_command);
  EXPECT_EQ(std::memcmp(dev->last_commander, "\xBE\xFE\xDB", 3), 0)
      << "the commander is at data[11..13] on a 0x71, not data[8..10]";
  EXPECT_EQ(dev->last_command_originator, 0x01);
}

TEST(HubStatus, StatusUpdateAddressedToAnotherControllerStillNamesTheLastCommander) {
  // Same corpus file, frame 5 (0x71) — addressed to CA0A18, but still names BEFEDB as the last
  // commander. Rules out "data[11..13] is just the destination echoed back".
  TestableHubComponent comp;
  comp.add_device("E461E9");

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0xE4, 0x61, 0xE9};
  uint8_t dst[3] = {0xCA, 0x0A, 0x18};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[16] = {0x05, 0x60, 0x10, 0x0A, 0x0B, 0x00, 0x00, 0x00,
                         0x00, 0x00, 0x00, 0xBE, 0xFE, 0xDB, 0x01, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("E461E9");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(std::memcmp(dev->last_commander, "\xBE\xFE\xDB", 3), 0)
      << "the last commander is not the frame's own destination address";
}

TEST(HubStatus, ExecuteAckDoesNotRecordALastCommander) {
  // tests/corpus/captures/statuspoll/tilt_cover_statuspoll_reports_settled_tilt_before.yaml's
  // 16-byte payload — long enough that decode_last_command_record() would happily produce a valid
  // record (the "_after" sibling fixture's identical-shape payload proves this, applied with
  // trust_position defaulted true, in TiltBlockIsNotMistakenForALastCommandParameter below).
  // Applied here with trust_position=false, as a real EXECUTE ack would be, so this test pins the
  // trust_position gate itself (C8) and not just the length guard — unlike a too-short payload,
  // which would pass even with the gate deleted.
  TestableHubComponent comp;
  comp.add_device("51C001");

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0x51, 0xC0, 0x01};
  uint8_t dst[3] = {0x31, 0xBA, 0xF7};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[16] = {0x05, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00,
                         0x31, 0xBA, 0xF7, 0x01, 0x20, 0xC8, 0x00, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));

  comp.update_device_status_(f, /*trust_position=*/false);

  auto *dev = comp.get_device("51C001");
  ASSERT_NE(dev, nullptr);
  EXPECT_FALSE(dev->has_last_command);
}

TEST(HubStatus, ShortPrivateResponseLeavesTheRecordUntouched) {
  // tests/corpus/captures/exchange/tilt_cover_exchange_close_ack_six_byte_no_hint.yaml's 6-byte
  // payload, applied with trust_position=true (the default) to isolate the >= 12 length guard
  // from the trust_position gate pinned above: a real device can send a short reply to a
  // *trusted* request too (e.g. a status poll from a device that omits the remaining time), and the
  // guard must reject the record without also rejecting the position decode it shares a branch
  // with.
  TestableHubComponent comp;
  comp.add_device("51C001");

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0x51, 0xC0, 0x01};
  uint8_t dst[3] = {0x31, 0xBA, 0xF7};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[6] = {0x2C, 0x80, 0xA0, 0x04, 0x00, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("51C001");
  ASSERT_NE(dev, nullptr);
  EXPECT_FALSE(dev->has_last_command) << "6 bytes is too short to carry the record (needs >= 12)";
  EXPECT_FALSE(dev->is_stopped) << "position decode must still work despite the last-command guard rejecting";
}

TEST(HubStatus, TiltBlockIsNotMistakenForALastCommandParameter) {
  // tests/corpus/captures/statuspoll/tilt_cover_statuspoll_reports_settled_tilt_after.yaml —
  // 16-byte 0x04 reply where data[12] == STATUS_TILT_SELECTOR (0x20), tilt raw 0x372C -> 72.4%.
  // Deliberately not the "_before" sibling fixture (tilt raw 0xC800 -> 0%): 0% is also
  // decode_tilt_report()'s zero value, so it can't distinguish "tilt decoded correctly" from
  // "tilt silently left at its zero-initialized default" the way a mid-range, non-boundary value
  // can. Pins C3: the last-command decode must not touch data[12..15], which the extended-tilt
  // decoder already owns, and the tilt value must still decode correctly alongside the
  // (different-offset) commander/originator.
  TestableHubComponent comp;
  comp.add_device("51C001", {DeviceType::INTERIOR_VENETIAN_BLIND, 0});

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0x51, 0xC0, 0x01};
  uint8_t dst[3] = {0x31, 0xBA, 0xF7};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[16] = {0x05, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00,
                         0x31, 0xBA, 0xF7, 0x01, 0x20, 0x37, 0x2C, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("51C001");
  ASSERT_NE(dev, nullptr);
  EXPECT_TRUE(dev->has_last_command);
  EXPECT_EQ(std::memcmp(dev->last_commander, "\x31\xBA\xF7", 3), 0);
  EXPECT_EQ(dev->last_command_originator, 0x01);
  EXPECT_NEAR(dev->tilt, 72.4f, 0.1f) << "the tilt block at data[12..15] must still decode correctly";
}

TEST(HubStatus, OwnHubIsRecordedAsTheLastCommander) {
  // tests/corpus/captures/statuspoll/somfy_awning_statuspoll_success_sx1276.yaml — commander ==
  // this hub's own node ID (C0FFEE), rendered via describe_last_commander() as "(this hub)".
  TestableHubComponent comp;
  comp.node_id_[0] = 0xC0;
  comp.node_id_[1] = 0xFF;
  comp.node_id_[2] = 0xEE;
  comp.add_device("30E1F2");

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0x30, 0xE1, 0xF2};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[14] = {0x04, 0x00, 0xC8, 0x00, 0x04, 0xE8, 0x00, 0x23, 0xC0, 0xFF, 0xEE, 0x01, 0x00, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("30E1F2");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(std::memcmp(dev->last_commander, "\xC0\xFF\xEE", 3), 0);
  EXPECT_EQ(comp.describe_last_commander(*dev), "C0FFEE (this hub)");
}

// ============================================================================
// detail::describe_prediction() — appended to log_status_update()'s line when a live optimistic
// prediction disagrees with what the device just reported. Pure, so tested directly (ESP_LOG* is a
// no-op stub on host). A prediction must never be substituted into the observed target/motion
// fields, only annotated alongside them.
// ============================================================================

TEST(HubStatus, DescribePredictionIsEmptyWhenNoPredictionStands) {
  IoDevice dev{};
  dev.target = 40.0f;
  dev.is_stopped = true;
  EXPECT_TRUE(detail::describe_prediction(dev).empty());
}

TEST(HubStatus, DescribePredictionIsEmptyWhenThePredictionAgreesWithTheObservation) {
  IoDevice dev{};
  dev.target = 100.0f;
  dev.is_stopped = false;
  dev.optimistic.target = 100.0f;
  dev.optimistic.motion = OptimisticState::Motion::MOVING;
  EXPECT_TRUE(detail::describe_prediction(dev).empty())
      << "a prediction that merely confirms the observation adds nothing to the line";
}

TEST(HubStatus, DescribePredictionNamesADivergingTargetWithoutSubstitutingTheObserved) {
  IoDevice dev{};
  dev.target = 0.0f;               // what the device last reported
  dev.optimistic.target = 100.0f;  // what the hub predicts
  const std::string out = detail::describe_prediction(dev);
  EXPECT_NE(out.find("target=100%"), std::string::npos) << "the predicted target is named";
  EXPECT_EQ(out.find("target=0%"), std::string::npos) << "the observed target is not replaced by the prediction";
}

TEST(HubStatus, DescribePredictionNamesADivergingMotion) {
  IoDevice dev{};
  dev.is_stopped = false;                                    // the device reports moving
  dev.optimistic.motion = OptimisticState::Motion::STOPPED;  // a STOP was sent; hub predicts stopped
  EXPECT_NE(detail::describe_prediction(dev).find("stopped"), std::string::npos);
}

// The exact case that made the prior issue #95 analysis misread the log: an optimistic CLOSE
// (target 100) then an EXECUTE ack whose echoed target field is stale, applied with
// trust_position=false so the overlay stands. The log line must flag that target=<observed> is
// not the whole story.
TEST(HubStatus, DescribePredictionFlagsTheIssue95ExecuteAckScenario) {
  TestableHubComponent comp;
  comp.add_device("ABC123", {DeviceType::ROLLER_SHUTTER, 0});
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 100.0f));

  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, own);
  set_src(frame, device);
  uint8_t payload[8] = {0x04, 0x60, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00};  // moving, stale echoed fields
  ASSERT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));
  comp.update_device_status_(frame, /*trust_position=*/false);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  const std::string out = detail::describe_prediction(*dev);
  EXPECT_FALSE(out.empty());
  EXPECT_NE(out.find("100%"), std::string::npos) << "and it names the predicted target";
}
