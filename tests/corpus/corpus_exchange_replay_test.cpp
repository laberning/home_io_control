/// @file corpus_exchange_replay_test.cpp
/// @brief Full-exchange replay through ExchangeEngine — the highest-fidelity
/// corpus test: scripts the real send_and_receive_() state machine with a capture's `rx`
/// frames and asserts what it actually transmits.
///
/// kind -> hub/engine-API mapping: AUTHENTICATED_COMMAND, DIRECT, and STATUS_POLL captures all
/// drive the same API, `IOHomeControlComponent::send_and_receive_(request, response, freq)`
/// (exchange_engine.cpp) — the origin `tx` frame becomes `request`; every `rx` frame is queued
/// into the mock radio in capture order, including any frames that classify IGNORE_UNRELATED
/// (ExchangeEngine's own wait loop skips those exactly as it would on real overheard traffic —
/// see corpus_classification_test.cpp for the same state machine asserted independently).
/// PAIRING captures are out of scope here — they have their own harness (corpus_pairing_replay_test.cpp).
///
/// Timing: a capture whose frames all carry `t_ms` replays on its own timeline under a
/// test_clock::ManualClock (corpus_test::queue_timed_rx(), tests/support/timed_replay.h). Each rx
/// frame arrives its captured gap after the start of the send it answers, and every send takes its
/// modelled time on air, so the engine's real windows, retry gap and total budget decide what it
/// hears: a response window shorter than a captured reply, a budget that no longer fits the
/// captured retries, or a try budget below the try the device answered all fail here. Captures
/// without timestamps (the synthetic ones) replay untimed, each rx frame delivered at once.
///
/// Try budget: send_and_receive_() defaults to the full EXCHANGE_RETRY_COUNT. A capture whose
/// real caller grants a different budget names it in REPLAY_CONTEXTS below, computed by the same
/// function that caller uses, so a change to that budget shows up against the captured behaviour.
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
#include "hub_decisions.h"
#include "test_helpers.h"
#include "timed_replay.h"
#include "stubs/radio_test_common.h"

#include <esp_random.h>
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <optional>
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

/// A capture's try budget when its real caller is not a plain send_and_receive_() call.
struct ReplayContext {
  const char *capture_id;
  uint8_t (*max_tries)();
};

const ReplayContext REPLAY_CONTEXTS[] = {
    // The status poll the scheduler sends to settle a STOP; the device answered only the retry.
    {"velux_ssl_statuspoll_stop_settle_preamble_escalation",
     [] { return decisions::scheduled_poll_max_tries(0, 0, /*settles_a_stop=*/true); }},
};

std::string hex_of(const uint8_t *data, size_t len) {
  static constexpr char DIGITS[] = "0123456789ABCDEF";
  std::string out;
  for (size_t i = 0; i < len; i++) {
    out += DIGITS[data[i] >> 4];
    out += DIGITS[data[i] & 0x0F];
    out += ' ';
  }
  return out;
}

uint8_t max_tries_for(const corpus::CorpusCapture *capture) {
  for (const ReplayContext &context : REPLAY_CONTEXTS) {
    if (std::strcmp(context.capture_id, capture->id) == 0)
      return context.max_tries();
  }
  return EXCHANGE_RETRY_COUNT;
}

}  // namespace

/// A capture, and the radio it is replayed over: the radio it was captured with (a mock, or the 2.4 GHz
/// bridge), or for a 868 MHz capture also the 868 MHz bridge, whose co-processor reports a reply late.
struct ReplayCase {
  const corpus::CorpusCapture *capture;
  bool over_868_bridge;
};

