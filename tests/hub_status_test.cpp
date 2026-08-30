#include "hub_core.h"
#include "hub_internal.h"
#include "radio_interface.h"
#include "proto_frame.h"
#include "esphome/core/component.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>

using namespace esphome::home_io_control;
using test::encode_device_metadata;
using test::make_rx_packet;
using test::RxTestableComponent;
using test::setup_rx_test_component;
using test::TestableHubComponent;

// ============================================================================
// HubStatus test suite
// ============================================================================
// Inbound status handling: update_device_status_/process_received_packet_ (hub_status.cpp) —
// status updates, INFO2/error responses, remote-activity-triggered polling, linked remotes,
// and RSSI/last-seen link-health tracking. Split out of hub_core_test.cpp (finding #11) since
// these all exercise the same file's responsibilities rather than hub_core.cpp's own setup/loop/
// queue-dispatch surface. Uses the HubStatus suite name, distinct from hub_core_test.cpp's
// HubCore suite.

TEST(HubStatus, PrivateResponseMarkerTargetUsesCurrentWhenStopped) {
  TestableHubComponent comp;
  comp.add_device("ABC123");

  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, own);
  set_src(frame, device);
  uint8_t payload[8] = {STATUS_STOPPED, 0x00, POS_UNKNOWN, 0x00, 0x64, 0x00, 0x00, 0x00};
  ASSERT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));

  comp.update_device_status_(frame);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->position, 50.0f) << "valid current should decode to 50 percent";
  EXPECT_FLOAT_EQ(dev->target, 50.0f) << "marker target should normalize to current when stopped";
  EXPECT_TRUE(dev->is_stopped) << "matching normalized target/current should remain stopped";
}

TEST(HubStatus, StoppedFlagMismatchKeepsDeviceMoving) {
  TestableHubComponent comp;
  comp.add_device("ABC123");

  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, own);
  set_src(frame, device);
  uint8_t payload[8] = {STATUS_STOPPED, 0x00, 0xC8, 0x00, 0x64, 0x00, 0x00, 0x00};
  ASSERT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));

  comp.update_device_status_(frame);

  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->target, 100.0f) << "valid target should decode to 100 percent";
  EXPECT_FLOAT_EQ(dev->position, 50.0f) << "valid current should decode to 50 percent";
  EXPECT_FALSE(dev->is_stopped) << "stopped flag should be overridden when target and current are still far apart";
}

// ============================================================================
// Optimistic-overlay supersede rule (a decoded observation replaces a prediction, per axis)
// ============================================================================

// §7.7 — a trusted status observation supersedes the position prediction.
TEST(HubStatus, TrustedStatusObservationSupersedesPositionPrediction) {
  TestableHubComponent comp;
  comp.add_device("ABC123", {DeviceType::ROLLER_SHUTTER, 0, false});
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 25.0f));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);
  ASSERT_EQ(dev->optimistic.motion, OptimisticState::Motion::MOVING);

  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, own);
  set_src(frame, device);
  // stopped, target=0xC800 (100%), current=0xC800 (100%)
  uint8_t payload[8] = {STATUS_STOPPED, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  ASSERT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));

  comp.update_device_status_(frame);

  EXPECT_EQ(dev->optimistic.target, UNKNOWN_POSITION) << "a decoded position supersedes the position prediction";
  EXPECT_EQ(dev->optimistic.motion, OptimisticState::Motion::NONE);
  EXPECT_FLOAT_EQ(dev->position, 100.0f);
  EXPECT_FLOAT_EQ(effective_target(*dev), 100.0f) << "effective now follows the observation";
}

// §7.8 — a trusted extended status observation supersedes the tilt prediction.
TEST(HubStatus, TrustedExtendedStatusObservationSupersedesTiltPrediction) {
  TestableHubComponent comp;
  comp.add_device("ABC123", {DeviceType::VENETIAN_BLIND, 0, false});
  ASSERT_TRUE(comp.apply_optimistic_tilt("ABC123", 70.0f));
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 25.0f));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, own);
  set_src(frame, device);
  // stopped, target/current 0xC800, then the extended tilt block: selector at [12], raw 0x6400 -> 50%.
  uint8_t payload[15] = {STATUS_STOPPED,       0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                         STATUS_TILT_SELECTOR, 0x64, 0x00};
  ASSERT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));

  comp.update_device_status_(frame);

  EXPECT_EQ(dev->optimistic.tilt, UNKNOWN_POSITION) << "a decoded tilt supersedes the tilt prediction";
  EXPECT_FLOAT_EQ(dev->tilt, 50.0f);
  EXPECT_EQ(dev->optimistic.target, UNKNOWN_POSITION) << "the decoded position also supersedes the position prediction";
}

