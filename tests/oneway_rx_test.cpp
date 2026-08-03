/// @file oneway_rx_test.cpp
/// @brief Tests for the 1W sender Home Assistant event (hub_status.cpp, hub_internal.h).

#include "hub_core.h"
#include "hub_internal.h"
#include "proto_frame.h"

#include "esphome/components/api/custom_api_device.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <algorithm>
#include <cstring>

using namespace esphome::home_io_control;

namespace {

class RxTestableComponent : public IOHomeControlComponent {
 public:
  using IOHomeControlComponent::process_received_packet_;
  using IOHomeControlComponent::resolve_1w_target_devices_;
  using IOHomeControlComponent::node_id_;
  using IOHomeControlComponent::initialized_;
  using IOHomeControlComponent::radio_;
};

void setup_component(RxTestableComponent &comp, MockRadio &radio) {
  memcpy(comp.node_id_, test::OWN_ID, NODE_ID_SIZE);
  comp.initialized_ = true;
  comp.radio_ = &radio;
}

RadioRxPacket make_rx_packet(const IoFrame &frame) {
  RadioRxPacket pkt{};
  pkt.len = serialize(frame, pkt.data, sizeof(pkt.data));
  pkt.freq_hz = FREQ_CH2;
  return pkt;
}

const uint8_t REMOTE_ID[NODE_ID_SIZE] = {0xAA, 0xBB, 0xCC};
const uint8_t BROADCAST_ROLLER[NODE_ID_SIZE] = {0x00, 0x00, 0xBF};  // typed broadcast: ROLLER_SHUTTER

/// Build a 1W execute frame: originator, ACEI, main0/main1 select the decoded intent.
IoFrame make_1w_execute(uint8_t main0, uint8_t main1 = 0x00) {
  IoFrame f{};
  init_frame(f, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  set_dst(f, BROADCAST_ROLLER);
  set_src(f, REMOTE_ID);
  const uint8_t payload[4] = {ORIGINATOR_USER_REMOTE, 0x41, main0, main1};
  set_cmd(f, CMD_EXECUTE, payload, sizeof(payload));
  return f;
}

/// Build a 1W frame with a command that carries no decodable intent (e.g. write-private).
IoFrame make_1w_non_intent_frame() {
  IoFrame f{};
  init_frame(f, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  set_dst(f, BROADCAST_ROLLER);
  set_src(f, REMOTE_ID);
  const uint8_t payload[2] = {0x00, 0x00};
  set_cmd(f, CMD_WRITE_PRIVATE, payload, sizeof(payload));
  return f;
}

}  // namespace

// ============================================================================
// build_sender_event_data() — pure mapping from decoded info to event data
// ============================================================================

TEST(OnewayRx, BuildEventData_MapsAllFieldsForCloseIntent) {
  IoFrame frame = make_1w_execute(0xC8, 0x00);  // main0=200 -> CLOSE
  const OneWayFrameInfo info = decode_1w_frame(frame);
  ASSERT_TRUE(info.has_intent);

  const auto data = detail::build_sender_event_data(info, /*linked=*/true);

  EXPECT_EQ(data.at("remote_id"), node_id_to_string(REMOTE_ID));
  EXPECT_EQ(data.at("target_class"), std::string(address_class_name(AddressClass::BROADCAST_TYPE)));
  EXPECT_EQ(data.at("target_type"), std::string(device_type_name(DeviceType::ROLLER_SHUTTER)));
  EXPECT_EQ(data.at("cmd"), std::string(command_name(CMD_EXECUTE)) + "(0x00)");
  EXPECT_EQ(data.at("intent"), "CLOSE");
  EXPECT_EQ(data.at("originator"), std::string(originator_name(ORIGINATOR_USER_REMOTE)));
  EXPECT_EQ(data.at("acei_level"), std::string(acei_level_name(ACEI_LEVEL_USER_HIGH)));
  EXPECT_EQ(data.at("linked"), "true");
}

TEST(OnewayRx, BuildEventData_LinkedFalse) {
  IoFrame frame = make_1w_execute(0x00, 0x00);  // main0=0 -> OPEN
  const OneWayFrameInfo info = decode_1w_frame(frame);

  const auto data = detail::build_sender_event_data(info, /*linked=*/false);
  EXPECT_EQ(data.at("intent"), "OPEN");
  EXPECT_EQ(data.at("linked"), "false");
}

TEST(OnewayRx, BuildEventData_StopIntent) {
  IoFrame frame = make_1w_execute(POS_STOP, 0x00);
  const OneWayFrameInfo info = decode_1w_frame(frame);

  const auto data = detail::build_sender_event_data(info, /*linked=*/false);
  EXPECT_EQ(data.at("intent"), "STOP");
}

// ============================================================================
// is_exposed_sender() — the exposed_senders allowlist check
// ============================================================================

TEST(OnewayRx, IsExposedSender_EmptyAllowlistRejectsEverything) {
  const std::vector<std::string> exposed_senders;
  EXPECT_FALSE(detail::is_exposed_sender(exposed_senders, "AABBCC"));
}

TEST(OnewayRx, IsExposedSender_MatchesOnlyListedIds) {
  const std::vector<std::string> exposed_senders{"AABBCC", "9D6085"};
  EXPECT_TRUE(detail::is_exposed_sender(exposed_senders, "AABBCC"));
  EXPECT_TRUE(detail::is_exposed_sender(exposed_senders, "9D6085"));
  EXPECT_FALSE(detail::is_exposed_sender(exposed_senders, "112233"));
}

// ============================================================================
// End-to-end: process_received_packet_() -> fire_homeassistant_event()
// ============================================================================

TEST(OnewayRx, RemoteButtonEvent_NotFiredByDefaultEvenWithIntent) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  // exposed_senders is empty by default — no add_exposed_sender() call.

  IoFrame frame = make_1w_execute(0xC8, 0x00);  // CLOSE
  RadioRxPacket pkt = make_rx_packet(frame);
  ASSERT_GT(pkt.len, 0);

  comp.process_received_packet_(pkt);

  EXPECT_TRUE(api_server.events_.empty())
      << "a remote not on the exposed_senders allowlist must not fire an event, even with a decodable intent";
}

TEST(OnewayRx, RemoteButtonEvent_FiresOnceForAllowlistedRemote) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.add_exposed_sender(node_id_to_string(REMOTE_ID));

