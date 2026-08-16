/// @file corpus_exchange_replay_test.cpp
/// @brief Full-exchange replay through ExchangeEngine (design §6.5) — the highest-fidelity
/// corpus test: scripts the real send_and_receive_() state machine with a capture's `rx`
/// frames and asserts what it actually transmits.
///
/// kind -> hub/engine-API mapping: AUTHENTICATED_COMMAND, DIRECT, and STATUS_POLL captures all
/// drive the same API, `IOHomeControlComponent::send_and_receive_(request, response, freq)`
/// (exchange_engine.cpp) — the origin `tx` frame becomes `request`; every `rx` frame is queued
/// into the mock radio in capture order, including any frames that classify IGNORE_UNRELATED
/// (ExchangeEngine's own wait loop skips those exactly as it would on real overheard traffic —
/// see corpus_classification_test.cpp for the same state machine asserted independently).
/// PAIRING captures are out of scope here (arrives with Step H2 and a dedicated pairing harness).
///
/// Byte-exactness: for `key: corpus` captures the challenge comes from the capture itself and
/// HMAC is a pure function of it, so every transmitted frame — including the computed 0x3D — is
/// asserted **byte-exact** against the captured `tx` frames. For `key: unknown` captures the
/// real system key is not available, so only **structural** fields (cmd/dst/src/start/end) are
/// asserted; a mismatched key still produces a well-formed, differently-keyed 0x3D that must
/// not be compared byte-for-byte.

#include "corpus_generated.h"
#include "hub_core.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_frame.h"
#include "proto_timing.h"
#include "radio_sx1262.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <esp_random.h>
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace esphome::home_io_control;

namespace {

std::vector<const corpus::CorpusCapture *> exchange_replay_captures() {
  return corpus_test::captures_where([](const corpus::CorpusCapture *cap) {
    return cap->has_exchange &&
           (cap->kind == corpus::ExchangeKind::AUTHENTICATED_COMMAND || cap->kind == corpus::ExchangeKind::DIRECT ||
            cap->kind == corpus::ExchangeKind::STATUS_POLL);
  });
}

std::unique_ptr<MockRadio> make_mock_radio(const corpus::CorpusCapture *capture) {
  // "sx1262" is the only chip whose mock (MockRadioSX1262) differs in exchange-timing behavior
  // (response_preamble/exchange_wait_slice_ms/has_fast_tx_rx_turnaround); every other
  // captured_with value (sx1276, lr1121, other, synthetic) is fine on the generic MockRadio,
  // which already mirrors the fast-hopping SX1276-like reference behavior (radio_test_common.h).
  // LR1121 shares its exchange-flow implementation with SX1262 (SoftPhyDriverBase) but has not
  // been shown to need SX1262's distinct timing mock for replay purposes -- revisit if an
  // LR1121-specific exchange-timing difference is ever found.
  if (std::string(capture->captured_with) == "sx1262")
    return std::make_unique<MockRadioSX1262>();
  return std::make_unique<MockRadio>();
}

}  // namespace

class CorpusExchangeReplay : public ::testing::TestWithParam<const corpus::CorpusCapture *> {};

