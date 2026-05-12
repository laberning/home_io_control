#include "hub_core.h"
#include "proto_commands.h"
#include "log_frame.h"
#include "radio_sx1276.h"
#include "radio_sx1262.h"
#include "esphome/core/log.h"
#include <new>
#include <string>
#include <cstdio>

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control";
static const uint32_t STATUS_RETRY_AFTER_FAIL_MS = 5000;
// Binary on/off maps to position 0/100.
static const uint8_t BINARY_ENTITY_ON_POSITION = 0;
static const uint8_t BINARY_ENTITY_OFF_POSITION = 100;

static bool is_binary_entity_position(uint8_t position) {
  return position == BINARY_ENTITY_ON_POSITION || position == BINARY_ENTITY_OFF_POSITION;
}

static bool known_device_matches_entity_class(const IoDevice &dev, DeviceCapabilityClass expected) {
  return dev.type == DeviceType::UNKNOWN || device_capability_class(dev.type) == expected;
}

static bool known_device_supports_status_requests(const IoDevice &dev) {
  return dev.type == DeviceType::UNKNOWN || device_supports_status_requests(dev.type);
}

static bool known_device_accepts_execute_position(const IoDevice &dev, uint8_t position) {
  if (dev.type == DeviceType::UNKNOWN)
    return true;
  if (device_supports_position_control(dev.type))
    return true;
  return is_binary_entity_position(position) && device_supports_binary_control(dev.type);
}

static void log_rejected_operation(const std::string &device_id, const IoDevice &dev, const char *operation,
                                   const char *expected) {
  ESP_LOGW(TAG, "Rejecting %s for device %s: type=%s (%u) class=%s profile=%s expected=%s", operation,
           device_id.c_str(), device_type_name(dev.type), static_cast<uint8_t>(dev.type),
           device_capability_class_name(dev.type), device_operation_profile_name(dev.type), expected);
}

static uint32_t saved_device_pref_hash(uint8_t index) {
  char key[16];
  snprintf(key, sizeof(key), "iohome_dev_%u", index);
  return fnv1_hash(key);
}

static uint32_t legacy_saved_device_pref_hash(uint8_t index) { return fnv1_hash("iohome_dev") + index; }

static void log_component_capture(const RadioDriver *radio, const char *stage, const uint8_t *buf, uint8_t len,
                                  const IoFrame *frame = nullptr) {
  const RadioCaptureInfo &capture = radio->get_last_capture();
  char payload_hex[220];
  bytes_to_hex(buf, len, payload_hex, sizeof(payload_hex));
  if (frame != nullptr) {
    ESP_LOGD(
        "io_capture",
        "chip=%s phase=component stage=%s freq=%u ts=%u len=%u cmd=0x%02X src=%02X%02X%02X dst=%02X%02X%02X payload=%s",
        radio->chip_name(), stage, capture.freq_hz, capture.timestamp_ms, len, frame->cmd, frame->src[0], frame->src[1],
        frame->src[2], frame->dst[0], frame->dst[1], frame->dst[2], payload_hex);
    return;
  }
  ESP_LOGD("io_capture", "chip=%s phase=component stage=%s freq=%u ts=%u len=%u payload=%s", radio->chip_name(), stage,
           capture.freq_hz, capture.timestamp_ms, len, payload_hex);
}

static void log_frame_issue(IOHomeControlComponent *component, const char *direction, const char *reason,
                            const IoFrame &frame, uint8_t len) {
  const std::string src_id = node_id_to_string(frame.src);
  const std::string dst_id = node_id_to_string(frame.dst);
  const bool src_registered = component->get_device(src_id) != nullptr;
  const bool dst_registered = component->get_device(dst_id) != nullptr;

  if (src_registered || dst_registered) {
    ESP_LOGW(TAG, "%s issue=%s cmd=0x%02X src=%s%s dst=%s%s len=%u data_len=%u", direction, reason, frame.cmd,
             src_id.c_str(), src_registered ? " (registered)" : "", dst_id.c_str(),
             dst_registered ? " (registered)" : "", len, frame.data_len);
    return;
  }

  ESP_LOGD(TAG, "%s issue=%s cmd=0x%02X src=%s dst=%s len=%u data_len=%u", direction, reason, frame.cmd, src_id.c_str(),
           dst_id.c_str(), len, frame.data_len);
}