/// Why a capture cannot be replayed over the 868 MHz bridge, or nullptr. The bridge recovers a frame with the
/// software PHY's probe, whose gate `is_known_io_command()` drops a command it does not list. 0x2F is left out
/// on purpose (see NEVER_RECEIVED_ALLOWLIST in radio_sx1262_rx_test.cpp): a device sends it only after an
/// addressed 0x2E, which the hub never transmits, so no exchange of the hub's waits for it. The capture replays
/// over the mock, which skips the gate, and over the SPI drivers it would be dropped the same way.
const char *bridge_skip_reason(const corpus::CorpusCapture *capture) {
  if (std::strcmp(capture->id, "velux_kux100_discovery_alt_addressed_challenge_response") == 0) {
    return "its final reply is CMD_DISCOVER_ALT_RESP (0x2F), which is_known_io_command() deliberately does not "
           "list (nothing the hub sends draws it): the software PHY's gate drops it for the SX1262 and the LR1121 "
           "as much as for the bridge";
  }
  return nullptr;
}

std::vector<ReplayCase> replay_cases() {
  std::vector<ReplayCase> cases;
  for (const corpus::CorpusCapture *capture : exchange_replay_captures()) {
    cases.push_back({capture, false});
    if (capture->phy != corpus::Phy::IO_2G4)
      cases.push_back({capture, true});
  }
  return cases;
}

std::string replay_case_name(const ::testing::TestParamInfo<ReplayCase> &info) {
  return std::string(info.param.capture->id) + (info.param.over_868_bridge ? "_over_the_868_bridge" : "");
}

class CorpusExchangeReplay : public ::testing::TestWithParam<ReplayCase> {};

