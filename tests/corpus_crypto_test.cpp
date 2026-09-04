/// @file corpus_crypto_test.cpp
/// @brief Recorded crypto ground truth from `key: corpus` golden-frame captures.
///
/// Runs over every capture whose `key` promise is `corpus` and whose exchange kind is
/// `authenticated_command`: rebuilds the HMAC transcript exactly as `create_challenge_resp()`
/// does (proto_commands.cpp) and asserts the recomputed HMAC, under the public corpus key
/// (test_helpers.h :: TEST_SYSTEM_KEY), matches the captured `0x3D` bytes byte-for-byte.
/// This pins IV construction, checksum-byte derivation, and truncation against a real
/// exchange shape instead of only synthetic vectors (tests/proto_crypto_test.cpp already
/// covers the synthetic case).
///
/// Captures that do not match the expected shape cause a loud failure (a corpus bug), not a
/// silent pass; captures with no matching content (e.g. no `0x32` key-transfer frame yet) are
/// skipped with a counted GTEST_SKIP(), never a vacuous pass.

#include "corpus_generated.h"
#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_crypto.h"
#include "proto_frame.h"

#include "corpus_test_helpers.h"
#include "test_helpers.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace esphome::home_io_control;

namespace {

/// Captures that promise `key: corpus` and describe an authenticated command exchange —
/// the only shape corpus_crypto_test.cpp knows how to replay today.
std::vector<const corpus::CorpusCapture *> authenticated_corpus_captures() {
  return corpus_test::captures_where([](const corpus::CorpusCapture *cap) {
    return cap->key == corpus::KeyMode::CORPUS && cap->has_exchange &&
           cap->kind == corpus::ExchangeKind::AUTHENTICATED_COMMAND;
  });
}

/// Captures that promise `key: corpus` and describe a pairing exchange — the shape the
/// key-transfer (0x32) sub-test below replays.
std::vector<const corpus::CorpusCapture *> pairing_corpus_captures() {
  return corpus_test::captures_where([](const corpus::CorpusCapture *cap) {
    return cap->key == corpus::KeyMode::CORPUS && cap->has_exchange && cap->kind == corpus::ExchangeKind::PAIRING;
  });
}

}  // namespace

class CorpusCryptoReplay : public ::testing::TestWithParam<const corpus::CorpusCapture *> {};