void IOHomeControlComponent::reset_exchange_debug_(uint8_t request_cmd) {
  this->last_exchange_debug_ = ExchangeDebugInfo{};
  this->last_exchange_debug_.request_cmd = request_cmd;
}

void IOHomeControlComponent::record_exchange_debug_(const char *stage, uint8_t tries, bool saw_challenge) {
  this->last_exchange_debug_.stage = stage;
  this->last_exchange_debug_.tries = tries;
  this->last_exchange_debug_.saw_challenge = saw_challenge;

  const RadioCaptureInfo &capture = this->radio_->get_last_capture();
  this->last_exchange_debug_.capture_valid = capture.valid;
  this->last_exchange_debug_.capture_rx_done = capture.rx_done;
  this->last_exchange_debug_.capture_crc_error = capture.crc_error;
  this->last_exchange_debug_.capture_freq_hz = capture.freq_hz;
  this->last_exchange_debug_.capture_irq_status = capture.irq_status;
  this->last_exchange_debug_.capture_packet_status = capture.packet_status;
  this->last_exchange_debug_.capture_reported_len = capture.reported_len;
  this->last_exchange_debug_.capture_frame_len = capture.frame_len;
  this->last_exchange_debug_.capture_rssi_dbm = capture.rssi_dbm;
}

void IOHomeControlComponent::log_exchange_debug_(const char *device_id) const {
  const auto &debug = this->last_exchange_debug_;
  ESP_LOGW(TAG,
           "Exchange failed: device=%s cmd=0x%02X stage=%s tries=%u saw_challenge=%u cap_valid=%u cap_rx_done=%u "
           "cap_crc_err=%u cap_freq=%u cap_irq=0x%04X cap_pkt=0x%02X cap_reported_len=%u cap_frame_len=%u cap_rssi=%d",
           device_id, debug.request_cmd, debug.stage, debug.tries, debug.saw_challenge, debug.capture_valid,
           debug.capture_rx_done, debug.capture_crc_error, debug.capture_freq_hz, debug.capture_irq_status,
           debug.capture_packet_status, debug.capture_reported_len, debug.capture_frame_len, debug.capture_rssi_dbm);
}

// === Setup ===

void IOHomeControlComponent::setup() {
  // IO-homecontrol exchanges are intentionally blocking and often take a few hundred
  // milliseconds, so use a higher warning threshold than ESPHome's generic 30-50 ms.
  this->warn_if_blocking_over_ = 250;
  ESP_LOGI(TAG, "Initializing...");
  if (!hex_to_bytes(this->node_id_str_, this->node_id_, NODE_ID_SIZE) ||
      !hex_to_bytes(this->system_key_str_, this->system_key_, AES_KEY_SIZE)) {
    ESP_LOGE(TAG, "Invalid node_id or system_key configuration");
    this->mark_failed();
    return;
  }

  this->spi_setup();

  // --- Radio driver selection ---
  bool use_sx1262 = false;

  if (this->radio_type_ == "sx1262") {
    use_sx1262 = true;
  } else if (this->radio_type_ == "sx1276") {
    use_sx1262 = false;
  } else {
    // Auto-detect: read SX1276 version register (REG_VERSION = 0x42, expected 0x12).
    // Falls back to SX1262 if the version does not match.
    this->enable();
    this->write_byte(0x42 & 0x7F);  // REG_VERSION read
    uint8_t const version = this->read_byte();
    this->disable();
    if (version == 0x12) {
      ESP_LOGI(TAG, "Auto-detected SX1276 (version=0x%02X)", version);
      use_sx1262 = false;
    } else {
      ESP_LOGI(TAG, "SX1276 not detected (version=0x%02X), trying SX1262", version);
      use_sx1262 = true;
    }
  }

  if (use_sx1262) {
    if (this->busy_pin_ == nullptr || this->dio1_pin_ == nullptr) {
      ESP_LOGE(TAG, "SX1262 requires busy_pin and dio1_pin");
      this->mark_failed();
      return;
    }
    this->radio_ =
        new (std::nothrow) RadioSX1262(this, this->rst_pin_, this->dio1_pin_, this->busy_pin_, this->tx_power_,
                                       this->tcxo_voltage_, this->fem_en_pin_, this->vfem_pin_, this->fem_pa_pin_);
  } else {
    if (this->dio0_pin_ == nullptr) {
      ESP_LOGE(TAG, "SX1276 requires dio0_pin");
      this->mark_failed();
      return;
    }
    this->radio_ = new (std::nothrow)
        RadioSX1276(this, this->rst_pin_, this->dio0_pin_, this->dio4_pin_, this->tx_power_, this->pa_pin_);
  }

  if (this->radio_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %s radio driver", use_sx1262 ? "SX1262" : "SX1276");
    this->mark_failed();
    return;
  }

  if (!this->radio_->init()) {
    delete this->radio_;
    this->radio_ = nullptr;
    this->mark_failed();
    return;
  }

  this->initialized_ = true;
  this->last_hop_us_ = micros();
  this->load_devices_();
  ESP_LOGI(TAG, "Radio initialized (%s), Node ID: %s, %zu device(s)", use_sx1262 ? "SX1262" : "SX1276",
           this->node_id_str_.c_str(), this->devices_.size());
}

