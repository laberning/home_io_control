#include "hub_pairing.h"

#include "hub_decisions.h"
#include "hub_core.h"
#include "hub_internal.h"
#include "proto_commands.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

/// @file hub_pairing.cpp
/// @brief Device pairing orchestration — discovery, key exchange, and finalization.
/// @ingroup hioc_hub
///
/// Implements IOHomeControlComponent::discover_and_pair() and the three-phase
/// helper methods: run_discovery_phase_(), run_key_exchange_phase_(),
/// finalize_pairing_configuration_(), plus low-level waiters
/// wait_for_discovery_response_() and wait_for_key_challenge_().
/// Also contains parse_device_from_discovery() to extract device metadata from
/// discovery frames.
///
/// Pairing is separated from the normal exchange path to keep authenticated
/// command execution independent from the one-time key-establishment flow.
/// @todo Validate the full discovery and re-pair flow on freshly reset SX1276-backed devices,
///       including discovery response capture, key exchange, and the final configuration step.
/// @todo Validate the full discovery and re-pair flow on freshly reset SX1262-backed devices,
///       including discovery response capture, key exchange, and the final configuration step.
/// @todo Add first-class platform coverage for additional paired device classes once real
///       hardware is available to validate their command semantics and status reporting.

namespace esphome {
namespace home_io_control {

namespace {

const char *const TAG = "home_io_control";
constexpr size_t DEVICE_TYPE_HEX_STRING_BUFFER_SIZE = 8;  ///< Buffer for strings such as "0x11" plus terminator.

/// Map PairingState enum to string for debug logging.
const char *pairing_stage_name(pairing::PairingState state) {
  switch (state) {
    case pairing::PairingState::IDLE:
      return "idle";
    case pairing::PairingState::TX_DISCOVER:
      return "tx_discover";
    case pairing::PairingState::WAIT_DISCOVER_RESPONSE:
      return "wait_discover_response";
    case pairing::PairingState::TX_KEY_INIT:
      return "tx_key_init";
    case pairing::PairingState::WAIT_KEY_CHALLENGE:
      return "wait_key_challenge";
    case pairing::PairingState::TX_KEY_TRANSFER:
      return "tx_key_transfer";
    case pairing::PairingState::WAIT_KEY_CONFIRM:
      return "wait_key_confirm";
    case pairing::PairingState::REGISTER_DEVICE:
      return "register_device";
    case pairing::PairingState::COMPLETE:
      return "complete";
    case pairing::PairingState::FAILED:
    default:
      return "failed";
  }
}

// response_wait_slice_ms provided by decisions namespace (hub_decisions.h)

/// Check if frame is a 0x33 key‑confirm message.
bool frame_is_key_confirm(const IoFrame &frame) { return frame.cmd == CMD_KEY_CONFIRM; }

/// Log discovery‑phase failure reason based on disposition.
///
/// Called by `discover_and_pair()` when the discovery phase does not return
/// ACCEPT. The messages distinguish between no traffic at all and traffic that
/// was seen but no valid discovery response arrived.
///
/// @param disp Discovery disposition value (NO_RESPONSE or INVALID).
void log_discovery_diagnostic(decisions::PairingDiscoveryDisposition disp) {
  switch (disp) {
    case decisions::PairingDiscoveryDisposition::NO_RESPONSE:
      ESP_LOGW(TAG, "No device responded to discovery");
      break;
    case decisions::PairingDiscoveryDisposition::INVALID:
      ESP_LOGW(TAG, "No valid discovery response received");
      break;
    case decisions::PairingDiscoveryDisposition::ACCEPT:
      break;
  }
}

/// Format a raw device type as hexadecimal for YAML and diagnostics.
std::string format_device_type_hex(DeviceType type) {
  char buf[DEVICE_TYPE_HEX_STRING_BUFFER_SIZE];
  snprintf(buf, sizeof(buf), "0x%02X", static_cast<uint8_t>(type));
  return std::string(buf);
}

/// Return a human-readable device type string including the raw numeric value.
std::string format_device_type_diagnostic(DeviceType type) {
  const char *name = device_type_name(type);
  std::string raw = format_device_type_hex(type);
  if (name != nullptr && strcmp(name, "unknown") != 0) {
    return std::string(name) + " (" + raw + ")";
  }
  return raw;
}

/// Build the YAML value for io_device_type.
/// Supported symbolic aliases stay readable; all other types fall back to raw hex.
std::string format_device_type_for_yaml(DeviceType type) {
  const char *name = yaml_device_type_name(type);
  if (name != nullptr) {
    return std::string("\"") + name + "\"";
  }
  return format_device_type_hex(type);
}

/// Map a supported capability class to the corresponding YAML platform name.
const char *pairing_platform_name(DeviceCapabilityClass capability_class) {
  switch (capability_class) {
    case DeviceCapabilityClass::COVER:
      return "cover";
    case DeviceCapabilityClass::LIGHT:
      return "light";
    case DeviceCapabilityClass::SWITCH:
      return "switch";
    default:
      return nullptr;
  }
}

}  // namespace

// --- Pairing helpers ---

/// Wait for a valid discovery response (0x29) within the timeout.
///
/// Listens for incoming packets and parses them. Frames that parse successfully
/// are passed to `decisions::classify_pairing_discovery_response()`. Only a
/// frame classified as ACCEPT (CMD_DISCOVER_RESP) returns true. All other
/// frames are ignored until the deadline expires.
///
/// The function distinguishes between NO_RESPONSE (no packets seen at all) and
/// INVALID (some packets seen but none were valid discovery responses).
///
/// Frequency hopping: hops between the 3 IO-homecontrol channels after each
/// wait slice (5ms for SX1276 with FastHop, 50ms for SX1262 whose frequency
/// change requires a full standby→SetRf→RX cycle). When preamble or sync word
/// detection fires, the dwell is extended to 15ms so the incoming frame can
/// complete without being interrupted by a hop.
decisions::PairingDiscoveryDisposition IOHomeControlComponent::wait_for_discovery_response_(uint32_t timeout_ms,
                                                                                            RadioRxPacket &packet,
                                                                                            IoFrame &response_frame) {
  // SX1262 frequency changes require standby→SetRf→RX (~10-15ms), so the listen
  // slice must be long enough that we spend more time listening than switching.
  // SX1276 uses FastHop (no standby needed), so short slices are fine.
  const uint32_t hop_slice_ms = strcmp(this->radio_->chip_name(), "sx1262") == 0
                                    ? detail::SX1262_PAIRING_DISCOVERY_HOP_SLICE_MS
                                    : detail::PAIRING_DISCOVERY_HOP_SLICE_MS;
  static constexpr uint32_t PREAMBLE_DWELL_MS = 15;

  // Helper: attempt to receive and classify a discovery response.
  auto try_accept = [&]() {
    return parse(packet.data, packet.len, response_frame) &&
           decisions::classify_pairing_discovery_response(response_frame) ==
               decisions::PairingDiscoveryDisposition::ACCEPT;
  };

  bool saw_traffic = false;
  const uint32_t deadline = millis() + timeout_ms;
  while ((int32_t) (deadline - millis()) > 0) {
    const uint32_t slice = std::min((uint32_t) (deadline - millis()), hop_slice_ms);
    if (this->radio_->wait_for_packet(packet, slice)) {
      saw_traffic = true;
      if (try_accept())
        return decisions::PairingDiscoveryDisposition::ACCEPT;
      // Non-discovery frame (e.g., beacon) — hop only if no further signal activity
      // is present, otherwise stay to catch a back-to-back frame on this channel.
      if ((int32_t) (deadline - millis()) > 0 && !this->radio_->is_preamble_detected() &&
          !this->radio_->is_sync_detected()) {
        this->hop_frequency_();
      }
      continue;
    }
    // No complete packet within slice — decide whether to hop or dwell.
    if ((int32_t) (deadline - millis()) <= 0)
      break;
    if (!this->radio_->is_preamble_detected() && !this->radio_->is_sync_detected()) {
      this->hop_frequency_();
      continue;
    }
    // Signal activity detected — stay on this channel with extended dwell.
    const uint32_t ext = std::min((uint32_t) (deadline - millis()), PREAMBLE_DWELL_MS);
    if (this->radio_->wait_for_packet(packet, ext)) {
      saw_traffic = true;
      if (try_accept())
        return decisions::PairingDiscoveryDisposition::ACCEPT;
    }
  }
  return saw_traffic ? decisions::PairingDiscoveryDisposition::INVALID
                     : decisions::PairingDiscoveryDisposition::NO_RESPONSE;
}

/// Wait for a valid key‑challenge frame (0x3C) or key‑confirm (0x33) from the target device.
///
/// During key exchange the device typically responds to our key‑init with a random 6‑byte
/// challenge (0x3C). However, some devices (particularly on SX1262 where preamble timing
/// differs) may skip the challenge and send 0x33 (key confirm) directly — indicating
/// immediate key acceptance without requiring 0x32 key transfer.
/// Both responses are accepted; the caller checks which was received.
bool IOHomeControlComponent::wait_for_key_challenge_(uint32_t timeout_ms, RadioRxPacket &packet,
                                                     IoFrame &challenge_frame,
                                                     const uint8_t device_node_id[NODE_ID_SIZE]) {
  bool saw_traffic = false;
  const uint32_t deadline = millis() + timeout_ms;
  while ((int32_t) (deadline - millis()) > 0) {
    const uint32_t remaining_ms = deadline - millis();
    const uint32_t slice = decisions::response_wait_slice_ms(remaining_ms);
    if (!this->radio_->wait_for_packet(packet, slice))
      continue;
    saw_traffic = true;
    if (!parse(packet.data, packet.len, challenge_frame))
      continue;
    // Accept 0x3C (challenge) or 0x33 (immediate confirm) from the target device to us
    if (challenge_frame.cmd == CMD_KEY_CONFIRM && memcmp(challenge_frame.src, device_node_id, NODE_ID_SIZE) == 0 &&
        memcmp(challenge_frame.dst, this->node_id_, NODE_ID_SIZE) == 0)
      return true;
    if (decisions::classify_pairing_key_challenge(challenge_frame, device_node_id, this->node_id_) !=
        decisions::PairingKeyChallengeDisposition::ACCEPT)
      continue;
    return true;
  }
  ESP_LOGW(TAG, saw_traffic ? "Key exchange: no valid challenge received" : "Key exchange: no challenge received");
  return false;
}

/// Transmit the 0x32 key transfer and wait for 0x33 key confirm.
///
/// Uses a dedicated wait loop with frequency hopping and the platform's
/// response preamble length. SX1262 needs a longer preamble than the reference
/// SHORT_PREAMBLE for the device to lock on. Retries up to EXCHANGE_RETRY_COUNT
/// times on timeout.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool IOHomeControlComponent::wait_for_key_confirm_(pairing::PairingContext &context) {
  for (uint8_t tries = 0; tries < EXCHANGE_RETRY_COUNT; tries++) {
    if (tries > 0) {
      App.feed_wdt();
      delay(EXCHANGE_RETRY_DELAY_MS);
    }
    if (!this->transmit_frame_(context.req, FREQ_CH2, this->radio_->response_preamble()))
      continue;

    const uint32_t deadline = millis() + detail::PAIRING_KEY_CONFIRM_TIMEOUT_MS;
    bool saw_any = false;
    while ((int32_t) (deadline - millis()) > 0) {
      const uint32_t remaining = deadline - millis();
      const uint32_t slice = std::min<uint32_t>(remaining, detail::PAIRING_KEY_CONFIRM_SLICE_MS);
      if (!this->radio_->wait_for_packet(context.packet, slice)) {
        if ((int32_t) (deadline - millis()) > 0)
          this->hop_frequency_();
        continue;
      }
      saw_any = true;
      ESP_LOGD(TAG, "Key confirm wait: got %u bytes on freq=%u", context.packet.len, context.packet.freq_hz);
      if (!parse(context.packet.data, context.packet.len, context.resp)) {
        ESP_LOGD(TAG, "Key confirm wait: parse failed");
        continue;
      }
      ESP_LOGD(TAG, "Key confirm wait: parsed cmd=0x%02X src=%02X%02X%02X dst=%02X%02X%02X", context.resp.cmd,
               context.resp.src[0], context.resp.src[1], context.resp.src[2], context.resp.dst[0], context.resp.dst[1],
               context.resp.dst[2]);
      if (!decisions::frame_matches_exchange_endpoints(context.req, context.resp))
        continue;
      if (frame_is_key_confirm(context.resp))
        return true;
      ESP_LOGW(TAG, "Key transfer: device responded with cmd=%s(0x%02X) (expected KEY_CONFIRM 0x33)",
               command_name(context.resp.cmd), context.resp.cmd);
      if (context.resp.cmd == CMD_ERROR_RESP && context.resp.data_len > 0)
        ESP_LOGW(TAG, "Key transfer: error code=0x%02X", context.resp.data[0]);
      return false;
    }
    ESP_LOGI(TAG, "Try %d ended: no response for key transfer (0x32) within %u ms (saw_any=%d)", tries + 1,
             detail::PAIRING_KEY_CONFIRM_TIMEOUT_MS, saw_any);
  }
  return false;
}

/// Parse a discovery response frame into device metadata and ID.
///
/// Extracts node ID, device type, and subtype from a CMD_DISCOVER_RESP frame.
/// The two-byte payload uses the shared packed device metadata layout defined in proto_frame.h.
/// When the full 9-byte discovery payload is present, also logs the manufacturer name
/// and backbone address for diagnostic purposes.
/// The inversion flag is derived from the type via `default_inverted_for_type()`.
void IOHomeControlComponent::parse_device_from_discovery(const IoFrame &frame, IoDevice &device,
                                                         std::string &device_id) {
  memcpy(device.node_id, frame.src, NODE_ID_SIZE);
  if (frame.data_len >= DEVICE_METADATA_SIZE) {
    device.type = decode_packed_device_type(frame.data[0], frame.data[1]);
    device.subtype = decode_packed_device_subtype(frame.data[1]);
    device.inverted = default_inverted_for_type(device.type);
  } else {
    device.type = DeviceType::UNKNOWN;
    device.subtype = 0;
    device.inverted = false;
  }
  device.position = UNKNOWN_POSITION;
  device.target = UNKNOWN_POSITION;
  device.is_stopped = true;
  device_id = node_id_to_string(device.node_id);

  // Log extended discovery fields when the full payload is present
  if (frame.data_len > DISCOVERY_RESP_MANUFACTURER_OFFSET) {
    uint8_t const mfr_id = frame.data[DISCOVERY_RESP_MANUFACTURER_OFFSET];
    const char *mfr_name = manufacturer_name(mfr_id);
    ESP_LOGI(TAG, "Discovery: device %s manufacturer=%u (%s)", device_id.c_str(), mfr_id, mfr_name);
    if (mfr_id == 0 || mfr_id > MANUFACTURER_ID_MAX) {
      ESP_LOGW(TAG,
               "Unknown manufacturer ID %u reported by device %s. "
               "Please file a GitHub issue with this ID and your device model so support can be added.",
               mfr_id, device_id.c_str());
    }
  }
  if (frame.data_len > DISCOVERY_RESP_BACKBONE_OFFSET + NODE_ID_SIZE - 1) {
    ESP_LOGD(TAG, "Discovery: backbone=%02X%02X%02X", frame.data[DISCOVERY_RESP_BACKBONE_OFFSET],
             frame.data[DISCOVERY_RESP_BACKBONE_OFFSET + 1], frame.data[DISCOVERY_RESP_BACKBONE_OFFSET + 2]);
  }
  if (frame.data_len > DISCOVERY_RESP_FLAGS_OFFSET) {
    uint8_t const flags = frame.data[DISCOVERY_RESP_FLAGS_OFFSET];
    uint8_t const att = (flags & DISCOVERY_FLAGS_ATT_MASK) >> DISCOVERY_FLAGS_ATT_SHIFT;
    uint8_t const power_save = flags & DISCOVERY_FLAGS_POWER_SAVE_MASK;
    ESP_LOGI(TAG, "Discovery: device %s turnaround=%s power_save=%s flags=0x%02X", device_id.c_str(),
             att_class_name(att), power_save_mode_name(power_save), flags);
    if (power_save == POWER_SAVE_LOW_POWER) {
      ESP_LOGI(TAG,
               "Device %s reports low-power mode. "
               "Consider adding 'low_power: true' to YAML if commands are unreliable.",
               device_id.c_str());
    }
  }
}

/// Execute Phase 1: discover a pairable device on channel 2.
///
/// Transmits a discovery broadcast (0x28) up to PAIRING_DISCOVERY_MAX_ATTEMPTS times,
/// waiting up to PAIRING_DISCOVERY_RESPONSE_TIMEOUT_MS after each TX for a valid
/// discovery response (0x29). On success the response is parsed into
/// `context.device` and `context.device_id` and the function returns ACCEPT.
/// If all attempts fail, returns NO_RESPONSE or INVALID.
decisions::PairingDiscoveryDisposition IOHomeControlComponent::run_discovery_phase_(pairing::PairingContext &context) {
  for (uint8_t attempt = 1; attempt <= detail::PAIRING_DISCOVERY_MAX_ATTEMPTS; ++attempt) {
    context.state = pairing::PairingState::TX_DISCOVER;
    this->record_exchange_debug_(pairing_stage_name(context.state), attempt, false);
    if (!create_discover(context.req, this->node_id_) || !this->transmit_frame_(context.req, FREQ_CH2, LONG_PREAMBLE)) {
      return decisions::PairingDiscoveryDisposition::NO_RESPONSE;
    }

    context.state = pairing::PairingState::WAIT_DISCOVER_RESPONSE;
    this->record_exchange_debug_(pairing_stage_name(context.state), attempt, false);
    auto result =
        this->wait_for_discovery_response_(detail::PAIRING_DISCOVERY_RESPONSE_TIMEOUT_MS, context.packet, context.rx);
    if (result == decisions::PairingDiscoveryDisposition::ACCEPT) {
      parse_device_from_discovery(context.rx, context.device, context.device_id);
      context.discovery_metadata_complete = context.rx.data_len >= DEVICE_METADATA_SIZE;
      return result;
    }

    if (attempt < detail::PAIRING_DISCOVERY_MAX_ATTEMPTS) {
      ESP_LOGI(TAG, "Discovery attempt %u/%u: no response, retrying...", attempt,
               detail::PAIRING_DISCOVERY_MAX_ATTEMPTS);
    }
  }
  return decisions::PairingDiscoveryDisposition::NO_RESPONSE;
}

/// Execute Phase 2: perform authenticated key exchange with the discovered device.
///
/// Performs the full four‑step key exchange:
///   1. TX_KEY_INIT   — send key‑init frame (0x31) to device
///   2. WAIT_KEY_CHALLENGE — wait for device's challenge (0x3C)
///   3. TX_KEY_TRANSFER — send challenge response + our key share (0x32)
///   4. WAIT_KEY_CONFIRM — wait for device's key‑confirm (0x33) using the
///      normal authenticated exchange (`send_and_receive_`) to inherit retry logic.
///
/// Each step updates the pairing state for observability. Any failure returns
/// false immediately; the orchestrator will clean up and abort pairing.
bool IOHomeControlComponent::run_key_exchange_phase_(pairing::PairingContext &context) {
  context.state = pairing::PairingState::TX_KEY_INIT;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, false);
  if (!create_key_init(context.key_init, this->node_id_, context.device.node_id) ||
      !this->transmit_frame_(context.key_init, FREQ_CH2, LONG_PREAMBLE)) {
    return false;
  }

