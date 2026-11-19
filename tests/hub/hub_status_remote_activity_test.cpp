/// @file hub_status_remote_activity_test.cpp
/// @brief Tests for the traffic of other controllers in hub_status.cpp: the poll a remote's activity triggers, and the
/// frames overheard while the hub polls.

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
#include <map>

using namespace esphome::home_io_control;
using test::make_rx_packet;
using test::RxTestableComponent;
using test::setup_rx_test_component;

// ============================================================================
// Remote activity detection tests (Issue #3)
// ============================================================================

TEST(HubStatus, RemoteActivity_TriggersDelayedPoll) {
  esphome::test_clock::ManualClock clock;
  const uint32_t t0 = esphome::test_clock::peek_ms();
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
  EXPECT_EQ(comp.last_timeout_id_, decisions::remote_poll_timer_id(dst)) << "timeout id should key the device";

  // Drive the scheduler forward to the poll's deadline instead of hand-firing the callback — the
  // timer must actually be pending in test_scheduler's registry, not just recorded on the compat field.
  EXPECT_EQ(esphome::test_scheduler::run_until(t0 + REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS), 1u)
      << "the poll timer should fire once it comes due";
  ASSERT_EQ(comp.op_queue_.size(), 1u) << "the fired timer should queue one operation";
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
  uint8_t payload[8] = {test::STATUS_BYTE_MOVING, 0x00, 0xC8, 0x00, 0x32, 0x00, 0x00, 0x05};
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
  esphome::test_clock::ManualClock clock;
  const uint32_t t0 = esphome::test_clock::peek_ms();
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
  const uint8_t linked_device[3] = {0x05, 0x4E, 0x17};
  EXPECT_EQ(comp.last_timeout_id_, decisions::remote_poll_timer_id(linked_device))
      << "timeout id should key the linked device";

  EXPECT_EQ(esphome::test_scheduler::run_until(t0 + REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS), 1u)
      << "the poll timer should fire once it comes due";
  ASSERT_EQ(comp.op_queue_.size(), 1u) << "the fired timer should queue one operation";
  EXPECT_EQ(comp.op_queue_.front().type, PendingOperationType::REQUEST_STATUS);
  EXPECT_EQ(comp.op_queue_.front().device_id, "054E17");
}