// §7.9 — regression guard: an execute ack decoded with trust_position=false clears *nothing*.
TEST(HubStatus, ExecuteAckWithoutTrustedPositionSupersedesNothing) {
  TestableHubComponent comp;
  comp.add_device("ABC123", {DeviceType::VENETIAN_BLIND, 0, false});
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 25.0f));
  ASSERT_TRUE(comp.apply_optimistic_tilt("ABC123", 70.0f));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, own);
  set_src(frame, device);
  // The ack reports stopped and echoes pre-command (here: zeroed) target/current.
  uint8_t payload[8] = {STATUS_STOPPED, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  ASSERT_TRUE(set_cmd(frame, CMD_PRIVATE_RESP, payload, sizeof(payload)));

  comp.update_device_status_(frame, /*trust_position=*/false);

  EXPECT_FLOAT_EQ(dev->optimistic.target, 25.0f) << "the execute-ack path decodes no position, so it clears nothing";
  EXPECT_EQ(dev->optimistic.motion, OptimisticState::Motion::MOVING);
  EXPECT_FLOAT_EQ(dev->optimistic.tilt, 70.0f);
  EXPECT_FLOAT_EQ(effective_target(*dev), 25.0f) << "the prediction still drives the effective value";
  EXPECT_FALSE(effective_is_stopped(*dev)) << "motion is still MOVING even though the ack said stopped";
}

// §7.10 — an unsolicited 0x71 supersedes the position prediction but leaves the tilt prediction standing.
TEST(HubStatus, UnsolicitedStatusUpdateSupersedesPositionButNotTilt) {
  TestableHubComponent comp;
  comp.add_device("ABC123", {DeviceType::VENETIAN_BLIND, 0, false});
  ASSERT_TRUE(comp.apply_optimistic_target("ABC123", 25.0f));
  ASSERT_TRUE(comp.apply_optimistic_tilt("ABC123", 70.0f));
  auto *dev = comp.get_device("ABC123");
  ASSERT_NE(dev, nullptr);

  IoFrame frame{};
  init_frame(frame, true, false, false, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  uint8_t device[3] = {0xAB, 0xC1, 0x23};
  set_dst(frame, own);
  set_src(frame, device);
  // 0x71 layout: stopped flag at [0], target MSB at [5], current MSB at [7]; both 0xC800 -> 100%.
  uint8_t payload[11] = {STATUS_STOPPED, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  ASSERT_TRUE(set_cmd(frame, CMD_STATUS_UPDATE, payload, sizeof(payload)));

  comp.update_device_status_(frame);

  EXPECT_EQ(dev->optimistic.target, UNKNOWN_POSITION) << "the 0x71 decodes a position, superseding that prediction";
  EXPECT_EQ(dev->optimistic.motion, OptimisticState::Motion::NONE);
  EXPECT_FLOAT_EQ(dev->optimistic.tilt, 70.0f) << "the 0x71 path decodes no tilt, so the tilt prediction stands";
  EXPECT_FLOAT_EQ(effective_tilt(*dev), 70.0f);
}

// ============================================================================
// Remote activity detection tests (Issue #3)
// ============================================================================

TEST(HubStatus, RemoteActivity_TriggersDelayedPoll) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  // Construct a 0x00 execute command from a remote to our registered device
  IoFrame f{};
  init_frame(f, true, true, false, false);  // 2W, start frame
  uint8_t src[3] = {0x43, 0x44, 0xE3};      // some remote
  uint8_t dst[3] = {0x05, 0x4E, 0x17};      // our registered device
  set_src(f, src);
  set_dst(f, dst);
  uint8_t exec_data[3] = {0xC8, 0x00, 0x00};
  set_cmd(f, CMD_EXECUTE, exec_data, 3);

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "frame should serialize";

  comp.process_received_packet_(pkt);

  // Should schedule the standard remote-activity timeout for the device.
  EXPECT_EQ(comp.last_timeout_ms_, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS) << "should schedule 2s timeout";
  EXPECT_NE(comp.last_timeout_name_.find("054E17"), std::string::npos) << "timeout name should contain device ID";
  ASSERT_TRUE(comp.last_timeout_callback_) << "callback should be set";

  // Invoke the callback — should queue a status request
  comp.last_timeout_callback_();
  ASSERT_EQ(comp.op_queue_.size(), 1u) << "callback should queue one operation";
  EXPECT_EQ(comp.op_queue_.front().type, PendingOperationType::REQUEST_STATUS);
  EXPECT_EQ(comp.op_queue_.front().device_id, "054E17");
}

TEST(HubStatus, RemoteActivityWithoutConfiguredIntervalArmsTrackedSettlePolling) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  IoFrame trigger{};
  init_frame(trigger, true, true, false, false);
  uint8_t remote[3] = {0x43, 0x44, 0xE3};
  uint8_t device[3] = {0x05, 0x4E, 0x17};
  set_src(trigger, remote);
  set_dst(trigger, device);
  uint8_t exec_data[3] = {0xC8, 0x00, 0x00};
  set_cmd(trigger, CMD_EXECUTE, exec_data, sizeof(exec_data));

  RadioRxPacket trigger_pkt = make_rx_packet(trigger);
  ASSERT_GT(trigger_pkt.len, 0) << "frame should serialize";
  comp.process_received_packet_(trigger_pkt);

  ASSERT_NE(comp.get_device("054E17"), nullptr);
  EXPECT_NE(comp.poll_policy_.get_poll_deadline("054E17"), 0u)
      << "remote activity without an explicit interval should arm bounded tracked polling";

  IoFrame moving{};
  init_frame(moving, true, false, true, false);
  uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  set_src(moving, device);
  set_dst(moving, own);
  uint8_t payload[8] = {0x00, 0x00, 0xC8, 0x00, 0x32, 0x00, 0x00, 0x05};
  set_cmd(moving, CMD_PRIVATE_RESP, payload, sizeof(payload));

  comp.update_device_status_(moving);

  EXPECT_NE(comp.poll_policy_.get_next_update("054E17"), 0u)
      << "a moving poll response after remote activity should schedule a hint-driven follow-up poll";
}

