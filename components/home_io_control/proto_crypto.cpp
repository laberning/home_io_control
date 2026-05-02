/// @file proto_crypto.cpp
/// @brief Cryptographic helpers for the IO-Homecontrol protocol.

#include "proto_crypto.h"

#include <esp_random.h>
#include <cstring>

namespace esphome {
namespace home_io_control {
namespace crypto {

namespace {

// This is a small self-contained AES-128 implementation derived from the standard
// Rijndael/AES algorithm as specified in FIPS-197. It was added here to avoid a
// build-time dependency on ESP-IDF/mbedTLS linkage from ESPHome's generated "src"
// component.

// Forward substitution box defined by AES. Each input byte is replaced by its
// corresponding non-linear S-box value during SubBytes.
const uint8_t AES_SBOX[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76, 0xCA, 0x82, 0xC9,
    0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0, 0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F,
    0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15, 0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07,
    0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75, 0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3,
    0x29, 0xE3, 0x2F, 0x84, 0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58,
    0xCF, 0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8, 0x51, 0xA3,
    0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2, 0xCD, 0x0C, 0x13, 0xEC, 0x5F,
    0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73, 0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88,
    0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB, 0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC,
    0x62, 0x91, 0x95, 0xE4, 0x79, 0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A,
    0xAE, 0x08, 0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A, 0x70,
    0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E, 0xE1, 0xF8, 0x98, 0x11,
    0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF, 0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42,
    0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
};

// Inverse substitution box defined by AES. This is required for the inverse
// cipher path used by AES-128 decryption.
const uint8_t AES_INV_SBOX[256] = {
    0x52, 0x09, 0x6A, 0xD5, 0x30, 0x36, 0xA5, 0x38, 0xBF, 0x40, 0xA3, 0x9E, 0x81, 0xF3, 0xD7, 0xFB, 0x7C, 0xE3, 0x39,
    0x82, 0x9B, 0x2F, 0xFF, 0x87, 0x34, 0x8E, 0x43, 0x44, 0xC4, 0xDE, 0xE9, 0xCB, 0x54, 0x7B, 0x94, 0x32, 0xA6, 0xC2,
    0x23, 0x3D, 0xEE, 0x4C, 0x95, 0x0B, 0x42, 0xFA, 0xC3, 0x4E, 0x08, 0x2E, 0xA1, 0x66, 0x28, 0xD9, 0x24, 0xB2, 0x76,
    0x5B, 0xA2, 0x49, 0x6D, 0x8B, 0xD1, 0x25, 0x72, 0xF8, 0xF6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xD4, 0xA4, 0x5C, 0xCC,
    0x5D, 0x65, 0xB6, 0x92, 0x6C, 0x70, 0x48, 0x50, 0xFD, 0xED, 0xB9, 0xDA, 0x5E, 0x15, 0x46, 0x57, 0xA7, 0x8D, 0x9D,
    0x84, 0x90, 0xD8, 0xAB, 0x00, 0x8C, 0xBC, 0xD3, 0x0A, 0xF7, 0xE4, 0x58, 0x05, 0xB8, 0xB3, 0x45, 0x06, 0xD0, 0x2C,
    0x1E, 0x8F, 0xCA, 0x3F, 0x0F, 0x02, 0xC1, 0xAF, 0xBD, 0x03, 0x01, 0x13, 0x8A, 0x6B, 0x3A, 0x91, 0x11, 0x41, 0x4F,
    0x67, 0xDC, 0xEA, 0x97, 0xF2, 0xCF, 0xCE, 0xF0, 0xB4, 0xE6, 0x73, 0x96, 0xAC, 0x74, 0x22, 0xE7, 0xAD, 0x35, 0x85,
    0xE2, 0xF9, 0x37, 0xE8, 0x1C, 0x75, 0xDF, 0x6E, 0x47, 0xF1, 0x1A, 0x71, 0x1D, 0x29, 0xC5, 0x89, 0x6F, 0xB7, 0x62,
    0x0E, 0xAA, 0x18, 0xBE, 0x1B, 0xFC, 0x56, 0x3E, 0x4B, 0xC6, 0xD2, 0x79, 0x20, 0x9A, 0xDB, 0xC0, 0xFE, 0x78, 0xCD,
    0x5A, 0xF4, 0x1F, 0xDD, 0xA8, 0x33, 0x88, 0x07, 0xC7, 0x31, 0xB1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xEC, 0x5F, 0x60,
    0x51, 0x7F, 0xA9, 0x19, 0xB5, 0x4A, 0x0D, 0x2D, 0xE5, 0x7A, 0x9F, 0x93, 0xC9, 0x9C, 0xEF, 0xA0, 0xE0, 0x3B, 0x4D,
    0xAE, 0x2A, 0xF5, 0xB0, 0xC8, 0xEB, 0xBB, 0x3C, 0x83, 0x53, 0x99, 0x61, 0x17, 0x2B, 0x04, 0x7E, 0xBA, 0x77, 0xD6,
    0x26, 0xE1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0C, 0x7D,
};

// AES round constants used only by the key schedule. Each non-zero entry is the
// Rcon value for one expansion round; entry 0 is unused and kept as padding so
// the round index can be used directly.
const uint8_t AES_RCON[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36};

// Multiply two bytes in AES's GF(2^8) finite field. MixColumns and inverse
// MixColumns are defined in terms of this field arithmetic.
uint8_t aes_mul(uint8_t value, uint8_t factor) {
  uint8_t result = 0;
  while (factor != 0) {
    if ((factor & 1U) != 0)
      result ^= value;
    bool high_bit = (value & 0x80U) != 0;
    value <<= 1;
    if (high_bit)
      value ^= 0x1BU;
    factor >>= 1;
  }
  return result;
}

void aes_add_round_key(uint8_t *state, const uint8_t *round_keys, uint8_t round) {
  const uint8_t *round_key = &round_keys[round * AES_BLOCK_SIZE];
  for (uint8_t i = 0; i < AES_BLOCK_SIZE; i++)
    state[i] ^= round_key[i];
}

void aes_sub_bytes(uint8_t *state) {
  for (uint8_t i = 0; i < AES_BLOCK_SIZE; i++)
    state[i] = AES_SBOX[state[i]];
}

void aes_inv_sub_bytes(uint8_t *state) {
  for (uint8_t i = 0; i < AES_BLOCK_SIZE; i++)
    state[i] = AES_INV_SBOX[state[i]];
}

void aes_shift_rows(uint8_t *state) {
  uint8_t tmp[AES_BLOCK_SIZE];
  memcpy(tmp, state, AES_BLOCK_SIZE);
  state[0] = tmp[0];
  state[4] = tmp[4];
  state[8] = tmp[8];
  state[12] = tmp[12];
  state[1] = tmp[5];
  state[5] = tmp[9];
  state[9] = tmp[13];
  state[13] = tmp[1];
  state[2] = tmp[10];
  state[6] = tmp[14];
  state[10] = tmp[2];
  state[14] = tmp[6];
  state[3] = tmp[15];
  state[7] = tmp[3];
  state[11] = tmp[7];
  state[15] = tmp[11];
}

void aes_inv_shift_rows(uint8_t *state) {
  uint8_t tmp[AES_BLOCK_SIZE];
  memcpy(tmp, state, AES_BLOCK_SIZE);
  state[0] = tmp[0];
  state[4] = tmp[4];
  state[8] = tmp[8];
  state[12] = tmp[12];
  state[1] = tmp[13];
  state[5] = tmp[1];
  state[9] = tmp[5];
  state[13] = tmp[9];
  state[2] = tmp[10];
  state[6] = tmp[14];
  state[10] = tmp[2];
  state[14] = tmp[6];
  state[3] = tmp[7];
  state[7] = tmp[11];
  state[11] = tmp[15];
  state[15] = tmp[3];
}

void aes_mix_columns(uint8_t *state) {
  for (uint8_t column = 0; column < 4; column++) {
    uint8_t *col = &state[column * 4];
    uint8_t a0 = col[0];
    uint8_t a1 = col[1];
    uint8_t a2 = col[2];
    uint8_t a3 = col[3];
    col[0] = aes_mul(a0, 0x02) ^ aes_mul(a1, 0x03) ^ a2 ^ a3;
    col[1] = a0 ^ aes_mul(a1, 0x02) ^ aes_mul(a2, 0x03) ^ a3;
    col[2] = a0 ^ a1 ^ aes_mul(a2, 0x02) ^ aes_mul(a3, 0x03);
    col[3] = aes_mul(a0, 0x03) ^ a1 ^ a2 ^ aes_mul(a3, 0x02);
  }
}

void aes_inv_mix_columns(uint8_t *state) {
  for (uint8_t column = 0; column < 4; column++) {
    uint8_t *col = &state[column * 4];
    uint8_t a0 = col[0];
    uint8_t a1 = col[1];
    uint8_t a2 = col[2];
    uint8_t a3 = col[3];
    col[0] = aes_mul(a0, 0x0E) ^ aes_mul(a1, 0x0B) ^ aes_mul(a2, 0x0D) ^ aes_mul(a3, 0x09);
    col[1] = aes_mul(a0, 0x09) ^ aes_mul(a1, 0x0E) ^ aes_mul(a2, 0x0B) ^ aes_mul(a3, 0x0D);
    col[2] = aes_mul(a0, 0x0D) ^ aes_mul(a1, 0x09) ^ aes_mul(a2, 0x0E) ^ aes_mul(a3, 0x0B);
    col[3] = aes_mul(a0, 0x0B) ^ aes_mul(a1, 0x0D) ^ aes_mul(a2, 0x09) ^ aes_mul(a3, 0x0E);
  }
}

void aes_expand_key(const uint8_t key[AES_KEY_SIZE], uint8_t round_keys[176]) {
  memcpy(round_keys, key, AES_KEY_SIZE);
  uint8_t bytes_generated = AES_KEY_SIZE;
  uint8_t rcon_index = 1;
  uint8_t temp[4];

  while (bytes_generated < 176) {
    for (uint8_t i = 0; i < 4; i++)
      temp[i] = round_keys[bytes_generated - 4 + i];
    if ((bytes_generated % AES_KEY_SIZE) == 0) {
      uint8_t rotated = temp[0];
      temp[0] = AES_SBOX[temp[1]] ^ AES_RCON[rcon_index++];
      temp[1] = AES_SBOX[temp[2]];
      temp[2] = AES_SBOX[temp[3]];
      temp[3] = AES_SBOX[rotated];
    }
    for (uint8_t value : temp) {
      round_keys[bytes_generated] = round_keys[bytes_generated - AES_KEY_SIZE] ^ value;
      bytes_generated++;
    }
  }
}

bool aes128_encrypt_block(const uint8_t in[AES_BLOCK_SIZE], const uint8_t key[AES_KEY_SIZE],
                          uint8_t out[AES_BLOCK_SIZE]) {
  uint8_t state[AES_BLOCK_SIZE];
  uint8_t round_keys[176];
  memcpy(state, in, AES_BLOCK_SIZE);
  aes_expand_key(key, round_keys);

  aes_add_round_key(state, round_keys, 0);
  for (uint8_t round = 1; round < 10; round++) {
    aes_sub_bytes(state);
    aes_shift_rows(state);
    aes_mix_columns(state);
    aes_add_round_key(state, round_keys, round);
  }
  aes_sub_bytes(state);
  aes_shift_rows(state);
  aes_add_round_key(state, round_keys, 10);
  memcpy(out, state, AES_BLOCK_SIZE);
  return true;
}

bool aes128_decrypt_block(const uint8_t in[AES_BLOCK_SIZE], const uint8_t key[AES_KEY_SIZE],
                          uint8_t out[AES_BLOCK_SIZE]) {
  uint8_t state[AES_BLOCK_SIZE];
  uint8_t round_keys[176];
  memcpy(state, in, AES_BLOCK_SIZE);
  aes_expand_key(key, round_keys);

  aes_add_round_key(state, round_keys, 10);
  for (int round = 9; round >= 1; round--) {
    aes_inv_shift_rows(state);
    aes_inv_sub_bytes(state);
    aes_add_round_key(state, round_keys, static_cast<uint8_t>(round));
    aes_inv_mix_columns(state);
  }
  aes_inv_shift_rows(state);
  aes_inv_sub_bytes(state);
  aes_add_round_key(state, round_keys, 0);
  memcpy(out, state, AES_BLOCK_SIZE);
  return true;
}

}  // namespace

/// Proprietary checksum algorithm used in IV construction.
/// This is NOT a standard CRC - it's a custom accumulator used by the IO-Homecontrol
/// protocol to mix frame data into the initialization vector for AES encryption.
void compute_checksum(uint8_t byte, uint8_t &c1, uint8_t &c2) {
  uint8_t tmp = byte ^ c2;
  c2 = ((c1 & 0x7F) << 1) & 0xFF;
  if ((c1 & 0x80) == 0) {
    if (tmp >= 128)
      c2 |= 1;
    c1 = c2;
    c2 = (tmp << 1) & 0xFF;
    return;
  }
  if (tmp >= 128)
    c2 |= 1;
  c1 = c2 ^ 0x55;
  c2 = ((tmp << 1) ^ 0x5B) & 0xFF;
}

/// Construct the 16-byte initialization vector (IV) for AES encryption.
/// Layout: [bytes 0-7: frame data or 0x55 padding] [bytes 8-9: checksums] [bytes 10-15: challenge]
/// The IV binds the HMAC to both the frame content and the random challenge,
/// preventing replay attacks.
void construct_iv(const uint8_t *data, uint8_t len, const uint8_t challenge[HMAC_SIZE], uint8_t iv[IV_SIZE]) {
  memset(iv, 0, IV_SIZE);
  uint8_t c1 = 0;
  uint8_t c2 = 0;
  for (uint8_t i = 0; i < len; i++) {
    compute_checksum(data[i], c1, c2);
    if (i < 8)
      iv[i] = data[i];
  }
  // Pad remaining bytes 0-7 with 0x55 if data is shorter than 8 bytes.
  for (uint8_t i = len; i < 8; i++)
    iv[i] = IV_PADDING;
  iv[8] = c1;
  iv[9] = c2;
  memcpy(&iv[10], challenge, HMAC_SIZE);
}

/// AES-128 ECB encrypt a single 16-byte block.
bool aes128_encrypt(const uint8_t in[AES_BLOCK_SIZE], const uint8_t key[AES_KEY_SIZE], uint8_t out[AES_BLOCK_SIZE]) {
  return aes128_encrypt_block(in, key, out);
}

bool aes128_decrypt(const uint8_t in[AES_BLOCK_SIZE], const uint8_t key[AES_KEY_SIZE], uint8_t out[AES_BLOCK_SIZE]) {
  return aes128_decrypt_block(in, key, out);
}

/// Create a 6-byte HMAC for authentication.
/// Process: build IV from frame data + challenge -> AES-128-ECB encrypt IV with system key -> take first 6 bytes.
/// This is not a standard HMAC - it's a proprietary scheme specific to IO-Homecontrol.
bool create_hmac(const uint8_t *data, uint8_t len, const uint8_t challenge[HMAC_SIZE], const uint8_t key[AES_KEY_SIZE],
                 uint8_t hmac[HMAC_SIZE]) {
  uint8_t iv[IV_SIZE];
  construct_iv(data, len, challenge, iv);
  uint8_t encrypted[AES_BLOCK_SIZE];
  if (!aes128_encrypt(iv, key, encrypted))
    return false;
  // Truncate 16-byte AES output to 6 bytes.
  memcpy(hmac, encrypted, HMAC_SIZE);
  return true;
}

/// Verify a received HMAC using constant-time comparison (prevents timing attacks).
bool verify_hmac(const uint8_t *data, uint8_t len, const uint8_t hmac[HMAC_SIZE], const uint8_t challenge[HMAC_SIZE],
                 const uint8_t key[AES_KEY_SIZE]) {
  uint8_t calc[HMAC_SIZE];
  if (!create_hmac(data, len, challenge, key, calc))
    return false;
  uint8_t diff = 0;
  for (int i = 0; i < HMAC_SIZE; i++)
    diff |= calc[i] ^ hmac[i];
  return diff == 0;
}

/// Encrypt or decrypt a system key during pairing.
/// The system key is XORed with AES(IV, TRANSFER_KEY) - the same operation encrypts and decrypts.
/// The TRANSFER_KEY is a hardcoded key known to all IO-Homecontrol devices.
bool crypt_key(const uint8_t *data, uint8_t len, const uint8_t challenge[HMAC_SIZE], const uint8_t in[AES_KEY_SIZE],
               uint8_t out[AES_KEY_SIZE]) {
  uint8_t iv[IV_SIZE];
  construct_iv(data, len, challenge, iv);
  uint8_t enc_iv[AES_BLOCK_SIZE];
  if (!aes128_encrypt(iv, TRANSFER_KEY, enc_iv))
    return false;
  for (int i = 0; i < AES_KEY_SIZE; i++)
    out[i] = in[i] ^ enc_iv[i];
  return true;
}

/// Generate 6 random bytes for a challenge using the ESP32 hardware RNG.
void generate_challenge(uint8_t out[HMAC_SIZE]) {
  for (int i = 0; i < HMAC_SIZE; i++)
    out[i] = static_cast<uint8_t>(esp_random() & 0xFF);
}

}  // namespace crypto
}  // namespace home_io_control
}  // namespace esphome