TEST_P(CorpusCryptoReplay, AuthenticatedCommandHmacMatchesCapture) {
  const corpus::CorpusCapture *capture = GetParam();
  SCOPED_TRACE(::testing::Message() << "capture=" << capture->id);

  // Locate the origin (first tx frame — a required schema field, not an expectation), then the
  // 0x3C challenge and 0x3D response by their *parsed* cmd byte rather than expect.cmd: cmd
  // expectations are optional per the corpus schema, so a valid capture that omits them must
  // still be locatable here.
  const corpus::CorpusFrame *origin_cf = nullptr;
  const corpus::CorpusFrame *challenge_cf = nullptr;
  const corpus::CorpusFrame *response_cf = nullptr;
  for (uint8_t i = 0; i < capture->frame_count; i++) {
    const corpus::CorpusFrame &cf = capture->frames[i];
    if (origin_cf == nullptr && cf.tx) {
      origin_cf = &cf;
      continue;
    }
    const IoFrame parsed = corpus_test::parse_capture_frame(cf);
    if (challenge_cf == nullptr && !cf.tx && parsed.cmd == CMD_CHALLENGE_REQ) {
      challenge_cf = &cf;
    } else if (response_cf == nullptr && cf.tx && parsed.cmd == CMD_CHALLENGE_RESP) {
      response_cf = &cf;
    }
  }
  ASSERT_NE(origin_cf, nullptr) << "authenticated_command capture must have an origin tx frame";

  // A capture whose own recorded outcome is timeout/failure legitimately has no challenge —
  // that's the shape of "the device never responded" (e.g. powered off), not a corpus bug. Only
  // a claimed-success exchange is required to have completed the real crypto handshake.
  if (capture->outcome != corpus::ExchangeOutcome::SUCCESS && (challenge_cf == nullptr || response_cf == nullptr)) {
    GTEST_SKIP() << "capture outcome is not success and has no completed 0x3C/0x3D pair — nothing to verify";
  }
  ASSERT_NE(challenge_cf, nullptr) << "authenticated_command capture must have a 0x3C challenge frame";
  ASSERT_NE(response_cf, nullptr) << "authenticated_command capture must have a 0x3D response frame";

  IoFrame origin = corpus_test::parse_capture_frame(*origin_cf);
  IoFrame challenge = corpus_test::parse_capture_frame(*challenge_cf);
  IoFrame response = corpus_test::parse_capture_frame(*response_cf);
  ASSERT_EQ(challenge.data_len, HMAC_SIZE) << "0x3C challenge payload must be exactly HMAC_SIZE bytes";
  ASSERT_EQ(response.data_len, HMAC_SIZE) << "0x3D response payload must be exactly HMAC_SIZE bytes";

  // Rebuild the transcript exactly as create_challenge_resp() does (proto_commands.cpp):
  // [origin.cmd, origin.data...], authenticated against the captured challenge.
  uint8_t transcript[FRAME_MAX_SIZE] = {0};
  transcript[0] = origin.cmd;
  std::memcpy(transcript + 1, origin.data, origin.data_len);
  const uint8_t transcript_len = static_cast<uint8_t>(origin.data_len + 1);

  uint8_t computed_hmac[HMAC_SIZE] = {0};
  ASSERT_TRUE(crypto::create_hmac(transcript, transcript_len, challenge.data, test::TEST_SYSTEM_KEY, computed_hmac));
  EXPECT_EQ(std::memcmp(computed_hmac, response.data, HMAC_SIZE), 0)
      << "recomputed HMAC does not match captured 0x3D bytes under the corpus key";

  const bool hmac_actually_valid =
      crypto::verify_hmac(transcript, transcript_len, response.data, challenge.data, test::TEST_SYSTEM_KEY);
  if (response_cf->has_hmac_valid) {
    // `hmac_valid` is a per-frame expectation (validate.py, build.py); wiring it here gives
    // `hmac_valid: false` a real meaning for a future deliberately-tampered fixture instead of
    // being an accepted-but-never-checked field.
    EXPECT_EQ(hmac_actually_valid, response_cf->hmac_valid)
        << "verify_hmac result does not match the capture's hmac_valid expectation";
  } else {
    EXPECT_TRUE(hmac_actually_valid) << "verify_hmac must accept the captured HMAC";
  }

  // Flip each byte position independently, not just byte 0 — see the identical rationale in
  // tests/proto_crypto_test.cpp's ChallengeResponseHmac.
  for (uint8_t i = 0; i < HMAC_SIZE; i++) {
    uint8_t tampered[HMAC_SIZE];
    std::memcpy(tampered, response.data, HMAC_SIZE);
    tampered[i] ^= 0x01;
    EXPECT_FALSE(crypto::verify_hmac(transcript, transcript_len, tampered, challenge.data, test::TEST_SYSTEM_KEY))
        << "a bit-flip in captured HMAC byte " << static_cast<int>(i) << " must be rejected";
  }
}

/// Key-transfer (0x32) sub-test: locates the (0x31 key-init, 0x3C
/// challenge, 0x32 key-transfer) triple in a pairing capture, exactly as
/// create_key_transfer() (proto_commands.cpp) builds it — IV data is the key-init frame's
/// single cmd byte, not a full transcript — and asserts crypto::crypt_key() recovers the corpus
/// system key. Mirrors the Python --rekey pipeline's own verification step
/// (scripts/corpus/ingest.py :: rekey_key_transfer_frames) against the real C++ implementation,
/// so a divergence between the two crypto ports fails here, not silently in production.
class CorpusCryptoKeyTransfer : public ::testing::TestWithParam<const corpus::CorpusCapture *> {};

