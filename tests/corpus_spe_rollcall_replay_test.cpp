/// @file corpus_spe_rollcall_replay_test.cpp
/// @brief Corpus replay coverage for decode_discovery_response() against real captured
/// CMD_DISCOVER_SPE_RESP (0x2B) roll-call replies.
///
/// Both fixtures (`somfy_awning_discovery_spe_paired_rollcall.yaml`,
/// `somfy_izymo_dimmer_discovery_spe_paired_rollcall.yaml`) are `has_exchange = false` /
/// `ExchangeKind::NONE` / `ExchangeOutcome::NONE` — a `kind`-based predicate like
/// CorpusPairingReplay's would select nothing here. The instantiation predicate below matches on
/// the frames themselves instead ("any frame with cmd == CMD_DISCOVER_SPE_RESP"), so it keeps
/// working as more roll-call captures arrive.

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

/// One real, already-paired responder's expected decode, keyed by its backbone address — the
/// device type and subtype decoded from the two corpus fixtures.
struct ExpectedResponder {
  uint8_t backbone[NODE_ID_SIZE];
  DeviceType type;
  uint8_t subtype;
};

constexpr ExpectedResponder EXPECTED_RESPONDERS[] = {
    {{0x30, 0xE1, 0xF2}, DeviceType::HORIZONTAL_AWNING, 0},  // somfy_awning: first unit
    {{0xD2, 0x68, 0xCE}, DeviceType::HORIZONTAL_AWNING, 0},  // somfy_awning: second unit
    {{0x41, 0x5C, 0xE4}, DeviceType::LIGHT, 0},              // somfy_izymo_dimmer
};

const ExpectedResponder *find_expected_responder(const uint8_t backbone[NODE_ID_SIZE]) {
  for (const auto &responder : EXPECTED_RESPONDERS) {
    if (memcmp(responder.backbone, backbone, NODE_ID_SIZE) == 0)
      return &responder;
  }
  return nullptr;
}

}  // namespace

class CorpusSpeRollcallReplay : public ::testing::TestWithParam<const corpus::CorpusCapture *> {};

TEST_P(CorpusSpeRollcallReplay, DecodeDiscoveryResponseMatchesRealCapturedBytes) {
  const corpus::CorpusCapture *capture = GetParam();

  std::vector<std::array<uint8_t, NODE_ID_SIZE>> backbones_seen;

  for (uint8_t i = 0; i < capture->frame_count; i++) {
    const corpus::CorpusFrame &cf = capture->frames[i];
    if (cf.tx)
      continue;  // only replies (rx) carry CMD_DISCOVER_SPE_RESP in these fixtures.

    IoFrame frame = corpus_test::parse_capture_frame(cf);
    ASSERT_EQ(frame.cmd, CMD_DISCOVER_SPE_RESP) << "every rx frame in an SPE roll-call fixture is a 0x2B reply";

    IoDevice device{};
    std::string device_id;
    const DiscoveryResponseInfo info = decode_discovery_response(frame, device, device_id);

    EXPECT_TRUE(info.has_extended) << "real 0x2B replies are always the full 9-byte DISCOVERY_RESP_FULL_SIZE layout";
    EXPECT_TRUE(info.metadata_complete);
    EXPECT_EQ(info.manufacturer, MANUFACTURER_SOMFY);
    EXPECT_EQ(info.flags, 0xCC) << "both fixtures report ATT class 3, power-save always-alive";
    EXPECT_EQ(memcmp(info.backbone, frame.src, NODE_ID_SIZE), 0)
        << "the reported backbone address matches the frame's source address";

    const ExpectedResponder *expected = find_expected_responder(info.backbone);
    ASSERT_NE(expected, nullptr) << "unexpected backbone address in corpus fixture " << capture->id;
    EXPECT_EQ(device.type, expected->type);
    EXPECT_EQ(device.subtype, expected->subtype);

    std::array<uint8_t, NODE_ID_SIZE> backbone_copy{};
    memcpy(backbone_copy.data(), info.backbone, NODE_ID_SIZE);
    backbones_seen.push_back(backbone_copy);
  }

  ASSERT_FALSE(backbones_seen.empty()) << "every selected capture must carry at least one 0x2B reply";

  // Pins that per-device fields really are per-device — the property the whole roll-call
  // feature depends on. Only fires when a capture has more than one reply (the awning fixture).
  for (size_t a = 0; a < backbones_seen.size(); a++) {
    for (size_t b = a + 1; b < backbones_seen.size(); b++) {
      EXPECT_NE(backbones_seen[a], backbones_seen[b])
          << "two replies within the same capture must not decode to the same backbone address";
    }
  }
}

INSTANTIATE_TEST_SUITE_P(CorpusSpeRollcallReplay, CorpusSpeRollcallReplay,
                         ::testing::ValuesIn(spe_rollcall_replay_captures()), corpus_test::capture_name_generator);
