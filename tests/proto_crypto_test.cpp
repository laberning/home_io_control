#include "proto_crypto.h"
#include "proto_commands.h"

#include "test_helpers.h"

#include <esp_random.h>

#include <array>
#include <set>

using namespace esphome::home_io_control;

// ============================================================================
// ProtoCrypto test suite
// ============================================================================
// Tests for HMAC computation, key encryption during pairing, and challenge-response.
// Covers the cryptographic proofs used for authenticated commands.

// ========================================================================================
// aes128_encrypt() / aes128_decrypt() — independent known-answer vectors
// ========================================================================================
// Plaintext, key, and ciphertext below are copied verbatim from published NIST test
// vectors — NOT generated from this codebase — so a match is evidence the primitive is
// correct, not merely self-consistent. The corpus KAT vectors elsewhere (crypto_kat.yaml,
// DISABLED_PrintCryptoKatVectors) pin the C++/Python ports against each other; these pin
// the primitive itself against an external, independently-computed answer.

TEST(ProtoCrypto, Aes128EncryptMatchesFips197AppendixBVector) {
  // FIPS-197, Appendix B, "Cipher Example".
  const uint8_t key[AES_KEY_SIZE] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                     0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
  const uint8_t plaintext[AES_BLOCK_SIZE] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                             0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  const uint8_t expected_ciphertext[AES_BLOCK_SIZE] = {0x69, 0xC4, 0xE0, 0xD8, 0x6A, 0x7B, 0x04, 0x30,
                                                       0xD8, 0xCD, 0xB7, 0x80, 0x70, 0xB4, 0xC5, 0x5A};

  uint8_t ciphertext[AES_BLOCK_SIZE] = {0};
  ASSERT_TRUE(crypto::aes128_encrypt(plaintext, key, ciphertext));
  EXPECT_EQ(0, memcmp(ciphertext, expected_ciphertext, AES_BLOCK_SIZE))
      << "AES-128 encrypt must match the published FIPS-197 Appendix B vector";

  uint8_t decrypted[AES_BLOCK_SIZE] = {0};
  ASSERT_TRUE(crypto::aes128_decrypt(expected_ciphertext, key, decrypted));
  EXPECT_EQ(0, memcmp(decrypted, plaintext, AES_BLOCK_SIZE))
      << "AES-128 decrypt must recover the published vector's plaintext";
}

TEST(ProtoCrypto, Aes128EncryptMatchesNistSp80038aVector) {
  // NIST SP 800-38A, Appendix F.1.1, "ECB-AES128.Encrypt", block #1.
  const uint8_t key[AES_KEY_SIZE] = {0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
                                     0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C};
  const uint8_t plaintext[AES_BLOCK_SIZE] = {0x6B, 0xC1, 0xBE, 0xE2, 0x2E, 0x40, 0x9F, 0x96,
                                             0xE9, 0x3D, 0x7E, 0x11, 0x73, 0x93, 0x17, 0x2A};
  const uint8_t expected_ciphertext[AES_BLOCK_SIZE] = {0x3A, 0xD7, 0x7B, 0xB4, 0x0D, 0x7A, 0x36, 0x60,
                                                       0xA8, 0x9E, 0xCA, 0xF3, 0x24, 0x66, 0xEF, 0x97};

  uint8_t ciphertext[AES_BLOCK_SIZE] = {0};
  ASSERT_TRUE(crypto::aes128_encrypt(plaintext, key, ciphertext));
  EXPECT_EQ(0, memcmp(ciphertext, expected_ciphertext, AES_BLOCK_SIZE))
      << "AES-128 encrypt must match the published NIST SP 800-38A vector";

  uint8_t decrypted[AES_BLOCK_SIZE] = {0};
  ASSERT_TRUE(crypto::aes128_decrypt(expected_ciphertext, key, decrypted));
  EXPECT_EQ(0, memcmp(decrypted, plaintext, AES_BLOCK_SIZE))
      << "AES-128 decrypt must recover the published vector's plaintext";
}

