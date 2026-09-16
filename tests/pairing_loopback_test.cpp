/// @file pairing_loopback_test.cpp
/// @brief One hub's PairingEngine (controller role) pairing to a second hub's key-extraction
/// responder (device role), frame by frame, with no scripted replies in between.
///
/// Every other pairing test scripts one side: pairing_helpers_test.cpp queues hand-built device
/// replies for PairingEngine, and hub_key_extraction_test.cpp feeds hand-built hub frames to the
/// responder. Both sides implement the same 0x28 → 0x29 → 0x2C → 0x2D → 0x31 → 0x3C → 0x32 → 0x33
/// sequence, so this suite connects the two real implementations and checks they agree: the hub
/// finishes pairing, and the responder recovers exactly the hub's system key.
///
/// What this cannot prove: both sides are this codebase, so an assumption they share (a wrong
/// framing bit, a wrong IV convention) passes here and still fails against real hardware. The
/// corpus suites (corpus_device_role_builder_test.cpp, corpus_pairing_replay_test.cpp) stay the
/// authority on wire correctness; this suite only catches the two roles drifting apart.
///
/// The simulated air: a frame the hub transmits is handed synchronously to the device hub's normal
/// RX dispatch (process_received_packet_()), and every frame the device transmits is parked with
/// its channel. The hub only hears a parked frame while its receiver is tuned to that channel, and
/// a new hub transmit discards everything still parked — the hub cannot hear frames sent before its
/// own TX. Link faults (a frame the other side never hears) are injected per command byte.

#include "hub_core.h"
#include "pairing_engine.h"
#include "pairing_responder.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace esphome::home_io_control;
using test::TestableHubComponent;

namespace {

constexpr uint8_t HUB_NODE_ID[NODE_ID_SIZE] = {0xC0, 0xFF, 0xEE};
constexpr uint8_t DEVICE_HUB_NODE_ID[NODE_ID_SIZE] = {0x12, 0x34, 0x56};
constexpr uint8_t HUB_SYSTEM_KEY[AES_KEY_SIZE] = {0x3A, 0x91, 0x0C, 0x5E, 0x77, 0xB2, 0x48, 0xD6,
                                                  0x1F, 0xE4, 0x60, 0x2B, 0x95, 0xC8, 0x03, 0x7D};
constexpr uint8_t DEVICE_HUB_SYSTEM_KEY[AES_KEY_SIZE] = {0xD1, 0x74, 0x34, 0x93, 0xFA, 0x94, 0x38, 0x45,
                                                         0xAC, 0x43, 0x50, 0xEE, 0xFF, 0x34, 0x29, 0x34};

/// Command byte of a serialized frame, or 0 if it is too short to carry one.
uint8_t wire_cmd(const uint8_t *data, uint8_t len) { return len > FRAME_CMD_OFFSET ? data[FRAME_CMD_OFFSET] : 0; }

/// Drop budget per command byte: each matching frame copy consumes one and is never delivered.
using DropBudget = std::map<uint8_t, int>;

bool consume_drop(DropBudget &budget, uint8_t cmd) {
  auto it = budget.find(cmd);
  if (it == budget.end() || it->second <= 0)
    return false;
  it->second--;
  return true;
}

/// The pairing hub's radio: forwards every transmit to the device hub and hears only what the
/// device parked on the channel the receiver is currently tuned to. Templated over the mock so the
/// same air serves both the fast-turnaround (SX1276-like) and slow-turnaround (SX1262-like) key
/// exchange paths.
template<typename Base> class HubAirRadio : public Base {
 public:
  TestableHubComponent *device{nullptr};
  DropBudget drop_to_device;  ///< Hub frames the device never hears.
  DropBudget drop_to_hub;     ///< Device frame copies the hub never hears (each channel copy counts).

  bool send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) override {
    const bool ok = Base::send_packet(data, len, tx_config);
    this->air_.clear();
    if (!ok || this->device == nullptr || consume_drop(this->drop_to_device, wire_cmd(data, len)))
      return ok;
    RadioRxPacket packet{};
    std::memcpy(packet.data, data, len);
    packet.len = len;
    packet.freq_hz = tx_config.freq_hz;
    this->device->process_received_packet_(packet);
    return ok;
  }