// Regression coverage for the WP1 registry itself, not reachable with the old "most-recent-call"
// stub: two devices' remote-activity polls pending at once, where re-triggering one device only
// pushes *its own* deadline back out and the other still fires on its original schedule.
TEST(HubStatus, TwoDevicesRemotePollsArePendingIndependently) {
  esphome::test_clock::ManualClock clock;
  const uint32_t t0 = esphome::test_clock::peek_ms();
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  comp.add_device("415684");  // second device, distinct from setup_rx_test_component()'s 054E17

  IoFrame trigger_a{};
  init_frame(trigger_a, true, true, false, false);
  uint8_t remote[3] = {0x43, 0x44, 0xE3};
  uint8_t device_a[3] = {0x05, 0x4E, 0x17};
  set_src(trigger_a, remote);
  set_dst(trigger_a, device_a);
  uint8_t exec_data[3] = {0xC8, 0x00, 0x00};
  set_cmd(trigger_a, CMD_EXECUTE, exec_data, sizeof(exec_data));
  comp.process_received_packet_(make_rx_packet(trigger_a));

  IoFrame trigger_b{};
  init_frame(trigger_b, true, true, false, false);
  uint8_t device_b[3] = {0x41, 0x56, 0x84};
  set_src(trigger_b, remote);
  set_dst(trigger_b, device_b);
  set_cmd(trigger_b, CMD_EXECUTE, exec_data, sizeof(exec_data));
  comp.process_received_packet_(make_rx_packet(trigger_b));

  ASSERT_EQ(esphome::test_scheduler::pending_count(), 2u) << "both devices' polls must be pending at once";

  // Re-trigger A only, partway through both windows — this must replace only A's pending item.
  esphome::test_clock::advance_ms(REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS / 2);
  comp.process_received_packet_(make_rx_packet(trigger_a));
  ASSERT_EQ(esphome::test_scheduler::pending_count(), 2u) << "re-triggering must replace, not add, A's item";

  // B's original deadline comes first: it must fire on time even though A was re-triggered.
  EXPECT_EQ(esphome::test_scheduler::run_until(t0 + REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS), 1u)
      << "only B's original poll should be due yet";
  ASSERT_EQ(comp.op_queue_.size(), 1u);
  EXPECT_EQ(comp.op_queue_.front().device_id, "415684") << "B must fire on its original schedule, unaffected by A";

  // A's pushed-out deadline is REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS after its re-trigger, i.e.
  // 1.5x the original delay from t=0.
  EXPECT_EQ(esphome::test_scheduler::run_until(t0 + REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS * 3 / 2), 1u)
      << "A's re-armed poll should now be due";
  ASSERT_EQ(comp.op_queue_.size(), 2u);
  EXPECT_EQ(comp.op_queue_.back().device_id, "054E17") << "A fires later, from when it was re-triggered";
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
  const uint8_t device[3] = {0x05, 0x4E, 0x17};
  EXPECT_EQ(comp.last_timeout_id_, decisions::remote_poll_timer_id(device)) << "timeout id should key the device";
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
// Overheard traffic from other controllers: when the hub polls (hub_status.cpp)
// ============================================================================

namespace {

constexpr uint32_t FOREIGN_T0 = 50000;
constexpr uint8_t FOREIGN_REMOTE[3] = {0x43, 0x44, 0xE3};  ///< Another controller.
constexpr uint8_t FOREIGN_DEVICE[3] = {0x05, 0x4E, 0x17};  ///< The registered device "054E17".

/// A hub with one registered device on a manual clock, fed frames of another controller.
struct ForeignTrafficRig {
  esphome::test_clock::ManualClock clock{FOREIGN_T0};
  RxTestableComponent comp;
  MockRadio radio;

  ForeignTrafficRig() { setup_rx_test_component(comp, radio); }

  /// Feed one frame `from` -> `to` carrying `cmd` (a 2W start frame).
  void hear(const uint8_t *from, const uint8_t *to, uint8_t cmd) {
    IoFrame f{};
    init_frame(f, true, true, false, false);
    set_src(f, from);
    set_dst(f, to);
    set_cmd(f, cmd);
    comp.process_received_packet_(make_rx_packet(f));
  }
  void controller_sends(uint8_t cmd) { hear(FOREIGN_REMOTE, FOREIGN_DEVICE, cmd); }
  void device_challenges(const uint8_t *controller) { hear(FOREIGN_DEVICE, controller, CMD_CHALLENGE_REQ); }

  /// Move time to `abs_ms`, firing due timers, and return how many status polls were queued since
  /// the last call.
  size_t polls_until(uint32_t abs_ms) {
    esphome::test_scheduler::run_until(abs_ms);
    size_t n = 0;
    while (auto op = comp.op_queue_.pop()) {
      if (op->type == PendingOperationType::REQUEST_STATUS)
        n++;
    }
    return n;
  }
};

}  // namespace

TEST(HubStatusForeign, AReadPollsOnceThenNotForTenMinutes) {
  ForeignTrafficRig r;
  r.controller_sends(CMD_PRIVATE);
  EXPECT_EQ(r.polls_until(FOREIGN_T0 + 2000), 1u) << "the first read after boot polls";
  r.controller_sends(CMD_PRIVATE);
  EXPECT_EQ(r.polls_until(FOREIGN_T0 + 60000), 0u);
  esphome::test_clock::set_ms(FOREIGN_T0 + FOREIGN_READ_RESYNC_MS - 1);
  r.controller_sends(CMD_PRIVATE);
  EXPECT_EQ(r.polls_until(FOREIGN_T0 + FOREIGN_READ_RESYNC_MS + 5000), 0u) << "still inside the 10 minutes";
  r.controller_sends(CMD_PRIVATE);
  EXPECT_EQ(r.polls_until(FOREIGN_T0 + FOREIGN_READ_RESYNC_MS + 10000), 1u) << "10 minutes after the first poll";
}

TEST(HubStatusForeign, AnExecutePollsTwoSecondsAfterTheLastFrame) {
  ForeignTrafficRig r;
  r.controller_sends(CMD_EXECUTE);
  EXPECT_EQ(r.polls_until(FOREIGN_T0 + 1000), 0u);
  r.controller_sends(CMD_EXECUTE);
  EXPECT_EQ(r.polls_until(FOREIGN_T0 + 2999), 0u) << "the second frame moved the poll out";
  EXPECT_EQ(r.polls_until(FOREIGN_T0 + 3000), 1u);
  // A write always polls, even right after a poll and with a fresh status.
  r.comp.get_device("054E17")->last_status = esphome::millis();
  r.controller_sends(CMD_EXECUTE);
  EXPECT_EQ(r.polls_until(esphome::millis() + 2000), 1u);
}

TEST(HubStatusForeign, AnExecuteThenTwoReadsPollsAfterTheLastReadAndRestartsTrackingOnce) {
  ForeignTrafficRig r;
  r.controller_sends(CMD_EXECUTE);
  const uint32_t deadline = r.comp.poll_policy_.get_poll_deadline("054E17");
  ASSERT_NE(deadline, 0u);
  esphome::test_scheduler::run_until(FOREIGN_T0 + 500);
  r.controller_sends(CMD_PRIVATE);
  esphome::test_scheduler::run_until(FOREIGN_T0 + 1000);
  r.controller_sends(CMD_PRIVATE);
  EXPECT_EQ(r.comp.poll_policy_.get_poll_deadline("054E17"), deadline) << "the reads did not restart tracking";
  EXPECT_EQ(r.polls_until(FOREIGN_T0 + 2999), 0u);
  EXPECT_EQ(r.polls_until(FOREIGN_T0 + 3000), 1u) << "2 s after the last read, one poll";
}

TEST(HubStatusForeign, AMissedExecuteWhoseChallengeIsHeardPolls) {
  ForeignTrafficRig r;
  r.comp.get_device("054E17")->last_status = esphome::millis();  // a read alone would not poll now
  r.device_challenges(FOREIGN_REMOTE);
  EXPECT_EQ(r.polls_until(FOREIGN_T0 + 2000), 1u);
}

TEST(HubStatusForeign, AReadFollowedByItsChallengeDoesNotPoll) {
  ForeignTrafficRig r;
  r.comp.get_device("054E17")->last_status = esphome::millis();
  r.controller_sends(CMD_PRIVATE);
  esphome::test_clock::advance_ms(3);
  r.device_challenges(FOREIGN_REMOTE);
  EXPECT_EQ(r.polls_until(FOREIGN_T0 + 5000), 0u);
  EXPECT_EQ(r.comp.poll_policy_.get_poll_deadline("054E17"), 0u) << "no tracking was begun";
}

TEST(HubStatusForeign, AChallengeAddressedToThisHubChangesNothing) {
  ForeignTrafficRig r;
  const uint8_t own[3] = {0xC0, 0xFF, 0xEE};
  r.device_challenges(own);
  EXPECT_EQ(r.polls_until(FOREIGN_T0 + 5000), 0u);
  EXPECT_EQ(r.comp.poll_policy_.get_poll_deadline("054E17"), 0u);
}

TEST(HubStatusForeign, AnIgnoredReadLeavesTrackingAndFailureCountsAlone) {
  ForeignTrafficRig r;
  r.comp.poll_policy_.begin_tracking("054E17", 0, esphome::millis());
  ASSERT_GT(r.comp.poll_policy_.on_exchange_failed("054E17", false, esphome::millis()), 0u);
  r.comp.get_device("054E17")->last_status = esphome::millis();
  const uint32_t deadline = r.comp.poll_policy_.get_poll_deadline("054E17");
  const uint32_t next = r.comp.poll_policy_.get_next_update("054E17");
  esphome::test_clock::advance_ms(1000);

  r.controller_sends(CMD_PRIVATE);

  EXPECT_EQ(r.comp.poll_policy_.get_poll_deadline("054E17"), deadline);
  EXPECT_EQ(r.comp.poll_policy_.get_next_update("054E17"), next);
  EXPECT_EQ(r.comp.poll_policy_.get_status_poll_failures("054E17"), 1u);
  EXPECT_TRUE(r.comp.last_timeout_name_.empty() || r.comp.op_queue_.empty());
}

TEST(HubStatusForeign, ALinked2WControllerFollowsTheSameRule) {
  ForeignTrafficRig r;
  const uint8_t linked[3] = {0x9D, 0x60, 0x85};
  const uint8_t elsewhere[3] = {0xAA, 0xBB, 0xCC};  // an address this hub has not registered
  r.comp.add_linked_remote("9D6085", "054E17");
  r.hear(linked, elsewhere, CMD_PRIVATE);
  EXPECT_EQ(r.polls_until(FOREIGN_T0 + 2000), 1u) << "the first read polls";
  r.hear(linked, elsewhere, CMD_PRIVATE);
  EXPECT_EQ(r.polls_until(FOREIGN_T0 + 6000), 0u) << "a second read inside 10 minutes does not";
  r.hear(linked, elsewhere, CMD_EXECUTE);
  EXPECT_EQ(r.polls_until(esphome::millis() + 2000), 1u) << "a write does";
}

TEST(HubStatusForeign, KlfStyleRoundRobinPollsEachDeviceOncePerTenMinutes) {
  // The recorded gateway reads nine devices in turn for 24 minutes. Each device is polled by this
  // hub at most once per 10 minutes, not once per read.
  const auto *capture = corpus_test::capture_by_id("velux_klf200_statuspoll_roundrobin_868_sx1262");
  ASSERT_NE(capture, nullptr);
  esphome::test_clock::ManualClock clock{FOREIGN_T0};
  RxTestableComponent comp;
  MockRadio radio;
  setup_rx_test_component(comp, radio);
  const uint8_t hub_node[3] = {0x11, 0x22, 0x33};  // the capture's gateway is C0FFEE
  std::memcpy(comp.node_id_, hub_node, 3);

  struct Seen {
    uint32_t first_ms{0};
    uint32_t last_ms{0};
    size_t reads{0};
    size_t polls{0};
  };
  std::map<std::string, Seen> seen;
  size_t reads = 0;
  for (uint8_t i = 0; i < capture->frame_count; i++) {
    const corpus::CorpusFrame &cf = capture->frames[i];
    IoFrame frame = corpus_test::parse_capture_frame(cf);
    if (frame.cmd != CMD_PRIVATE)
      continue;
    const std::string device = node_id_to_string(frame.dst);
    if (comp.get_device(device) == nullptr)
      comp.add_device(device);
    const uint32_t at = FOREIGN_T0 + cf.t_ms;
    esphome::test_scheduler::run_until(at);
    while (auto op = comp.op_queue_.pop()) {
      if (op->type == PendingOperationType::REQUEST_STATUS)
        seen[op->device_id].polls++;
    }
    RadioRxPacket pkt{};
    pkt.len = corpus_test::wire_len(cf);
    std::memcpy(pkt.data, cf.bytes, pkt.len);
    pkt.freq_hz = cf.freq_hz;
    comp.process_received_packet_(pkt);
    Seen &s = seen[device];
    if (s.reads == 0)
      s.first_ms = cf.t_ms;
    s.last_ms = cf.t_ms;
    s.reads++;
    reads++;
  }
  esphome::test_scheduler::run_until(FOREIGN_T0 + 25 * 60 * 1000);
  while (auto op = comp.op_queue_.pop()) {
    if (op->type == PendingOperationType::REQUEST_STATUS)
      seen[op->device_id].polls++;
  }

  ASSERT_GE(seen.size(), 9u);
  size_t polls = 0;
  for (const auto &entry : seen) {
    const Seen &s = entry.second;
    EXPECT_GE(s.polls, 1u) << entry.first << " is polled after its first read";
    EXPECT_LE(s.polls, (s.last_ms - s.first_ms) / FOREIGN_READ_RESYNC_MS + 1u)
        << entry.first << ": at most one poll per 10 minutes";
    polls += s.polls;
  }
  EXPECT_LT(polls * 5, reads) << "far fewer polls than reads";
}
