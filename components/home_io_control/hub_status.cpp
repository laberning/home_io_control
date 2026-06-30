#include "hub_internal.h"

#include "hub_decisions.h"
#include "proto_commands.h"

/// @file hub_status.cpp
/// @brief Inbound status handling and passive receive-side state updates.
/// @ingroup hioc_hub
///
/// This file owns the receive-side state path for the hub:
/// - decode status-bearing frames into normalized device state,
/// - decide when passive traffic should arm one-shot or tracked follow-up polls,
/// - ACK authenticated device-initiated status updates.
///
/// @todo Validate the unsolicited CMD_STATUS_UPDATE path on hardware that actually emits
///       device-initiated updates after pairing, including inbound authentication,
///       three-channel ACK broadcast, and Home Assistant state publication without polling.
///
/// The goal of the split is to keep hub_core.cpp focused on lifecycle,
/// device registry, and scheduling while leaving the protocol-specific receive
/// interpretation in one place.

namespace esphome {
namespace home_io_control {

namespace {

constexpr uint8_t PRIVATE_RESPONSE_MIN_DATA_LEN = 8;     ///< Minimum payload length for 0x04 position-bearing replies.
constexpr uint8_t STATUS_UPDATE_MIN_DATA_LEN = 11;       ///< Minimum payload length for 0x71 device-initiated updates.
constexpr uint8_t GET_NAME_RESPONSE_MIN_DATA_LEN = 1;    ///< Minimum payload length for 0x51 name-bearing replies.
constexpr uint8_t GET_INFO2_RESPONSE_MIN_DATA_LEN = 12;  ///< Minimum payload length for 0x57 type/subtype metadata.
constexpr uint8_t ERROR_RESPONSE_MIN_DATA_LEN = 1;       ///< Minimum payload length for 0xFE result-bearing replies.
constexpr uint8_t EXTENDED_TILT_RESPONSE_MIN_DATA_LEN =
    15;  ///< Minimum payload length for tilt-capable extended status replies.
constexpr uint8_t STATUS_STOPPED_FLAGS_OFFSET = 0;         ///< Byte containing STATUS_STOPPED.
constexpr uint8_t PRIVATE_RESPONSE_DELAY_HINT_OFFSET = 7;  ///< Coarse follow-up delay hint byte in many 0x04 replies.
constexpr uint8_t PRIVATE_RESPONSE_TARGET_OFFSET = 2;      ///< Target-position MSB offset in 0x04 replies.
constexpr uint8_t PRIVATE_RESPONSE_CURRENT_OFFSET = 4;     ///< Current-position MSB offset in 0x04 replies.
constexpr uint8_t STATUS_UPDATE_TARGET_OFFSET = 5;         ///< Target-position MSB offset in 0x71 updates.
constexpr uint8_t STATUS_UPDATE_CURRENT_OFFSET = 7;        ///< Current-position MSB offset in 0x71 updates.
constexpr uint8_t GET_INFO2_TYPE_OFFSET = 10;              ///< Packed device type byte in 0x57 replies.
constexpr uint8_t GET_INFO2_TYPE_SUBTYPE_OFFSET = 11;      ///< Packed type low bits plus subtype byte in 0x57 replies.
constexpr uint8_t EXTENDED_TILT_SELECTOR_OFFSET = 12;      ///< Selector byte announcing extended tilt payload.
constexpr uint8_t EXTENDED_TILT_MSB_OFFSET = 13;           ///< Tilt-position MSB within extended replies.
constexpr uint8_t EXTENDED_TILT_LSB_OFFSET = 14;           ///< Tilt-position LSB within extended replies.
constexpr uint8_t PRIVATE_RESPONSE_HINT_UNUSED = 0xFF;  ///< Value used by devices that do not expose a follow-up timer.
constexpr uint8_t PRIVATE_RESPONSE_HINT_ZERO =
    0x00;  ///< Value treated as invalid or uninformative for follow-up timing.
constexpr uint32_t PRIVATE_RESPONSE_HINT_SCALE_MS = 1000;  ///< Private-response delay hint is expressed in seconds.
constexpr uint32_t PRIVATE_RESPONSE_HINT_BIAS_MS =
    1000;  ///< Observed devices need an extra second beyond the hint value.
constexpr uint32_t DEFAULT_SINGLE_FOLLOW_UP_POLL_DELAY_MS =
    60000;  ///< Legacy worst-case settle poll when neither a device hint nor an explicit interval is available.

/// @brief Decode the shared target/current position fields used by private response and status‑update frames.
/// Different frame types use different byte offsets, but the normalization policy is identical once offsets known.
/// @param dev Device record to update.
/// @param frame IoFrame containing a status‑bearing command.
/// @param target_offset Byte offset of target MSB within frame.data.
/// @param current_offset Byte offset of current MSB within frame.data.
/// @param allow_tilt_from_extended_response If true and frame is extended, decode tilt from the extended tilt bytes.
void decode_status_fields(IoDevice &dev, const IoFrame &frame, uint8_t target_offset, uint8_t current_offset,
                          bool allow_tilt_from_extended_response) {
  uint16_t const tgt = (frame.data[target_offset] << 8) | frame.data[target_offset + 1];
  uint16_t const cur = (frame.data[current_offset] << 8) | frame.data[current_offset + 1];
  decode_position_report(tgt, cur, dev.is_stopped, dev.target, dev.position);
  detail::normalize_stopped_state(dev);

  if (allow_tilt_from_extended_response && device_supports_tilt(dev.type) &&
      frame.data_len >= EXTENDED_TILT_RESPONSE_MIN_DATA_LEN &&
      frame.data[EXTENDED_TILT_SELECTOR_OFFSET] == STATUS_TILT_SELECTOR) {
    uint16_t const tilt_raw = (frame.data[EXTENDED_TILT_MSB_OFFSET] << 8) | frame.data[EXTENDED_TILT_LSB_OFFSET];
    dev.tilt = decode_tilt_report(tilt_raw);
  }
}

/// @brief Compute the delay before the next status poll for a private‑response device.
/// @param dev Device record.
/// @param frame The private response frame (may contain a coarse retry hint in byte 7).
/// @return Delay in milliseconds.
uint32_t compute_private_response_delay_ms(const IoDevice &dev, const IoFrame &frame) {
  if (dev.is_stopped)
    return 0;

  // Private responses carry a coarse follow‑up timer in byte 7 on many devices.
  // Delay priority is: device hint when present, otherwise the configured interval, otherwise the
  // legacy one-shot settle delay. When both a hint and an explicit interval exist, cap the next
  // poll to the shorter of the two so YAML cannot stretch a device-reported settle window.
  if (frame.data[PRIVATE_RESPONSE_DELAY_HINT_OFFSET] != PRIVATE_RESPONSE_HINT_UNUSED &&
      frame.data[PRIVATE_RESPONSE_DELAY_HINT_OFFSET] != PRIVATE_RESPONSE_HINT_ZERO) {
    uint32_t const hinted_delay_ms =
        frame.data[PRIVATE_RESPONSE_DELAY_HINT_OFFSET] * PRIVATE_RESPONSE_HINT_SCALE_MS + PRIVATE_RESPONSE_HINT_BIAS_MS;
    if (dev.status_poll_interval_ms == 0)
      return hinted_delay_ms;
    return hinted_delay_ms < dev.status_poll_interval_ms ? hinted_delay_ms : dev.status_poll_interval_ms;
  }
  return dev.status_poll_interval_ms != 0 ? dev.status_poll_interval_ms : DEFAULT_SINGLE_FOLLOW_UP_POLL_DELAY_MS;
}

/// @brief Compute the delay before the next status poll for a device‑originated status update.
/// @param dev Device record.
/// @return Delay in milliseconds for tracked polling; no-interval devices stop after the unsolicited update.
uint32_t compute_status_update_delay_ms(const IoDevice &dev) {
  return dev.is_stopped ? 0 : dev.status_poll_interval_ms;
}

/// @brief Apply a private-response frame to the device record.
/// @param dev Device record to update.
/// @param frame Private-response frame.
void apply_private_response_status(IoDevice &dev, const IoFrame &frame) {
  dev.is_stopped = (frame.data[STATUS_STOPPED_FLAGS_OFFSET] & STATUS_STOPPED) != 0;
  dev.last_status = millis();
  decode_status_fields(dev, frame, PRIVATE_RESPONSE_TARGET_OFFSET, PRIVATE_RESPONSE_CURRENT_OFFSET, true);

  const bool tracked_polling_active = detail::status_poll_tracking_active(dev, dev.last_status);
  if (dev.is_stopped || !tracked_polling_active) {
    if (!dev.is_stopped && dev.single_follow_up_poll_pending) {
      dev.next_update = dev.last_status + compute_private_response_delay_ms(dev, frame);
      dev.single_follow_up_poll_pending = false;
      return;
    }
    detail::clear_status_poll_tracking(dev);
    return;
  }

  dev.next_update = dev.last_status + compute_private_response_delay_ms(dev, frame);
}

/// @brief Apply a device-originated status-update frame to the device record.
/// @param dev Device record to update.
/// @param frame Status-update frame.
void apply_unsolicited_status_update(IoDevice &dev, const IoFrame &frame) {
  dev.is_stopped = (frame.data[STATUS_STOPPED_FLAGS_OFFSET] & STATUS_STOPPED) != 0;
  dev.last_status = millis();
  decode_status_fields(dev, frame, STATUS_UPDATE_TARGET_OFFSET, STATUS_UPDATE_CURRENT_OFFSET, false);

  if (dev.is_stopped || !detail::status_poll_tracking_active(dev, dev.last_status)) {
    detail::clear_status_poll_tracking(dev);
    return;
  }

  dev.next_update = dev.last_status + compute_status_update_delay_ms(dev);
}

/// @brief Apply INFO2 metadata to the device record when YAML has not already declared it.
/// @param dev Device record to update.
/// @param frame INFO2 response frame.
void apply_info2_response(IoDevice &dev, const IoFrame &frame) {
  if (dev.type != DeviceType::UNKNOWN)
    return;

  dev.type = decode_packed_device_type(frame.data[GET_INFO2_TYPE_OFFSET], frame.data[GET_INFO2_TYPE_SUBTYPE_OFFSET]);
  dev.subtype = decode_packed_device_subtype(frame.data[GET_INFO2_TYPE_SUBTYPE_OFFSET]);
  if (default_inverted_for_type(dev.type))
    dev.inverted = true;
}

/// @brief Apply a name response frame to the device record.
/// @param dev Device record to update.
/// @param frame Name response frame.
void apply_name_response(IoDevice &dev, const IoFrame &frame) {
  std::string const name = decode_device_name_payload(frame.data, frame.data_len);
  memset(dev.name, 0, sizeof(dev.name));
  if (!name.empty())
    memcpy(dev.name, name.c_str(), name.length());
}

}  // namespace

void IOHomeControlComponent::begin_status_poll_tracking_(const std::string &device_id, uint32_t initial_delay_ms) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || dev->status_poll_interval_ms == 0)
    return;

