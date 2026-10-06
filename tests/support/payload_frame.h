#pragma once

/// @file payload_frame.h
/// @brief Build an IoFrame from a command byte and a payload, for decoder and renderer tests.

#include "proto_frame.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>

namespace test {

/// Frame carrying only `cmd` and `data` (addresses and control bytes stay zero), for tests that
/// exercise a payload decoder or renderer without a wire round trip.
inline esphome::home_io_control::IoFrame make_payload_frame(uint8_t cmd, std::initializer_list<uint8_t> data) {
  esphome::home_io_control::IoFrame f{};
  f.cmd = cmd;
  f.data_len = static_cast<uint8_t>(data.size());
  std::copy(data.begin(), data.end(), f.data);  // not memcpy: an empty list's begin() is null
  return f;
}

}  // namespace test
