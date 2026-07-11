/// @file pairing_telemetry.cpp
/// @brief Structured per-attempt telemetry recorder implementation.
/// @ingroup hioc_hub

#include "pairing_telemetry.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace home_io_control {

namespace {

const char *const TAG = "home_io_control.pairing";

/// Buffer size for the frozen `v1;` result sensor string.
constexpr size_t RESULT_SENSOR_STRING_BUFFER_SIZE = 160;

/// Saturation ceiling for the uint8_t retry/attempt counters — a real pairing attempt never
/// gets close to 255 of either, this just guards against wraparound.
constexpr uint8_t COUNTER_SATURATION_MAX = 0xFF;

const char *outcome_name(PairingOutcome outcome) {
  switch (outcome) {
    case PairingOutcome::PAIRED:
      return "paired";
    case PairingOutcome::NO_RESPONSE:
      return "no_response";
    case PairingOutcome::INVALID_RESPONSE:
      return "invalid_response";
    case PairingOutcome::KEY_EXCHANGE_FAILED:
      return "key_exchange_failed";
    case PairingOutcome::CONFIG_FAILED:
      return "config_failed";
    case PairingOutcome::NONE:
    default:
      return "none";
  }
}

const char *event_kind_name(PairingTelemetryEventKind kind) {
  switch (kind) {
    case PairingTelemetryEventKind::TX:
      return "tx";
    case PairingTelemetryEventKind::RX:
      return "rx";
    case PairingTelemetryEventKind::RX_REJECT:
      return "rx_reject";
    case PairingTelemetryEventKind::LBT_DEFER:
      return "lbt_defer";
    case PairingTelemetryEventKind::HOP:
      return "hop";
    case PairingTelemetryEventKind::PHASE:
    default:
      return "phase";
  }
}

}  // namespace

void PairingTelemetry::begin() {
  this->event_count_ = 0;
  this->heard_count_ = 0;
  this->start_ms_ = millis();
  this->end_ms_ = this->start_ms_;
  this->ended_ = false;
  this->phase_ = pairing::PairingState::IDLE;
  this->outcome_ = PairingOutcome::NONE;
  this->discovery_attempts_ = 0;
  this->lbt_retries_ = 0;
  this->has_paired_device_ = false;
  memset(this->paired_node_id_, 0, sizeof(this->paired_node_id_));
  this->paired_device_type_ = DeviceType::UNKNOWN;
  this->advice_codes_.clear();
}

void PairingTelemetry::record_(PairingTelemetryEventKind kind, uint8_t cmd, const uint8_t *src, const uint8_t *dst,
                               int16_t rssi, uint8_t aux, bool oneway) {
  if (kind == PairingTelemetryEventKind::RX || kind == PairingTelemetryEventKind::RX_REJECT) {
    if (this->heard_count_ < UINT16_MAX)
      this->heard_count_++;
  }
  if (this->event_count_ >= PAIRING_TELEMETRY_MAX_EVENTS)
    return;
  PairingTelemetryEvent &event = this->events_[this->event_count_++];
  event.millis_offset = millis() - this->start_ms_;
  event.kind = kind;
  event.cmd = cmd;
  if (src != nullptr) {
    memcpy(event.src_node, src, NODE_ID_SIZE);
  } else {
    memset(event.src_node, 0, NODE_ID_SIZE);
  }
  if (dst != nullptr) {
    memcpy(event.dst_node, dst, NODE_ID_SIZE);
  } else {
    memset(event.dst_node, 0, NODE_ID_SIZE);
  }
  event.rssi = rssi;
  event.aux = aux;
  event.oneway = oneway;
}

void PairingTelemetry::record_tx(uint8_t cmd) {
  this->record_(PairingTelemetryEventKind::TX, cmd, nullptr, nullptr, 0, 0, false);
}

void PairingTelemetry::record_rx(const IoFrame &frame, int16_t rssi) {
  this->record_(PairingTelemetryEventKind::RX, frame.cmd, frame.src, frame.dst, rssi, 0,
                (frame.ctrl0 & CTRL0_PROTOCOL_1W) != 0);
}

void PairingTelemetry::record_rx_reject(const IoFrame &frame, int16_t rssi) {
  this->record_(PairingTelemetryEventKind::RX_REJECT, frame.cmd, frame.src, frame.dst, rssi, 0,
                (frame.ctrl0 & CTRL0_PROTOCOL_1W) != 0);
}