  bool wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) override {
    // Keeps the base mock's wait/timeout bookkeeping; its own rx queue is always empty here.
    Base::wait_for_packet(packet, timeout_ms);
    for (auto it = this->air_.begin(); it != this->air_.end(); ++it) {
      if (it->freq_hz != this->get_current_freq())
        continue;
      packet = *it;
      this->air_.erase(it);
      return true;
    }
    return false;
  }

  void park_from_device(const uint8_t *data, uint8_t len, uint32_t freq_hz) {
    if (consume_drop(this->drop_to_hub, wire_cmd(data, len)))
      return;
    RadioRxPacket packet{};
    std::memcpy(packet.data, data, len);
    packet.len = len;
    packet.freq_hz = freq_hz;
    this->air_.push_back(packet);
  }

 private:
  std::deque<RadioRxPacket> air_;
};

/// The device hub's radio: records like the plain mock and parks every transmit on the air.
class DeviceAirRadio : public MockRadio {
 public:
  std::function<void(const uint8_t *, uint8_t, uint32_t)> to_air;

  bool send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) override {
    const bool ok = MockRadio::send_packet(data, len, tx_config);
    if (ok && this->to_air)
      this->to_air(data, len, tx_config.freq_hz);
    return ok;
  }
};

/// Every transmitted frame's command byte, in order.
std::vector<uint8_t> sent_cmds(const MockRadio &radio) {
  std::vector<uint8_t> cmds;
  for (const auto &frame : radio.get_sent_data())
    cmds.push_back(wire_cmd(frame.data(), static_cast<uint8_t>(frame.size())));
  return cmds;
}

int count_cmd(const std::vector<uint8_t> &cmds, uint8_t cmd) {
  int count = 0;
  for (uint8_t c : cmds)
    count += c == cmd ? 1 : 0;
  return count;
}

enum class HubChip { FAST_TURNAROUND, SLOW_TURNAROUND };

std::string chip_name(const ::testing::TestParamInfo<HubChip> &info) {
  return info.param == HubChip::FAST_TURNAROUND ? "FastTurnaroundSx1276" : "SlowTurnaroundSx1262";
}

/// Both hubs, wired together. The pairing hub's radio type is chosen per test parameter.
class PairingLoopback : public ::testing::TestWithParam<HubChip> {
 protected:
  void SetUp() override {
    if (GetParam() == HubChip::FAST_TURNAROUND) {
      hub_radio_ = &fast_radio_;
      park_ = [this](const uint8_t *d, uint8_t l, uint32_t f) { fast_radio_.park_from_device(d, l, f); };
    } else {
      hub_radio_ = &slow_radio_;
      park_ = [this](const uint8_t *d, uint8_t l, uint32_t f) { slow_radio_.park_from_device(d, l, f); };
    }
    fast_radio_.device = &device_;
    slow_radio_.device = &device_;

    std::memcpy(hub_.node_id_, HUB_NODE_ID, NODE_ID_SIZE);
    std::memcpy(hub_.system_key_, HUB_SYSTEM_KEY, AES_KEY_SIZE);
    hub_.initialized_ = true;
    hub_.radio_ = hub_radio_;

    std::memcpy(device_.node_id_, DEVICE_HUB_NODE_ID, NODE_ID_SIZE);
    std::memcpy(device_.system_key_, DEVICE_HUB_SYSTEM_KEY, AES_KEY_SIZE);
    device_.initialized_ = true;
    device_.radio_ = &device_radio_;
    device_radio_.to_air = [this](const uint8_t *d, uint8_t l, uint32_t f) { park_(d, l, f); };

    device_.set_key_extraction_armed(true);
  }

  DropBudget &drop_to_device() {
    return GetParam() == HubChip::FAST_TURNAROUND ? fast_radio_.drop_to_device : slow_radio_.drop_to_device;
  }
  DropBudget &drop_to_hub() {
    return GetParam() == HubChip::FAST_TURNAROUND ? fast_radio_.drop_to_hub : slow_radio_.drop_to_hub;
  }

  const pairing_responder::ResponderContext &responder() const { return device_.key_extraction_.key_extraction_ctx_; }

  /// Assert the device side finished: key recovered and equal to the pairing hub's system key.
  void expect_hub_key_extracted() const {
    EXPECT_EQ(responder().state, pairing_responder::ResponderState::EXTRACTED);
    EXPECT_EQ(std::memcmp(responder().recovered_key, HUB_SYSTEM_KEY, AES_KEY_SIZE), 0)
        << "the responder must recover exactly the key the pairing hub transferred";
    EXPECT_EQ(std::memcmp(responder().hub_node_id, HUB_NODE_ID, NODE_ID_SIZE), 0);
  }

