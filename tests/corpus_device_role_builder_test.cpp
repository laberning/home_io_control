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
/// against every genuine device frame of that command in the corpus. That is evidence no
/// self-pair test can provide, because the counterparty here is a real Somfy/Velux device.
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
