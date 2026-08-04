/// @file corpus_classification_test.cpp
/// @brief Decision-layer expectations (design §6.4) — pure `decisions::` classifiers fed with
/// captured frames in recorded order.
///
/// Scope: captures with `expect.exchange` (`has_exchange`). The origin (first `tx` frame) is
/// used as `request`; `rx` frames are classified in capture order exactly as ExchangeEngine
/// does (exchange_engine.cpp: `wait_for_first_response_()` uses
/// `classify_exchange_first_response(request, ...)` for the first inbound frame,
/// `wait_for_final_response_()` uses `classify_exchange_final_response(request, ...)` — same
/// `request` object throughout — for every inbound frame after that). A per-frame
/// `classification` expectation that this file has no wiring for is a hard failure, not a
/// silent skip — an ingested capture's expectation must always be checked or rejected loudly.
///
/// Name<->value pinning: the generated header stores `classification` as a bare `uint8_t`
/// (scripts/corpus/build.py :: CLASSIFICATION_ENUM). The static_asserts below pin every name in
/// that table against the real enum in hub_decisions.h, so a reorder there breaks the build
/// instead of silently mis-scoring a capture.

#include "corpus_generated.h"
#include "hub_decisions.h"
#include "proto_constants.h"
#include "proto_frame.h"

#include "corpus_test_helpers.h"

#include <gtest/gtest.h>

#include <vector>

using namespace esphome::home_io_control;

namespace {

// --- Pin scripts/corpus/build.py :: CLASSIFICATION_ENUM against the real enums -------------
static_assert(static_cast<uint8_t>(decisions::ExchangeFirstResponseDisposition::IGNORE_UNRELATED) == 0,
              "CLASSIFICATION_ENUM['IGNORE_UNRELATED'] must match ExchangeFirstResponseDisposition::IGNORE_UNRELATED");
static_assert(static_cast<uint8_t>(decisions::ExchangeFirstResponseDisposition::COMPLETE_DIRECT) == 1,
              "CLASSIFICATION_ENUM['COMPLETE_DIRECT'] must match ExchangeFirstResponseDisposition::COMPLETE_DIRECT");
static_assert(static_cast<uint8_t>(decisions::ExchangeFirstResponseDisposition::REQUIRE_AUTH) == 2,
              "CLASSIFICATION_ENUM['REQUIRE_AUTH'] must match ExchangeFirstResponseDisposition::REQUIRE_AUTH");
static_assert(static_cast<uint8_t>(decisions::ExchangeFinalResponseDisposition::IGNORE_UNRELATED) == 0,
              "CLASSIFICATION_ENUM['IGNORE_UNRELATED'] must match ExchangeFinalResponseDisposition::IGNORE_UNRELATED");
static_assert(static_cast<uint8_t>(decisions::ExchangeFinalResponseDisposition::ACCEPT) == 1,
              "CLASSIFICATION_ENUM['ACCEPT'] must match ExchangeFinalResponseDisposition::ACCEPT");
static_assert(static_cast<uint8_t>(decisions::PairingDiscoveryDisposition::NO_RESPONSE) == 0,
              "CLASSIFICATION_ENUM['NO_RESPONSE'] must match PairingDiscoveryDisposition::NO_RESPONSE");
static_assert(static_cast<uint8_t>(decisions::PairingDiscoveryDisposition::INVALID) == 1,
              "CLASSIFICATION_ENUM['INVALID'] must match PairingDiscoveryDisposition::INVALID");
static_assert(static_cast<uint8_t>(decisions::PairingDiscoveryDisposition::ACCEPT) == 2,
              "CLASSIFICATION_ENUM['DISCOVERY_ACCEPT'] must match PairingDiscoveryDisposition::ACCEPT");
static_assert(static_cast<uint8_t>(decisions::PairingKeyChallengeDisposition::IGNORE) == 0,
              "CLASSIFICATION_ENUM['IGNORE'] must match PairingKeyChallengeDisposition::IGNORE");
static_assert(static_cast<uint8_t>(decisions::PairingKeyChallengeDisposition::ACCEPT) == 1,
              "CLASSIFICATION_ENUM['ACCEPT'] must match PairingKeyChallengeDisposition::ACCEPT");

std::vector<const corpus::CorpusCapture *> exchange_captures() {
  return corpus_test::captures_where([](const corpus::CorpusCapture *cap) { return cap->has_exchange; });
}

}  // namespace

class CorpusClassification : public ::testing::TestWithParam<const corpus::CorpusCapture *> {};

