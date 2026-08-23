/// @file corpus_device_role_builder_test.cpp
/// @brief Pins the device-role frame builders (proto_commands.h) against real devices' captured
/// frames in the golden-frame corpus.
///
/// The "Accept Foreign Pairing" key-extraction responder is the one place this project emits
/// *device*-originated frames instead of controller-originated ones (see proto_commands.h's
/// device-side builder group). Everything else it does — and therefore every other test — speaks
/// the controller side, so a device-role builder that frames its CTRL0/CTRL1 bits the
/// controller way is self-consistent and passes both host tests and a self-pair hardware test
/// where this same codebase sits on both ends. It is still wrong on air, and a real hub can
/// simply not answer it.
///
/// This suite closes that specific gap: for each device-role builder, it asserts our framing bits
/// against every genuine device frame of that command in the corpus — with one deliberate
/// exception. CMD_CHALLENGE_RESP (0x3D)'s framing is context-dependent rather than role-constant
/// (a device's 0x3D sets END when it closes an exchange, and clears it answering a mid-exchange
/// controller challenge), so the all-captures assertion cannot express it for that one command;
/// expect_matches_real_device_capture() below scopes that builder's pin to the one capture that
/// shows the terminal shape it actually builds, instead of weakening the general assertion.
///
/// Only frames a genuine third-party device actually transmitted count as evidence here. Two
/// kinds of corpus `rx` frame do not, and are excluded (see `INDEPENDENT_SOURCE_EXCLUSIONS` and
/// `is_reconstructed_frame()` below for the per-capture reasons):
///
/// 1. **Captures where this project's own firmware played the device.** The self-pair and loopback
///    rigs, and the issue #45 field captures whose hub is real but whose device side is our own
///    responder. Including them would let our own output vote on its own correctness, which is
///    precisely the blind spot this suite exists to cover — they are in fact the only captures
///    holding 0x3C/0x33 frames that disagree with the real-device consensus asserted here. They
///    stay valuable as end-to-end crypto/state-machine fixtures in the other suites.
/// 2. **Frames documented as reconstructed rather than captured.** `field_rs100_*`'s 0x33 has its
///    flag bits guessed by analogy (its own `note:` says so); `corpus_pairing_replay_test.cpp`
///    excludes the same capture for the same reason.
///
/// Note that "our own firmware" is the test, not `source.origin` — plenty of `own-hardware`
/// captures are of Lars's real Somfy/Velux devices and are exactly the evidence wanted here.

#include "corpus_generated.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_device_model.h"
#include "proto_frame.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using namespace esphome::home_io_control;

namespace {

/// The framing bits a builder decides, isolated from length/address/payload content.
struct FramingBits {
  bool start{false};
  bool end{false};
  bool low_power{false};

