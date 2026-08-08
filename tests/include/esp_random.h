#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

/// @brief Scriptable RNG queue for host tests that need deterministic esp_random() output.
///
/// Added for pairing replay tests to reproduce a captured controller challenge
/// (crypto::generate_challenge() consumes esp_random() one byte at a time via `& 0xFF`) so a
/// pairing/discovery replay can assert byte-exact transmitted frames instead of merely
/// structural ones. Empty queue -> esp_random() falls back to a deterministic LCG, so every test
/// that never calls test_rng::enqueue() is unaffected; ProtoCrypto.GenerateChallenge* in
/// proto_crypto_test.cpp relies on exactly that fallback (via reset()) to check
/// generate_challenge()'s output shape without needing real entropy.
namespace test_rng {

inline std::deque<uint32_t> &queue() {
  static std::deque<uint32_t> q;
  return q;
}

/// Enqueue a raw 32-bit value to be returned by the next esp_random() call.
inline void enqueue(uint32_t value) { queue().push_back(value); }

/// Enqueue a single byte as the low byte of a returned word — matches how
/// crypto::generate_challenge() consumes esp_random() (`esp_random() & 0xFF` per byte).
inline void enqueue_byte(uint8_t byte) { queue().push_back(static_cast<uint32_t>(byte)); }

/// Enqueue every byte of a buffer in order, e.g. to script generate_challenge()'s output
/// byte-for-byte so it reproduces a captured controller challenge.
inline void enqueue_bytes(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++)
    enqueue_byte(data[i]);
}

/// Discard any queued values and revert to the default LCG behavior. Call this after a test
/// that enqueues values, so state never leaks into an unrelated test run after it.
inline void reset() { queue().clear(); }

}  // namespace test_rng

// Stub for esp_random() used in host tests.
inline uint32_t esp_random() {
  if (!test_rng::queue().empty()) {
    const uint32_t value = test_rng::queue().front();
    test_rng::queue().pop_front();
    return value;
  }
  // Simple LCG for testing — not cryptographic, only for deterministic test vectors
  static uint32_t seed = 0x12345678;
  seed = seed * 1664525 + 1013904223;
  return seed;
}