// ========================================================================================
// Challenge-response HMAC verification
// ========================================================================================

TEST(ProtoCrypto, ChallengeResponseHmac) {
  IoFrame origin = test::make_execute(0);
  IoFrame auth_resp{};
  ASSERT_TRUE(
      create_challenge_resp(auth_resp, test::DST_ID, test::OWN_ID, test::TEST_CHALLENGE, origin, test::TEST_SYSTEM_KEY))
      << "challenge response should be created with valid HMAC";
  EXPECT_EQ(auth_resp.cmd, CMD_CHALLENGE_RESP) << "command should be CMD_CHALLENGE_RESP (0x3D)";
  EXPECT_EQ(auth_resp.data_len, HMAC_SIZE) << "challenge response payload should be 6 bytes (HMAC)";

  uint8_t transcript[FRAME_MAX_SIZE] = {0};
  transcript[0] = origin.cmd;
  std::memcpy(transcript + 1, origin.data, origin.data_len);
  EXPECT_TRUE(
      crypto::verify_hmac(transcript, origin.data_len + 1, auth_resp.data, test::TEST_CHALLENGE, test::TEST_SYSTEM_KEY))
      << "valid HMAC should verify against origin transcript";

  // Flip each byte position independently, not just byte 0 — verify_hmac() is meant to be
  // load-bearing across all 6 bytes (proto_crypto.cpp's constant-time XOR/OR loop), and a
  // mutation that narrowed the comparison to e.g. `hmac[0] == expected[0]` would still pass a
  // byte-0-only tamper test.
  for (uint8_t i = 0; i < HMAC_SIZE; i++) {
    uint8_t tampered[HMAC_SIZE] = {0};
    std::memcpy(tampered, auth_resp.data, HMAC_SIZE);
    tampered[i] ^= 0x01;
    EXPECT_FALSE(
        crypto::verify_hmac(transcript, origin.data_len + 1, tampered, test::TEST_CHALLENGE, test::TEST_SYSTEM_KEY))
        << "a tampered HMAC byte at position " << static_cast<int>(i) << " should fail verification";
  }
}

// ========================================================================================
// crypt_key() / construct_iv() — independent known-answer vectors
// ========================================================================================
// Two real over-the-air key-transfer exchanges with known plaintext keys, posted by the
// iown-homecontrol reverse-engineering project. Independent of this codebase, so a match here
// is evidence crypt_key()'s IV construction is correct, not merely self-consistent: our other
// crypto tests below (CryptKeyRoundTrip, KeyTransferRoundTrip,
// RecoverSystemKeyFromTransferRoundTrip) all encrypt and decrypt with our own code and check
// mutual agreement, which can't catch a bug where both directions share the same wrong
// assumption — only a known answer from an outside source can.

TEST(ProtoCrypto, CryptKeyMatchesDocumentedIownHomecontrolPushCapture) {
  // Controller-initiated ("push") key transfer: a CMD_KEY_INIT (0x31) request, a 6-byte
  // challenge from the device, and the CMD_KEY_TRANSFER (0x32) reply masking a known stack key.
  const uint8_t iv_data[1] = {CMD_KEY_INIT};
  const uint8_t challenge[HMAC_SIZE] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
  const uint8_t wire_ciphertext[AES_KEY_SIZE] = {0x10, 0x2E, 0x49, 0xA1, 0x6D, 0x3B, 0x69, 0x72,
                                                 0x6F, 0x31, 0x92, 0xCF, 0x17, 0x53, 0x4A, 0xD9};
  const uint8_t expected_key[AES_KEY_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                              0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};

  uint8_t recovered[AES_KEY_SIZE] = {0};
  ASSERT_TRUE(crypto::crypt_key(iv_data, sizeof(iv_data), challenge, wire_ciphertext, recovered));
  EXPECT_EQ(0, memcmp(recovered, expected_key, AES_KEY_SIZE))
      << "crypt_key() must recover the documented Push-flow stack key from the captured wire ciphertext";
}