  IoFrame frame = make_1w_execute(0xC8, 0x00);  // CLOSE
  RadioRxPacket pkt = make_rx_packet(frame);
  ASSERT_GT(pkt.len, 0);

  comp.process_received_packet_(pkt);

  ASSERT_EQ(api_server.events_.size(), 1u);
  const auto &event = api_server.events_.front();
  EXPECT_EQ(event.event_type, "esphome.home_io_control_sender_event");
  EXPECT_EQ(event.data.at("intent"), "CLOSE");
  EXPECT_EQ(event.data.at("linked"), "false");
}

TEST(OnewayRx, RemoteButtonEvent_LinkedTrueWhenRemoteLinkedToDevice) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.add_device("112233");
  comp.add_linked_remote(node_id_to_string(REMOTE_ID), "112233");
  comp.add_exposed_sender(node_id_to_string(REMOTE_ID));

  IoFrame frame = make_1w_execute(0xC8, 0x00);
  RadioRxPacket pkt = make_rx_packet(frame);
  comp.process_received_packet_(pkt);

  ASSERT_EQ(api_server.events_.size(), 1u);
  EXPECT_EQ(api_server.events_.front().data.at("linked"), "true");
}

TEST(OnewayRx, RemoteButtonEvent_LinkedButNotAllowlistedStillSuppressesEvent) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.add_device("112233");
  comp.add_linked_remote(node_id_to_string(REMOTE_ID), "112233");
  // Deliberately no add_exposed_sender(): linking a remote to a device must not, by itself,
  // grant it HA-event access — the two mechanisms are independent.

  IoFrame frame = make_1w_execute(0xC8, 0x00);
  RadioRxPacket pkt = make_rx_packet(frame);
  comp.process_received_packet_(pkt);

  EXPECT_TRUE(api_server.events_.empty()) << "linked_remotes must not implicitly grant exposed_senders access";
}

TEST(OnewayRx, RemoteButtonEvent_NoEventForNonIntentFrame) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.add_exposed_sender(node_id_to_string(REMOTE_ID));

  IoFrame frame = make_1w_non_intent_frame();
  RadioRxPacket pkt = make_rx_packet(frame);
  ASSERT_GT(pkt.len, 0);

  comp.process_received_packet_(pkt);

  EXPECT_TRUE(api_server.events_.empty()) << "a 1W frame without a decodable intent must not fire an event";
}

TEST(OnewayRx, RemoteButtonEvent_SkippedWhenApiDisconnected) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();
  api_server.set_connected(false);

  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.add_exposed_sender(node_id_to_string(REMOTE_ID));

  IoFrame frame = make_1w_execute(0xC8, 0x00);
  RadioRxPacket pkt = make_rx_packet(frame);
  comp.process_received_packet_(pkt);

  EXPECT_TRUE(api_server.events_.empty());
}

