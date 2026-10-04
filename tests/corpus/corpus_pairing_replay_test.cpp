/// @file corpus_pairing_replay_test.cpp
/// @brief Full-pairing replay through PairingEngine —
/// scripts the real `IOHomeControlComponent::discover_and_pair()` state machine with a capture's
/// `rx` frames and asserts what it actually transmits, plus the resulting device registration.
///
/// Scope: only `kind: pairing`, `outcome: success`, **own-hardware** captures are replayed here.
/// `source.origin == "own-hardware"` is used as the completeness proxy — these are raw captures
/// from this project's own log, not user-pasted excerpts. The two `somfy_rs100_pairing_*`
/// captures (`source.origin: github-issue`) are deliberately excluded: one is `outcome: timeout`
/// (this suite only covers success), and the other has frames explicitly marked
/// "RECONSTRUCTED"/"UNVERIFIED" in their own notes (no raw 0x33 ever existed to check against)
/// and is missing its 0x29 discovery-response frame entirely — neither is a byte-exact replay
/// target, by the capture's own documentation.
///
/// Timing: every capture replayed here is timestamped, so it replays on its own timeline under a
/// test_clock::ManualClock (corpus_test::queue_timed_rx(), tests/support/timed_replay.h). Each rx
/// frame arrives its captured gap after the first send with the same bytes as the tx frame it
/// followed, and every send takes its modelled time on air. The engine's real discovery windows,
/// key-exchange waits and retries therefore decide what it hears: a window shorter than a captured
/// reply fails the replay.
///
/// Three rules shape what this replay asserts:
///
/// 1. **Retry counts.** A real capture may show the same command transmitted N times before a
///    response arrives (e.g. 3 identical DISCOVER_REQ attempts, or an unanswered KEY_TRANSFER
///    retried once), and a capture's excerpt may also show fewer tries than the hub really made.
///    How many tries it took was RF conditions on the day, not protocol behavior worth freezing.
///    Runs of consecutive byte-identical frames are therefore collapsed to one on **both** sides,
///    the capture's tx and the engine's sends, before comparing. The retried frames are
///    byte-identical (challenge/key-derived content is deterministic), so this still asserts real
///    byte content and order; each reply is timed from the last try of its run, the one it
///    actually followed.
/// 2. **Trailing, wholly-unanswered tx.** SetConfig1 (0x6F) is sent at the end of pairing and is
///    optional (finalize_pairing_configuration_()'s own doc); a capture where nothing answers it
///    may record more tries than today's PAIRING_SET_CONFIG1_MAX_TRIES, from a build that
///    retried it.
///    Such a trailing run is excluded from the tx-count assertion (checked with a >=, not an
///    exact count) — everything up to and including the last *answered* exchange is still
///    asserted byte-exact.
///
/// 3. **A step-on discover-confirm (0x2C) replay needs its own 0x2D in the capture.** Each
///    replayed capture sets `comp.tuning_.pairing_discover_confirm` to match what it actually
///    shows: `SKIP` when the capture has no tx 0x2C at all, or `SEND`/`SEND_WITH_ACK` — chosen
///    from the captured 0x2C's own CTRL1_ACK bit — when it does (the `*_discover_confirm_*`
///    captures, which also carry the device's 0x2D). A capture that shows a 0x2C but no 0x2D (the
///    device never confirmed) has nothing for the step's listen to hear and must be excluded from
///    this suite with this reason; it is not a successful pairing in any case.

#include "corpus_generated.h"
#include "hub_core.h"
#include "pairing_engine.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_frame.h"
#include "radio_sx1262.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"
#include "timed_replay.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace esphome::home_io_control;

