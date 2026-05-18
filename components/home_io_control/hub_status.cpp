#include "hub_internal.h"

#include "hub_decisions.h"
#include "proto_commands.h"

/// @file hub_status.cpp
/// @brief Inbound status handling and passive receive-side state updates.
///
/// This file owns the receive-side state path for the hub:
/// - decode status-bearing frames into normalized device state,
/// - decide when unsolicited traffic should trigger follow-up polls,
/// - ACK authenticated device-initiated status updates.
///
/// The goal of the split is to keep hub_core.cpp focused on lifecycle,
/// device registry, and scheduling while leaving the protocol-specific receive
/// interpretation in one place.

namespace esphome {
namespace home_io_control {

namespace {

/// @brief Decode the shared target/current position fields used by private response and status‑update frames.
/// Different frame types use different byte offsets, but the normalization policy is identical once offsets known.
/// @param dev Device record to update.
/// @param frame IoFrame containing a status‑bearing command.
/// @param target_offset Byte offset of target MSB within frame.data.
/// @param current_offset Byte offset of current MSB within frame.data.
/// @param allow_tilt_from_extended_response If true and frame is extended, decode tilt from bytes 13–14.
void decode_status_fields(IoDevice &dev, const IoFrame &frame, uint8_t target_offset, uint8_t current_offset,
                          bool allow_tilt_from_extended_response) {
  uint16_t const tgt = (frame.data[target_offset] << 8) | frame.data[target_offset + 1];
  uint16_t const cur = (frame.data[current_offset] << 8) | frame.data[current_offset + 1];
  decode_position_report(tgt, cur, dev.is_stopped, dev.target, dev.position);
  detail::normalize_stopped_state(dev);

  if (allow_tilt_from_extended_response && device_supports_tilt(dev.type) && frame.data_len >= 15 &&
      frame.data[12] == 0x20) {
    uint16_t const tilt_raw = (frame.data[13] << 8) | frame.data[14];
    dev.tilt = decode_tilt_report(tilt_raw);
  }
}

/// @brief Compute the delay before the next status poll for a private‑response device.
/// @param dev Device record.
/// @param frame The private response frame (may contain a coarse retry hint in byte 7).
/// @return Delay in milliseconds.
uint32_t compute_private_response_delay_ms(const IoDevice &dev, const IoFrame &frame) {
  if (dev.is_stopped) {
    return 3600000;
  }

  // Private responses carry a coarse follow‑up timer in byte 7 on many devices.
  // When it is missing or clearly invalid, fall back to the standard short retry.
  if (frame.data[7] != 0xFF && frame.data[7] != 0x00) {
    return frame.data[7] * 1000 + 1000;
  }
  return 60000;
}

/// @brief Compute the delay before the next status poll for a device‑originated status update.
/// @param dev Device record.
/// @return Delay in milliseconds (long when idle, short while moving).
uint32_t compute_status_update_delay_ms(const IoDevice &dev) { return dev.is_stopped ? 3600000 : 60000; }

}  // namespace

void IOHomeControlComponent::schedule_status_poll_(const std::string &device_id, uint32_t delay_ms) {
  // The timeout name is per-device so repeated remote traffic resets the pending poll instead of
  // stacking multiple delayed callbacks for the same actuator.
  const std::string timeout_name = "remote_poll_" + device_id;
  this->set_timeout(timeout_name.c_str(), delay_ms,
                    [this, device_id]() { this->queue_request_device_status(device_id); });
}

void IOHomeControlComponent::update_device_status_(const IoFrame &frame) {
  const std::string id = node_id_to_string(frame.src);
  auto it = this->devices_.find(id);
  if (it == this->devices_.end()) {
    detail::log_frame_issue(this, "rx", "unregistered_device", frame, frame_length(frame));
    return;
  }
  IoDevice &dev = it->second;

  if (frame.cmd == CMD_PRIVATE_RESP && frame.data_len >= 8) {
    // CMD_PRIVATE_RESP (0x04) serves as the reply to both status polls (0x03) and execute
    // commands (0x00). The position fields below are shared across both response types, so
    // normalize them once here before the entity layer decides how to present the state.
    dev.is_stopped = (frame.data[0] & STATUS_STOPPED) != 0;
    dev.last_status = millis();
    decode_status_fields(dev, frame, 2, 4, true);
    dev.next_update = millis() + compute_private_response_delay_ms(dev, frame);
    detail::log_status_update(id, dev);
    this->notify_device_update_(id);
  } else if (frame.cmd == CMD_STATUS_UPDATE && frame.data_len >= 11) {
    // Status-update frames come from the device itself rather than from a direct controller poll.
    // They use different offsets for the target/current fields and do not carry reliable tilt data.
    dev.is_stopped = (frame.data[0] & STATUS_STOPPED) != 0;
    dev.last_status = millis();
    decode_status_fields(dev, frame, 5, 7, false);
    dev.next_update = millis() + compute_status_update_delay_ms(dev);
    detail::log_status_update(id, dev, " (status update)");
    this->notify_device_update_(id);
  } else if (frame.cmd == CMD_GET_INFO2_RESP && frame.data_len >= 12) {
    // INFO2 is metadata, not movement state. Only learn type from radio if still UNKNOWN;
    // YAML-declared type takes priority.
    if (dev.type == DeviceType::UNKNOWN) {
      dev.type = static_cast<DeviceType>(frame.data[10] << 2 | frame.data[11] >> 6);
      dev.subtype = frame.data[11] & 0x3F;
      if (default_inverted_for_type(dev.type))
        dev.inverted = true;
    }
    ESP_LOGI(detail::TAG, "Device %s: type=%s (%u) class=%s profile=%s subtype=%u", id.c_str(),
             device_type_name(dev.type), (uint8_t) dev.type, device_capability_class_name(dev.type),
             device_operation_profile_name(dev.type), dev.subtype);
  } else if (frame.cmd == CMD_PRIVATE_RESP || frame.cmd == CMD_STATUS_UPDATE || frame.cmd == CMD_GET_INFO2_RESP) {
    detail::log_frame_issue(this, "rx", "unsupported_payload", frame, frame_length(frame));
  }
}

void IOHomeControlComponent::process_received_packet_(const RadioRxPacket &packet) {
  IoFrame frame;
  if (!parse(packet.data, packet.len, frame)) {
    detail::log_component_capture(this->radio_, "parse_fail", packet.data, packet.len);
    return;
  }

  detail::log_component_capture(this->radio_, "parse_ok", packet.data, packet.len, &frame);

  // Exchange-internal frames (0x3C challenge request, 0x3D challenge response) are part of
  // another controller's authenticated exchange. They carry no extractable status data for
  // a passive observer — skip silently. They remain visible in io_capture (stage=parse_ok).
  if (decisions::is_exchange_internal_command(frame.cmd)) {
    return;
  }

  if (frame.cmd == CMD_STATUS_UPDATE && memcmp(frame.dst, this->node_id_, NODE_ID_SIZE) == 0) {
    if (this->authenticate_request_(frame, packet.freq_hz)) {
      IoFrame resp;
      if (!create_status_update_resp(resp, this->node_id_, frame.src)) {
        detail::log_frame_issue(this, "rx", "ack_build_failed", frame, packet.len);
        return;
      }
      // Device-originated updates may arrive while the sender and receiver are not aligned on the
      // same hop channel anymore. Broadcasting the ACK across all three IO-homecontrol channels
      // matched the behavior of real controllers and made updates reliable in practice.
      this->transmit_frame_(resp, FREQ_CH1, SHORT_PREAMBLE);
      this->transmit_frame_(resp, FREQ_CH2, SHORT_PREAMBLE);
      this->transmit_frame_(resp, FREQ_CH3, SHORT_PREAMBLE);
      this->update_device_status_(frame);
    } else {
      detail::log_frame_issue(this, "rx", "auth_failed", frame, packet.len);
    }
    return;
  }

  if (frame.cmd == CMD_PRIVATE_RESP || frame.cmd == CMD_STATUS_UPDATE) {
    // Passive receive mode can still observe replies/status from other exchanges. If a frame
    // is status-bearing and not exchange-internal, try to merge it into known device state.
    this->update_device_status_(frame);
    return;
  }

  // Check if this frame targets one of our registered devices (e.g., a physical remote
  // commanding a shutter we also control). If so, schedule a status poll after 2 seconds
  // to pick up the resulting position change. The timeout name includes the device ID so
  // repeated remote activity resets the timer rather than stacking redundant polls.
  // The 2-second delay gives the device time to complete the exchange and start moving.
  const std::string dst_id = node_id_to_string(frame.dst);
  if (this->get_device(dst_id) != nullptr && memcmp(frame.src, this->node_id_, NODE_ID_SIZE) != 0) {
    ESP_LOGD(detail::TAG, "rx remote_activity src=%s dst=%s cmd=0x%02X, scheduling status poll",
             node_id_to_string(frame.src).c_str(), dst_id.c_str(), frame.cmd);
    this->schedule_status_poll_(dst_id, 2000);
    return;
  }

  // Check if the frame source is a linked remote (e.g., a 1W remote whose destination address
  // differs from the device's 2W ID). When a linked remote is active, schedule status polls
  // for all devices it controls.
  const std::string src_id = node_id_to_string(frame.src);
  auto remote_it = this->linked_remotes_.find(src_id);
  if (remote_it != this->linked_remotes_.end()) {
    for (const auto &device_id : remote_it->second) {
      ESP_LOGD(detail::TAG, "rx remote_activity (linked) remote=%s device=%s cmd=0x%02X, scheduling status poll",
               src_id.c_str(), device_id.c_str(), frame.cmd);
      this->schedule_status_poll_(device_id, 2000);
    }
    return;
  }

  detail::log_frame_issue(this, "rx", "unhandled_cmd", frame, packet.len);
}

}  // namespace home_io_control
}  // namespace esphome