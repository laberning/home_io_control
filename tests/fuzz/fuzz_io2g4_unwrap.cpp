/// @file fuzz_io2g4_unwrap.cpp
/// @brief libFuzzer target for the 2.4 GHz PHY's receive path (Io2g4Phy::unwrap) and its wrapper.
///
/// The first input byte is the CRC state the radio reports (taken modulo the three states); the
/// rest is a received PSDU. Beyond crashes and sanitizer findings, the target traps on a broken
/// contract: a payload reported bad never yields a frame, a recovered frame has exactly the
/// length the PSDU leaves after the header and FCS, and wrapping it again reproduces the PSDU
/// without its FCS. Not part of `make check`; run via `make fuzz-io2g4-unwrap`.

#include "radio_io2g4_phy.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

using namespace esphome::home_io_control::bridle;

namespace {

/// Aborts the run when a stated contract does not hold, so it is reported like a crash.
inline void require(bool condition) {
  if (!condition)
    __builtin_trap();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size == 0)
    return 0;
  const CrcState crc = static_cast<CrcState>(data[0] % 3);
  const uint8_t *psdu = data + 1;
  const size_t len = size - 1;

  uint8_t frame[255];
  const UnwrapResult result = io2g4_phy().unwrap(psdu, len, crc, frame, sizeof(frame));
  if (result.status != UnwrapResult::Status::OK)
    return 0;
  require(crc != CrcState::BAD);
  require(result.frame_len == len - IO2G4_PSDU_HEADER_SIZE - IO2G4_FCS_SIZE);
  uint8_t rewrapped[255 + IO2G4_PSDU_HEADER_SIZE];
  const size_t n = io2g4_phy().wrap(frame, result.frame_len, rewrapped, sizeof(rewrapped));
  require(n == len - IO2G4_FCS_SIZE && std::memcmp(rewrapped, psdu, n) == 0);
  return 0;
}