// === Frequency hopping ===

void IOHomeControlComponent::hop_frequency_() {
  uint32_t const cur = this->radio_->get_current_freq();
  uint32_t next;
  switch (cur) {
    case FREQ_CH1:
      next = FREQ_CH2;
      break;
    case FREQ_CH3:
      next = FREQ_CH1;
      break;
    default:
      next = FREQ_CH3;
      break;
  }
  this->radio_->change_frequency(next);
  this->last_hop_us_ = micros();
}

// === Protocol send/receive ===

// Listen-before-talk (LBT) for ETSI EN 300 220 compliance on 868 MHz SRD band.
// Before transmitting, read instantaneous RSSI to check channel occupancy. If
// above threshold, back off and retry up to LBT_MAX_RETRIES times. If the channel
// remains busy after all retries, transmit anyway — our duty cycle is very low and
// failing silently would be worse than a potential collision.
bool IOHomeControlComponent::transmit_frame_(const IoFrame &frame, uint32_t freq, uint16_t preamble) {
  uint8_t buf[FRAME_MAX_SIZE];
  uint8_t const len = serialize(frame, buf, sizeof(buf));
  if (len == 0) {
    log_frame_issue(this, "tx", "serialize_failed", frame, 0);
    return false;
  }
  // LBT: check channel is clear before transmitting
  for (uint8_t lbt = 0; lbt < LBT_MAX_RETRIES; lbt++) {
    int16_t const rssi = this->radio_->read_rssi();
    if (rssi < LBT_RSSI_THRESHOLD_DBM)
      break;
    ESP_LOGD("home_io_control", "LBT: channel busy (RSSI %d dBm), retry %u/%u", rssi, lbt + 1, LBT_MAX_RETRIES);
    delay(LBT_RETRY_DELAY_MS);
  }
  log_component_capture(this->radio_, "tx_frame", buf, len, &frame);
  RadioTxConfig tx_config{};
  tx_config.freq_hz = freq;
  tx_config.preamble_len = preamble;
  if (!this->radio_->send_packet(buf, len, tx_config)) {
    log_frame_issue(this, "tx", "send_failed", frame, len);
    return false;
  }
  return true;
}

// === Device status ===