  context.state = pairing::PairingState::WAIT_KEY_CHALLENGE;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, true);
  if (!this->wait_for_key_challenge_(detail::PAIRING_KEY_CHALLENGE_TIMEOUT_MS, context.packet, context.rx,
                                     context.device.node_id)) {
    return false;
  }

  // Some devices send 0x33 (key confirm) directly after 0x31 without requiring 0x32.
  // If we received 0x33 instead of 0x3C, pairing is already complete.
  if (context.rx.cmd == CMD_KEY_CONFIRM) {
    ESP_LOGI(TAG, "Device accepted key immediately (0x33 without 0x32 exchange)");
    context.resp = context.rx;
    return true;
  }

  // Debug: log the challenge bytes we received from the device
  ESP_LOGI(TAG, "Challenge (0x3C) received: data_len=%u bytes=[%02X %02X %02X %02X %02X %02X] freq=%u rssi=%d",
           context.rx.data_len, context.rx.data_len > 0 ? context.rx.data[0] : 0,
           context.rx.data_len > 1 ? context.rx.data[1] : 0, context.rx.data_len > 2 ? context.rx.data[2] : 0,
           context.rx.data_len > 3 ? context.rx.data[3] : 0, context.rx.data_len > 4 ? context.rx.data[4] : 0,
           context.rx.data_len > 5 ? context.rx.data[5] : 0, context.packet.freq_hz,
           this->radio_->get_last_capture().rssi_dbm);

  context.state = pairing::PairingState::TX_KEY_TRANSFER;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, true);
  if (!create_key_transfer(context.req, context.key_init, context.device.node_id, this->node_id_, this->system_key_,
                           context.rx.data)) {
    return false;
  }

  context.state = pairing::PairingState::WAIT_KEY_CONFIRM;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, true);
  // SX1276 uses the proven send_and_receive_ path. SX1262 has a timing issue where
  // the device responds with 0x33 faster than the TX→RX transition completes. Strategy:
  // send 0x32 once (device accepts the key), then if we miss the 0x33, re-send 0x31
  // which triggers the auto-confirm path (device already has the key, responds with 0x33).
  bool key_ok = false;
  if (strcmp(this->radio_->chip_name(), "sx1262") == 0) {
    // First attempt: send 0x32 and try to catch 0x33
    key_ok = this->wait_for_key_confirm_(context);
    // If that failed, retry by re-sending 0x31 — device should auto-confirm with 0x33
    for (int re = 0; !key_ok && re < 2; re++) {
      ESP_LOGI(TAG, "Key confirm missed, re-sending key-init to trigger auto-confirm (attempt %d/2)", re + 1);
      App.feed_wdt();
      delay(EXCHANGE_RETRY_DELAY_MS);
      if (!this->transmit_frame_(context.key_init, FREQ_CH2, LONG_PREAMBLE))
        continue;
      if (this->wait_for_key_challenge_(detail::PAIRING_KEY_CHALLENGE_TIMEOUT_MS, context.packet, context.rx,
                                        context.device.node_id) &&
          context.rx.cmd == CMD_KEY_CONFIRM) {
        key_ok = true;
      }
    }
  } else {
    key_ok = this->send_and_receive_(context.req, context.resp, FREQ_CH2) && frame_is_key_confirm(context.resp);
  }
  if (!key_ok) {
    ESP_LOGW(TAG, "Key exchange failed");
    return false;
  }
  return true;
}