TEST_P(CorpusCryptoKeyTransfer, KeyTransferDecryptsToCorpusKey) {
  const corpus::CorpusCapture *capture = GetParam();
  SCOPED_TRACE(::testing::Message() << "capture=" << capture->id);

  const corpus::CorpusFrame *key_init_cf = nullptr;
  const corpus::CorpusFrame *challenge_cf = nullptr;
  const corpus::CorpusFrame *transfer_cf = nullptr;
  for (uint8_t i = 0; i < capture->frame_count; i++) {
    const corpus::CorpusFrame &cf = capture->frames[i];
    const IoFrame parsed = corpus_test::parse_capture_frame(cf);
    if (key_init_cf == nullptr && cf.tx && parsed.cmd == CMD_KEY_INIT) {
      key_init_cf = &cf;
    } else if (key_init_cf != nullptr && challenge_cf == nullptr && !cf.tx && parsed.cmd == CMD_CHALLENGE_REQ) {
      challenge_cf = &cf;
    } else if (challenge_cf != nullptr && transfer_cf == nullptr && cf.tx && parsed.cmd == CMD_KEY_TRANSFER) {
      transfer_cf = &cf;
    }
  }
  if (key_init_cf == nullptr || challenge_cf == nullptr || transfer_cf == nullptr) {
    GTEST_SKIP() << "no complete 0x31/0x3C/0x32 triple in this pairing capture (e.g. a capture that failed "
                    "before ever reaching key-transfer)";
  }

  const IoFrame key_init = corpus_test::parse_capture_frame(*key_init_cf);
  const IoFrame challenge = corpus_test::parse_capture_frame(*challenge_cf);
  const IoFrame transfer = corpus_test::parse_capture_frame(*transfer_cf);
  ASSERT_EQ(challenge.data_len, HMAC_SIZE) << "0x3C challenge payload must be exactly HMAC_SIZE bytes";
  ASSERT_EQ(transfer.data_len, AES_KEY_SIZE) << "0x32 key-transfer payload must be exactly AES_KEY_SIZE bytes";

  uint8_t recovered_key[AES_KEY_SIZE] = {0};
  ASSERT_TRUE(crypto::crypt_key(&key_init.cmd, 1, challenge.data, transfer.data, recovered_key));
  EXPECT_EQ(std::memcmp(recovered_key, test::TEST_SYSTEM_KEY, AES_KEY_SIZE), 0)
      << "key-transfer payload does not decrypt to the corpus key";
}

/// Self-authenticated frames (design: one frame carrying a whole challenge-response). A
/// CMD_DISCOVER_SPE_REQ (0x2A) payload is [6-byte challenge | 6-byte HMAC over the command byte
/// alone] — no 0x3C/0x3D round trip, which is what lets it be broadcast. Asserts the real C++
/// crypto reproduces the captured HMAC, mirroring scripts/corpus/validate.py's enforcement of the
/// same frames, so the two crypto ports cannot drift apart on this shape either. The rule itself
/// was established from real KLR200 bytes (velux_kux100_pairing_full.yaml, recomputed under that
/// installation's own key before re-keying) and is what create_discovery_request() builds.
class CorpusCryptoSelfAuthenticated : public ::testing::TestWithParam<const corpus::CorpusCapture *> {};

