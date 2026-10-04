/// @file corpus_spe_rollcall_replay_test.cpp
/// @brief Corpus replay coverage for decode_discovery_response() against real captured
/// CMD_DISCOVER_SPE_RESP (0x2B) roll-call replies.
///
/// The fixtures are `has_exchange = false` / `ExchangeKind::NONE` / `ExchangeOutcome::NONE` — a
/// `kind`-based predicate like CorpusPairingReplay's would select nothing here. The instantiation
/// predicate below matches on the frames themselves instead ("any frame with cmd ==
/// CMD_DISCOVER_SPE_RESP"), so it keeps working as more roll-call captures arrive.
///
/// Two shapes are covered: our own hub's roll-call answered by Somfy devices (`rx` = the 0x2B
/// replies only), and a passive capture of a real VELUX KLR300's roll-call, where the hub's own
/// 0x2A and the traffic around it are `rx` too — so only the 0x2B frames are decoded, and every
/// per-device field is pinned per responder rather than assumed brand-wide.

#include "corpus_generated.h"
#include "proto_codecs.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include "corpus_test_helpers.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace esphome::home_io_control;

namespace {

std::vector<const corpus::CorpusCapture *> spe_rollcall_replay_captures() {
  return corpus_test::captures_where([](const corpus::CorpusCapture *cap) {
    for (uint8_t i = 0; i < cap->frame_count; i++) {
      if (cap->frames[i].has_cmd && cap->frames[i].cmd == CMD_DISCOVER_SPE_RESP)
        return true;
    }
    return false;
  });
}

/// One real, already-paired responder's expected decode, keyed by the reply's source address.
/// Keyed by `src`, not the payload's backbone address: Somfy repeats its own node ID as the
/// backbone, but VELUX reports 00 00 00, so the backbone does not identify a VELUX responder —
/// which is also why scan_paired_devices() deduplicates by `frame.src`.
struct ExpectedResponder {
  uint8_t src[NODE_ID_SIZE];
  DeviceType type;
  uint8_t subtype;
  uint8_t manufacturer;
  uint8_t flags;  ///< Multi Information Byte (ATT class + power-save mode).
  uint8_t backbone[NODE_ID_SIZE];
};

constexpr ExpectedResponder EXPECTED_RESPONDERS[] = {
    // somfy_awning / somfy_izymo_dimmer: ATT class 3, power-save always-alive, backbone == src.
    {{0x30, 0xE1, 0xF2}, DeviceType::HORIZONTAL_AWNING, 0, MANUFACTURER_SOMFY, 0xCC, {0x30, 0xE1, 0xF2}},
    {{0xD2, 0x68, 0xCE}, DeviceType::HORIZONTAL_AWNING, 0, MANUFACTURER_SOMFY, 0xCC, {0xD2, 0x68, 0xCE}},
    {{0x41, 0x5C, 0xE4}, DeviceType::LIGHT, 0, MANUFACTURER_SOMFY, 0xCC, {0x41, 0x5C, 0xE4}},
    // velux_klr300: ATT class 2 / 3, power-save low-power, backbone 00 00 00.
    {{0x15, 0x5D, 0x81}, DeviceType::ROLLER_SHUTTER, 0, MANUFACTURER_VELUX, 0x8D, {0x00, 0x00, 0x00}},
    {{0x56, 0x22, 0x92}, DeviceType::ROLLER_SHUTTER, 0, MANUFACTURER_VELUX, 0xCD, {0x00, 0x00, 0x00}},
};

const ExpectedResponder *find_expected_responder(const uint8_t src[NODE_ID_SIZE]) {
  for (const auto &responder : EXPECTED_RESPONDERS) {
    if (memcmp(responder.src, src, NODE_ID_SIZE) == 0)
      return &responder;
  }
  return nullptr;
}

}  // namespace

class CorpusSpeRollcallReplay : public ::testing::TestWithParam<const corpus::CorpusCapture *> {};

TEST_P(CorpusSpeRollcallReplay, DecodeDiscoveryResponseMatchesRealCapturedBytes) {
  const corpus::CorpusCapture *capture = GetParam();

  std::vector<std::array<uint8_t, NODE_ID_SIZE>> responders_seen;

  for (uint8_t i = 0; i < capture->frame_count; i++) {
    const corpus::CorpusFrame &cf = capture->frames[i];
    if (cf.tx || !cf.has_cmd || cf.cmd != CMD_DISCOVER_SPE_RESP)
      continue;  // a passive capture also carries the controller's 0x2A and surrounding traffic as rx.

    IoFrame frame = corpus_test::parse_capture_frame(cf);
    ASSERT_EQ(frame.cmd, CMD_DISCOVER_SPE_RESP);

    IoDevice device{};
    std::string device_id;
    const DiscoveryResponseInfo info = decode_discovery_response(frame, device, device_id);

    const ExpectedResponder *expected = find_expected_responder(frame.src);
    ASSERT_NE(expected, nullptr) << "unexpected 0x2B source address in corpus fixture " << capture->id;

    EXPECT_TRUE(info.has_extended) << "real 0x2B replies are always the full 9-byte DISCOVERY_RESP_FULL_SIZE layout";
    EXPECT_TRUE(info.metadata_complete);
    EXPECT_EQ(info.manufacturer, expected->manufacturer);
    EXPECT_EQ(info.flags, expected->flags);
    EXPECT_EQ(memcmp(info.backbone, expected->backbone, NODE_ID_SIZE), 0);
    EXPECT_EQ(device.type, expected->type);
    EXPECT_EQ(device.subtype, expected->subtype);

    std::array<uint8_t, NODE_ID_SIZE> src_copy{};
    memcpy(src_copy.data(), frame.src, NODE_ID_SIZE);
    responders_seen.push_back(src_copy);
  }

  ASSERT_FALSE(responders_seen.empty()) << "every selected capture must carry at least one 0x2B reply";

  // A roll-call reports each device once; two replies from the same source within one capture
  // would mean the fixture (or its selection) is wrong. Only fires when a capture has more than
  // one reply.
  for (size_t a = 0; a < responders_seen.size(); a++) {
    for (size_t b = a + 1; b < responders_seen.size(); b++) {
      EXPECT_NE(responders_seen[a], responders_seen[b])
          << "two 0x2B replies within the same capture must come from different devices";
    }
  }
}

INSTANTIATE_TEST_SUITE_P(CorpusSpeRollcallReplay, CorpusSpeRollcallReplay,
                         ::testing::ValuesIn(spe_rollcall_replay_captures()), corpus_test::capture_name_generator);
