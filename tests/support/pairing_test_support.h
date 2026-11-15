#pragma once

/// @file pairing_test_support.h
/// @brief Frames and queueing helpers shared by the pairing key-step, key-pull and key-holder tests.
/// @ingroup hioc_hub

#include "device_registry.h"
#include "exchange_engine.h"
#include "pairing_telemetry.h"
#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "proto_frame.h"
#include "radio_interface.h"
#include "tuning_config.h"

#include "esp_random.h"
#include "test_helpers.h"
#include "stubs/radio_test_common.h"

#include <cstring>
#include <vector>

namespace pairing_test {

using namespace esphome::home_io_control;

inline constexpr uint8_t DEVICE_ID[NODE_ID_SIZE] = {0xB6, 0xBD, 0xFB};
inline constexpr size_t WIRE_CMD_OFFSET = 8;  // [CTRL0][CTRL1][DST 3B][SRC 3B][CMD ...]

inline RadioRxPacket to_packet(const IoFrame &frame) {
  RadioRxPacket pkt{};
  pkt.len = serialize(frame, pkt.data, sizeof(pkt.data));
  pkt.freq_hz = FREQ_CH2;
  return pkt;
}

inline IoFrame device_frame(const uint8_t dst[NODE_ID_SIZE], uint8_t cmd, const uint8_t *data, uint8_t len) {
  IoFrame f{};
  init_frame(f, true, false, false, false);
  set_dst(f, dst);
  set_src(f, DEVICE_ID);
  set_cmd(f, cmd, data, len);
  return f;
}

inline IoFrame challenge_frame(const uint8_t dst[NODE_ID_SIZE]) {
  static const uint8_t challenge[HMAC_SIZE] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  return device_frame(dst, CMD_CHALLENGE_REQ, challenge, HMAC_SIZE);
}

inline IoFrame error_frame(const uint8_t dst[NODE_ID_SIZE], uint8_t code) {
  return device_frame(dst, CMD_ERROR_RESP, &code, 1);
}

/// The Atlantic AGILIA discovery reply from tests/corpus/captures/pairing/
/// atlantic_agilia_pairing_key_transfer_command_unknown.yaml, readdressed to `dst`: node type bytes
/// FF C0 (remote controller), manufacturer 12, flags 0xCC.
inline IoFrame atlantic_discovery_reply(const uint8_t dst[NODE_ID_SIZE]) {
  const uint8_t payload[] = {0xFF, 0xC0, 0xB6, 0xBD, 0xFB, 0x0C, 0xCC, 0x00, 0x00};
  IoFrame f{};
  init_frame(f, true, true, true, false);
  set_dst(f, dst);
  set_src(f, DEVICE_ID);
  set_cmd(f, CMD_DISCOVER_RESP, payload, sizeof(payload));
  return f;
}

/// The collaborators the pairing parts are built from, standing alone on a MockRadio: a real exchange
/// engine, registry and telemetry, with no hub and no PairingEngine behind them.
struct PairingPartsRig {
  explicit PairingPartsRig(MockRadio &radio) : radio_ptr(&radio) {
    memcpy(node_id, test::OWN_ID, NODE_ID_SIZE);
    memcpy(system_key, test::TEST_SYSTEM_KEY, AES_KEY_SIZE);
  }