namespace {

std::vector<const corpus::CorpusCapture *> pairing_replay_captures() {
  return corpus_test::captures_where([](const corpus::CorpusCapture *cap) {
    return cap->has_exchange && cap->kind == corpus::ExchangeKind::PAIRING &&
           cap->outcome == corpus::ExchangeOutcome::SUCCESS && std::string(cap->source_origin) == "own-hardware";
  });
}

std::unique_ptr<MockRadio> make_mock_radio(const corpus::CorpusCapture *capture) {
  if (std::string(capture->captured_with) == "sx1262")
    return std::make_unique<MockRadioSX1262>();
  return std::make_unique<MockRadio>();
}

/// True if a captured frame is byte-identical to another (same length, same content) — used to
/// collapse consecutive identical-content tx frames (retries) to one expected transmission.
bool same_bytes(const corpus::CorpusFrame &a, const corpus::CorpusFrame &b) {
  return corpus_test::wire_len(a) == corpus_test::wire_len(b) &&
         std::memcmp(a.bytes, b.bytes, corpus_test::wire_len(a)) == 0;
}

/// The engine's sends and their configs with runs of consecutive byte-identical sends (retries)
/// collapsed to their first, the same treatment the capture's own tx frames get (limitation 1).
struct DedupedSends {
  std::vector<std::vector<uint8_t>> data;
  std::vector<RadioTxConfig> configs;
};

DedupedSends dedup_sends(const MockRadio &radio) {
  DedupedSends out;
  const auto &sent = radio.get_sent_data();
  for (size_t i = 0; i < sent.size(); i++) {
    if (!out.data.empty() && out.data.back() == sent[i])
      continue;
    out.data.push_back(sent[i]);
    out.configs.push_back(radio.get_tx_configs()[i]);
  }
  return out;
}

}  // namespace

class CorpusPairingReplay : public ::testing::TestWithParam<const corpus::CorpusCapture *> {};