  uint32_t const now = millis();
  dev->poll_deadline = now + detail::MAX_TRACKED_STATUS_POLL_WINDOW_MS;
  dev->next_update = initial_delay_ms == 0 ? 0 : now + initial_delay_ms;
  dev->status_poll_failures = 0;
  dev->auth_poll_failures = 0;
}

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

  if (frame.cmd == CMD_PRIVATE_RESP) {
    if (frame.data_len < PRIVATE_RESPONSE_MIN_DATA_LEN) {
      detail::log_frame_issue(this, "rx", "unsupported_payload", frame, frame_length(frame));
      return;
    }

    // CMD_PRIVATE_RESP (0x04) serves as the reply to both status polls (0x03) and execute
    // commands (0x00). The position fields below are shared across both response types, so
    // normalize them once here before the entity layer decides how to present the state.
    apply_private_response_status(dev, frame);
    detail::log_status_update(id, dev);
    this->notify_device_update_(id);
    return;
  }

  if (frame.cmd == CMD_STATUS_UPDATE) {
    if (frame.data_len < STATUS_UPDATE_MIN_DATA_LEN) {
      detail::log_frame_issue(this, "rx", "unsupported_payload", frame, frame_length(frame));
      return;
    }

    // Status-update frames come from the device itself rather than from a direct controller poll.
    // They use different offsets for the target/current fields and do not carry reliable tilt data.
    apply_unsolicited_status_update(dev, frame);

    // The originator byte at data[1] tells us what caused the device to move.
    // Log it so users can understand device-initiated movements (e.g., wind sensor, timer).
    if (frame.data_len > 1) {
      ESP_LOGD(detail::TAG, "Device %s: status update originator=%s(0x%02X)", id.c_str(),
               originator_name(frame.data[1]), frame.data[1]);
    }

    detail::log_status_update(id, dev, " (status update)");
    this->notify_device_update_(id);
    return;
  }

  if (frame.cmd == CMD_GET_NAME_RESP) {
    if (frame.data_len < GET_NAME_RESPONSE_MIN_DATA_LEN) {
      detail::log_frame_issue(this, "rx", "unsupported_payload", frame, frame_length(frame));
      return;
    }

    apply_name_response(dev, frame);
    ESP_LOGI(detail::TAG, "Device %s: name=%s", id.c_str(), dev.name[0] == '\0' ? "" : dev.name);
    this->notify_device_update_(id);
    return;
  }

  if (frame.cmd == CMD_GET_INFO2_RESP) {
    if (frame.data_len < GET_INFO2_RESPONSE_MIN_DATA_LEN) {
      detail::log_frame_issue(this, "rx", "unsupported_payload", frame, frame_length(frame));
      return;
    }

    // INFO2 is metadata, not movement state. Only learn type from radio if still UNKNOWN;
    // YAML-declared type takes priority.
    apply_info2_response(dev, frame);
    ESP_LOGI(detail::TAG, "Device %s: type=%s (%u) class=%s profile=%s subtype=%u", id.c_str(),
             device_type_name(dev.type), (uint8_t) dev.type, device_capability_class_name(dev.type),
             device_operation_profile_name(dev.type), dev.subtype);
    return;
  }

  if (frame.cmd == CMD_ERROR_RESP) {
    if (frame.data_len < ERROR_RESPONSE_MIN_DATA_LEN) {
      detail::log_frame_issue(this, "rx", "unsupported_payload", frame, frame_length(frame));
      return;
    }

    detail::log_command_result(id, frame.data[0]);
    return;
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
    if (auto *dev = this->get_device(dst_id); dev != nullptr)
      dev->single_follow_up_poll_pending = dev->status_poll_interval_ms == 0;
    ESP_LOGD(detail::TAG, "rx remote_activity src=%s dst=%s cmd=%s(0x%02X), scheduling status poll",
             node_id_to_string(frame.src).c_str(), dst_id.c_str(), command_name(frame.cmd), frame.cmd);
    this->begin_status_poll_tracking_(dst_id, 0);
    this->schedule_status_poll_(dst_id, detail::REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS);
    return;
  }

  // Check if the frame source is a linked remote (e.g., a 1W remote whose destination address
  // differs from the device's 2W ID). When a linked remote is active, schedule status polls
  // for all devices it controls.
  const std::string src_id = node_id_to_string(frame.src);
  auto remote_it = this->linked_remotes_.find(src_id);
  if (remote_it != this->linked_remotes_.end()) {
    for (const auto &device_id : remote_it->second) {
      if (auto *dev = this->get_device(device_id); dev != nullptr)
        dev->single_follow_up_poll_pending = dev->status_poll_interval_ms == 0;
      ESP_LOGD(detail::TAG, "rx remote_activity (linked) remote=%s device=%s cmd=%s(0x%02X), scheduling status poll",
               src_id.c_str(), device_id.c_str(), command_name(frame.cmd), frame.cmd);
      this->begin_status_poll_tracking_(device_id, 0);
      this->schedule_status_poll_(device_id, detail::REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS);
    }
    return;
  }

  detail::log_frame_issue(this, "rx", "unhandled_cmd", frame, packet.len);

  // If the command is not in our known set, it may be a protocol extension we haven't documented.
  // Ask the user to report it so we can add support.
  if (std::strcmp(command_name(frame.cmd), "UNKNOWN_CMD") == 0) {
    const std::string src_id = node_id_to_string(frame.src);
    ESP_LOGW(detail::TAG,
             "Received unknown command 0x%02X from %s. "
             "If you see this repeatedly, please file a GitHub issue with this command ID, "
             "your device model, and the log context so protocol support can be extended.",
             frame.cmd, src_id.c_str());
  }
}

}  // namespace home_io_control
}  // namespace esphome