TEST(OnewayRx, RemoteButtonEvent_BurstOfFourYieldsOneEvent) {
  esphome::api::APIServer api_server;
  esphome::api::ScopedGlobalApiServer scoped_api_server(api_server);
  api_server.reset();

  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.add_exposed_sender(node_id_to_string(REMOTE_ID));

  IoFrame frame = make_1w_execute(0xC8, 0x00);
  RadioRxPacket pkt = make_rx_packet(frame);

  // 1W remotes repeat each command 4x at 40ms intervals; the existing dedup window (2s)
  // collapses this into a single logical press, so the event must fire only once.
  for (int i = 0; i < 4; i++)
    comp.process_received_packet_(pkt);

  EXPECT_EQ(api_server.events_.size(), 1u) << "dedup window should collapse the 4x burst to one event";
}

// ============================================================================
// Optimistic state for linked 1W remotes
// ============================================================================

TEST(OnewayRx, OptimisticState_LinkedDeviceGetsTargetFromCloseIntent) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.add_device("112233", {DeviceType::ROLLER_SHUTTER, 0, false});
  comp.add_linked_remote(node_id_to_string(REMOTE_ID), "112233");

  IoFrame frame = make_1w_execute(0xC8, 0x00);  // CLOSE -> IO target 100
  RadioRxPacket pkt = make_rx_packet(frame);
  comp.process_received_packet_(pkt);

  const auto *dev = comp.get_device("112233");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->target, 100.0f) << "CLOSE intent should set an optimistic target of 100 (closed)";
  EXPECT_FALSE(dev->is_stopped) << "optimistic apply should mark the device as moving";
  EXPECT_EQ(comp.last_timeout_ms_, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS)
      << "the confirmation poll should still be scheduled at the normal delay for a non-STOP intent";
}

TEST(OnewayRx, OptimisticState_StopClearsTargetAndPollsImmediately) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.add_device("112233", {DeviceType::ROLLER_SHUTTER, 0, false});
  comp.add_linked_remote(node_id_to_string(REMOTE_ID), "112233");
  comp.get_device("112233")->target = 50.0f;
  comp.get_device("112233")->is_stopped = false;

  IoFrame frame = make_1w_execute(POS_STOP, 0x00);
  RadioRxPacket pkt = make_rx_packet(frame);
  comp.process_received_packet_(pkt);

  const auto *dev = comp.get_device("112233");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->target, UNKNOWN_POSITION) << "STOP intent should clear any optimistic target";
  EXPECT_TRUE(dev->is_stopped) << "STOP must also mark the device stopped, or the HA cover UI keeps animating";
  EXPECT_EQ(comp.last_timeout_ms_, 0u) << "STOP should schedule the confirmation poll immediately";
}

TEST(OnewayRx, OptimisticState_TypeMismatchSkipsOptimisticApplyButStillPolls) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  // BROADCAST_ROLLER targets ROLLER_SHUTTER; link a device of a different known type.
  comp.add_device("112233", {DeviceType::AWNING, 0, false});
  comp.add_linked_remote(node_id_to_string(REMOTE_ID), "112233");

  IoFrame frame = make_1w_execute(0xC8, 0x00);  // CLOSE
  RadioRxPacket pkt = make_rx_packet(frame);
  comp.process_received_packet_(pkt);

  const auto *dev = comp.get_device("112233");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->target, UNKNOWN_POSITION)
      << "a typed broadcast for a different device type must not optimistically move a linked device";
  EXPECT_EQ(comp.last_timeout_ms_, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS) << "the device should still be polled";
}

TEST(OnewayRx, OptimisticState_UnlinkedRemoteAppliesNoState) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.add_device("112233", {DeviceType::ROLLER_SHUTTER, 0, false});
  // Deliberately no add_linked_remote(): REMOTE_ID is not linked to "112233".

  IoFrame frame = make_1w_execute(0xC8, 0x00);  // CLOSE
  RadioRxPacket pkt = make_rx_packet(frame);
  comp.process_received_packet_(pkt);

  const auto *dev = comp.get_device("112233");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->target, UNKNOWN_POSITION) << "an unlinked remote's traffic must not apply optimistic state";
}

TEST(OnewayRx, OptimisticState_FavoriteIntentAppliesNoTargetButStillPolls) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.add_device("112233", {DeviceType::ROLLER_SHUTTER, 0, false});
  comp.add_linked_remote(node_id_to_string(REMOTE_ID), "112233");

  IoFrame frame = make_1w_execute(POS_FAVORITE, 0x00);  // FAVORITE: no resolvable numeric target
  RadioRxPacket pkt = make_rx_packet(frame);
  comp.process_received_packet_(pkt);

  const auto *dev = comp.get_device("112233");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->target, UNKNOWN_POSITION) << "FAVORITE has no known numeric target, so no optimistic claim is made";
  EXPECT_EQ(comp.last_timeout_ms_, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS) << "the device should still be polled";
}