/// Execute Phase 3: finalize pairing configuration on the device.
///
/// Sends a SetConfig1 command (0x6F) using the authenticated exchange path.
/// This step is optional in the sense that the command may be skipped if
/// `create_set_config1()` returns false, but the pairing itself is already
/// complete at this point. The function always returns true to avoid aborting
/// a successfully paired device.
bool IOHomeControlComponent::finalize_pairing_configuration_(pairing::PairingContext &context) {
  if (create_set_config1(context.req, this->node_id_, context.device.node_id))
    this->send_and_receive_(context.req, context.resp, FREQ_CH2);
  return true;
}

/// Pairing orchestrator — high‑level three‑phase flow.
///
/// Phase 1: Discovery (run_discovery_phase_) finds a device in pairing mode.
/// Phase 2: Key exchange (run_key_exchange_phase_) performs authenticated
///   key establishment using the challenge‑response protocol.
/// Phase 3: Finalization (finalize_pairing_configuration_) sends SetConfig1.
///
/// On success the device is added to the current runtime registry and either a valid
/// YAML snippet or a follow-up guidance message is printed in the logs. Any phase
/// failure aborts early, logging an appropriate warning.
///
/// @return true if pairing completed; false otherwise.
bool IOHomeControlComponent::discover_and_pair() {
  if (!this->initialized_)
    return false;
  ESP_LOGI(TAG, "Starting device discovery...");

  this->busy_ = true;
  pairing::PairingContext context;

  // Phase 1: Discovery — find a pairable device
  auto disc_disp = this->run_discovery_phase_(context);
  if (disc_disp != decisions::PairingDiscoveryDisposition::ACCEPT) {
    log_discovery_diagnostic(disc_disp);
    this->busy_ = false;
    return false;
  }

  // Phase 2: Key exchange — establish shared system key
  // Retry up to 3 times as defense-in-depth against transient RX decode failures.
  // Each attempt gets a fresh challenge (0x3C) from the device via a full
  // 0x31 → 0x3C → 0x32 → 0x33 sequence, so a corrupted challenge on one attempt
  // does not poison subsequent tries.
  bool key_exchanged = false;
  for (int ke_attempt = 0; ke_attempt < 3; ke_attempt++) {
    if (ke_attempt > 0) {
      ESP_LOGI(TAG, "Retrying key exchange (attempt %d/3)...", ke_attempt + 1);
      App.feed_wdt();
      delay(EXCHANGE_RETRY_DELAY_MS);
    }
    if (this->run_key_exchange_phase_(context)) {
      key_exchanged = true;
      break;
    }
  }
  if (!key_exchanged) {
    this->busy_ = false;
    return false;
  }

  // Phase 3: Final configuration — best-effort SetConfig1
  this->finalize_pairing_configuration_(context);

  context.state = pairing::PairingState::REGISTER_DEVICE;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, true);

  // Register device in runtime (user still needs to add it to YAML manually)
  this->devices_[context.device_id] = context.device;

  const auto capability_class = device_capability_class(context.device.type);
  const char *platform = pairing_platform_name(capability_class);
  const std::string type_diag = format_device_type_diagnostic(context.device.type);
  const std::string type_yaml = format_device_type_for_yaml(context.device.type);
  std::string extra_lines;

  if (platform == nullptr) {
    if (context.discovery_metadata_complete) {
      ESP_LOGW(TAG,
               "Device %s paired successfully, but this repo does not yet expose an ESPHome platform for type=%s "
               "class=%s subtype=%u.",
               context.device_id.c_str(), type_diag.c_str(), device_capability_class_name(context.device.type),
               context.device.subtype);
      ESP_LOGW(TAG,
               "No ready-to-paste YAML was generated. If you want to experiment manually, choose the most likely "
               "platform and set io_device_type: %s.",
               type_yaml.c_str());
      ESP_LOGW(TAG, "Please file a GitHub issue with this device type, subtype, model, and the pairing log so support "
                    "can be added.");
    } else {
      ESP_LOGW(TAG, "Device %s paired successfully, but the discovery response did not include type/subtype metadata.",
               context.device_id.c_str());
      ESP_LOGW(TAG, "No ready-to-paste YAML was generated. Add the device ID to the correct cover/light/switch entry "
                    "manually and leave io_device_type/io_subtype unset for now.");
      ESP_LOGW(TAG, "Please file a GitHub issue with the pairing log and device model so this discovery edge case can "
                    "be investigated.");
    }

    context.state = pairing::PairingState::COMPLETE;
    this->record_exchange_debug_(pairing_stage_name(context.state), 1, true);
    this->busy_ = false;
    return true;
  }

  if (capability_class == DeviceCapabilityClass::COVER && context.device.inverted)
    extra_lines += "    invert_position: true\n";

  std::string subtype_line;
  if (context.discovery_metadata_complete) {
    subtype_line = "    io_subtype: " + std::to_string(context.device.subtype) + "\n";
  }

  ESP_LOGI(TAG,
           "Device %s paired successfully! Add this to your YAML:\n"
           "  %s:\n"
           "  - platform: home_io_control\n"
           "    name: \"My Device\"\n"
           "    io_device_id: \"%s\"\n"
           "    io_device_type: %s\n"
           "%s"
           "%s",
           context.device_id.c_str(), platform, context.device_id.c_str(), type_yaml.c_str(), subtype_line.c_str(),
           extra_lines.c_str());

  if (!context.discovery_metadata_complete) {
    ESP_LOGW(TAG, "This device did not report a subtype during discovery, so io_subtype was omitted. The controller "
                  "will try to learn it later at runtime.");
  } else if (yaml_device_type_name(context.device.type) == nullptr) {
    ESP_LOGW(TAG,
             "This snippet uses the raw device type %s because the project does not yet expose a named YAML alias "
             "for %s.",
             type_yaml.c_str(), type_diag.c_str());
    ESP_LOGW(TAG, "Please file a GitHub issue with this type, subtype, device model, and the pairing log so support "
                  "can be added.");
  }

  context.state = pairing::PairingState::COMPLETE;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, true);
  this->busy_ = false;
  return true;
}

}  // namespace home_io_control
}  // namespace esphome