void IOHomeControlComponent::update_device_status_(const IoFrame &frame) {
  const std::string id = node_id_to_string(frame.src);
  auto it = this->devices_.find(id);
  if (it == this->devices_.end()) {
    log_frame_issue(this, "rx", "unregistered_device", frame, frame_length(frame));
    return;
  }
  IoDevice &dev = it->second;

  if (frame.cmd == CMD_PRIVATE_RESP && frame.data_len >= 8) {
    // CMD_PRIVATE_RESP (0x04) serves as the reply to both status polls (0x03) and execute
    // commands (0x00). The position fields below are shared across both response types.
    dev.is_stopped = (frame.data[0] & STATUS_STOPPED) != 0;
    dev.last_status = millis();
    uint16_t const tgt = (frame.data[2] << 8) | frame.data[3];
    uint16_t const cur = (frame.data[4] << 8) | frame.data[5];
    decode_position_report(tgt, cur, dev.is_stopped, dev.target, dev.position);
    uint32_t update_delay_ms = 60000;  // default: standard poll interval
    if (dev.is_stopped) {
      update_delay_ms = 3600000;
    } else if (frame.data[7] != 0xFF && frame.data[7] != 0x00) {
      update_delay_ms = frame.data[7] * 1000 + 1000;
    }
    dev.next_update = millis() + update_delay_ms;
    ESP_LOGI(TAG, "Device %s: position=%s target=%s %s", id.c_str(), format_position(dev.position).c_str(),
             format_position(dev.target).c_str(), dev.is_stopped ? "stopped" : "moving");
    this->notify_device_update_(id);
  } else if (frame.cmd == CMD_STATUS_UPDATE && frame.data_len >= 11) {
    dev.is_stopped = (frame.data[0] & STATUS_STOPPED) != 0;
    dev.last_status = millis();
    uint16_t const tgt = (frame.data[5] << 8) | frame.data[6];
    uint16_t const cur = (frame.data[7] << 8) | frame.data[8];
    decode_position_report(tgt, cur, dev.is_stopped, dev.target, dev.position);
    dev.next_update = dev.is_stopped ? millis() + 3600000 : millis() + 60000;
    ESP_LOGI(TAG, "Device %s: position=%s target=%s %s (status update)", id.c_str(),
             format_position(dev.position).c_str(), format_position(dev.target).c_str(),
             dev.is_stopped ? "stopped" : "moving");
    this->notify_device_update_(id);
  } else if (frame.cmd == CMD_GET_INFO2_RESP && frame.data_len >= 12) {
    dev.type = static_cast<DeviceType>(frame.data[10] << 2 | frame.data[11] >> 6);
    dev.subtype = frame.data[11] & 0x3F;
    if (default_inverted_for_type(dev.type))
      dev.inverted = true;
    ESP_LOGI(TAG, "Device %s: type=%s (%u) class=%s profile=%s subtype=%u", id.c_str(), device_type_name(dev.type),
             (uint8_t) dev.type, device_capability_class_name(dev.type), device_operation_profile_name(dev.type),
             dev.subtype);
  } else if (frame.cmd == CMD_PRIVATE_RESP || frame.cmd == CMD_STATUS_UPDATE || frame.cmd == CMD_GET_INFO2_RESP) {
    log_frame_issue(this, "rx", "unsupported_payload", frame, frame_length(frame));
  }
}

void IOHomeControlComponent::process_received_packet_(const RadioRxPacket &packet) {
  IoFrame frame;
  if (!parse(packet.data, packet.len, frame)) {
    log_component_capture(this->radio_, "parse_fail", packet.data, packet.len);
    return;
  }

  log_component_capture(this->radio_, "parse_ok", packet.data, packet.len, &frame);

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
        log_frame_issue(this, "rx", "ack_build_failed", frame, packet.len);
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
      log_frame_issue(this, "rx", "auth_failed", frame, packet.len);
    }
    return;
  }

  if (frame.cmd == CMD_PRIVATE_RESP || frame.cmd == CMD_STATUS_UPDATE) {
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
    ESP_LOGD(TAG, "rx remote_activity src=%s dst=%s cmd=0x%02X, scheduling status poll",
             node_id_to_string(frame.src).c_str(), dst_id.c_str(), frame.cmd);
    const std::string timeout_name = "remote_poll_" + dst_id;
    this->set_timeout(timeout_name.c_str(), 2000, [this, dst_id]() { this->queue_request_device_status(dst_id); });
    return;
  }

  // Check if the frame source is a linked remote (e.g., a 1W remote whose destination address
  // differs from the device's 2W ID). When a linked remote is active, schedule status polls
  // for all devices it controls.
  const std::string src_id = node_id_to_string(frame.src);
  auto remote_it = this->linked_remotes_.find(src_id);
  if (remote_it != this->linked_remotes_.end()) {
    for (const auto &device_id : remote_it->second) {
      ESP_LOGD(TAG, "rx remote_activity (linked) remote=%s device=%s cmd=0x%02X, scheduling status poll",
               src_id.c_str(), device_id.c_str(), frame.cmd);
      const std::string timeout_name = "remote_poll_" + device_id;
      this->set_timeout(timeout_name.c_str(), 2000,
                        [this, device_id]() { this->queue_request_device_status(device_id); });
    }
    return;
  }

  log_frame_issue(this, "rx", "unhandled_cmd", frame, packet.len);
}

void IOHomeControlComponent::notify_device_update_(const std::string &id) {
  auto it = this->devices_.find(id);
  if (it == this->devices_.end())
    return;
  for (auto &cb : this->callbacks_)
    cb(id, it->second);
}

// === Device management ===