  /// Assert the pairing hub registered the responder's throwaway identity.
  void expect_device_registered() {
    const IoDevice *registered = hub_.get_device(node_id_to_string(responder().throwaway_id));
    ASSERT_NE(registered, nullptr) << "the pairing hub should register the device it paired";
    EXPECT_EQ(registered->type, responder().advertised_type);
  }

  /// The pairing hub's first transmitted 0x2C, parsed; fails the test if none was sent.
  IoFrame first_discover_confirm_sent() const {
    for (const auto &raw : hub_radio_->get_sent_data()) {
      if (wire_cmd(raw.data(), static_cast<uint8_t>(raw.size())) != CMD_DISCOVER_CONFIRM)
        continue;
      IoFrame frame{};
      EXPECT_TRUE(parse(raw.data(), static_cast<uint8_t>(raw.size()), frame));
      return frame;
    }
    ADD_FAILURE() << "the pairing hub never sent a 0x2C";
    return IoFrame{};
  }

  TestableHubComponent hub_;
  TestableHubComponent device_;
  HubAirRadio<MockRadio> fast_radio_;
  HubAirRadio<MockRadioSX1262> slow_radio_;
  MockRadio *hub_radio_{nullptr};
  DeviceAirRadio device_radio_;
  std::function<void(const uint8_t *, uint8_t, uint32_t)> park_;
};

// ============================================================================
// Clean link
// ============================================================================

/// Default `send`: the full sequence, with the responder acknowledging the discover-confirm.
TEST_P(PairingLoopback, DefaultModeCompletesFullSequenceAndResponderRecoversHubKey) {
  ASSERT_TRUE(hub_.discover_and_pair()) << "pairing to the responder should succeed";

  const std::vector<uint8_t> hub_cmds = sent_cmds(*hub_radio_);
  ASSERT_GE(hub_cmds.size(), 4u);
  EXPECT_EQ(std::vector<uint8_t>(hub_cmds.begin(), hub_cmds.begin() + 4),
            (std::vector<uint8_t>{CMD_DISCOVER_REQ, CMD_DISCOVER_CONFIRM, CMD_KEY_INIT, CMD_KEY_TRANSFER}))
      << "0x28, one 0x2C (answered on the first try), 0x31, 0x32";

  const std::vector<uint8_t> device_cmds = sent_cmds(device_radio_);
  EXPECT_EQ(count_cmd(device_cmds, CMD_DISCOVER_CONFIRM_ACK), 3) << "one 0x2D, on all three channels";
  EXPECT_EQ(count_cmd(device_cmds, CMD_KEY_CONFIRM), 3) << "one 0x33, on all three channels";

  expect_hub_key_extracted();
  expect_device_registered();
}

/// The responder advertises a low-power device, so the hub's 0x2C must take the low-power shape:
/// CTRL1_LOW_POWER set, and the long wake-up preamble.
TEST_P(PairingLoopback, DiscoverConfirmFollowsTheResponderAdvertisedPowerClass) {
  ASSERT_TRUE(hub_.discover_and_pair());

  const IoFrame confirm = first_discover_confirm_sent();
  EXPECT_EQ(confirm.ctrl1, CTRL1_LOW_POWER);
  EXPECT_EQ(std::memcmp(confirm.dst, responder().throwaway_id, NODE_ID_SIZE), 0)
      << "0x2C goes to the identity the responder advertised in its 0x29";

  for (size_t i = 0; i < hub_radio_->get_sent_data().size(); i++) {
    const auto &raw = hub_radio_->get_sent_data()[i];
    if (wire_cmd(raw.data(), static_cast<uint8_t>(raw.size())) == CMD_DISCOVER_CONFIRM) {
      EXPECT_EQ(hub_radio_->get_tx_configs()[i].preamble_len, LONG_PREAMBLE);
    }
  }
}

/// `send_with_ack` only changes the frame for an always-alive target; to this low-power responder
/// it sends the same 0x2C as `send`, and the pairing completes the same way.
TEST_P(PairingLoopback, SendWithAckSendsTheSameFrameToALowPowerTarget) {
  hub_.tuning_.pairing_discover_confirm = DiscoverConfirmMode::SEND_WITH_ACK;

  ASSERT_TRUE(hub_.discover_and_pair());

  EXPECT_EQ(first_discover_confirm_sent().ctrl1, CTRL1_LOW_POWER) << "no CTRL1_ACK for a low-power target";
  expect_hub_key_extracted();
}

