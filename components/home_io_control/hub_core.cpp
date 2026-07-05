/// @file hub_core.cpp
/// @brief Component lifecycle and main-loop scheduling.
/// @ingroup hioc_hub
///
/// The core file owns the parts of IOHomeControlComponent that are primarily about
/// runtime orchestration rather than protocol interpretation:
/// - hardware/radio setup,
/// - main loop scheduling,
/// - device registry and callback fan-out.
///
/// Protocol exchange, pairing, inbound status handling, and outbound operations live
/// in dedicated translation units so this file remains the place to understand how the
/// component is brought up and driven over time.

#include "hub_internal.h"

#include "radio_sx1276.h"
#include "radio_sx1262.h"
#include "tuning_config.h"

#include <new>
#include <vector>

namespace esphome {
namespace home_io_control {

namespace {

constexpr uint32_t BLOCKING_WARNING_THRESHOLD_MS =
    250;  ///< setup() and exchanges can legitimately block longer than generic ESPHome components.
constexpr uint8_t SX1276_VERSION_REGISTER = 0x42;            ///< SX1276 version register used for auto-detection.
constexpr uint8_t SX1276_VERSION_REGISTER_READ_MASK = 0x7F;  ///< SX1276 SPI read transactions clear bit 7.
constexpr uint8_t SX1276_EXPECTED_VERSION = 0x12;            ///< Known chip ID returned by SX1276 silicon.

}  // namespace

static const char *const TAG = detail::TAG;

void IOHomeControlComponent::reset_exchange_debug_(uint8_t request_cmd) {
  this->last_exchange_debug_ = ExchangeDebugInfo{};
  this->last_exchange_debug_.request_cmd = request_cmd;
}

void IOHomeControlComponent::record_exchange_debug_(const char *stage, uint8_t tries, bool saw_challenge) {
  this->last_exchange_debug_.stage = stage;
  this->last_exchange_debug_.tries = tries;
  this->last_exchange_debug_.saw_challenge = this->last_exchange_debug_.saw_challenge || saw_challenge;

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
  ESP_LOGW(detail::TAG,
           "Exchange failed: device=%s cmd=%s(0x%02X) stage=%s tries=%u saw_challenge=%u cap_valid=%u cap_rx_done=%u "
           "cap_crc_err=%u cap_freq=%u cap_irq=0x%04X cap_pkt=0x%02X cap_reported_len=%u cap_frame_len=%u cap_rssi=%d",
           device_id, command_name(debug.request_cmd), debug.request_cmd, debug.stage, debug.tries, debug.saw_challenge,
           debug.capture_valid, debug.capture_rx_done, debug.capture_crc_error, debug.capture_freq_hz,
           debug.capture_irq_status, debug.capture_packet_status, debug.capture_reported_len, debug.capture_frame_len,
           debug.capture_rssi_dbm);
}

// === Setup ===

/// Initialize the IO‑Homecontrol component and radio hardware.
///
/// This is the main setup entry point called by ESPHome during startup.
/// The sequence:
///   1. Parse node_id and system_key from hex strings (fails early if malformed).
///   2. Initialize the SPI bus via spi_setup().
///   3. Auto‑detect or explicitly select the radio chip:
///      - If radio_type is "sx1276" or "sx1262", select that driver.
///      - If empty (default), attempt to read SX1276 version register (0x42 = 0x12).
///        If the read fails or returns wrong version, fall back to SX1262.
///   4. Allocate the appropriate RadioDriver (SX1276 needs DIO0; SX1262 needs BUSY+DIO1).
///   5. Call radio_->init() which performs chip reset, calibration, and register configuration.
///   6. Enter normal loop() operation with radio in RX mode.
///
/// @note Blocking operations in setup() temporarily raise the ESPHome WDT threshold
///       to 250 ms (warn_if_blocking_over_) because radio init can
///       exceed the default 30–50 ms budget.
void IOHomeControlComponent::setup() {
  // IO-homecontrol exchanges are intentionally blocking and often take a few hundred
  // milliseconds, so use a higher warning threshold than ESPHome's generic 30-50 ms.
  this->warn_if_blocking_over_ = BLOCKING_WARNING_THRESHOLD_MS;
  ESP_LOGI(detail::TAG, "Initializing...");
  if (!hex_to_bytes(this->node_id_str_, this->node_id_, NODE_ID_SIZE) ||
      !hex_to_bytes(this->system_key_str_, this->system_key_, AES_KEY_SIZE)) {
    ESP_LOGE(detail::TAG, "Invalid node_id or system_key configuration");
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
    // Auto-detect: read SX1276 version register and compare against the known chip ID.
    // Falls back to SX1262 if the version does not match.
    this->enable();
    this->write_byte(SX1276_VERSION_REGISTER & SX1276_VERSION_REGISTER_READ_MASK);
    uint8_t const version = this->read_byte();
    this->disable();
    if (version == SX1276_EXPECTED_VERSION) {
      ESP_LOGI(detail::TAG, "Auto-detected SX1276 (version=0x%02X)", version);
      use_sx1262 = false;
    } else {
      ESP_LOGI(detail::TAG, "SX1276 not detected (version=0x%02X), trying SX1262", version);
      use_sx1262 = true;
    }
  }

  if (use_sx1262) {
    if (this->busy_pin_ == nullptr || this->dio1_pin_ == nullptr) {
      ESP_LOGE(detail::TAG, "SX1262 requires busy_pin and dio1_pin");
      this->mark_failed();
      return;
    }
    this->radio_ =
        new (std::nothrow) RadioSX1262(this, this->rst_pin_, this->dio1_pin_, this->busy_pin_, this->tx_power_,
                                       this->tcxo_voltage_, this->fem_en_pin_, this->vfem_pin_, this->fem_pa_pin_);
  } else {
    if (this->dio0_pin_ == nullptr) {
      ESP_LOGE(detail::TAG, "SX1276 requires dio0_pin");
      this->mark_failed();
      return;
    }
    this->radio_ = new (std::nothrow)
        RadioSX1276(this, this->rst_pin_, this->dio0_pin_, this->dio4_pin_, this->tx_power_, this->pa_pin_);
  }

  if (this->radio_ == nullptr) {
    ESP_LOGE(detail::TAG, "Failed to allocate %s radio driver", use_sx1262 ? "SX1262" : "SX1276");
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
  this->register_management_actions_();
  this->last_hop_us_ = micros();
  this->apply_tuning_to_radio_();
  if (this->tuning_.active) {
    std::string const snapshot = tuning_config_full_snapshot(this->tuning_);
    ESP_LOGI(detail::TAG, "%s", snapshot.c_str());
  }
  ESP_LOGI(detail::TAG, "Radio initialized (%s), Node ID: %s", use_sx1262 ? "SX1262" : "SX1276",
           this->node_id_str_.c_str());
}

// === Tuning layer ===

/// Apply the current tuning configuration to the active radio driver.
///
/// Only chip-specific parameters are forwarded; the rest are consumed by the
/// pairing flow and LBT logic. This is called once at the end of setup() and
/// again whenever a UI-driven change modifies a radio parameter.
void IOHomeControlComponent::apply_tuning_to_radio_() {
  if (this->radio_ == nullptr)
    return;
  this->radio_->set_rx_bandwidth(this->tuning_.sx1262_rx_bandwidth);
  this->radio_->set_response_preamble(this->tuning_.sx1262_response_preamble);
  this->radio_->set_post_tx_settle_us(this->tuning_.sx1262_post_tx_settle_us);
}

/// Update a numeric tuning parameter from a Home Assistant `number` entity.
///
/// Parses the parameter name and applies the new value to the in-memory tuning
/// configuration. Radio-affecting parameters are forwarded to the active driver
/// immediately; the change is logged in YAML-compatible form so it can be copied
/// back into the configuration file.
void IOHomeControlComponent::update_tuning_number(const std::string &name, float value) {
  if (name == "sx1262_response_preamble") {
    this->tuning_.sx1262_response_preamble = static_cast<uint16_t>(value);
    this->apply_tuning_to_radio_();
  } else if (name == "sx1262_post_tx_settle_us") {
    this->tuning_.sx1262_post_tx_settle_us = static_cast<uint16_t>(value);
    this->apply_tuning_to_radio_();
  } else if (name == "sx1276_discovery_hop_slice_ms") {
    this->tuning_.sx1276_discovery_hop_slice_ms = static_cast<uint16_t>(value);
  } else if (name == "sx1262_discovery_hop_slice_ms") {
    this->tuning_.sx1262_discovery_hop_slice_ms = static_cast<uint16_t>(value);
  } else if (name == "lbt_max_retries") {
    this->tuning_.lbt_max_retries = static_cast<uint8_t>(value);
  } else if (name == "lbt_rssi_threshold_dbm") {
    this->tuning_.lbt_rssi_threshold_dbm = static_cast<int16_t>(value);
  } else if (name == "pairing_discovery_wait_ms") {
    this->tuning_.pairing_discovery_wait_ms = static_cast<uint16_t>(value);
  } else if (name == "pairing_discovery_initial_dwell_ms") {
    this->tuning_.pairing_discovery_initial_dwell_ms = static_cast<uint16_t>(value);
  } else if (name == "pairing_key_exchange_retries") {
    this->tuning_.pairing_key_exchange_retries = static_cast<uint8_t>(value);
  } else {
    ESP_LOGW(detail::TAG, "Unknown tuning number parameter: %s", name.c_str());
    return;
  }
  ESP_LOGI(detail::TAG, "%s", tuning_update_log_line(name, std::to_string(static_cast<int>(value))).c_str());
}

/// Update a select tuning parameter from a Home Assistant `select` entity.
///
/// Parses the selected option string and applies it to the in-memory tuning
/// configuration. Radio-affecting parameters are forwarded to the active driver
/// immediately; the change is logged in YAML-compatible form.
void IOHomeControlComponent::update_tuning_select(const std::string &name, const std::string &value) {
  if (name == "sx1262_rx_bandwidth") {
    auto bw = sx1262_bandwidth_from_string(value);
    if (bw.has_value()) {
      this->tuning_.sx1262_rx_bandwidth = bw.value();
      this->apply_tuning_to_radio_();
    }
  } else if (name == "pairing_discovery_commands") {
    this->tuning_.pairing_discovery_commands.clear();
    // Support both individual commands and comma-separated preset strings.
    size_t start = 0;
    while (start < value.size()) {
      const size_t comma = value.find(',', start);
      std::string token = value.substr(start, comma - start);
      // Trim whitespace.
      token.erase(0, token.find_first_not_of(" \t"));
      token.erase(token.find_last_not_of(" \t") + 1);
      auto cmd = discovery_command_from_string(token);
      if (cmd.has_value())
        this->tuning_.pairing_discovery_commands.push_back(cmd.value());
      if (comma == std::string::npos)
        break;
      start = comma + 1;
    }
  } else if (name == "pairing_discovery_destination") {
    if (value == "auto") {
      this->tuning_.pairing_discovery_destination_auto = true;
    } else if (value == "0x00003B") {
      this->tuning_.pairing_discovery_destination_auto = false;
      this->tuning_.pairing_discovery_destination = {0x00, 0x00, ADDRESS_SUFFIX_DISCOVERY};
    } else if (value == "0x00003F") {
      this->tuning_.pairing_discovery_destination_auto = false;
      this->tuning_.pairing_discovery_destination = {0x00, 0x00, ADDRESS_SUFFIX_BROADCAST};
    }
  } else if (name == "pairing_discovery_payload") {
    if (value == "none") {
      this->tuning_.pairing_discovery_payload_enabled = false;
    } else if (value == "0x00") {
      this->tuning_.pairing_discovery_payload_enabled = true;
      this->tuning_.pairing_discovery_payload = 0x00;
    }
  } else if (name == "pairing_discovery_low_power") {
    this->tuning_.pairing_discovery_low_power = (value == "On");
  } else {
    ESP_LOGW(detail::TAG, "Unknown tuning select parameter: %s", name.c_str());
    return;
  }
  ESP_LOGI(detail::TAG, "%s", tuning_update_log_line(name, value).c_str());
}

/// Return the current value of a numeric tuning parameter.
///
/// Mirror of update_tuning_number(); used by IOHomeTuningNumber::setup() to publish
/// the boot-time value so the Home Assistant slider reflects the active configuration
/// (default or YAML override) without restating any default on the Python side.
float IOHomeControlComponent::get_tuning_number_value(const std::string &name) const {
  if (name == "sx1262_response_preamble")
    return this->tuning_.sx1262_response_preamble;
  if (name == "sx1262_post_tx_settle_us")
    return this->tuning_.sx1262_post_tx_settle_us;
  if (name == "sx1276_discovery_hop_slice_ms")
    return this->tuning_.sx1276_discovery_hop_slice_ms;
  if (name == "sx1262_discovery_hop_slice_ms")
    return this->tuning_.sx1262_discovery_hop_slice_ms;
  if (name == "lbt_max_retries")
    return this->tuning_.lbt_max_retries;
  if (name == "lbt_rssi_threshold_dbm")
    return this->tuning_.lbt_rssi_threshold_dbm;
  if (name == "pairing_discovery_wait_ms")
    return this->tuning_.pairing_discovery_wait_ms;
  if (name == "pairing_discovery_initial_dwell_ms")
    return this->tuning_.pairing_discovery_initial_dwell_ms;
  if (name == "pairing_key_exchange_retries")
    return this->tuning_.pairing_key_exchange_retries;
  ESP_LOGW(detail::TAG, "Unknown tuning number parameter: %s", name.c_str());
  return 0.0F;
}

/// Return the current option string of a select tuning parameter.
///
/// Mirror of update_tuning_select(); used by IOHomeTuningSelect::setup() to publish the
/// boot-time option so the Home Assistant dropdown reflects the active configuration. The
/// returned strings match the YAML/UI option labels exactly. The command list is returned
/// as a comma-separated preset string (e.g. "0x28,0x2E") matching the dropdown options.
std::string IOHomeControlComponent::get_tuning_select_value(const std::string &name) const {
  if (name == "sx1262_rx_bandwidth")
    return sx1262_bandwidth_to_string(this->tuning_.sx1262_rx_bandwidth);
  if (name == "pairing_discovery_commands")
    return discovery_commands_to_csv(this->tuning_.pairing_discovery_commands);
  if (name == "pairing_discovery_destination") {
    return discovery_destination_to_string(this->tuning_.pairing_discovery_destination_auto,
                                           this->tuning_.pairing_discovery_destination.data());
  }
  if (name == "pairing_discovery_payload") {
    return discovery_payload_to_string(this->tuning_.pairing_discovery_payload_enabled,
                                       this->tuning_.pairing_discovery_payload);
  }
  if (name == "pairing_discovery_low_power")
    return this->tuning_.pairing_discovery_low_power ? "On" : "Off";
  ESP_LOGW(detail::TAG, "Unknown tuning select parameter: %s", name.c_str());
  return "";
}

// === Frequency hopping ===

/// Hop to the next channel in the 3‑channel sequence: CH1 → CH2 → CH3 → CH1.
/// Called periodically by the main loop when idle to maintain synchronization
/// with the protocol's ~2.7 ms hopping schedule. Devices also hop, so the controller
/// must hop even while waiting for a response; the next transmit will use whatever
/// channel is current.
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
    detail::log_frame_issue(this, "tx", "serialize_failed", frame, 0);
    return false;
  }
  // LBT: check channel is clear before transmitting
  for (uint8_t lbt = 0; lbt < this->tuning_.lbt_max_retries; lbt++) {
    int16_t const rssi = this->radio_->read_rssi();
    if (rssi < this->tuning_.lbt_rssi_threshold_dbm)
      break;
    ESP_LOGD(detail::TAG, "LBT: channel busy (RSSI %d dBm), retry %u/%u", rssi, lbt + 1, this->tuning_.lbt_max_retries);
    delay(LBT_RETRY_DELAY_MS);
  }
  detail::log_component_capture(this->radio_, "tx_frame", buf, len, &frame);
  RadioTxConfig tx_config{};
  tx_config.freq_hz = freq;
  tx_config.preamble_len = preamble;
  if (!this->radio_->send_packet(buf, len, tx_config)) {
    detail::log_frame_issue(this, "tx", "send_failed", frame, len);
    return false;
  }
  return true;
}

void IOHomeControlComponent::notify_device_update_(const std::string &id) {
  auto it = this->devices_.find(id);
  if (it == this->devices_.end())
    return;
  for (auto &cb : this->callbacks_)
    cb(id, it->second);
}

// === Device management ===

void IOHomeControlComponent::set_device_status_poll_interval(const std::string &device_id, uint32_t poll_interval_ms) {
  auto *dev = this->get_device(device_id);
  if (dev == nullptr)
    return;
  dev->status_poll_interval_ms = poll_interval_ms;
}

void IOHomeControlComponent::add_device(const std::string &device_id) {
  this->add_device(device_id, DeviceType::UNKNOWN, 0, false);
}

void IOHomeControlComponent::add_device(const std::string &device_id, DeviceType type, uint8_t subtype, bool inverted) {
  if (this->devices_.count(device_id) != 0)
    return;
  IoDevice dev{};
  if (!hex_to_bytes(device_id, dev.node_id, NODE_ID_SIZE)) {
    ESP_LOGW(detail::TAG, "Ignoring invalid device ID %s", device_id.c_str());
    return;
  }
  dev.type = type;
  dev.subtype = subtype;
  if (inverted)
    dev.inverted = true;
  this->devices_[device_id] = dev;
}

IoDevice *IOHomeControlComponent::get_device(const std::string &device_id) {
  auto it = this->devices_.find(device_id);
  return (it != this->devices_.end()) ? &it->second : nullptr;
}

// === Main loop ===

void IOHomeControlComponent::loop() {
  if (!this->initialized_)
    return;
  if (this->radio_test_mode_)
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
        bool const should_dispatch_one_shot_poll = pair.second.status_poll_interval_ms == 0;
        if (!should_dispatch_one_shot_poll && !detail::status_poll_tracking_active(pair.second, now)) {
          detail::clear_status_poll_tracking(pair.second);
          continue;
        }
        // Mark the poll as handed to the main-loop queue so an overdue timestamp does not enqueue
        // the same status request again on every loop iteration while the first request is pending.
        pair.second.next_update = 0;
        this->queue_request_device_status(pair.first);
        break;
      }
    }
  }
}