void IOHomeControlComponent::add_device(const std::string &device_id) {
  if (this->devices_.count(device_id) != 0)
    return;
  IoDevice dev{};
  if (!hex_to_bytes(device_id, dev.node_id, NODE_ID_SIZE)) {
    ESP_LOGW(TAG, "Ignoring invalid device ID %s", device_id.c_str());
    return;
  }
  this->devices_[device_id] = dev;
}

IoDevice *IOHomeControlComponent::get_device(const std::string &device_id) {
  auto it = this->devices_.find(device_id);
  return (it != this->devices_.end()) ? &it->second : nullptr;
}

// === High-level operations ===

bool IOHomeControlComponent::set_device_position(const std::string &device_id, uint8_t position) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  const char *action = "set position";
  if (position == 0) {
    action = "open";
  } else if (position == 100) {
    action = "close";
  } else if (position == POS_STOP) {
    action = "stop";
  }

  // Once a device family is known, use the profile helpers to reject YAML/entity mismatches
  // before they hit the radio path. Unknown types still pass through so discovery and imported
  // devices keep working as before.
  if (!known_device_accepts_execute_position(*dev, position)) {
    log_rejected_operation(device_id, *dev, action,
                           is_binary_entity_position(position) ? "cover_position or binary_on_off" : "cover_position");
    return false;
  }

  ESP_LOGI(TAG, "Sending %s to device %s (profile=%s)", action, device_id.c_str(),
           device_operation_profile_name(dev->type));

  IoFrame req;
  IoFrame resp;
  if (!create_execute(req, this->node_id_, dev->node_id, true, position))
    return false;
  if (!this->send_and_receive_(req, resp, FREQ_CH2)) {
    this->log_exchange_debug_(device_id.c_str());
    ESP_LOGW(TAG, "No response from device %s", device_id.c_str());
    return false;
  }
  this->update_device_status_(resp);
  return true;
}

bool IOHomeControlComponent::request_device_status(const std::string &device_id) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  if (!known_device_supports_status_requests(*dev)) {
    log_rejected_operation(device_id, *dev, "status request", "status-capable actuator");
    return false;
  }

  IoFrame req;
  IoFrame resp;
  if (!create_get_status(req, this->node_id_, dev->node_id))
    return false;
  if (!this->send_and_receive_(req, resp, FREQ_CH2)) {
    dev->next_update = millis() + STATUS_RETRY_AFTER_FAIL_MS;
    this->log_exchange_debug_(device_id.c_str());
    return false;
  }
  this->update_device_status_(resp);
  return true;
}

bool IOHomeControlComponent::set_light_state(const std::string &device_id, bool on) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  if (!known_device_matches_entity_class(*dev, DeviceCapabilityClass::LIGHT)) {
    log_rejected_operation(device_id, *dev, "light command", "light entity");
    return false;
  }

  // Light entities are binary-only for now, so they intentionally reuse the controller's
  // existing execute path with the proven on/off position encoding.
  return this->set_device_position(device_id, on ? BINARY_ENTITY_ON_POSITION : BINARY_ENTITY_OFF_POSITION);
}

bool IOHomeControlComponent::set_switch_state(const std::string &device_id, bool on) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr || !this->initialized_)
    return false;

  if (!known_device_matches_entity_class(*dev, DeviceCapabilityClass::SWITCH)) {
    log_rejected_operation(device_id, *dev, "switch command", "switch entity");
    return false;
  }

  // Switches share the same transport-level representation as binary lights.
  return this->set_device_position(device_id, on ? BINARY_ENTITY_ON_POSITION : BINARY_ENTITY_OFF_POSITION);
}

void IOHomeControlComponent::queue_set_device_position(const std::string &device_id, uint8_t position) {
  IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !known_device_matches_entity_class(*dev, DeviceCapabilityClass::COVER)) {
    log_rejected_operation(device_id, *dev, "queued cover command", "cover entity");
    return;
  }
  this->pending_operations_.push_back({PendingOperationType::SET_POSITION, device_id, position});
}

void IOHomeControlComponent::queue_request_device_status(const std::string &device_id) {
  IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !known_device_supports_status_requests(*dev)) {
    log_rejected_operation(device_id, *dev, "queued status request", "status-capable actuator");
    return;
  }
  this->pending_operations_.push_back({PendingOperationType::REQUEST_STATUS, device_id, 0});
}