TEST(HubStatus, RemoteActivity_UnregisteredDevice_NoTrigger) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  // Frame addressed to an unregistered device
  IoFrame f{};
  init_frame(f, true, true, false, false);
  uint8_t src[3] = {0x43, 0x44, 0xE3};
  uint8_t dst[3] = {0xAA, 0xBB, 0xCC};  // not registered
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_EXECUTE);

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "frame should serialize";

  comp.process_received_packet_(pkt);

  // Should NOT schedule a timeout — falls through to unhandled_cmd
  EXPECT_TRUE(comp.last_timeout_name_.empty()) << "unregistered dst should not trigger timeout";
  EXPECT_TRUE(comp.op_queue_.empty()) << "should not queue any operation";
}

TEST(HubStatus, RemoteActivity_OwnEcho_NoTrigger) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  // Frame where src is our own node ID (echo of our own TX)
  IoFrame f{};
  init_frame(f, true, true, false, false);
  uint8_t src[3] = {0xC0, 0xFF, 0xEE};  // our node ID
  uint8_t dst[3] = {0x05, 0x4E, 0x17};  // our registered device
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_PRIVATE);

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "frame should serialize";

  comp.process_received_packet_(pkt);

  // Should NOT schedule a timeout — it's our own frame
  EXPECT_TRUE(comp.last_timeout_name_.empty()) << "own echo should not trigger timeout";
  EXPECT_TRUE(comp.op_queue_.empty()) << "should not queue any operation";
}