void IOHomeControlComponent::dump_config() {
  ESP_LOGCONFIG(detail::TAG, "IO-Homecontrol:");
  ESP_LOGCONFIG(detail::TAG, "  Node ID: %s", this->node_id_str_.c_str());
  ESP_LOGCONFIG(detail::TAG, "  Radio: %s", this->radio_type_.empty() ? "auto-detected" : this->radio_type_.c_str());
  ESP_LOGCONFIG(detail::TAG, "  TX Power: %u dBm", this->tx_power_);
  LOG_PIN("  RST Pin: ", this->rst_pin_);
  if (this->dio0_pin_ != nullptr)
    LOG_PIN("  DIO0 Pin: ", this->dio0_pin_);
  if (this->dio1_pin_ != nullptr)
    LOG_PIN("  DIO1 Pin: ", this->dio1_pin_);
  if (this->dio4_pin_ != nullptr)
    LOG_PIN("  DIO4 Pin: ", this->dio4_pin_);
  if (this->busy_pin_ != nullptr)
    LOG_PIN("  BUSY Pin: ", this->busy_pin_);
  ESP_LOGCONFIG(detail::TAG, "  Devices: %zu", this->devices_.size());
  if (!this->linked_remotes_.empty()) {
    ESP_LOGCONFIG(detail::TAG, "  Linked Remotes: %zu", this->linked_remotes_.size());
    for (const auto &pair : this->linked_remotes_) {
      for (const auto &device_id : pair.second) {
        ESP_LOGCONFIG(detail::TAG, "    - remote %s -> device %s", pair.first.c_str(), device_id.c_str());
      }
    }
  }

  if (this->radio_ != nullptr)
    this->radio_->dump_debug();
}

}  // namespace home_io_control
}  // namespace esphome