TEST_P(CorpusCryptoSelfAuthenticated, SelfAuthenticatedPayloadVerifiesUnderCorpusKey) {
  const corpus::CorpusCapture *capture = GetParam();
  SCOPED_TRACE(::testing::Message() << "capture=" << capture->id);

  uint8_t checked = 0;
  for (uint8_t i = 0; i < capture->frame_count; i++) {
    const IoFrame parsed = corpus_test::parse_capture_frame(capture->frames[i]);
    if (parsed.cmd != CMD_DISCOVER_SPE_REQ || parsed.data_len != HMAC_SIZE * 2)
      continue;
    const uint8_t *challenge = parsed.data;
    const uint8_t *hmac = parsed.data + HMAC_SIZE;

    uint8_t computed[HMAC_SIZE] = {0};
    ASSERT_TRUE(crypto::create_hmac(&parsed.cmd, 1, challenge, test::TEST_SYSTEM_KEY, computed));
    EXPECT_EQ(std::memcmp(computed, hmac, HMAC_SIZE), 0)
        << "frame " << static_cast<int>(i) << ": 0x2A HMAC does not match the corpus key over the command byte";
    checked++;
  }
  if (checked == 0)
    GTEST_SKIP() << "no self-authenticated frame in this capture";
}

/// Reproduces the KLR200 pairing capture's address-verification 0x3D literally, byte-for-byte,
/// under the corpus key -- the one claim in this feature's design that rests on hand-verified
/// crypto rather than a general rule. Kept separate from HubKeyExtraction's end-to-end replay
/// (tests/hub_key_extraction_test.cpp): that suite drives the real dispatch/handler code but with
/// a *scripted* challenge and key, so its 0x3D bytes are whatever those inputs produce, not the
/// capture's literal `F0 30 3C C7 78 EF` -- this is where the C++ port and the Python port
/// (scripts/corpus/validate.py's `key: corpus` enforcement) are pinned against the identical bytes,
/// so a divergence between the two crypto implementations fails here, not silently in production.
TEST(CorpusCryptoKat, ChallengeRespDeviceRoleReproducesKlr200AddressProof) {
  const corpus::CorpusCapture *capture = corpus_test::capture_by_id("velux_kux100_pairing_full");
  ASSERT_NE(capture, nullptr) << "capture not found -- was it renamed?";

  const corpus::CorpusFrame *address_resp_cf = nullptr;  // rx 0x37: our own preceding frame
  const corpus::CorpusFrame *challenge_cf = nullptr;     // tx 0x3C: the controller's challenge on it
  const corpus::CorpusFrame *response_cf = nullptr;      // rx 0x3D: the terminal address-proof
  for (uint8_t i = 0; i < capture->frame_count; i++) {
    const corpus::CorpusFrame &cf = capture->frames[i];
    const IoFrame parsed = corpus_test::parse_capture_frame(cf);
    if (address_resp_cf == nullptr && !cf.tx && parsed.cmd == CMD_ADDRESS_RESP) {
      address_resp_cf = &cf;
    } else if (challenge_cf == nullptr && cf.tx && parsed.cmd == CMD_CHALLENGE_REQ) {
      challenge_cf = &cf;
    } else if (response_cf == nullptr && !cf.tx && parsed.cmd == CMD_CHALLENGE_RESP) {
      response_cf = &cf;
    }
  }
  ASSERT_NE(address_resp_cf, nullptr) << "capture must have a device-originated 0x37";
  ASSERT_NE(challenge_cf, nullptr) << "capture must have a controller-issued 0x3C";
  ASSERT_NE(response_cf, nullptr) << "capture must have the device's terminal 0x3D";

  const IoFrame address_resp = corpus_test::parse_capture_frame(*address_resp_cf);
  const IoFrame challenge = corpus_test::parse_capture_frame(*challenge_cf);
  const IoFrame response = corpus_test::parse_capture_frame(*response_cf);
  ASSERT_EQ(challenge.data_len, HMAC_SIZE);
  ASSERT_EQ(response.data_len, HMAC_SIZE);

  IoFrame built{};
  // address_resp (our own preceding 0x37) is device-originated: its src is our (device) address,
  // its dst is the hub's -- create_challenge_resp_device_role()'s (dst, src) order is the other
  // way around (dst=hub, src=us), matching create_address_resp_device_role()'s own convention.
  ASSERT_TRUE(create_challenge_resp_device_role(built, /*dst=*/address_resp.dst, /*src=*/address_resp.src,
                                                challenge.data, address_resp, test::TEST_SYSTEM_KEY));
  EXPECT_EQ(std::memcmp(built.data, response.data, HMAC_SIZE), 0)
      << "create_challenge_resp_device_role() does not reproduce the captured 0x3D bytes";
}

