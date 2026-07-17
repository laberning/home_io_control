#pragma once

/// @file proto_crypto.h
/// @brief Cryptographic helpers for the IO‑Homecontrol protocol.
/// @ingroup hioc_protocol
///
/// IO‑Homecontrol uses AES‑128 encryption and a proprietary 6‑byte HMAC construction
/// derived from the original Somfy implementation. The "HMAC" here is not a standard
/// HMAC-SHA; it is a custom construction: the IV is built from frame bytes and checksums,
/// then encrypted with the system key via AES‑128‑ECB, and the first 6 bytes are taken.
///
/// During pairing, the system key itself is transferred to the device using an XOR‑AES
/// obfuscation with a globally shared transfer key. crypt_key() handles both encryption
/// and decryption; the operation is symmetric.
///
/// @warning The transfer key (TRANSFER_KEY) is hardcoded and public—its only purpose
///          is to obfuscate the system key during over‑the‑air transfer. The security
///          of the installation relies entirely on keeping the system key secret.

#include "proto_sizes.h"

namespace esphome {
namespace home_io_control {
namespace crypto {

/// Update running checksum bytes (c1, c2) with a new data byte (IO‑Homecontrol IV derivation).
/// @param byte Input data byte.
/// @param c1 First checksum byte (inout).
/// @param c2 Second checksum byte (inout).
void compute_checksum(uint8_t byte, uint8_t &c1, uint8_t &c2);

/// Construct the 16‑byte IV for AES encryption from frame data and challenge.
/// Layout: bytes 0–7 = up to 8 frame bytes padded with 0x55, bytes 8–9 = checksums,
/// bytes 10–15 = challenge.
/// @param data Frame data bytes.
/// @param len Number of data bytes.
/// @param challenge 6‑byte challenge from the device.
/// @param iv Output: 16‑byte IV.
void construct_iv(const uint8_t *data, uint8_t len, const uint8_t challenge[HMAC_SIZE], uint8_t iv[IV_SIZE]);

/// AES‑128 ECB encrypt a single 16‑byte block.
/// @param in 16‑byte plaintext block.
/// @param key 16‑byte AES key.
/// @param out Output: 16‑byte ciphertext block.
/// @return true on success.
bool aes128_encrypt(const uint8_t in[AES_BLOCK_SIZE], const uint8_t key[AES_KEY_SIZE], uint8_t out[AES_BLOCK_SIZE]);

/// AES‑128 ECB decrypt a single 16‑byte block.
/// @param in 16‑byte ciphertext block.
/// @param key 16‑byte AES key.
/// @param out Output: 16‑byte plaintext block.
/// @return true on success.
bool aes128_decrypt(const uint8_t in[AES_BLOCK_SIZE], const uint8_t key[AES_KEY_SIZE], uint8_t out[AES_BLOCK_SIZE]);

/// Create a 6‑byte HMAC for authentication (IO‑Homecontrol proprietary scheme).
/// Process: build IV from [data + challenge] → AES‑128‑ECB encrypt IV with system key → take first 6 bytes.
/// This is NOT a standard HMAC; it is specific to IO‑Homecontrol and matches
/// the protocol specification as implemented in compatible devices.
/// @param data Frame data bytes (usually command + payload).
/// @param len Length of data.
/// @param challenge 6‑byte random challenge.
/// @param key 16‑byte system key.
/// @param hmac Output: 6‑byte HMAC.
/// @note The IV construction appends two checksum bytes derived from the data stream
///       (see compute_checksum()) followed by the 6‑byte challenge, padded to 16 bytes
///       with 0x55. The AES result is truncated to 6 bytes for transmission.
/// @return true on success.
bool create_hmac(const uint8_t *data, uint8_t len, const uint8_t challenge[HMAC_SIZE], const uint8_t key[AES_KEY_SIZE],
                 uint8_t hmac[HMAC_SIZE]);

/// Verify a received 6‑byte HMAC using constant‑time comparison.
/// @param data Frame data bytes.
/// @param len Length of data.
/// @param hmac Received 6‑byte HMAC.
/// @param challenge Challenge used in HMAC calculation.
/// @param key 16‑byte system key.
/// @return true if HMAC matches.
bool verify_hmac(const uint8_t *data, uint8_t len, const uint8_t hmac[HMAC_SIZE], const uint8_t challenge[HMAC_SIZE],
                 const uint8_t key[AES_KEY_SIZE]);

/// Encrypt or decrypt the system key during pairing (XOR with AES‑encrypted IV).
/// The same operation works for both directions: encrypting for the device and
/// decrypting the device's acknowledgement.
/// @param data Frame data (typically the key‑init command byte).
/// @param len Length of data (usually 1).
/// @param challenge Device's 6‑byte challenge.
/// @param in Input key (plaintext for encrypt, ciphertext for decrypt).
/// @param out Output key.
/// @warning This primitive is used ONLY during the key‑transfer phase (0x32). The
///          resulting system key is then used for all normal authenticated exchanges.
///          Never call this with arbitrary data outside the pairing sequence.
/// @return true on success.
bool crypt_key(const uint8_t *data, uint8_t len, const uint8_t challenge[HMAC_SIZE], const uint8_t in[AES_KEY_SIZE],
               uint8_t out[AES_KEY_SIZE]);

/// Generate 6 random bytes for a challenge using the ESP hardware RNG.
/// @param out Output buffer (6 bytes).
void generate_challenge(uint8_t out[HMAC_SIZE]);

}  // namespace crypto
}  // namespace home_io_control
}  // namespace esphome