TEST_P(CorpusPairingReplay, EngineReproducesCapturedPairing) {
  const corpus::CorpusCapture *capture = GetParam();
  SCOPED_TRACE(::testing::Message() << "capture=" << capture->id);
  ASSERT_GT(capture->frame_count, 0u);

  std::vector<const corpus::CorpusFrame *> rx_frames;
  std::vector<const corpus::CorpusFrame *> expected_tx;  // deduped consecutive identical tx
  for (uint8_t i = 0; i < capture->frame_count; i++) {
    const corpus::CorpusFrame &cf = capture->frames[i];
    if (!cf.tx) {
      rx_frames.push_back(&cf);
      continue;
    }
    if (!expected_tx.empty() && same_bytes(*expected_tx.back(), cf))
      continue;  // retry of the immediately preceding tx — see file header, limitation 1
    expected_tx.push_back(&cf);
  }
  ASSERT_FALSE(expected_tx.empty()) << "pairing capture must have at least one tx frame";

  // Trailing, wholly-unanswered tx run (limitation 2): the capture's very last frame is itself
  // a tx with nothing after it. Drop it from the byte-exact assertion; sent.size() is checked
  // with >= instead of == for this capture.
  const bool trailing_unanswered = capture->frames[capture->frame_count - 1].tx;
  if (trailing_unanswered)
    expected_tx.pop_back();
  ASSERT_FALSE(expected_tx.empty()) << "capture has no answered tx frames to assert against";

  std::unique_ptr<MockRadio> radio = make_mock_radio(capture);
  test::TestableHubComponent comp;
  comp.initialized_ = true;
  comp.radio_ = radio.get();
  std::memcpy(comp.node_id_, expected_tx.front()->bytes + 5, NODE_ID_SIZE);  // origin tx SRC offset
  std::memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  // See limitation 3 above: SKIP for a capture with no tx 0x2C (every capture replayed today),
  // else SEND/SEND_WITH_ACK matching the captured frame's own CTRL1_ACK bit.
  comp.tuning_.pairing_discover_confirm = DiscoverConfirmMode::SKIP;
  for (uint8_t i = 0; i < capture->frame_count; i++) {
    const corpus::CorpusFrame &cf = capture->frames[i];
    if (!cf.tx || !cf.has_cmd || cf.cmd != CMD_DISCOVER_CONFIRM)
      continue;
    IoFrame parsed{};
    ASSERT_TRUE(parse(cf.bytes, corpus_test::wire_len(cf), parsed)) << "captured 0x2C frame must parse";
    comp.tuning_.pairing_discover_confirm =
        (parsed.ctrl1 & CTRL1_ACK) != 0 ? DiscoverConfirmMode::SEND_WITH_ACK : DiscoverConfirmMode::SEND;
    break;
  }

  const corpus::CorpusFrame *discover_resp_cf = nullptr;
  for (const corpus::CorpusFrame *rx_cf : rx_frames) {
    if (rx_cf->has_cmd && rx_cf->cmd == CMD_DISCOVER_RESP)
      discover_resp_cf = rx_cf;
  }
  ASSERT_NE(discover_resp_cf, nullptr) << "pairing capture must contain a DISCOVER_RESP frame";

  ASSERT_TRUE(corpus_test::capture_is_timed(*capture)) << "every replayed pairing capture is timestamped";
  esphome::test_clock::ManualClock clock;
  radio->set_model_tx_airtime(true);
  const std::string error =
      corpus_test::queue_timed_rx(*radio, *capture, corpus_test::ReplayAnchor::FIRST_MATCHING_SEND);
  ASSERT_TRUE(error.empty()) << error;

  const bool ok = comp.discover_and_pair();
  EXPECT_TRUE(ok) << "expected the replayed pairing to succeed, matching expect.exchange.outcome";

  const DedupedSends deduped = dedup_sends(*radio);
  const auto &sent = deduped.data;
  const auto &tx_configs = deduped.configs;
  if (trailing_unanswered) {
    ASSERT_GE(sent.size(), expected_tx.size()) << "engine transmitted fewer frames than the capture's answered prefix";
  } else {
    ASSERT_EQ(sent.size(), expected_tx.size())
        << "engine's deduped sends differ in number from the capture's deduped tx sequence";
  }

  for (size_t i = 0; i < expected_tx.size(); i++) {
    const corpus::CorpusFrame *cf = expected_tx[i];
    const uint8_t non_crc_len = corpus_test::wire_len(*cf);
    SCOPED_TRACE(::testing::Message() << "tx frame index=" << i);

    if (cf->freq_hz != 0) {
      EXPECT_EQ(tx_configs[i].freq_hz, cf->freq_hz) << "TX frequency mismatch";
    }

    if (capture->key == corpus::KeyMode::CORPUS) {
      ASSERT_EQ(sent[i].size(), non_crc_len) << "byte-exact length mismatch (key: corpus)";
      EXPECT_EQ(std::memcmp(sent[i].data(), cf->bytes, non_crc_len), 0)
          << "byte-exact content mismatch (key: corpus) — engine did not reproduce the captured frame";
      continue;
    }

    IoFrame captured_frame{};
    ASSERT_TRUE(parse(cf->bytes, non_crc_len, captured_frame));
    IoFrame sent_frame{};
    ASSERT_TRUE(parse(sent[i].data(), static_cast<uint8_t>(sent[i].size()), sent_frame));
    EXPECT_EQ(sent_frame.cmd, captured_frame.cmd) << "cmd mismatch";
    EXPECT_EQ(std::memcmp(sent_frame.dst, captured_frame.dst, NODE_ID_SIZE), 0) << "dst mismatch";
    EXPECT_EQ(std::memcmp(sent_frame.src, captured_frame.src, NODE_ID_SIZE), 0) << "src mismatch";
  }

  // The paired device must be registered with the type/subtype the capture's own DISCOVER_RESP
  // encodes — cross-checked via the same static helper the engine itself uses, not hardcoded
  // per-capture expectations.
  IoFrame discover_resp{};
  ASSERT_TRUE(parse(discover_resp_cf->bytes, corpus_test::wire_len(*discover_resp_cf), discover_resp));
  IoDevice expected_device{};
  std::string expected_device_id;
  PairingEngine::parse_device_from_discovery(discover_resp, expected_device, expected_device_id);

  const IoDevice *registered = comp.get_device(expected_device_id);
  ASSERT_NE(registered, nullptr) << "paired device should be registered after discover_and_pair()";
  EXPECT_EQ(registered->type, expected_device.type) << "registered device type should match DISCOVER_RESP";
  EXPECT_EQ(registered->subtype, expected_device.subtype) << "registered device subtype should match DISCOVER_RESP";
}

INSTANTIATE_TEST_SUITE_P(CorpusPairingReplay, CorpusPairingReplay, ::testing::ValuesIn(pairing_replay_captures()),
                         corpus_test::capture_name_generator);