TEST_P(CorpusClassification, FramesClassifyInRecordedOrder) {
  const corpus::CorpusCapture *capture = GetParam();
  SCOPED_TRACE(::testing::Message() << "capture=" << capture->id);

  switch (capture->kind) {
    case corpus::ExchangeKind::AUTHENTICATED_COMMAND:
    case corpus::ExchangeKind::DIRECT:
    case corpus::ExchangeKind::STATUS_POLL: {
      // Mirrors the state machine ExchangeEngine::send_and_receive() actually drives, not just
      // encounter order: wait_for_first_response_() classifies *every* inbound frame via
      // classify_exchange_first_response() and keeps looping past IGNORE_UNRELATED ones — a
      // capture can legitimately contain unrelated overheard traffic (another device's frames)
      // interleaved before or during a real exchange, and those must still classify
      // IGNORE_UNRELATED rather than being mistaken for the challenge/final response by simple
      // position. Once a non-ignored disposition arrives (REQUIRE_AUTH), wait_for_final_response_()
      // takes over the same way with classify_exchange_final_response(), until ACCEPT.
      const corpus::CorpusFrame *origin_cf = nullptr;
      for (uint8_t i = 0; i < capture->frame_count; i++) {
        if (capture->frames[i].tx) {
          origin_cf = &capture->frames[i];
          break;
        }
      }
      ASSERT_NE(origin_cf, nullptr) << "exchange-shaped capture must have an origin tx frame";
      IoFrame origin = corpus_test::parse_capture_frame(*origin_cf);

      enum class Stage { SEEKING_FIRST, SEEKING_FINAL, DONE } stage = Stage::SEEKING_FIRST;
      for (uint8_t i = 0; i < capture->frame_count; i++) {
        const corpus::CorpusFrame &cf = capture->frames[i];
        if (cf.tx)
          continue;
        IoFrame candidate = corpus_test::parse_capture_frame(cf);

        if (stage == Stage::SEEKING_FIRST) {
          auto disp = decisions::classify_exchange_first_response(origin, candidate);
          if (cf.has_classification) {
            EXPECT_EQ(static_cast<uint8_t>(disp), cf.classification)
                << "classification mismatch on frame " << static_cast<int>(i);
          }
          if (disp == decisions::ExchangeFirstResponseDisposition::REQUIRE_AUTH) {
            stage = Stage::SEEKING_FINAL;
          } else if (disp == decisions::ExchangeFirstResponseDisposition::COMPLETE_DIRECT) {
            stage = Stage::DONE;
          }
          // IGNORE_UNRELATED: stay in SEEKING_FIRST, exactly like the production wait loop.
        } else if (stage == Stage::SEEKING_FINAL) {
          auto disp = decisions::classify_exchange_final_response(origin, candidate);
          if (cf.has_classification) {
            EXPECT_EQ(static_cast<uint8_t>(disp), cf.classification)
                << "classification mismatch on frame " << static_cast<int>(i);
          }
          if (disp == decisions::ExchangeFinalResponseDisposition::ACCEPT) {
            stage = Stage::DONE;
          }
        } else {
          ASSERT_FALSE(cf.has_classification) << "frame " << static_cast<int>(i)
                                              << " has a classification expectation but the exchange already "
                                                 "completed (DONE) — no classifier applies to frames after that";
        }
      }
      break;
    }

    case corpus::ExchangeKind::PAIRING: {
      // classify_pairing_discovery_response() needs our own controller node ID, which the corpus
      // schema does not carry as a dedicated field — but every tx frame in a capture shares the
      // same src (this controller), so the first tx frame's src doubles as that ID, the same way
      // the DIRECT/STATUS_POLL case above derives `origin` from it. classify_pairing_key_challenge()
      // additionally needs the discovered device's node ID, which the schema does not carry today
      // (arrives with a real pairing capture's node_map in Step H2) — a classification expectation
      // on a non-discovery frame in a pairing capture fails loudly rather than silently passing.
      const corpus::CorpusFrame *origin_cf = nullptr;
      for (uint8_t i = 0; i < capture->frame_count; i++) {
        if (capture->frames[i].tx) {
          origin_cf = &capture->frames[i];
          break;
        }
      }
      ASSERT_NE(origin_cf, nullptr) << "pairing capture must have an origin tx frame to supply our own node ID";
      IoFrame origin = corpus_test::parse_capture_frame(*origin_cf);

      for (uint8_t i = 0; i < capture->frame_count; i++) {
        const corpus::CorpusFrame &cf = capture->frames[i];
        if (cf.tx || !cf.has_classification)
          continue;
        IoFrame candidate = corpus_test::parse_capture_frame(cf);
        if (cf.has_cmd && cf.cmd == CMD_DISCOVER_RESP) {
          const uint8_t actual =
              static_cast<uint8_t>(decisions::classify_pairing_discovery_response(candidate, origin.src));
          EXPECT_EQ(actual, cf.classification) << "discovery classification mismatch on frame " << static_cast<int>(i);
        } else {
          ADD_FAILURE() << "frame " << static_cast<int>(i) << " (cmd=0x" << std::hex << static_cast<int>(candidate.cmd)
                        << ") has a classification expectation but no classifier wiring exists for it in a pairing "
                           "capture (only CMD_DISCOVER_RESP is wired)";
        }
      }
      break;
    }

    case corpus::ExchangeKind::ONEWAY:
    case corpus::ExchangeKind::NONE:
    default:
      for (uint8_t i = 0; i < capture->frame_count; i++) {
        ASSERT_FALSE(capture->frames[i].has_classification)
            << "frame " << static_cast<int>(i) << " has a classification expectation but capture kind "
            << static_cast<int>(capture->kind) << " has no classifier wiring";
      }
      break;
  }
}

INSTANTIATE_TEST_SUITE_P(CorpusClassification, CorpusClassification, ::testing::ValuesIn(exchange_captures()),
                         corpus_test::capture_name_generator);
