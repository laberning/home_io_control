#include "device_event_log.h"

#include "log_helpers.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cstdio>

/// @file device_event_log.cpp
/// @brief The log of device event reports behind the "Last Device Event" sensor.
/// @ingroup hioc_hub

namespace esphome {
namespace home_io_control {

namespace {
constexpr size_t DEVICE_EVENT_HEAD_BUFFER_SIZE = 48;  ///< "#<n> t=<s> ": two uint32 values and punctuation.
constexpr size_t DEVICE_EVENT_CMD_BUFFER_SIZE = 8;    ///< " 0xNN" plus the terminator.
constexpr uint32_t DEVICE_EVENT_MS_PER_SECOND = 1000;
}  // namespace

void DeviceEventLog::record(const IoFrame &frame, bool verbose) {
  std::string payload;
  char byte_hex[4];
  for (uint8_t i = 0; i < frame.data_len; i++) {
    snprintf(byte_hex, sizeof(byte_hex), i == 0 ? "%02X" : " %02X", frame.data[i]);
    payload += byte_hex;
  }
  const std::string src_id = node_id_to_string(frame.src);
  const std::string dst_id = node_id_to_string(frame.dst);
  this->count_++;
  if (verbose) {
    ESP_LOGI(detail::TAG, "Device event from %s: cmd=0x%02X dst=%s payload=%s", src_id.c_str(), frame.cmd,
             dst_id.c_str(), payload.c_str());
  } else {
    ESP_LOGD(detail::TAG, "Device event from %s: cmd=0x%02X dst=%s payload=%s", src_id.c_str(), frame.cmd,
             dst_id.c_str(), payload.c_str());
  }
  char head[DEVICE_EVENT_HEAD_BUFFER_SIZE];
  snprintf(head, sizeof(head), "#%" PRIu32 " t=%" PRIu32 " ", this->count_, millis() / DEVICE_EVENT_MS_PER_SECOND);
  char cmd_hex[DEVICE_EVENT_CMD_BUFFER_SIZE];
  snprintf(cmd_hex, sizeof(cmd_hex), " 0x%02X", frame.cmd);
  this->last_ = std::string(head) + src_id + cmd_hex + (payload.empty() ? "" : " ") + payload;
  if (this->callback_)
    this->callback_();
}

}  // namespace home_io_control
}  // namespace esphome