TEST_P(CorpusExchangeReplay, EngineReproducesCapturedExchange) {
  const corpus::CorpusCapture *capture = GetParam().capture;
  const bool over_bridge = GetParam().over_868_bridge;
  if (over_bridge && bridge_skip_reason(capture) != nullptr)
    GTEST_SKIP() << bridge_skip_reason(capture);
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

  // Constructed for a timed capture, and for any capture over the Bridle driver, whose link and
  // co-processor live on the manual clock: a ManualClock is opt-in per test (hal.h).
  std::optional<esphome::test_clock::ManualClock> clock;
  if (corpus_test::capture_is_timed(*capture) || capture->phy == corpus::Phy::IO_2G4 || over_bridge)
    clock.emplace();
  std::unique_ptr<RadioStandIn> radio =
      over_bridge ? corpus_test::make_868_bridge_stand_in() : corpus_test::make_stand_in(*capture);
  test::TestableHubComponent comp;
  comp.initialized_ = true;
  comp.radio_ = &radio->driver();
  std::memcpy(comp.node_id_, request.src, NODE_ID_SIZE);
  std::memcpy(comp.system_key_, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);

  if (corpus_test::capture_is_timed(*capture)) {
    radio->set_model_tx_airtime(true);
    const std::string error = corpus_test::queue_timed_rx(*radio, *capture, corpus_test::ReplayAnchor::SEND_INDEX);
    ASSERT_TRUE(error.empty()) << error;
  } else {
    for (const corpus::CorpusFrame *rx_cf : rx_frames) {
      RadioRxPacket packet = corpus_test::to_rx_packet(*rx_cf);
      packet.freq_hz = corpus_test::logical_freq(*rx_cf, *capture);
      radio->queue_rx(packet);
    }
  }

  IoFrame response{};
  const uint32_t tx_freq =
      corpus_test::logical_freq(*origin_cf, *capture) != 0 ? corpus_test::logical_freq(*origin_cf, *capture) : FREQ_CH2;
  // A rotating band starts on the last channel that carried an answer: the one the capture began on.
  comp.exchange_engine_.set_last_answered_freq(tx_freq);
  const bool ok = comp.send_and_receive_(request, response, tx_freq, max_tries_for(capture)) ==
                  ExchangeOutcome::SUCCESS_WITH_RESPONSE;

  // On a timed replay, a failure names when the engine sent against when the capture did, and the
  // windows it listened for: a shrunk window or budget reads straight off these three lines.
  std::string timeline;
  if (clock.has_value()) {
    timeline += "\nengine sends at ms:";
    const std::vector<uint32_t> send_times = radio->send_times_ms();
    for (uint32_t t : send_times)
      timeline += " " + std::to_string(t - send_times.front());
    timeline += "\ncaptured sends at ms:";
    for (uint8_t i = 0; i < capture->frame_count; i++) {
      if (capture->frames[i].tx)
        timeline += " " + std::to_string(
                              (corpus_test::stamp_us(capture->frames[i]) - corpus_test::stamp_us(*origin_cf)) / 1000);
    }
    timeline += "\nengine listen windows ms:";
    for (uint32_t w : radio->wait_timeouts())
      timeline += " " + std::to_string(w);
  }
  SCOPED_TRACE(timeline);

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

  const auto sent = radio->sent_frames();
  ASSERT_EQ(sent.size(), expected_tx.size())
      << "engine transmitted a different number of frames than the capture recorded";

  const auto &tx_configs = radio->tx_configs();
  for (size_t i = 0; i < expected_tx.size(); i++) {
    const corpus::CorpusFrame *cf = expected_tx[i];
    const uint8_t non_crc_len = corpus_test::wire_len(*cf);
    SCOPED_TRACE(::testing::Message() << "tx frame index=" << i);

    if (cf->freq_hz != 0) {
      // A repeat of the request is a retry; on a rotating band it goes out on the next channel
      // (decisions::retry_frequency()), where the capture's bench tool repeated on the same one.
      size_t request_repeats = 0;
      for (size_t j = 0; j < i; j++)
        request_repeats += expected_tx[j]->bytes[FRAME_CMD_OFFSET] == origin_cf->bytes[FRAME_CMD_OFFSET] ? 1 : 0;
      const bool is_repeat = i > 0 && cf->bytes[FRAME_CMD_OFFSET] == origin_cf->bytes[FRAME_CMD_OFFSET];
      const uint32_t expected_freq =
          (capture->phy == corpus::Phy::IO_2G4 && is_repeat)
              ? decisions::retry_frequency(true, 0, corpus_test::logical_freq(*origin_cf, *capture),
                                           static_cast<uint8_t>(request_repeats))
              : corpus_test::logical_freq(*cf, *capture);
      EXPECT_EQ(tx_configs[i].freq_hz, expected_freq) << "TX frequency mismatch";
    }

    if (capture->key == corpus::KeyMode::CORPUS) {
      ASSERT_EQ(sent[i].size(), non_crc_len) << "byte-exact length mismatch (key: corpus)";
      // One known difference, asserted rather than hidden: the hub marks its 0x3D LOW_POWER, as the
      // Somfy 868 MHz captures show (`0E 20 ...`), while the VELUX KLR 200 on 2.4 GHz sends it
      // with CTRL1 0x00. Everything else, the HMAC included, must match to the byte.
      std::vector<uint8_t> expected(cf->bytes, cf->bytes + non_crc_len);
      if (capture->phy == corpus::Phy::IO_2G4 && expected[FRAME_CMD_OFFSET] == CMD_CHALLENGE_RESP)
        expected[1] |= CTRL1_LOW_POWER;
      EXPECT_EQ(std::memcmp(sent[i].data(), expected.data(), non_crc_len), 0)
          << "byte-exact content mismatch (key: corpus) — engine did not reproduce the captured frame\n  sent:     "
          << hex_of(sent[i].data(), sent[i].size()) << "\n  captured: " << hex_of(cf->bytes, non_crc_len);
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

  // A 2.4 GHz origin frame carries the wake train its controller sent; the level the hub asked
  // for must be the level that controller used (START + LOW_POWER: the long train).
  if (capture->phy == corpus::Phy::IO_2G4 && origin_cf->has_wake_train && !tx_configs.empty()) {
    const TxWake used = (request.ctrl1 & CTRL1_LOW_POWER) != 0 ? TxWake::LONG : TxWake::SHORT;
    EXPECT_EQ(tx_configs[0].wake, used) << "the hub woke the receiver differently from the captured controller";
  }
}

INSTANTIATE_TEST_SUITE_P(CorpusExchangeReplay, CorpusExchangeReplay, ::testing::ValuesIn(replay_cases()),
                         replay_case_name);

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
