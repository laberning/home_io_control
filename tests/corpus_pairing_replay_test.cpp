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
/// Two real-hardware timing gaps the host-side MockRadio can't model (it has no per-attempt
/// clock — see tests/stubs/radio_test_common.h — `millis()` in host tests just increments once
/// per call, so a "2000ms" wait is really "however many loop iterations", not real elapsed time):
///
/// 1. **Retried TX with no intervening response.** A real capture may show the same command
///    transmitted N times before a response finally arrives (e.g. 3 identical DISCOVER_REQ
///    attempts, or an unanswered KEY_TRANSFER retried once). Replayed here, the queued response
///    is available on the very first attempt (MockRadio just returns the next queued packet on
///    any receive() call), so the engine only transmits once. This is a REPLAY LIMITATION, not
///    a code bug: the retried frames are byte-identical (challenge/key-derived content is
///    deterministic, so a retried KEY_TRANSFER is not a "different" frame), so collapsing
///    consecutive identical-content tx frames to a single expected transmission still asserts
///    real byte content — it just can't pin the real-world retry *count*, which was RF flakiness
///    on the day of capture, not protocol behavior worth freezing into a test.
/// 2. **Trailing, wholly-unanswered tx.** SetConfig1 (0x6F) is sent best-effort at the end of
///    pairing (finalize_pairing_configuration_()'s own doc: "the return value is informational
///    only"); a capture where nothing answers it does not reliably record how many of
///    EXCHANGE_RETRY_COUNT retries the real device actually saw before the operator moved on.
///    Such a trailing run is excluded from the tx-count assertion (checked with a >=, not an
///    exact count) — everything up to and including the last *answered* exchange is still
///    asserted byte-exact.

#include "corpus_generated.h"
#include "hub_core.h"
#include "pairing_engine.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_frame.h"
#include "radio_sx1262.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"
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

  const corpus::CorpusFrame *discover_resp_cf = nullptr;
  for (const corpus::CorpusFrame *rx_cf : rx_frames) {
    RadioRxPacket packet{};
    packet.len = rx_cf->crc_present ? static_cast<uint8_t>(rx_cf->len - 2) : rx_cf->len;
    std::memcpy(packet.data, rx_cf->bytes, packet.len);
    packet.freq_hz = rx_cf->freq_hz != 0 ? rx_cf->freq_hz : FREQ_CH2;
    radio->queue_rx(packet);
    if (rx_cf->has_cmd && rx_cf->cmd == CMD_DISCOVER_RESP)
      discover_resp_cf = rx_cf;
  }
  ASSERT_NE(discover_resp_cf, nullptr) << "pairing capture must contain a DISCOVER_RESP frame";

  const bool ok = comp.discover_and_pair();
  EXPECT_TRUE(ok) << "expected the replayed pairing to succeed, matching expect.exchange.outcome";

  const auto &sent = radio->get_sent_data();
  const auto &tx_configs = radio->get_tx_configs();
  if (trailing_unanswered) {
    ASSERT_GE(sent.size(), expected_tx.size()) << "engine transmitted fewer frames than the capture's answered prefix";
  } else {
    ASSERT_EQ(sent.size(), expected_tx.size())
        << "engine transmitted a different number of frames than the capture's (deduped) tx sequence";
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