TEST(OnewayRx, OptimisticState_RespectsOptimisticStateFalse) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.add_device("112233", {DeviceType::ROLLER_SHUTTER, 0, false, /*optimistic_state=*/false});
  comp.add_linked_remote(node_id_to_string(REMOTE_ID), "112233");

  IoFrame frame = make_1w_execute(0xC8, 0x00);  // CLOSE
  RadioRxPacket pkt = make_rx_packet(frame);
  comp.process_received_packet_(pkt);

  const auto *dev = comp.get_device("112233");
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->target, UNKNOWN_POSITION) << "optimistic_state=false must leave target untouched (poll-only)";
  EXPECT_EQ(comp.last_timeout_ms_, REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS) << "the device should still be polled";
}

// ============================================================================
// Class-target linked remotes
// ============================================================================

TEST(OnewayRx, OptimisticState_ClassLinkedDeviceGetsOptimisticStateWithoutIdLink) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.add_device("112233", {DeviceType::ROLLER_SHUTTER, 0, false});
  comp.add_linked_remote_class(DeviceType::ROLLER_SHUTTER, "112233");
  // Deliberately no add_linked_remote(): "112233" is only linked by class, not by REMOTE_ID.

  IoFrame frame = make_1w_execute(0xC8, 0x00);  // CLOSE, broadcast to typed ROLLER_SHUTTER
  RadioRxPacket pkt = make_rx_packet(frame);
  comp.process_received_packet_(pkt);

  const auto *dev = comp.get_device("112233");
  ASSERT_NE(dev, nullptr);
  EXPECT_FLOAT_EQ(dev->target, 100.0f) << "a class-linked device should get optimistic state from a typed broadcast";
}

TEST(OnewayRx, ResolveTargetDevices_DedupsDeviceLinkedByBothIdAndClass) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.add_device("112233", {DeviceType::ROLLER_SHUTTER, 0, false});
  comp.add_linked_remote(node_id_to_string(REMOTE_ID), "112233");
  comp.add_linked_remote_class(DeviceType::ROLLER_SHUTTER, "112233");

  IoFrame frame = make_1w_execute(0xC8, 0x00);
  const OneWayFrameInfo info = decode_1w_frame(frame);

  const auto devices = comp.resolve_1w_target_devices_(info, node_id_to_string(REMOTE_ID));
  ASSERT_EQ(devices.size(), 1u) << "a device linked both by id and by class must be resolved exactly once";
  EXPECT_EQ(devices[0], "112233");
}

TEST(OnewayRx, ResolveTargetDevices_UnionsIdLinkedAndClassLinkedDevices) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.add_device("112233", {DeviceType::ROLLER_SHUTTER, 0, false});
  comp.add_device("445566", {DeviceType::ROLLER_SHUTTER, 0, false});
  comp.add_linked_remote(node_id_to_string(REMOTE_ID), "112233");
  comp.add_linked_remote_class(DeviceType::ROLLER_SHUTTER, "445566");

  IoFrame frame = make_1w_execute(0xC8, 0x00);
  const OneWayFrameInfo info = decode_1w_frame(frame);

  const auto devices = comp.resolve_1w_target_devices_(info, node_id_to_string(REMOTE_ID));
  ASSERT_EQ(devices.size(), 2u);
  EXPECT_NE(std::find(devices.begin(), devices.end(), "112233"), devices.end());
  EXPECT_NE(std::find(devices.begin(), devices.end(), "445566"), devices.end());
}

TEST(OnewayRx, ResolveTargetDevices_UnicastFrameDoesNotFanOutToClassLinks) {
  RxTestableComponent comp;
  MockRadio radio;
  setup_component(comp, radio);
  comp.add_device("445566", {DeviceType::ROLLER_SHUTTER, 0, false});
  comp.add_linked_remote_class(DeviceType::ROLLER_SHUTTER, "445566");

  // A unicast (non-broadcast) 1W frame must not resolve any class-linked devices, even if the
  // sender happens to also address a ROLLER_SHUTTER-typed device elsewhere.
  IoFrame frame{};
  init_frame(frame, /*is_2w=*/false, /*start=*/true, /*end=*/true, /*low_power=*/false);
  const uint8_t unicast_dst[NODE_ID_SIZE] = {0x11, 0x22, 0x33};
  set_dst(frame, unicast_dst);
  set_src(frame, REMOTE_ID);
  const uint8_t payload[4] = {ORIGINATOR_USER_REMOTE, 0x41, 0xC8, 0x00};
  set_cmd(frame, CMD_EXECUTE, payload, sizeof(payload));
  const OneWayFrameInfo info = decode_1w_frame(frame);

  const auto devices = comp.resolve_1w_target_devices_(info, node_id_to_string(REMOTE_ID));
  EXPECT_TRUE(devices.empty()) << "a unicast 1W frame must not fan out to class-linked devices";
}
