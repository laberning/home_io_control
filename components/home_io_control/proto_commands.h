#pragma once

/// @file proto_commands.h
/// @brief Command builders for the IO-Homecontrol protocol.

#include "proto_frame.h"

namespace esphome {
namespace home_io_control {

bool create_execute(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t position);
bool create_get_status(IoFrame &f, const uint8_t *own, const uint8_t *dst);
bool create_execute_tilt(IoFrame &f, const uint8_t *own, const uint8_t *dst, bool low_power, uint8_t tilt_percent);
bool create_get_status_tilt(IoFrame &f, const uint8_t *own, const uint8_t *dst);
bool create_discover(IoFrame &f, const uint8_t *own);
bool create_key_init(IoFrame &f, const uint8_t *own, const uint8_t *dst);
bool create_key_transfer(IoFrame &f, IoFrame &old_frame, const uint8_t *dst, const uint8_t *src,
                         const uint8_t key[AES_KEY_SIZE], const uint8_t challenge[HMAC_SIZE]);
bool create_challenge_req(IoFrame &f, const uint8_t *dst, const uint8_t *src);
bool create_challenge_resp(IoFrame &f, const uint8_t *dst, const uint8_t *src, const uint8_t challenge[HMAC_SIZE],
                           const IoFrame &origin, const uint8_t *key);
bool create_status_update_resp(IoFrame &f, const uint8_t *own, const uint8_t *dst);
bool create_set_config1(IoFrame &f, const uint8_t *own, const uint8_t *dst);

}  // namespace home_io_control
}  // namespace esphome