TEST(HubStatus, RemoteActivity_LinkedRemote_TriggersDelayedPoll) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  // Link remote 9D6085 to device 054E17
  comp.add_linked_remote("9D6085", "054E17");

  // Construct a 1W frame from the linked remote to a different address (1W addressing)
  IoFrame f{};
  init_frame(f, false, true, true, false);  // 1W mode
  uint8_t src[3] = {0x9D, 0x60, 0x85};      // linked remote
  uint8_t dst[3] = {0x00, 0x01, 0xBF};      // 1W device address (different from 2W ID)
  set_src(f, src);
  set_dst(f, dst);
  uint8_t exec_data[3] = {0x00, 0x00, 0x00};
  set_cmd(f, CMD_EXECUTE, exec_data, 3);

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "frame should serialize";

  comp.process_received_packet_(pkt);

  // Should schedule the standard remote-activity timeout for the linked device.
  EXPECT_EQ(comp.last_timeout_ms_, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS) << "should schedule 2s timeout";
  EXPECT_NE(comp.last_timeout_name_.find("054E17"), std::string::npos)
      << "timeout name should contain linked device ID";
  ASSERT_TRUE(comp.last_timeout_callback_) << "callback should be set";

  // Invoke the callback — should queue a status request
  comp.last_timeout_callback_();
  ASSERT_EQ(comp.op_queue_.size(), 1u) << "callback should queue one operation";
  EXPECT_EQ(comp.op_queue_.front().type, PendingOperationType::REQUEST_STATUS);
  EXPECT_EQ(comp.op_queue_.front().device_id, "054E17");
}

TEST(HubStatus, LinkedRemotes_MultipleRemotesOneDevice) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);

  // Link two remotes to the same device
  comp.add_linked_remote("AABBCC", "054E17");
  comp.add_linked_remote("DDEEFF", "054E17");

  // Frame from second remote
  IoFrame f{};
  init_frame(f, false, true, true, false);
  uint8_t src[3] = {0xDD, 0xEE, 0xFF};
  uint8_t dst[3] = {0x00, 0x01, 0xBF};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_EXECUTE);

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "frame should serialize";

  comp.process_received_packet_(pkt);

  EXPECT_EQ(comp.last_timeout_ms_, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS) << "should schedule 2s timeout";
  EXPECT_NE(comp.last_timeout_name_.find("054E17"), std::string::npos) << "timeout name should contain device ID";
}

TEST(HubStatus, LinkedRemotes_OneRemoteMultipleDevices) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  comp.add_device("415684");  // second device

  // One remote controls two devices
  comp.add_linked_remote("AABBCC", "054E17");
  comp.add_linked_remote("AABBCC", "415684");

  // Frame from the shared remote
  IoFrame f{};
  init_frame(f, false, true, true, false);
  uint8_t src[3] = {0xAA, 0xBB, 0xCC};
  uint8_t dst[3] = {0x00, 0x01, 0xBF};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_EXECUTE);

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "frame should serialize";

  comp.process_received_packet_(pkt);

  // The last set_timeout call wins in our stub, but both should have been called.
  // Verify at least one timeout was scheduled with 2s delay.
  EXPECT_EQ(comp.last_timeout_ms_, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS) << "should schedule 2s timeout";
}

// ============================================================================
// Inbound status update tests (update_device_status_ paths)
// ============================================================================

TEST(HubStatus, StatusUpdateFrameHandling) {
  // The CMD_STATUS_UPDATE path through process_received_packet_ triggers
  // authentication (authenticate_request_) which requires a challenge response
  // from the mock radio. Test update_device_status_ directly instead.
  TestableHubComponent comp;
  comp.add_device("054E17");

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // Status update payload: stopped, target at [5..6]=0xC800 (100%), current at [7..8]=0xC800 (100%)
  uint8_t payload[11] = {STATUS_STOPPED, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->position, 100.0f) << "status update should decode position to 100%";
  EXPECT_TRUE(dev->is_stopped) << "device should be stopped";
}

TEST(HubStatus, StatusUpdateMovingFrameHandling) {
  TestableHubComponent comp;
  comp.add_device("054E17");

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // Status update: moving (STATUS_STOPPED NOT set), target at [5..6]=0xC800 (100%), current at [7..8]=0x3200 (25%)
  uint8_t payload[11] = {0x00, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0x32, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->target, 100.0f) << "target should decode to 100%";
  EXPECT_FLOAT_EQ(dev->position, 25.0f) << "position should decode to 25%";
  EXPECT_FALSE(dev->is_stopped) << "device should be moving";
}