TEST_P(CorpusExchangeReplay, EngineReproducesCapturedExchange) {
  const corpus::CorpusCapture *capture = GetParam();
  SCOPED_TRACE(::testing::Message() << "capture=" << capture->id);

  const corpus::CorpusFrame *origin_cf = nullptr;
  std::vector<const corpus::CorpusFrame *> rx_frames;
  std::vector<const corpus::CorpusFrame *> tx_frames_after_origin;
  for (uint8_t i = 0; i < capture->frame_count; i++) {
    const corpus::CorpusFrame &cf = capture->frames[i];
    if (cf.tx) {
      if (origin_cf == nullptr) {
        origin_cf = &cf;
      } else {
        tx_frames_after_origin.push_back(&cf);
      }
    } else {
      rx_frames.push_back(&cf);
    }
  }
  ASSERT_NE(origin_cf, nullptr) << "exchange-shaped capture must have an origin tx frame";

  IoFrame request = corpus_test::parse_capture_frame(*origin_cf);

  std::unique_ptr<MockRadio> radio = make_mock_radio(capture);
  test::TestableHubComponent comp;
  comp.initialized_ = true;
  comp.radio_ = radio.get();
  std::memcpy(comp.node_id_, request.src, NODE_ID_SIZE);
  std::memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  for (const corpus::CorpusFrame *rx_cf : rx_frames) {
    RadioRxPacket packet{};
    packet.len = rx_cf->crc_present ? static_cast<uint8_t>(rx_cf->len - 2) : rx_cf->len;
    std::memcpy(packet.data, rx_cf->bytes, packet.len);
    packet.freq_hz = rx_cf->freq_hz != 0 ? rx_cf->freq_hz : FREQ_CH2;
    radio->queue_rx(packet);
  }

  IoFrame response{};
  const uint32_t tx_freq = origin_cf->freq_hz != 0 ? origin_cf->freq_hz : FREQ_CH2;
  const bool ok = comp.send_and_receive_(request, response, tx_freq);

  if (capture->outcome == corpus::ExchangeOutcome::SUCCESS) {
    EXPECT_TRUE(ok) << "expected the replayed exchange to succeed, matching expect.exchange.outcome";
  } else if (capture->outcome == corpus::ExchangeOutcome::TIMEOUT ||
             capture->outcome == corpus::ExchangeOutcome::FAILURE) {
    EXPECT_FALSE(ok) << "expected the replayed exchange to fail, matching expect.exchange.outcome";
  }

  std::vector<const corpus::CorpusFrame *> expected_tx;
  expected_tx.push_back(origin_cf);
  for (const corpus::CorpusFrame *cf : tx_frames_after_origin)
    expected_tx.push_back(cf);

  const auto &sent = radio->get_sent_data();
  ASSERT_EQ(sent.size(), expected_tx.size())
      << "engine transmitted a different number of frames than the capture recorded";

  const auto &tx_configs = radio->get_tx_configs();
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

    // key: unknown — structural only; a 0x3D built under our test key will differ byte-for-byte
    // from one built under the real (unavailable) key, but cmd/endpoints/flags must still match.
    IoFrame captured_frame{};
    ASSERT_TRUE(parse(cf->bytes, non_crc_len, captured_frame));
    IoFrame sent_frame{};
    ASSERT_TRUE(parse(sent[i].data(), static_cast<uint8_t>(sent[i].size()), sent_frame));
    EXPECT_EQ(sent_frame.cmd, captured_frame.cmd) << "cmd mismatch";
    EXPECT_EQ(std::memcmp(sent_frame.dst, captured_frame.dst, NODE_ID_SIZE), 0) << "dst mismatch";
    EXPECT_EQ(std::memcmp(sent_frame.src, captured_frame.src, NODE_ID_SIZE), 0) << "src mismatch";
    EXPECT_EQ(is_start(sent_frame), is_start(captured_frame)) << "start-flag mismatch";
    EXPECT_EQ(is_end(sent_frame), is_end(captured_frame)) << "end-flag mismatch";
  }
}

INSTANTIATE_TEST_SUITE_P(CorpusExchangeReplay, CorpusExchangeReplay, ::testing::ValuesIn(exchange_replay_captures()),
                         corpus_test::capture_name_generator);

/// Smoke test for tests/include/esp_random.h's test_rng stub — pins the enqueue/reset contract
/// relied on elsewhere (this file's pairing replay tests; ProtoCrypto.GenerateChallenge* in
/// proto_crypto_test.cpp, which uses reset() to force the deterministic LCG fallback).
TEST(TestRng, EnqueueReturnsQueuedValuesThenFallsBackToLcg) {
  test_rng::enqueue_byte(0xAB);
  test_rng::enqueue_byte(0xCD);
  EXPECT_EQ(esp_random() & 0xFF, 0xABu);
  EXPECT_EQ(esp_random() & 0xFF, 0xCDu);
  test_rng::reset();
  // Queue is empty again — falls back to the deterministic LCG, not another queued value.
  const uint32_t lcg_value = esp_random();
  EXPECT_NE(lcg_value, 0xABu);
  EXPECT_NE(lcg_value, 0xCDu);
}