void IOHomeControlComponent::queue_discover_and_pair() {
  for (const auto &operation : this->pending_operations_) {
    if (operation.type == PendingOperationType::DISCOVER_AND_PAIR)
      return;
  }
  this->pending_operations_.push_back({PendingOperationType::DISCOVER_AND_PAIR, {}, 0});
}

void IOHomeControlComponent::queue_set_light_state(const std::string &device_id, bool on) {
  IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !known_device_matches_entity_class(*dev, DeviceCapabilityClass::LIGHT)) {
    log_rejected_operation(device_id, *dev, "queued light command", "light entity");
    return;
  }

  // Queue through the same scheduler as covers so radio work stays serialized while still keeping
  // the light-vs-switch semantics available for capability checks at dispatch time.
  this->pending_operations_.push_back(
      {PendingOperationType::SET_LIGHT_STATE, device_id, on ? BINARY_ENTITY_ON_POSITION : BINARY_ENTITY_OFF_POSITION});
}

void IOHomeControlComponent::queue_set_switch_state(const std::string &device_id, bool on) {
  IoDevice *dev = this->get_device(device_id);
  if (dev != nullptr && !known_device_matches_entity_class(*dev, DeviceCapabilityClass::SWITCH)) {
    log_rejected_operation(device_id, *dev, "queued switch command", "switch entity");
    return;
  }

  // Queue through the same scheduler as covers so radio work stays serialized while still keeping
  // the light-vs-switch semantics available for capability checks at dispatch time.
  this->pending_operations_.push_back(
      {PendingOperationType::SET_SWITCH_STATE, device_id, on ? BINARY_ENTITY_ON_POSITION : BINARY_ENTITY_OFF_POSITION});
}

void IOHomeControlComponent::process_pending_operation_() {
  if (this->busy_ || this->pending_operations_.empty())
    return;

  PendingOperation const operation = std::move(this->pending_operations_.front());
  this->pending_operations_.pop_front();

  switch (operation.type) {
    case PendingOperationType::SET_POSITION:
      this->set_device_position(operation.device_id, operation.position);
      break;
    case PendingOperationType::SET_LIGHT_STATE:
      this->set_light_state(operation.device_id, operation.position == BINARY_ENTITY_ON_POSITION);
      break;
    case PendingOperationType::SET_SWITCH_STATE:
      this->set_switch_state(operation.device_id, operation.position == BINARY_ENTITY_ON_POSITION);
      break;
    case PendingOperationType::REQUEST_STATUS:
      this->request_device_status(operation.device_id);
      break;
    case PendingOperationType::DISCOVER_AND_PAIR:
      this->discover_and_pair();
      break;
  }
}

// === Main loop ===

void IOHomeControlComponent::loop() {
  if (!this->initialized_)
    return;

  // Check for received packets (non-blocking)
  if (!this->busy_) {
    RadioRxPacket packet{};
    if (this->radio_->check_for_packet(packet))
      this->process_received_packet_(packet);
  }

  if (!this->busy_)
    this->process_pending_operation_();

  // Frequency hopping — protocol specifies 2.7ms per channel, but ESPHome calls
  // loop() every ~16-30ms. This is acceptable for a controller: we initiate all
  // exchanges with a long preamble (1024 bytes ≈ 330ms airtime) so the device has
  // time to detect us regardless of channel alignment. Precise hopping would only
  // matter for a passive receiver scanning for unsolicited frames.
  if (!this->busy_ && (micros() - this->last_hop_us_) > HOP_TIME_US)
    this->hop_frequency_();

  // Periodic status polling
  if (!this->busy_) {
    uint32_t const now = millis();
    for (auto &pair : this->devices_) {
      if (pair.second.next_update != 0 && now > pair.second.next_update) {
        this->queue_request_device_status(pair.first);
        break;
      }
    }
  }
}

void IOHomeControlComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "IO-Homecontrol:");
  ESP_LOGCONFIG(TAG, "  Node ID: %s", this->node_id_str_.c_str());
  ESP_LOGCONFIG(TAG, "  Radio: %s", this->radio_type_.empty() ? "auto-detected" : this->radio_type_.c_str());
  ESP_LOGCONFIG(TAG, "  TX Power: %u dBm", this->tx_power_);
  LOG_PIN("  RST Pin: ", this->rst_pin_);
  if (this->dio0_pin_ != nullptr)
    LOG_PIN("  DIO0 Pin: ", this->dio0_pin_);
  if (this->dio1_pin_ != nullptr)
    LOG_PIN("  DIO1 Pin: ", this->dio1_pin_);
  if (this->dio4_pin_ != nullptr)
    LOG_PIN("  DIO4 Pin: ", this->dio4_pin_);
  if (this->busy_pin_ != nullptr)
    LOG_PIN("  BUSY Pin: ", this->busy_pin_);
  ESP_LOGCONFIG(TAG, "  Devices: %zu", this->devices_.size());
  for (const auto &pair : this->devices_) {
    const auto &device = pair.second;
    ESP_LOGCONFIG(TAG, "    - %s: type=%s (%u) class=%s profile=%s subtype=%u inverted=%s", pair.first.c_str(),
                  device_type_name(device.type), static_cast<uint8_t>(device.type),
                  device_capability_class_name(device.type), device_operation_profile_name(device.type), device.subtype,
                  YESNO(device.inverted));
  }
  if (!this->linked_remotes_.empty()) {
    ESP_LOGCONFIG(TAG, "  Linked Remotes: %zu", this->linked_remotes_.size());
    for (const auto &pair : this->linked_remotes_) {
      for (const auto &device_id : pair.second) {
        ESP_LOGCONFIG(TAG, "    - remote %s -> device %s", pair.first.c_str(), device_id.c_str());
      }
    }
  }

  if (this->radio_ != nullptr)
    this->radio_->dump_debug();
}

// === Persistence ===

struct SavedDevice {
  uint8_t node_id[NODE_ID_SIZE];
  uint8_t type;
  uint8_t subtype;
  bool inverted;
};

void IOHomeControlComponent::save_devices_() {
  uint8_t const count = std::min((uint8_t) this->devices_.size(), (uint8_t) 16);
  auto pref_count = global_preferences->make_preference<uint8_t>(fnv1_hash("iohome_dev_count"));
  pref_count.save(&count);
  uint8_t i = 0;
  for (auto &pair : this->devices_) {
    if (i >= 16)
      break;
    SavedDevice sd{};
    memcpy(sd.node_id, pair.second.node_id, NODE_ID_SIZE);
    sd.type = (uint8_t) pair.second.type;
    sd.subtype = pair.second.subtype;
    sd.inverted = pair.second.inverted;
    auto pref = global_preferences->make_preference<SavedDevice>(saved_device_pref_hash(i));
    pref.save(&sd);
    i++;
  }
  global_preferences->sync();
}

void IOHomeControlComponent::load_devices_() {
  uint8_t count = 0;
  auto pref_count = global_preferences->make_preference<uint8_t>(fnv1_hash("iohome_dev_count"));
  if (!pref_count.load(&count))
    return;
  for (uint8_t i = 0; i < count && i < 16; i++) {
    SavedDevice sd{};
    auto pref = global_preferences->make_preference<SavedDevice>(saved_device_pref_hash(i));
    bool const loaded = pref.load(&sd);
    if (!loaded) {
      auto legacy_pref = global_preferences->make_preference<SavedDevice>(legacy_saved_device_pref_hash(i));
      if (!legacy_pref.load(&sd))
        continue;
    }
    // Ignore obviously invalid persisted IDs so stale or partially written flash data does not
    // resurrect phantom devices that the runtime can never communicate with.
    if (!persisted_node_id_is_valid(sd.node_id)) {
      ESP_LOGW(TAG, "Skipping invalid persisted device entry %u", i);
      continue;
    }
    std::string const id(node_id_to_string(sd.node_id));
    if (this->devices_.count(id) != 0) {
      auto &dev = this->devices_[id];
      if (dev.type == DeviceType::UNKNOWN)
        dev.type = static_cast<DeviceType>(sd.type);
      // Persisted devices may have been saved before we added automatic inversion defaults for
      // newly recognized families, so recompute them during restore as well.
      if (sd.inverted || default_inverted_for_type(static_cast<DeviceType>(sd.type)))
        dev.inverted = true;
      dev.subtype = sd.subtype;
    } else {
      IoDevice dev{};
      memcpy(dev.node_id, sd.node_id, NODE_ID_SIZE);
      dev.type = static_cast<DeviceType>(sd.type);
      dev.subtype = sd.subtype;
      dev.inverted = sd.inverted || default_inverted_for_type(dev.type);
      this->devices_[id] = dev;
    }
  }
}

}  // namespace home_io_control
}  // namespace esphome