TEST(HubStatus, GetInfo2RespUpdatesDeviceType) {
  TestableHubComponent comp;
  comp.add_device("054E17");

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // INFO2 response: data[10..11] use the shared packed type/subtype metadata layout.
  uint8_t payload[12] = {0};
  encode_device_metadata(DeviceType::ROLLER_SHUTTER, 0, &payload[10]);
  set_cmd(f, CMD_GET_INFO2_RESP, payload, sizeof(payload));

  // update_device_status_ is called directly (not through process_received_packet_)
  comp.update_device_status_(f);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->type, DeviceType::ROLLER_SHUTTER) << "INFO2 response should update device type";
  EXPECT_EQ(dev->subtype, 0u) << "INFO2 response should update device subtype";
}

TEST(HubStatus, GetInfo2RespDoesNotOverwriteDeclaredType) {
  TestableHubComponent comp;
  comp.add_device("054E17", {DeviceType::AWNING, 7, false});

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[12] = {0};
  encode_device_metadata(DeviceType::ROLLER_SHUTTER, 0, &payload[10]);
  set_cmd(f, CMD_GET_INFO2_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->type, DeviceType::AWNING) << "INFO2 must not overwrite a YAML-declared device type";
  EXPECT_EQ(dev->subtype, 7u) << "INFO2 must not overwrite a YAML-declared subtype";
}

TEST(HubStatus, ErrorRespDoesNotMutateTrackedPosition) {
  TestableHubComponent comp;
  comp.add_device("054E17");

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  dev->position = 25.0f;
  dev->target = 40.0f;
  dev->is_stopped = false;

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[1] = {RESULT_LIMITATION_BY_WIND};
  set_cmd(f, CMD_ERROR_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_FLOAT_EQ(dev->position, 25.0f) << "error responses should not overwrite last known position";
  EXPECT_FLOAT_EQ(dev->target, 40.0f) << "error responses should not overwrite last known target";
  EXPECT_FALSE(dev->is_stopped) << "error responses should not change movement state";
}

TEST(HubStatus, ErrorRespSetsLastResultCode) {
  TestableHubComponent comp;
  comp.add_device("054E17");

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->last_result_code, 0u) << "newly added device should start with no recorded result";

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[1] = {RESULT_LIMITATION_BY_RAIN};
  set_cmd(f, CMD_ERROR_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_result_code, RESULT_LIMITATION_BY_RAIN) << "unsolicited 0xFE should record the result code";
  EXPECT_NE(dev->last_result_at_ms, 0u) << "unsolicited 0xFE should stamp a recorded-at timestamp";
}

TEST(HubStatus, ErrorRespWithNonLimitationCodeStillRecords) {
  TestableHubComponent comp;
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  uint8_t payload[1] = {RESULT_COMMAND_COMPLETED_OK};
  set_cmd(f, CMD_ERROR_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_result_code, RESULT_COMMAND_COMPLETED_OK)
      << "non-limitation result codes should still be recorded on the device";
}

TEST(HubStatus, ErrorRespWithEmptyPayloadDoesNotSetLastResultCode) {
  TestableHubComponent comp;
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);

  IoFrame f{};
  init_frame(f, true, false, true, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  set_cmd(f, CMD_ERROR_RESP);  // zero-length payload — fails the ERROR_RESPONSE_MIN_DATA_LEN guard

  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_result_code, 0u) << "a too-short 0xFE must keep hitting the existing rejection path unchanged";
}

TEST(HubStatus, SuccessfulPrivateResponseClearsLastResultCode) {
  TestableHubComponent comp;
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  dev->last_result_code = RESULT_LIMITATION_BY_WIND;
  dev->last_result_at_ms = 12345;

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // Bytes 0=flags(stopped), 2-3=target(0x0000=0%), 4-5=current(0x0000=0%).
  uint8_t payload[6] = {STATUS_STOPPED, 0x00, 0x00, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_PRIVATE_RESP, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_result_code, 0u) << "a subsequent successful status reply should clear a stale result reason";
  EXPECT_EQ(dev->last_result_at_ms, 0u) << "clearing the result code should also clear its timestamp";
}

TEST(HubStatus, SuccessfulStatusUpdateClearsLastResultCode) {
  TestableHubComponent comp;
  comp.add_device("054E17");
  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  dev->last_result_code = RESULT_LIMITATION_BY_RAIN;
  dev->last_result_at_ms = 12345;

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // 0x71 status update: byte0=flags(stopped), byte1=status byte, bytes5-6=target, bytes7-8=current.
  uint8_t payload[11] = {STATUS_STOPPED, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);

  EXPECT_EQ(dev->last_result_code, 0u)
      << "a subsequent device-initiated status update should clear a stale result reason";
  EXPECT_EQ(dev->last_result_at_ms, 0u) << "clearing the result code should also clear its timestamp";
}

