#include "proto_crypto.h"
#include "proto_commands.h"

#include "test_helpers.h"

using namespace esphome::home_io_control;

// ============================================================================
// ProtoCrypto test suite
// ============================================================================
// Tests for HMAC computation, key encryption during pairing, and challenge-response.
// Covers the cryptographic proofs used for authenticated commands.

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

  uint8_t tampered[HMAC_SIZE] = {0};
  std::memcpy(tampered, auth_resp.data, HMAC_SIZE);
  tampered[0] ^= 0x01;
  EXPECT_FALSE(
      crypto::verify_hmac(transcript, origin.data_len + 1, tampered, test::TEST_CHALLENGE, test::TEST_SYSTEM_KEY))
      << "tampered HMAC should fail verification";
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