void PairingTelemetry::record_lbt_defer(int16_t rssi) {
  if (this->lbt_retries_ < COUNTER_SATURATION_MAX)
    this->lbt_retries_++;
  this->record_(PairingTelemetryEventKind::LBT_DEFER, 0, nullptr, nullptr, rssi, this->lbt_retries_, false);
}

void PairingTelemetry::record_hop() { this->record_(PairingTelemetryEventKind::HOP, 0, nullptr, nullptr, 0, 0, false); }

void PairingTelemetry::set_phase(pairing::PairingState phase) {
  this->phase_ = phase;
  this->record_(PairingTelemetryEventKind::PHASE, 0, nullptr, nullptr, 0, static_cast<uint8_t>(phase), false);
}

void PairingTelemetry::set_outcome(PairingOutcome outcome) {
  this->outcome_ = outcome;
  this->end_ms_ = millis();
  this->ended_ = true;
}

void PairingTelemetry::set_paired_device(const uint8_t node_id[NODE_ID_SIZE], DeviceType type) {
  this->has_paired_device_ = true;
  memcpy(this->paired_node_id_, node_id, NODE_ID_SIZE);
  this->paired_device_type_ = type;
}

void PairingTelemetry::increment_discovery_attempt() {
  if (this->discovery_attempts_ < COUNTER_SATURATION_MAX)
    this->discovery_attempts_++;
}

uint32_t PairingTelemetry::duration_ms() const { return (this->ended_ ? this->end_ms_ : millis()) - this->start_ms_; }

void PairingTelemetry::log_summary() const {
  ESP_LOGI(TAG, "Pairing attempt summary: outcome=%s phase=%s attempts=%u lbt=%u dur_ms=%u heard=%u events=%u%s",
           outcome_name(this->outcome_), pairing_stage_name(this->phase_), this->discovery_attempts_,
           this->lbt_retries_, this->duration_ms(), this->heard_count_, this->event_count_,
           this->heard_count_ > this->event_count_ ? " (truncated)" : "");
  for (uint8_t i = 0; i < this->event_count_; i++) {
    const PairingTelemetryEvent &event = this->events_[i];
    if (event.kind == PairingTelemetryEventKind::RX || event.kind == PairingTelemetryEventKind::RX_REJECT) {
      ESP_LOGI(TAG, "  [%4u ms] %s cmd=0x%02X src=%s rssi=%d", event.millis_offset, event_kind_name(event.kind),
               event.cmd, node_id_to_string(event.src_node).c_str(), event.rssi);
    } else if (event.kind == PairingTelemetryEventKind::TX) {
      ESP_LOGI(TAG, "  [%4u ms] %s cmd=0x%02X", event.millis_offset, event_kind_name(event.kind), event.cmd);
    } else if (event.kind == PairingTelemetryEventKind::PHASE) {
      ESP_LOGI(TAG, "  [%4u ms] %s -> %s", event.millis_offset, event_kind_name(event.kind),
               pairing_stage_name(static_cast<pairing::PairingState>(event.aux)));
    } else {
      ESP_LOGI(TAG, "  [%4u ms] %s aux=%u rssi=%d", event.millis_offset, event_kind_name(event.kind), event.aux,
               event.rssi);
    }
  }
  if (this->has_paired_device_) {
    ESP_LOGI(TAG, "  paired node=%s type=%s", node_id_to_string(this->paired_node_id_).c_str(),
             device_type_name(this->paired_device_type_));
  }
}

std::string PairingTelemetry::result_sensor_string() const {
  char buf[RESULT_SENSOR_STRING_BUFFER_SIZE];
  const std::string node = this->has_paired_device_ ? node_id_to_string(this->paired_node_id_) : "-";
  const std::string type = this->has_paired_device_ ? device_type_name(this->paired_device_type_) : "-";
  const std::string advice = this->advice_codes_.empty() ? "none" : this->advice_codes_;
  snprintf(buf, sizeof(buf),
           "v1; outcome=%s; phase=%s; node=%s; type=%s; attempts=%u; lbt=%u; dur_ms=%u; heard=%u; advice=%s",
           outcome_name(this->outcome_), pairing_stage_name(this->phase_), node.c_str(), type.c_str(),
           this->discovery_attempts_, this->lbt_retries_, this->duration_ms(), this->heard_count_, advice.c_str());
  return std::string(buf);
}

}  // namespace home_io_control
}  // namespace esphome
