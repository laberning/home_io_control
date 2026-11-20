#pragma once

/// @file oneway_wire_frames.h
/// @brief The three one-way frame sizes as bytes on the wire, and the air time they take, for the radio driver tests.
///
/// A one-way command is a 23-byte frame, an add-controller frame is 29 bytes, and one with the MAC trailer is
/// 35 bytes. The driver tests send each of them through their scripted bus, so a size no driver test ever sends
/// cannot hide a limit in a driver.
/// @ingroup hioc_protocol

#include "proto_commands.h"
#include "proto_frame.h"
#include "proto_crypto.h"

#include "test_helpers.h"

#include <cstdint>
#include <string>
#include <vector>

namespace oneway_wire {

using namespace esphome::home_io_control;

/// One one-way frame as the radio is handed it.
struct WireFrame {
  const char *name;
  std::vector<uint8_t> bytes;  ///< serialize() output: what send_packet() takes.
};

/// Serialized wire size of the EXECUTE command frame.
inline constexpr uint8_t EXECUTE_WIRE_LEN = 23;
/// Serialized wire size of the add-controller frame without the MAC trailer.
inline constexpr uint8_t ADD_WIRE_LEN = 29;
/// Serialized wire size of the add-controller frame with the six-byte MAC trailer.
inline constexpr uint8_t ADD_WITH_MAC_WIRE_LEN = 35;

inline std::vector<uint8_t> wire_bytes(const IoFrame &frame) {
  uint8_t buf[FRAME_MAX_WIRE_SIZE] = {0};
  const uint8_t len = serialize(frame, buf, sizeof(buf));
  return std::vector<uint8_t>(buf, buf + len);
}

/// The three frames, built by the production builders.
inline std::vector<WireFrame> frames() {
  const uint8_t src[NODE_ID_SIZE] = {0x9D, 0x60, 0x85};
  IoFrame execute{};
  EXPECT_TRUE(create_1w_execute_command(execute, src, DeviceType::VERTICAL_EXTERIOR_AWNING, CoverCommand::STOP, 0x0102,
                                        test::TEST_SYSTEM_KEY));
  IoFrame add{};
  EXPECT_TRUE(create_1w_add_controller(add, src, DeviceType::UNKNOWN, MANUFACTURER_SOMFY, 0x0102, test::TEST_SYSTEM_KEY,
                                       /*with_mac=*/false));
  IoFrame add_with_mac{};
  EXPECT_TRUE(create_1w_add_controller(add_with_mac, src, DeviceType::UNKNOWN, MANUFACTURER_SOMFY, 0x0102,
                                       test::TEST_SYSTEM_KEY, /*with_mac=*/true));
  return {{"execute", wire_bytes(execute)},
          {"add_controller", wire_bytes(add)},
          {"add_controller_with_mac", wire_bytes(add_with_mac)}};
}

/// Name of a test case for gtest output: frame name plus preamble length.
inline std::string case_name(const WireFrame &frame, uint16_t preamble) {
  return std::string(frame.name) + " at preamble " + std::to_string(preamble);
}

/// Air time in microseconds of a frame of `wire_len` bytes behind `preamble_bytes`, worked out from the line:
/// 38400 bit/s, a preamble byte taking `preamble_bits_per_byte` bit periods, the 3-byte sync word, then the frame
/// and its 2-byte CRC as 10-bit UART cells packed into whole bytes. Rounded up.
inline uint32_t expected_air_time_us(uint16_t preamble_bytes, uint8_t wire_len, uint8_t preamble_bits_per_byte) {
  const uint64_t cell_bits = static_cast<uint64_t>(wire_len + 2) * 10;
  const uint64_t coded_bytes = (cell_bits + 7) / 8;
  const uint64_t bits = static_cast<uint64_t>(preamble_bytes) * preamble_bits_per_byte + (3 + coded_bytes) * 8;
  return static_cast<uint32_t>((bits * 1000000 + 38400 - 1) / 38400);
}

/// Length of the UART-packed frame (with CRC) the software-PHY chips load into their transmit buffer.
inline uint8_t expected_encoded_len(uint8_t wire_len) { return static_cast<uint8_t>(((wire_len + 2) * 10 + 7) / 8); }

}  // namespace oneway_wire