/// `skip`: the hub goes from discovery straight to key-init, and the responder accepts that too.
TEST_P(PairingLoopback, SkipModeGoesStraightToKeyInitAndResponderStillExtracts) {
  hub_.tuning_.pairing_discover_confirm = DiscoverConfirmMode::SKIP;

  ASSERT_TRUE(hub_.discover_and_pair());

  const std::vector<uint8_t> hub_cmds = sent_cmds(*hub_radio_);
  EXPECT_EQ(count_cmd(hub_cmds, CMD_DISCOVER_CONFIRM), 0);
  EXPECT_EQ(count_cmd(sent_cmds(device_radio_), CMD_DISCOVER_CONFIRM_ACK), 0);
  expect_hub_key_extracted();
  expect_device_registered();
}

// ============================================================================
// Link faults
// ============================================================================

/// The device misses the first 0x2C. Try 2 (the rotating listen) hears the 0x2D.
TEST_P(PairingLoopback, DeviceMissesFirstDiscoverConfirmSecondTryIsAnswered) {
  drop_to_device()[CMD_DISCOVER_CONFIRM] = 1;

  ASSERT_TRUE(hub_.discover_and_pair());

  EXPECT_EQ(count_cmd(sent_cmds(*hub_radio_), CMD_DISCOVER_CONFIRM), 2);
  EXPECT_EQ(count_cmd(sent_cmds(device_radio_), CMD_DISCOVER_CONFIRM_ACK), 3) << "answered exactly once";
  expect_hub_key_extracted();
}

/// The hub misses every copy of the first 0x2D. It retries the 0x2C, and the responder, already
/// past that step, acknowledges again rather than ignoring the repeat.
TEST_P(PairingLoopback, HubMissesTheAckAndResponderAcknowledgesTheRetry) {
  drop_to_hub()[CMD_DISCOVER_CONFIRM_ACK] = 3;

  ASSERT_TRUE(hub_.discover_and_pair());

  EXPECT_EQ(count_cmd(sent_cmds(*hub_radio_), CMD_DISCOVER_CONFIRM), 2);
  EXPECT_EQ(count_cmd(sent_cmds(device_radio_), CMD_DISCOVER_CONFIRM_ACK), 6) << "one 0x2D per 0x2C";
  expect_hub_key_extracted();
}

/// The device never hears any 0x2C. The hub spends all three tries, then continues to the key
/// exchange anyway, and the responder accepts a key-init that arrives without a confirm.
TEST_P(PairingLoopback, DeviceNeverHearsDiscoverConfirmAndPairingStillCompletes) {
  drop_to_device()[CMD_DISCOVER_CONFIRM] = PAIRING_DISCOVER_CONFIRM_TRIES;

  ASSERT_TRUE(hub_.discover_and_pair()) << "the confirm step must never fail the pairing";

  const std::vector<uint8_t> hub_cmds = sent_cmds(*hub_radio_);
  EXPECT_EQ(count_cmd(hub_cmds, CMD_DISCOVER_CONFIRM), PAIRING_DISCOVER_CONFIRM_TRIES);
  EXPECT_EQ(count_cmd(hub_cmds, CMD_KEY_INIT), 1);
  EXPECT_EQ(count_cmd(sent_cmds(device_radio_), CMD_DISCOVER_CONFIRM_ACK), 0);
  expect_hub_key_extracted();
  expect_device_registered();
}

/// The hub misses every copy of the device's 0x33. Extraction has already happened on the device
/// side, and the responder deliberately answers nothing once it holds a key (every responder guard
/// rejects EXTRACTED), so the hub's retries go unanswered and its pairing attempt fails. The
/// feature's goal, the recovered key, is unaffected.
TEST_P(PairingLoopback, HubMissesKeyConfirmResponderKeepsTheKeyHubPairingFails) {
  drop_to_hub()[CMD_KEY_CONFIRM] = 3;

  EXPECT_FALSE(hub_.discover_and_pair()) << "a responder holding the key does not re-confirm a retried 0x32 or 0x31";

  expect_hub_key_extracted();
  EXPECT_EQ(count_cmd(sent_cmds(device_radio_), CMD_KEY_CONFIRM), 3) << "the responder confirmed exactly once";
}

INSTANTIATE_TEST_SUITE_P(PairingLoopback, PairingLoopback,
                         ::testing::Values(HubChip::FAST_TURNAROUND, HubChip::SLOW_TURNAROUND), chip_name);

}  // namespace