// The 0x71 Command Originator used to be read at data[1] — the status byte, which names no
// ORIGINATOR_* value, so the log line rendered "unknown" on every frame ever captured. The
// rendered line is unobservable on host (ESP_LOG* is a no-op stub), so the log call was split
// into detail::describe_status_update_originator() (hub_internal.h) and these tests assert that
// pure function's output directly — the same call the production log line makes. The second test
// pins that the length guard skips only the log line and does not tighten branch acceptance.
TEST(HubStatus, StatusUpdateOriginatorIsAtOffset14AndDecodePathUndisturbed) {
  TestableHubComponent comp;
  comp.add_device("054E17");

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // Real fixture bytes: issue_45_unsolicited_status_update_burst frame 0 (16-byte 0x71 payload).
  // data[0]=0x04 moving, data[1]=0x61 status byte, data[5..6]=C8 00 target 100%,
  // data[7..8]=D4 00 current unknown, data[14]=0x01 Command Originator (User Remote Control).
  uint8_t payload[16] = {0x04, 0x61, 0x10, 0x0A, 0x0B, 0xC8, 0x00, 0xD4,
                         0x00, 0xFF, 0xFF, 0x0A, 0x6F, 0x56, 0x01, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  comp.update_device_status_(f);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->target, 100.0f) << "target at data[5..6]=C8 00 should still decode to 100%";
  EXPECT_FLOAT_EQ(dev->position, UNKNOWN_POSITION) << "current at data[7..8]=D4 00 is unknown, unchanged by the fix";
  EXPECT_FALSE(dev->is_stopped) << "data[0]=0x04 has STATUS_STOPPED clear";

  // The production log line's own argument. Reading data[1] instead would render
  // "unknown(0x61)" here, which is exactly the bug this pins.
  EXPECT_EQ(detail::describe_status_update_originator(f), "user_remote(0x01)")
      << "the Command Originator is data[14], not data[1] (the status byte 0x61)";
}

TEST(HubStatus, StatusUpdateMinimumLengthFrameStillApplied) {
  TestableHubComponent comp;
  comp.add_device("054E17");

  IoFrame f{};
  init_frame(f, true, false, false, false);
  uint8_t src[3] = {0x05, 0x4E, 0x17};
  uint8_t dst[3] = {0xC0, 0xFF, 0xEE};
  set_src(f, src);
  set_dst(f, dst);
  // 11-byte 0x71 (== STATUS_UPDATE_MIN_DATA_LEN): stopped, target [5..6]=C8 00, current [7..8]=C8 00.
  // Too short to carry data[14], so the originator log line is skipped — but the frame must still
  // be decoded and applied.
  uint8_t payload[11] = {STATUS_STOPPED, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  EXPECT_TRUE(detail::describe_status_update_originator(f).empty())
      << "a frame too short to carry data[14] must report no originator rather than a stale byte";

  comp.update_device_status_(f);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->target, 100.0f) << "a minimum-length 0x71 must still decode its target";
  EXPECT_FLOAT_EQ(dev->position, 100.0f) << "a minimum-length 0x71 must still decode its position";
  EXPECT_TRUE(dev->is_stopped) << "the originator length guard must not tighten branch acceptance";
}

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
  EXPECT_NE(comp.last_timeout_name_.find("054E17"), std::string::npos) << "timeout name should reference our device";
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
  uint8_t payload[8] = {0x00, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};  // moving, target/current=100%
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
  uint8_t payload[11] = {0x00, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0x00, 0x00};
  set_cmd(f, CMD_STATUS_UPDATE, payload, sizeof(payload));

  RadioRxPacket pkt = make_rx_packet(f);
  ASSERT_GT(pkt.len, 0) << "status update frame should serialize";

  comp.process_received_packet_(pkt);

  auto *dev = comp.get_device("054E17");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->position, UNKNOWN_POSITION) << "unauthenticated foreign status update must not set position";
  EXPECT_TRUE(dev->is_stopped) << "unauthenticated foreign status update must not change is_stopped from its default";
}