  bool operator==(const FramingBits &other) const {
    return start == other.start && end == other.end && low_power == other.low_power;
  }
};

FramingBits framing_of(const IoFrame &f) {
  return FramingBits{is_start(f), is_end(f), (f.ctrl1 & CTRL1_LOW_POWER) != 0};
}

std::string describe(const FramingBits &b) {
  return "START=" + std::to_string(static_cast<int>(b.start)) + " END=" + std::to_string(static_cast<int>(b.end)) +
         " LOW_POWER=" + std::to_string(static_cast<int>(b.low_power));
}

/// Captures whose `rx` (device-side) frames this project's own firmware transmitted, so they are
/// not independent evidence of what a real device sends. See the file header.
constexpr const char *INDEPENDENT_SOURCE_EXCLUSIONS[] = {
    // Both ends are this codebase: our responder as the device, our PairingEngine as the hub.
    "key_extraction_self_pair_sx1262_device_sx1276_hub",
    "key_extraction_self_pair_sx1276_device_sx1262_hub",
    // A Heltec V3 <-> V2 loopback rig — this project's firmware on both ends.
    "pairing_lab_loopback_challenge_reply",
    // Real third-party *hubs*, but the device side is our own key-extraction responder: every
    // `rx` frame in these is our output, which is exactly what they were captured to document.
    "issue_45_velux_kig300_key_extraction_stall",
    "issue_45_somfy_connectivity_kit_key_extraction_stall",
};

bool is_independent_device_source(std::string_view capture_id) {
  for (const std::string_view excluded : INDEPENDENT_SOURCE_EXCLUSIONS) {
    if (capture_id == excluded)
      return false;
  }
  return true;
}

/// True for individual frames a capture's own `note:` documents as reconstructed rather than
/// captured, so their flag bits are a guess and cannot pin a builder. Only one such frame exists.
bool is_reconstructed_frame(std::string_view capture_id, uint8_t cmd) {
  return capture_id == "field_rs100_pairing_key_exchange_retry_success" && cmd == CMD_KEY_CONFIRM;
}

/// A genuine device-originated frame of one command: corpus `rx` frames (device -> controller),
/// after the exclusions documented in the file header.
struct DeviceFrame {
  const char *capture_id;
  IoFrame frame;
};

std::vector<DeviceFrame> real_device_frames(uint8_t cmd) {
  std::vector<DeviceFrame> result;
  for (const corpus::CorpusCapture *cap : corpus_test::all_captures()) {
    if (!is_independent_device_source(cap->id))
      continue;
    for (uint8_t i = 0; i < cap->frame_count; i++) {
      const corpus::CorpusFrame &cf = cap->frames[i];
      if (cf.tx)
        continue;  // controller-originated
      const IoFrame parsed = corpus_test::parse_capture_frame(cf);
      if (parsed.cmd == cmd && !is_reconstructed_frame(cap->id, parsed.cmd))
        result.push_back(DeviceFrame{cap->id, parsed});
    }
  }
  return result;
}

/// Assert `built` frames the same way every real device frame of `cmd` in the corpus does.
void expect_matches_real_devices(uint8_t cmd, const IoFrame &built) {
  const std::vector<DeviceFrame> captured = real_device_frames(cmd);
  ASSERT_FALSE(captured.empty()) << "no real device-originated 0x" << std::hex << static_cast<int>(cmd)
                                 << " frame in the corpus to pin this builder against";

  const FramingBits ours = framing_of(built);
  for (const DeviceFrame &df : captured) {
    SCOPED_TRACE(::testing::Message() << "captured in " << df.capture_id);
    EXPECT_EQ(ours, framing_of(df.frame))
        << "device-role builder frames 0x" << std::hex << static_cast<int>(cmd) << " as [" << describe(ours)
        << "] but a real device sent [" << describe(framing_of(df.frame)) << "]";
  }
}

/// Assert `built` frames the same way one specific capture's device frames of `cmd` do. Needed
/// where a command's framing is context-dependent rather than role-constant: a device's 0x3D sets
/// END when it closes an exchange (the KLR200's answer to a 0x36/0x37 round) and clears it
/// mid-exchange (a Somfy actuator answering a controller's challenge on an open command exchange),
/// so expect_matches_real_devices() above cannot express it -- it would fail on every capture but
/// the one this test cares about, whatever the builder does.
void expect_matches_real_device_capture(uint8_t cmd, std::string_view capture_id, const IoFrame &built) {
  const corpus::CorpusCapture *cap = corpus_test::capture_by_id(std::string(capture_id).c_str());
  ASSERT_NE(cap, nullptr) << "capture '" << capture_id << "' not found -- was it renamed?";

  const FramingBits ours = framing_of(built);
  uint8_t checked = 0;
  for (uint8_t i = 0; i < cap->frame_count; i++) {
    const corpus::CorpusFrame &cf = cap->frames[i];
    if (cf.tx)
      continue;  // controller-originated
    const IoFrame parsed = corpus_test::parse_capture_frame(cf);
    if (parsed.cmd != cmd)
      continue;
    SCOPED_TRACE(::testing::Message() << "captured in " << capture_id);
    EXPECT_EQ(ours, framing_of(parsed)) << "device-role builder frames 0x" << std::hex << static_cast<int>(cmd)
                                        << " as [" << describe(ours) << "] but capture '" << capture_id << "' shows ["
                                        << describe(framing_of(parsed)) << "]";
    checked++;
  }
  ASSERT_GT(checked, 0u) << "no device-originated 0x" << std::hex << static_cast<int>(cmd) << " frame in capture '"
                         << capture_id << "' to pin this builder against";
}

}  // namespace

/// A hub sends CMD_DISCOVER_CONFIRM (0x2C) straight to a device it just discovered and generally
/// will not start the key exchange until the device answers. Our 0x2D must look like a device's.
TEST(CorpusDeviceRoleBuilders, DiscoverConfirmAckMatchesRealDevices) {
  IoFrame built{};
  ASSERT_TRUE(create_discover_confirm_ack(built, test::OWN_ID, test::DST_ID));
  expect_matches_real_devices(CMD_DISCOVER_CONFIRM_ACK, built);
}