  RadioDriver *radio_ptr;
  TuningConfig tuning{};
  uint8_t node_id[NODE_ID_SIZE]{};
  uint8_t system_key[AES_KEY_SIZE]{};
  ExchangeEngine engine{&radio_ptr, node_id, system_key, &tuning};
  DeviceRegistry registry;
  PairingTelemetry telemetry;
};

/// The frames we sent with command `cmd`, parsed.
inline std::vector<IoFrame> sent_frames_of(const MockRadio &radio, uint8_t cmd) {
  std::vector<IoFrame> frames;
  for (const auto &raw : radio.get_sent_data()) {
    IoFrame frame{};
    if (parse(raw.data(), static_cast<uint8_t>(raw.size()), frame) && frame.cmd == cmd)
      frames.push_back(frame);
  }
  return frames;
}

inline std::vector<uint8_t> sent_commands(const MockRadio &radio) {
  std::vector<uint8_t> cmds;
  for (const auto &frame : radio.get_sent_data())
    cmds.push_back(frame.size() > WIRE_CMD_OFFSET ? frame[WIRE_CMD_OFFSET] : 0);
  return cmds;
}

inline constexpr uint8_t PULL_CHALLENGE[HMAC_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
inline constexpr uint8_t VERIFY_CHALLENGE[HMAC_SIZE] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
inline constexpr uint8_t OTHER_KEY[AES_KEY_SIZE] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
                                                    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};

/// Our 0x38 carrying PULL_CHALLENGE.
inline IoFrame pull_request(const uint8_t own[NODE_ID_SIZE]) {
  IoFrame request{};
  create_launch_key_transfer(request, own, DEVICE_ID, PULL_CHALLENGE);
  return request;
}

/// The device's 0x32 answering pull_request(), wrapping `key` with the whole-request IV. Encrypting
/// and decrypting are the same XOR, so recover_key_from_pull() also wraps.
inline IoFrame pulled_key_frame(const uint8_t dst[NODE_ID_SIZE], const uint8_t key[AES_KEY_SIZE]) {
  uint8_t wrapped[AES_KEY_SIZE];
  recover_key_from_pull(pull_request(dst), key, wrapped);
  return device_frame(dst, CMD_KEY_TRANSFER, wrapped, AES_KEY_SIZE);
}

/// The device's 0x3D answering our VERIFY_CHALLENGE, signed with `key` over its own 0x32.
inline IoFrame proof_frame(const uint8_t dst[NODE_ID_SIZE], const IoFrame &its_key_transfer,
                           const uint8_t key[AES_KEY_SIZE]) {
  uint8_t mac[HMAC_SIZE];
  challenge_resp_hmac(its_key_transfer, VERIFY_CHALLENGE, key, mac);
  return device_frame(dst, CMD_CHALLENGE_RESP, mac, HMAC_SIZE);
}

struct RngGuard {
  RngGuard() {
    test_rng::reset();
    test_rng::enqueue_bytes(PULL_CHALLENGE, HMAC_SIZE);
    test_rng::enqueue_bytes(VERIFY_CHALLENGE, HMAC_SIZE);
  }
  ~RngGuard() { test_rng::reset(); }
};

/// The heater's roll-call reply (0x2B): type 52 subtype 1 (packed 0D 01) at the address after the
/// pairing node, manufacturer 12, always-alive.
inline IoFrame heater_roll_call_reply(const uint8_t dst[NODE_ID_SIZE], uint8_t type_msb = 0x0D,
                                      uint8_t type_low = 0x01) {
  const uint8_t payload[] = {type_msb, type_low, 0xB6, 0xBD, 0xFC, 0x0C, 0xCC, 0x00, 0x00};
  IoFrame f{};
  init_frame(f, true, true, true, false);
  set_dst(f, dst);
  const uint8_t heater[NODE_ID_SIZE] = {0xB6, 0xBD, 0xFC};
  set_src(f, heater);
  set_cmd(f, CMD_DISCOVER_SPE_RESP, payload, sizeof(payload));
  return f;
}

inline constexpr uint8_t HEATER_ID[NODE_ID_SIZE] = {0xB6, 0xBD, 0xFC};

/// A frame from the heater at the address after the pairing node.
inline IoFrame heater_frame(const uint8_t dst[NODE_ID_SIZE], uint8_t cmd, const uint8_t *data, uint8_t len) {
  IoFrame f{};
  init_frame(f, true, false, false, false);
  set_dst(f, dst);
  set_src(f, HEATER_ID);
  set_cmd(f, cmd, data, len);
  return f;
}

inline constexpr uint8_t HEATER_CHALLENGE[HMAC_SIZE] = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26};

/// Queue the heater answering the probe: its challenge, then its maximum setpoint (28.0 C), as in
/// tests/corpus/captures/exchange/atlantic_agilia_exchange_status_read.yaml.
inline void queue_heater_answers_probe(MockRadio &radio, const uint8_t dst[NODE_ID_SIZE]) {
  radio.queue_rx(to_packet(heater_frame(dst, CMD_CHALLENGE_REQ, HEATER_CHALLENGE, HMAC_SIZE)));
  const uint8_t max_setpoint[] = {0x0C, 0x60, 0x01, 0x02, 0x18, 0x01};
  radio.queue_rx(to_packet(heater_frame(dst, CMD_WRITE_PRIVATE_ACK, max_setpoint, sizeof(max_setpoint))));
}

}  // namespace pairing_test