std::vector<const corpus::CorpusCapture *> corpus_key_captures() {
  return corpus_test::captures_where(
      [](const corpus::CorpusCapture *cap) { return cap->key == corpus::KeyMode::CORPUS; });
}

INSTANTIATE_TEST_SUITE_P(CorpusCrypto, CorpusCryptoReplay, ::testing::ValuesIn(authenticated_corpus_captures()),
                         corpus_test::capture_name_generator);
INSTANTIATE_TEST_SUITE_P(CorpusCrypto, CorpusCryptoSelfAuthenticated, ::testing::ValuesIn(corpus_key_captures()),
                         corpus_test::capture_name_generator);
INSTANTIATE_TEST_SUITE_P(CorpusCrypto, CorpusCryptoKeyTransfer, ::testing::ValuesIn(pairing_corpus_captures()),
                         corpus_test::capture_name_generator);

/// Cross-language known-answer vectors, hardcoded here and in
/// scripts/corpus/tests/data/crypto_kat.yaml — both generated from this same C++
/// implementation via tests/corpus_bootstrap_dump_test.cpp :: DISABLED_PrintCryptoKatVectors.
/// The Python port (scripts/corpus/protolib.py, used by ingest.py --rekey and validate.py's
/// key:corpus enforcement) is asserted against the identical vectors in
/// scripts/corpus/tests/run_tests.py. A divergence between the two implementations fails a gate
/// on both sides; regenerate both files together if this ever needs to change.
TEST(CorpusCryptoKat, CppVectorsMatchPythonPort) {
  const uint8_t kat1_data[1] = {0x00};
  const uint8_t kat1_hmac[HMAC_SIZE] = {0x13, 0x68, 0x06, 0x37, 0xCD, 0xF3};
  const uint8_t kat2_data[8] = {0x00, 0x01, 0x43, 0x64, 0x00, 0x80, 0xD8, 0x06};
  const uint8_t kat2_hmac[HMAC_SIZE] = {0x25, 0xF8, 0xE7, 0x5F, 0xA2, 0x1E};
  const uint8_t kat3_data[14] = {0x20, 0x02, 0x03, 0x05, 0x04, 0x00, 0x08, 0xAF, 0xC0, 0xB8, 0x12, 0x1F, 0x73, 0xA2};
  const uint8_t kat3_hmac[HMAC_SIZE] = {0x83, 0xC3, 0xC2, 0x0D, 0xFF, 0xF8};

  uint8_t hmac[HMAC_SIZE] = {0};
  ASSERT_TRUE(crypto::create_hmac(kat1_data, sizeof(kat1_data), test::TEST_CHALLENGE, test::TEST_SYSTEM_KEY, hmac));
  EXPECT_EQ(std::memcmp(hmac, kat1_hmac, HMAC_SIZE), 0) << "kat1 mismatch";

  ASSERT_TRUE(crypto::create_hmac(kat2_data, sizeof(kat2_data), test::TEST_CHALLENGE, test::TEST_SYSTEM_KEY, hmac));
  EXPECT_EQ(std::memcmp(hmac, kat2_hmac, HMAC_SIZE), 0) << "kat2 mismatch";

  ASSERT_TRUE(crypto::create_hmac(kat3_data, sizeof(kat3_data), test::TEST_CHALLENGE, test::TEST_SYSTEM_KEY, hmac));
  EXPECT_EQ(std::memcmp(hmac, kat3_hmac, HMAC_SIZE), 0) << "kat3 mismatch";
}
