/// @file proto_commands.cpp
/// @brief Command builders for the IO-Homecontrol protocol.

#include "proto_commands.h"

#include "proto_crypto.h"

#include <cstring>

namespace esphome {
namespace home_io_control {

/// Build an execute command (0x00) to control a device.
/// @param position 0=open, 100=closed, POS_STOP=stop, POS_FAVORITE=favorite position
/// For real positions (0-100), the value is doubled in the frame (0x00=0%, 0xC8=100%).
/// For special commands (stop/favorite), a shorter 6-byte payload is used.
bool create_execute(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t position) {
  init_frame(f, true, true, false, low_power);
  set_dst(f, dst);
  set_src(f, own);
  uint8_t d[8];
  uint8_t plen;
  // Command originator: 0x01 = User (remote control action).
  d[0] = 0x01;
  // ACEI: priority "User Level 2", IsValid flag (observed from Somfy connectivity kit).
  d[1] = 0x67;
  if (position <= 100) {
    // Real position: doubled value (0-200 maps to 0-100%).
    d[2] = 2 * position;
    d[3] = 0x00;
    d[4] = 0x80;
    d[5] = 0xD8;
    d[6] = 0x06;
    d[7] = 0x00;
    plen = 8;
  } else {
    // Special command (stop=0xD2, favorite=0xD8).
    d[2] = position;
    d[3] = 0x00;
    d[4] = 0x00;
    d[5] = 0x00;
    plen = 6;
  }
  return set_cmd(f, CMD_EXECUTE, d, plen);
}

/// Build a get-status request (0x03). The device responds with its current position.
bool create_get_status(IoFrame &f, const uint8_t *own, const uint8_t *dst) {
  // low_power=true for solar devices.
  init_frame(f, true, true, false, true);
  set_dst(f, dst);
  set_src(f, own);
  // Sub-command 0x03 = get position status.
  uint8_t d[3] = {0x03, 0x00, 0x00};
  return set_cmd(f, CMD_PRIVATE, d, 3);
}

/// Build a tilt execute command (0x00) for devices that support slat angle control.
/// @param tilt_percent 0=fully closed, 100=fully open.
bool create_execute_tilt(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t tilt_percent) {
  init_frame(f, true, true, false, low_power);
  set_dst(f, dst);
  set_src(f, own);

  auto const tilt_value = static_cast<uint16_t>((100 - tilt_percent) * STATUS_POS_MAX / 100);
  uint8_t d[8] = {0x01,
                  0xE7,
                  POS_UNKNOWN,
                  0x00,
                  0x20,
                  static_cast<uint8_t>((tilt_value >> 8) & 0xFF),
                  static_cast<uint8_t>(tilt_value & 0xFF),
                  0x00};
  return set_cmd(f, CMD_EXECUTE, d, 8);
}

/// Build a tilt-aware get-status request (0x03) that returns the extended 16-byte tilt payload.
bool create_get_status_tilt(IoFrame &f, const uint8_t *own, const uint8_t *dst) {
  init_frame(f, true, true, false, true);
  set_dst(f, dst);
  set_src(f, own);
  uint8_t d[4] = {0x03, 0x20, 0x01, 0x00};
  return set_cmd(f, CMD_PRIVATE, d, 4);
}

/// Build a discovery broadcast (0x28). Sent to the broadcast address 0x00003B.
/// Only devices in pairing mode (PROG button pressed) will respond.
bool create_discover(IoFrame &f, const uint8_t *own) {
  // start+end: single broadcast frame.
  init_frame(f, true, true, true, false);
  set_dst(f, BROADCAST_DISCOVER);
  set_src(f, own);
  return set_cmd(f, CMD_DISCOVER_REQ);
}

/// Build a key-init request (0x31) to start the pairing key exchange with a discovered device.
bool create_key_init(IoFrame &f, const uint8_t *own, const uint8_t *dst) {
  init_frame(f, true, true, false, true);
  set_dst(f, dst);
  set_src(f, own);
  return set_cmd(f, CMD_KEY_INIT);
}

/// Build a key-transfer frame (0x32) containing the system key encrypted with the transfer key.
/// @param old_frame The previous frame sent (key-init), used to derive the encryption IV.
/// @param challenge The 6-byte challenge received from the device in its 0x3C response.
bool create_key_transfer(IoFrame &f, IoFrame &old_frame, const uint8_t *dst, const uint8_t *src,
                         const uint8_t key[AES_KEY_SIZE], const uint8_t challenge[HMAC_SIZE]) {
  init_frame(f, true, false, false, false);
  set_dst(f, dst);
  set_src(f, src);
  // The pairing capture we matched derives the IV from the previous command byte only. Treating
  // the key-init frame that narrowly keeps our key transfer aligned with real controllers.
  uint8_t enc_key[AES_KEY_SIZE];
  if (!crypto::crypt_key(&old_frame.cmd, 1, challenge, key, enc_key))
    return false;
  return set_cmd(f, CMD_KEY_TRANSFER, enc_key, AES_KEY_SIZE);
}

/// Build a challenge request (0x3C) containing 6 random bytes.
/// Used when WE need to authenticate an incoming request from a device.
bool create_challenge_req(IoFrame &f, const uint8_t *dst, const uint8_t *src) {
  init_frame(f, true, true, false, false);  // start=true, end=false
  set_dst(f, dst);
  set_src(f, src);
  uint8_t challenge[HMAC_SIZE];
  crypto::generate_challenge(challenge);
  return set_cmd(f, CMD_CHALLENGE_REQ, challenge, HMAC_SIZE);
}

/// Build a challenge response (0x3D) proving we know the system key.
/// The HMAC is computed over [original_command_id + original_data] using the challenge.
bool create_challenge_resp(IoFrame &f, const uint8_t *dst, const uint8_t *src, const uint8_t challenge[HMAC_SIZE],
                           const IoFrame &origin, const uint8_t *key) {
  init_frame(f);
  set_dst(f, dst);
  set_src(f, src);
  // The authenticated transcript covers the original request, not the 0x3D wrapper. Using the
  // origin command byte and payload here was one of the key interoperability findings.
  uint8_t frame_data[FRAME_MAX_SIZE];
  frame_data[0] = origin.cmd;
  memcpy(frame_data + 1, origin.data, origin.data_len);
  uint8_t hmac[HMAC_SIZE];
  if (!crypto::create_hmac(frame_data, origin.data_len + 1, challenge, key, hmac))
    return false;
  return set_cmd(f, CMD_CHALLENGE_RESP, hmac, HMAC_SIZE);
}

/// Build a status-update acknowledgment (0x72). Sent after authenticating a device's status update.
/// The response is sent on all 3 channels to ensure the device receives it.
bool create_status_update_resp(IoFrame &f, const uint8_t *own, const uint8_t *dst) {
  // end=true: final frame.
  init_frame(f, true, false, true, false);
  set_dst(f, dst);
  set_src(f, own);
  // Acknowledgment data (observed from Somfy connectivity kit).
  uint8_t d[2] = {0x05, 0x00};
  return set_cmd(f, CMD_STATUS_UPDATE_RESP, d, 2);
}

/// Build a set-config command (0x6F) to tell the device to automatically send status updates
/// when controlled by any remote (not just us). Not all devices support this.
bool create_set_config1(IoFrame &f, const uint8_t *own, const uint8_t *dst) {
  init_frame(f, true, true, false, false);
  set_dst(f, dst);
  set_src(f, own);
  // Observed from Somfy connectivity kit.
  uint8_t d[5] = {0xE0, 0x10, 0x0A, 0x08, 0x00};
  return set_cmd(f, CMD_SET_CONFIG1, d, 5);
}

}  // namespace home_io_control
}  // namespace esphome