/// The responder's answer to a hub's CMD_KEY_INIT. Real devices' pairing 0x3C sets neither START
/// nor LOW_POWER; the controller-role builder sets both, which is correct only in that direction.
TEST(CorpusDeviceRoleBuilders, ChallengeReqDeviceRoleMatchesRealDevices) {
  IoFrame built{};
  ASSERT_TRUE(create_challenge_req_device_role(built, test::DST_ID, test::OWN_ID, test::TEST_CHALLENGE));
  expect_matches_real_devices(CMD_CHALLENGE_REQ, built);
}

/// Closes the key exchange that CMD_KEY_INIT opened, so real devices set END on it.
TEST(CorpusDeviceRoleBuilders, KeyConfirmMatchesRealDevices) {
  IoFrame built{};
  ASSERT_TRUE(create_key_confirm(built, test::OWN_ID, test::DST_ID));
  expect_matches_real_devices(CMD_KEY_CONFIRM, built);
}

/// Discovery already worked against both real hubs in issue #45, so this is a guard rather than a
/// fix — it pins the one device-role builder that was already right.
TEST(CorpusDeviceRoleBuilders, DiscoverRespMatchesRealDevices) {
  IoFrame built{};
  ASSERT_TRUE(
      create_discover_resp(built, test::OWN_ID, test::DST_ID, DeviceType::ROLLER_SHUTTER, 0, MANUFACTURER_SOMFY));
  expect_matches_real_devices(CMD_DISCOVER_RESP, built);
}

/// Our answer to a hub's CMD_ADDRESS_REQ (0x36). The KLR200 pairing capture is the only corpus
/// evidence for this command in either direction, so this is a single-capture pin by necessity,
/// not a choice -- expect_matches_real_devices() still applies unmodified because there's exactly
/// one capture to sweep.
TEST(CorpusDeviceRoleBuilders, AddressRespDeviceRoleMatchesRealDevices) {
  IoFrame built{};
  ASSERT_TRUE(create_address_resp_device_role(built, test::OWN_ID, test::DST_ID));
  expect_matches_real_devices(CMD_ADDRESS_RESP, built);
}

/// The KLR200 capture shows our 0x37 payload as byte-identical to our own earlier 0x29's backbone
/// address (data[DISCOVERY_RESP_BACKBONE_OFFSET..+3), tests/corpus/captures/velux_kux100/
/// pairing_full.yaml lines 47 and 87) -- both report the same throwaway node ID we advertised.
/// create_discover_resp() and create_address_resp_device_role() are two independent call sites for
/// that same `own` value, so nothing stops a future edit from swapping an argument at either one
/// without any other test noticing: the framing-bits pin above covers CTRL0/CTRL1 only, not
/// payload content.
TEST(CorpusDeviceRoleBuilders, AddressRespPayloadMatchesOwnDiscoverRespBackboneAddress) {
  IoFrame discover_resp{};
  ASSERT_TRUE(create_discover_resp(discover_resp, test::OWN_ID, test::DST_ID, DeviceType::ROLLER_SHUTTER, 0,
                                   MANUFACTURER_SOMFY));
  IoFrame address_resp{};
  ASSERT_TRUE(create_address_resp_device_role(address_resp, test::OWN_ID, test::DST_ID));

  ASSERT_EQ(address_resp.data_len, NODE_ID_SIZE);
  EXPECT_EQ(0, memcmp(address_resp.data, discover_resp.data + DISCOVERY_RESP_BACKBONE_OFFSET, NODE_ID_SIZE))
      << "0x37 payload must equal our own 0x29's backbone address, the cross-check the KLR200 capture documents";
}

/// Our answer to a hub-issued CMD_CHALLENGE_REQ (0x3C) challenging our own 0x37 -- the terminal
/// 0x3D that closes the address-verification round. Scoped to the one capture that shows this
/// specific shape (END set): see expect_matches_real_device_capture()'s doxygen for why the
/// all-captures assertion can't be used for this command.
TEST(CorpusDeviceRoleBuilders, ChallengeRespDeviceRoleMatchesKlr200AddressVerification) {
  IoFrame origin{};
  ASSERT_TRUE(create_address_resp_device_role(origin, test::OWN_ID, test::DST_ID));
  IoFrame built{};
  ASSERT_TRUE(create_challenge_resp_device_role(built, test::DST_ID, test::OWN_ID, test::TEST_CHALLENGE, origin,
                                                test::TEST_SYSTEM_KEY));
  expect_matches_real_device_capture(CMD_CHALLENGE_RESP, "velux_kux100_pairing_full", built);
}