TEST(ProtoCrypto, CryptKeyMatchesDocumentedIownHomecontrolPullCapture) {
  // Device-initiated ("pull") key transfer: a CMD_LAUNCH_KEY_TRANSFER (0x38) request whose own
  // payload carries the challenge, and the CMD_KEY_TRANSFER (0x32) reply masking a known device
  // key using an IV built from the full 7-byte request (command byte + 6-byte challenge).
  const uint8_t iv_data[7] = {CMD_LAUNCH_KEY_TRANSFER, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
  const uint8_t challenge[HMAC_SIZE] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
  const uint8_t wire_ciphertext[AES_KEY_SIZE] = {0xEA, 0x42, 0x5A, 0x7A, 0x18, 0x28, 0x85, 0xD4,
                                                 0xEA, 0xEE, 0xFD, 0x41, 0x6D, 0x62, 0x5E, 0x01};
  const uint8_t expected_key[AES_KEY_SIZE] = {0xAB, 0xCD, 0xEF, 0x01, 0x02, 0x03, 0x04, 0x05,
                                              0x06, 0x07, 0x08, 0x09, 0x10, 0x11, 0x12, 0x13};

  uint8_t recovered[AES_KEY_SIZE] = {0};
  ASSERT_TRUE(crypto::crypt_key(iv_data, sizeof(iv_data), challenge, wire_ciphertext, recovered));
  EXPECT_EQ(0, memcmp(recovered, expected_key, AES_KEY_SIZE))
      << "crypt_key() must recover the documented Pull-flow device key from the captured wire ciphertext";
}

TEST(ProtoCrypto, CryptKeyRoundTrip) {
  uint8_t encrypted[AES_KEY_SIZE] = {0};
  uint8_t decrypted[AES_KEY_SIZE] = {0};
  uint8_t iv_seed[1] = {CMD_KEY_INIT};

  EXPECT_TRUE(crypto::crypt_key(iv_seed, sizeof(iv_seed), test::TEST_CHALLENGE, test::TEST_SYSTEM_KEY, encrypted))
      << "key encryption with system key should succeed";
  EXPECT_TRUE(crypto::crypt_key(iv_seed, sizeof(iv_seed), test::TEST_CHALLENGE, encrypted, decrypted))
      << "key decryption with same challenge should recover plaintext";
  EXPECT_EQ(0, memcmp(decrypted, test::TEST_SYSTEM_KEY, AES_KEY_SIZE))
      << "decrypted key must equal original system key";
}

// ========================================================================================
// Key transfer encryption round-trip
// ========================================================================================

TEST(ProtoCrypto, KeyTransferRoundTrip) {
  IoFrame key_init = test::make_key_init();
  IoFrame key_transfer{};
  ASSERT_TRUE(create_key_transfer(key_transfer, key_init, test::DST_ID, test::OWN_ID, test::TEST_SYSTEM_KEY,
                                  test::TEST_CHALLENGE))
      << "key transfer should encrypt system key with transfer key";

  EXPECT_EQ(key_transfer.cmd, CMD_KEY_TRANSFER) << "command should be CMD_KEY_TRANSFER (0x32)";
  EXPECT_EQ(key_transfer.data_len, AES_KEY_SIZE) << "key transfer payload must be 16 bytes";

  uint8_t decrypted[AES_KEY_SIZE] = {0};
  uint8_t iv_seed[1] = {key_init.cmd};
  EXPECT_TRUE(crypto::crypt_key(iv_seed, sizeof(iv_seed), test::TEST_CHALLENGE, key_transfer.data, decrypted))
      << "key transfer payload should decrypt using same IV seed";
  EXPECT_EQ(0, memcmp(decrypted, test::TEST_SYSTEM_KEY, AES_KEY_SIZE))
      << "decrypted key transfer must equal original system key";
}

// ========================================================================================
// recover_system_key_from_transfer() — key-extraction responder's decode path
// ========================================================================================
// The single most important test in the key-extraction feature (see
// analysis/key_extraction_feature_plan.md §3, §9): pins down the IV-`data` convention
// (`{CMD_KEY_INIT}, len 1`) so a future refactor can't quietly break decryption.

TEST(ProtoCrypto, RecoverSystemKeyFromTransferRoundTrip) {
  IoFrame key_init = test::make_key_init();
  IoFrame key_transfer{};
  ASSERT_TRUE(create_key_transfer(key_transfer, key_init, test::DST_ID, test::OWN_ID, test::TEST_SYSTEM_KEY,
                                  test::TEST_CHALLENGE))
      << "key transfer should encrypt system key with transfer key";

  uint8_t recovered[AES_KEY_SIZE] = {0};
  ASSERT_TRUE(recover_system_key_from_transfer(key_transfer.data, test::TEST_CHALLENGE, recovered))
      << "recover_system_key_from_transfer should succeed";
  EXPECT_EQ(0, memcmp(recovered, test::TEST_SYSTEM_KEY, AES_KEY_SIZE))
      << "recovered key must equal the original system key";
}

TEST(ProtoCrypto, RecoverSystemKeyFromTransferWrongChallengeFails) {
  IoFrame key_init = test::make_key_init();
  IoFrame key_transfer{};
  ASSERT_TRUE(create_key_transfer(key_transfer, key_init, test::DST_ID, test::OWN_ID, test::TEST_SYSTEM_KEY,
                                  test::TEST_CHALLENGE));

  const uint8_t wrong_challenge[HMAC_SIZE] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t recovered[AES_KEY_SIZE] = {0};
  ASSERT_TRUE(recover_system_key_from_transfer(key_transfer.data, wrong_challenge, recovered));
  EXPECT_NE(0, memcmp(recovered, test::TEST_SYSTEM_KEY, AES_KEY_SIZE))
      << "decrypting with the wrong challenge must not silently recover the correct key";
}

TEST(ProtoCrypto, RecoverSystemKeyFromTransferWrongIvDataFails) {
  IoFrame key_init = test::make_key_init();
  IoFrame key_transfer{};
  ASSERT_TRUE(create_key_transfer(key_transfer, key_init, test::DST_ID, test::OWN_ID, test::TEST_SYSTEM_KEY,
                                  test::TEST_CHALLENGE));

  // Decrypt using the 0x32 frame's own command byte instead of the preceding CMD_KEY_INIT (0x31)
  // — the exact mistake the IV-`data` convention exists to prevent (see proto_commands.h).
  uint8_t wrong_iv_seed[1] = {key_transfer.cmd};
  uint8_t recovered[AES_KEY_SIZE] = {0};
  ASSERT_TRUE(
      crypto::crypt_key(wrong_iv_seed, sizeof(wrong_iv_seed), test::TEST_CHALLENGE, key_transfer.data, recovered));
  EXPECT_NE(0, memcmp(recovered, test::TEST_SYSTEM_KEY, AES_KEY_SIZE))
      << "decrypting with the wrong IV-data convention must not silently recover the correct key";
}

// ========================================================================================
// create_1w_hmac() / construct_iv_1w_sequence() — the 1W frame authenticator
// ========================================================================================
// Two independent published vectors from reference/iown-homecontrol's docs/linklayer.md
// (CC0-1.0), both captured in tests/corpus/captures/reference_1w_vectors/. They pin different
// halves of the primitive: the add-controller vector pins the final MAC under a known key, and
// the execute vector pins the IV construction itself (checksum bytes at 8-9, sequence at 10-11)
// independently of any key — its own MAC is a documented placeholder and is deliberately not
// asserted here.

TEST(ProtoCrypto, ConstructIv1wSequenceMatchesPublishedExecuteVector) {
  // oneway_execute_iv_vector.yaml: payload 00 01 43 D2 00 00 00 (7 bytes, padded to 8 with 0x55),
  // sequence 0x0599. The document states the resulting IV explicitly, so this asserts our IV
  // layout against a source outside this codebase rather than against our own expectations.
  const uint8_t span[] = {0x00, 0x01, 0x43, 0xD2, 0x00, 0x00, 0x00};
  const uint8_t expected_iv[IV_SIZE] = {0x00, 0x01, 0x43, 0xD2, 0x00, 0x00, 0x00, 0x55,
                                        0x05, 0x00, 0x05, 0x99, 0x55, 0x55, 0x55, 0x55};

  uint8_t iv[IV_SIZE] = {0};
  crypto::construct_iv_1w_sequence(span, sizeof(span), 0x0599, iv);
  EXPECT_EQ(0, memcmp(iv, expected_iv, IV_SIZE))
      << "construct_iv_1w_sequence() must reproduce the published execute-frame IV byte-for-byte";
}

TEST(ProtoCrypto, Create1wHmacMatchesPublishedAddControllerVector) {
  // oneway_add_controller_kat.yaml: span is cmd(0x30) followed by the 16 encrypted-key bytes —
  // 17 bytes, NOT the whole payload. The span is command-specific and does not generalise; see
  // Create1wHmacRejectsWrongSpan below for what a wrong choice looks like.
  const uint8_t span[] = {0x30, 0x7E, 0x60, 0x49, 0x1F, 0x97, 0x6A, 0xDF, 0x65,
                          0x3D, 0xB0, 0xED, 0x78, 0x5E, 0x49, 0xA2, 0x01};
  const uint8_t controller_key[AES_KEY_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                                0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
  const uint8_t expected_hmac[HMAC_SIZE] = {0x19, 0xE8, 0x1E, 0xC4, 0x3D, 0x5E};

  uint8_t hmac[HMAC_SIZE] = {0};
  ASSERT_TRUE(crypto::create_1w_hmac(span, sizeof(span), 0x1234, controller_key, hmac));
  EXPECT_EQ(0, memcmp(hmac, expected_hmac, HMAC_SIZE))
      << "create_1w_hmac() must reproduce the published add-controller MAC 19E81EC43D5E";
}

TEST(ProtoCrypto, Create1wHmacRejectsWrongSpan) {
  // The failure mode create_1w_hmac()'s @warning exists for: a wrong span still produces a
  // well-formed 6-byte MAC that looks entirely plausible, and only a known-answer vector catches
  // it. Here the span omits the leading command byte — the single most likely mistake — and the
  // result must differ from the published MAC.
  const uint8_t enc_key_only[] = {0x7E, 0x60, 0x49, 0x1F, 0x97, 0x6A, 0xDF, 0x65,
                                  0x3D, 0xB0, 0xED, 0x78, 0x5E, 0x49, 0xA2, 0x01};
  const uint8_t controller_key[AES_KEY_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                                0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
  const uint8_t published_hmac[HMAC_SIZE] = {0x19, 0xE8, 0x1E, 0xC4, 0x3D, 0x5E};

  uint8_t hmac[HMAC_SIZE] = {0};
  ASSERT_TRUE(crypto::create_1w_hmac(enc_key_only, sizeof(enc_key_only), 0x1234, controller_key, hmac));
  EXPECT_NE(0, memcmp(hmac, published_hmac, HMAC_SIZE))
      << "dropping the command byte from the span must not still yield the published MAC";
}

// ========================================================================================
// crypt_1w_key() / construct_iv_1w_node() — the 1W add-controller key wrap
// ========================================================================================
// Published worked example from reference/iown-homecontrol's docs/linklayer.md (CC0-1.0),
// also captured verbatim in tests/corpus/captures/reference_1w_vectors/oneway_add_controller_kat.yaml:
// node ABCDEF, controller key 0102...1516, sequence 0x1234. Independent of this codebase, so a
// match here is evidence crypt_1w_key()'s IV construction is correct, not merely self-consistent
// — the same role the CryptKeyMatchesDocumentedIownHomecontrol* vectors play for the 2W sibling.
// This is the primary, hardware-free KAT for Phase 3A Step 1.

TEST(ProtoCrypto, ConstructIv1wNodeMatchesPublishedVector) {
  const uint8_t node[NODE_ID_SIZE] = {0xAB, 0xCD, 0xEF};
  const uint8_t expected_iv[IV_SIZE] = {0xAB, 0xCD, 0xEF, 0xAB, 0xCD, 0xEF, 0xAB, 0xCD,
                                        0xEF, 0xAB, 0xCD, 0xEF, 0xAB, 0xCD, 0xEF, 0xAB};

  uint8_t iv[IV_SIZE] = {0};
  crypto::construct_iv_1w_node(node, iv);
  EXPECT_EQ(0, memcmp(iv, expected_iv, IV_SIZE))
      << "construct_iv_1w_node() must reproduce ABCDEFABCDEFABCDEFABCDEFABCDEFAB for node ABCDEF";
}

TEST(ProtoCrypto, Crypt1wKeyMatchesPublishedAddControllerVector) {
  const uint8_t node[NODE_ID_SIZE] = {0xAB, 0xCD, 0xEF};
  const uint8_t plaintext_key[AES_KEY_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                               0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
  const uint8_t expected_ciphertext[AES_KEY_SIZE] = {0x7E, 0x60, 0x49, 0x1F, 0x97, 0x6A, 0xDF, 0x65,
                                                     0x3D, 0xB0, 0xED, 0x78, 0x5E, 0x49, 0xA2, 0x01};

  uint8_t encrypted[AES_KEY_SIZE] = {0};
  ASSERT_TRUE(crypto::crypt_1w_key(node, plaintext_key, encrypted));
  EXPECT_EQ(0, memcmp(encrypted, expected_ciphertext, AES_KEY_SIZE))
      << "crypt_1w_key() must encrypt the published controller key to the published ciphertext";

  uint8_t decrypted[AES_KEY_SIZE] = {0};
  ASSERT_TRUE(crypto::crypt_1w_key(node, expected_ciphertext, decrypted));
  EXPECT_EQ(0, memcmp(decrypted, plaintext_key, AES_KEY_SIZE))
      << "crypt_1w_key() must decrypt the published ciphertext back to the published key";
}

TEST(ProtoCrypto, Crypt1wKeyRoundTripsForVariousNodesAndKeys) {
  struct Case {
    uint8_t node[NODE_ID_SIZE];
    uint8_t key[AES_KEY_SIZE];
  };
  const Case cases[] = {
      {{0x00, 0x00, 0x00}, {0}},
      {{0xFF, 0xFF, 0xFF},
       {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}},
      {{0x11, 0x22, 0x33},
       {0xDE, 0xCA, 0xFC, 0x0F, 0xFE, 0xE0, 0xFF, 0x1C, 0xE0, 0xFF, 0xEE, 0xBA, 0xBE, 0x01, 0x02, 0x03}},
      {{0x77, 0x1A, 0x5C},
       {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}},
  };

  for (const auto &c : cases) {
    uint8_t encrypted[AES_KEY_SIZE] = {0};
    uint8_t decrypted[AES_KEY_SIZE] = {0};
    ASSERT_TRUE(crypto::crypt_1w_key(c.node, c.key, encrypted));
    ASSERT_TRUE(crypto::crypt_1w_key(c.node, encrypted, decrypted));
    EXPECT_EQ(0, memcmp(decrypted, c.key, AES_KEY_SIZE))
        << "crypt_1w_key() round trip must recover the original key for node " << static_cast<int>(c.node[0]) << "."
        << static_cast<int>(c.node[1]) << "." << static_cast<int>(c.node[2]);
  }
}

TEST(ProtoCrypto, Crypt1wKeyDoesNotShareOutputWithCryptKeyOnSameBytes) {
  // Guards against a refactor accidentally collapsing the two primitives: feeding crypt_key()
  // the node bytes as its `data`/`challenge` inputs must not reproduce crypt_1w_key()'s output
  // — the two use different, non-interchangeable IV derivations by design.
  const uint8_t node[NODE_ID_SIZE] = {0xAB, 0xCD, 0xEF};
  const uint8_t plaintext_key[AES_KEY_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                               0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};

  uint8_t via_1w[AES_KEY_SIZE] = {0};
  ASSERT_TRUE(crypto::crypt_1w_key(node, plaintext_key, via_1w));

  uint8_t challenge_from_node[HMAC_SIZE] = {node[0], node[1], node[2], node[0], node[1], node[2]};
  uint8_t via_2w[AES_KEY_SIZE] = {0};
  ASSERT_TRUE(crypto::crypt_key(node, NODE_ID_SIZE, challenge_from_node, plaintext_key, via_2w));

  EXPECT_NE(0, memcmp(via_1w, via_2w, AES_KEY_SIZE))
      << "crypt_1w_key() and crypt_key() must not be interchangeable even when fed the same bytes";
}

// ========================================================================================
// generate_challenge() — output shape, not entropy quality
// ========================================================================================
// generate_challenge() feeds the entire authentication scheme: the challenge-response protocol
// is only as fresh as the challenge is unpredictable per exchange. This test can't judge the
// ESP32 hardware TRNG's entropy quality on host (esp_random() falls back to a plain LCG here,
// see tests/include/esp_random.h), but it can catch the class of glue-code bug that would
// survive even good entropy underneath: e.g. a loop bound off by one that leaves trailing bytes
// always zero, or a copy that only ever touches byte 0.

TEST(ProtoCrypto, GenerateChallengeProducesDistinctNonDegenerateOutput) {
  test_rng::reset();  // Use the deterministic LCG fallback, not a leftover scripted queue.

  constexpr int kSamples = 20;
  std::set<std::array<uint8_t, HMAC_SIZE>> seen;
  std::array<bool, HMAC_SIZE> byte_varied{};

  uint8_t previous[HMAC_SIZE] = {0};
  for (int sample = 0; sample < kSamples; sample++) {
    uint8_t challenge[HMAC_SIZE] = {0};
    crypto::generate_challenge(challenge);

    uint8_t all_zero = 0x00, all_ff = 0xFF;
    for (uint8_t i = 0; i < HMAC_SIZE; i++) {
      all_zero |= challenge[i];
      all_ff &= challenge[i];
      if (sample > 0 && challenge[i] != previous[i])
        byte_varied[i] = true;
    }
    EXPECT_NE(all_zero, 0x00) << "challenge #" << sample << " must not be all-zero";
    EXPECT_NE(all_ff, 0xFF) << "challenge #" << sample << " must not be all-0xFF";

    std::array<uint8_t, HMAC_SIZE> as_array;
    std::memcpy(as_array.data(), challenge, HMAC_SIZE);
    EXPECT_TRUE(seen.insert(as_array).second) << "challenge #" << sample << " repeats an earlier sample";

    std::memcpy(previous, challenge, HMAC_SIZE);
  }

  // Every byte position must change at least once across the run — catches a mutation that
  // only ever writes a subset of the 6 bytes (the whole-value distinctness check above would
  // miss that as long as some other byte kept changing).
  for (uint8_t i = 0; i < HMAC_SIZE; i++) {
    EXPECT_TRUE(byte_varied[i]) << "byte position " << static_cast<int>(i) << " never changed across " << kSamples
                                << " samples";
  }
}
