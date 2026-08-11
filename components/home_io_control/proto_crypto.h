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
/// crypt_1w_key() is a sibling of crypt_key() for the one-way (1W) protocol: it wraps a
/// network key inside a CMD_ONEWAY_ADD_CONTROLLER (0x30) frame with the exact same
/// encrypt-under-TRANSFER_KEY-then-XOR core, differing only in how the IV is built
/// (construct_iv_1w_node() vs. construct_iv()). The two share that core rather than
/// duplicating it — see xor_with_encrypted_iv() in proto_crypto.cpp.
///
/// create_1w_hmac() is create_hmac()'s 1W sibling: it authenticates an explicit,
/// command-specific span with a rolling sequence number in place of a random challenge,
/// sharing construct_iv()'s checksum/pad core (construct_iv_prefix() in proto_crypto.cpp)
/// rather than duplicating it — see construct_iv_1w_sequence(). Unlike create_hmac(), it has
/// no default span: the caller must supply exactly the bytes the target command authenticates,
/// which differ per command (see create_1w_hmac()'s warning below).
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

/// Construct the 16‑byte IV for the 1W key‑wrap primitive (crypt_1w_key()) from a sender's
/// node address alone. Layout: the 3‑byte node address repeated through byte 14
/// ({N0,N1,N2,N0,N1,N2,…}), with iv[15] = N0. Unlike construct_iv() (2W), this IV carries no
/// challenge or frame data — a 1W broadcast has no per‑exchange challenge to bind to, so the
/// sender's node address (already public: it is the frame header's plaintext source field) is
/// the only per‑sender distinguishing input available.
/// @param node 3‑byte sender node address.
/// @param iv Output: 16‑byte IV.
void construct_iv_1w_node(const uint8_t node[NODE_ID_SIZE], uint8_t iv[IV_SIZE]);

/// Construct the 16‑byte IV for the 1W sequence‑keyed HMAC (create_1w_hmac()).
/// Layout: bytes 0–7 = up to 8 span bytes padded with 0x55, bytes 8–9 = checksums (the same
/// shared core construct_iv() uses), bytes 10–11 = the 2‑byte sequence (big‑endian), bytes
/// 12–15 = 0x55 padding. A 1W frame has no per‑exchange challenge to bind to — the transmitted
/// sequence is the only per‑transmission value available, so it takes the challenge's place in
/// the tail while the padded bytes fill the rest.
/// @param data Span bytes to authenticate. The span is command‑specific — see create_1w_hmac().
/// @param len Number of span bytes.
/// @param sequence 2‑byte rolling sequence for this transmission.
/// @param iv Output: 16‑byte IV.
void construct_iv_1w_sequence(const uint8_t *data, uint8_t len, uint16_t sequence, uint8_t iv[IV_SIZE]);

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

/// Create a 6‑byte HMAC for a 1W (one‑way) transmission — the sequence‑keyed sibling of
/// create_hmac(). Process: build the IV from [span + sequence] (construct_iv_1w_sequence()) →
/// AES‑128‑ECB encrypt with the controller's key → take the first 6 bytes.
/// @warning There is NO default span. The authenticated bytes differ per command — e.g.
///          CMD_ONEWAY_ADD_CONTROLLER (0x30) signs only `cmd + enc_key` (17 bytes), while
///          CMD_EXECUTE (0x00) signs the full payload `cmd` through `fp2` (7 bytes) — see
///          `tests/corpus/captures/reference_1w_vectors/oneway_execute_iv_vector.yaml` and
///          `tests/corpus/captures/reference_1w_vectors/oneway_add_controller_kat.yaml`. The
///          caller must pass the span the target command actually authenticates, confirmed by a
///          known-answer vector — a wrong span silently produces a plausible-looking MAC that no
///          device will accept, and only a KAT catches the mistake.
/// @param data Command‑specific span bytes to authenticate (see @warning above).
/// @param len Number of span bytes.
/// @param sequence 2‑byte rolling sequence for this transmission.
/// @param controller_key 16‑byte key held by the transmitting controller identity. This may be
///        the hub's own system_key, or a foreign key adopted from another network (Phase 3A
///        "key adoption"); the primitive is key‑agnostic and simply uses whichever key the
///        caller supplies.
/// @param hmac Output: 6‑byte HMAC.
/// @return true on success.
bool create_1w_hmac(const uint8_t *data, uint8_t len, uint16_t sequence, const uint8_t controller_key[AES_KEY_SIZE],
                    uint8_t hmac[HMAC_SIZE]);

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

/// Encrypt or decrypt a 1W controller key during add-controller key adoption (CMD 0x30).
/// Shares its core with crypt_key() — XOR with AES‑128‑ECB(IV, TRANSFER_KEY) — and differs only
/// in IV construction: construct_iv_1w_node() derives the IV from the sender's node address
/// alone, in place of crypt_key()'s frame-data-and-challenge IV. As with crypt_key(), the
/// operation is its own inverse (XOR with the same keystream both ways), so there is no
/// direction flag: the same call encrypts a plaintext key for transmission or decrypts an
/// overheard ciphertext.
/// @param node 3‑byte sender node address (from the frame header, plaintext).
/// @param in Input key (plaintext to encrypt, or ciphertext to decrypt).
/// @param out Output key.
/// @warning `in`/`out` are real key material (a 1W installation's network key). Callers must
///          never log, print, or otherwise let these bytes reach any path other than the one
///          intentional, user-facing "adopted key" emission.
/// @return true on success.
bool crypt_1w_key(const uint8_t node[NODE_ID_SIZE], const uint8_t in[AES_KEY_SIZE], uint8_t out[AES_KEY_SIZE]);

/// Generate 6 random bytes for a challenge using the ESP hardware RNG.
/// @param out Output buffer (6 bytes).
void generate_challenge(uint8_t out[HMAC_SIZE]);

}  // namespace crypto
}  // namespace home_io_control
}  // namespace esphome
