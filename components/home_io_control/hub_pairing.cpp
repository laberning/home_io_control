#include "hub_pairing.h"

#include "hub_decisions.h"
#include "hub_core.h"
#include "proto_commands.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

/// @file hub_pairing.cpp
/// @brief Device pairing orchestration — discovery, key exchange, and finalization.
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

namespace esphome {
namespace home_io_control {

namespace {

const char *const TAG = "home_io_control";

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
    case pairing::PairingState::PERSIST_DEVICE:
      return "persist_device";
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
decisions::PairingDiscoveryDisposition IOHomeControlComponent::wait_for_discovery_response_(uint32_t timeout_ms,
                                                                                            RadioRxPacket &packet,
                                                                                            IoFrame &response_frame) {
  bool saw_traffic = false;
  const uint32_t deadline = millis() + timeout_ms;
  while ((int32_t) (deadline - millis()) > 0) {
    const uint32_t remaining_ms = deadline - millis();
    const uint32_t slice = decisions::response_wait_slice_ms(remaining_ms);
    if (!this->radio_->wait_for_packet(packet, slice))
      continue;
    saw_traffic = true;
    if (!parse(packet.data, packet.len, response_frame))
      continue;
    auto disp = decisions::classify_pairing_discovery_response(response_frame);
    if (disp == decisions::PairingDiscoveryDisposition::ACCEPT)
      return disp;
  }
  if (!saw_traffic)
    return decisions::PairingDiscoveryDisposition::NO_RESPONSE;
  return decisions::PairingDiscoveryDisposition::INVALID;
}

/// Wait for a valid key‑challenge frame (0x3C from the target device).
///
/// During key exchange the device responds to our key‑init with a random 6‑byte
/// challenge. This helper loops until such a frame is received and validated
/// by `decisions::classify_pairing_key_challenge()` — it must be a 0x3C
/// command, 6 bytes long, and come from the discovered device node ID.
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
    if (decisions::classify_pairing_key_challenge(challenge_frame, device_node_id, this->node_id_) !=
        decisions::PairingKeyChallengeDisposition::ACCEPT)
      continue;
    return true;
  }
  ESP_LOGW(TAG, saw_traffic ? "Key exchange: no valid challenge received" : "Key exchange: no challenge received");
  return false;
}

/// Parse a discovery response frame into device metadata and ID.
///
/// Extracts node ID, device type, and subtype from a CMD_DISCOVER_RESP frame.
/// The two-byte payload encodes:
///   type    = data[0] << 2 | data[1] >> 6
///   subtype = data[1] & 0x3F
/// The inversion flag is derived from the type via `default_inverted_for_type()`.
void IOHomeControlComponent::parse_device_from_discovery(const IoFrame &frame, IoDevice &device,
                                                         std::string &device_id) {
  memcpy(device.node_id, frame.src, NODE_ID_SIZE);
  if (frame.data_len >= 2) {
    device.type = static_cast<DeviceType>((frame.data[0] << 2) | (frame.data[1] >> 6));
    device.subtype = frame.data[1] & 0x3F;
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
}

/// Execute Phase 1: discover a pairable device on channel 2.
///
/// Transmits a discovery broadcast (0x28) and waits up to 2000 ms for a valid
/// discovery response (0x29). On success the response is parsed into
/// `context.device` and `context.device_id` and the function returns ACCEPT.
/// On failure returns NO_RESPONSE (no packets) or INVALID (packets seen but
/// none were valid discovery frames).
decisions::PairingDiscoveryDisposition IOHomeControlComponent::run_discovery_phase_(pairing::PairingContext &context) {
  context.state = pairing::PairingState::TX_DISCOVER;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, false);
  if (!create_discover(context.req, this->node_id_) || !this->transmit_frame_(context.req, FREQ_CH2, LONG_PREAMBLE)) {
    return decisions::PairingDiscoveryDisposition::NO_RESPONSE;
  }

  context.state = pairing::PairingState::WAIT_DISCOVER_RESPONSE;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, false);
  auto result = this->wait_for_discovery_response_(2000, context.packet, context.rx);
  if (result == decisions::PairingDiscoveryDisposition::ACCEPT) {
    parse_device_from_discovery(context.rx, context.device, context.device_id);
  }
  return result;
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
  if (!this->wait_for_key_challenge_(500, context.packet, context.rx, context.device.node_id)) {
    return false;
  }

  context.state = pairing::PairingState::TX_KEY_TRANSFER;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, true);
  if (!create_key_transfer(context.req, context.key_init, context.device.node_id, this->node_id_, this->system_key_,
                           context.rx.data)) {
    return false;
  }

  context.state = pairing::PairingState::WAIT_KEY_CONFIRM;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, true);
  if (!this->send_and_receive_(context.req, context.resp, FREQ_CH2) || !frame_is_key_confirm(context.resp)) {
    ESP_LOGW(TAG, "Key exchange failed");
    return false;
  }
  return true;
}

/// Execute Phase 3: finalize pairing configuration on the device.
///
/// Sends a SetConfig1 command (0x71) using the authenticated exchange path.
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
/// On success the device is persisted to flash and the function returns true.
/// Any phase failure aborts early, logging an appropriate warning.
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
  if (!this->run_key_exchange_phase_(context)) {
    this->busy_ = false;
    return false;
  }

  // Phase 3: Final configuration — best-effort SetConfig1
  this->finalize_pairing_configuration_(context);

  // Persist device
  this->devices_[context.device_id] = context.device;
  this->save_devices_();
  ESP_LOGI(TAG, "Device %s paired successfully!", context.device_id.c_str());

  context.state = pairing::PairingState::COMPLETE;
  this->record_exchange_debug_(pairing_stage_name(context.state), 1, true);
  this->busy_ = false;
  return true;
}

}  // namespace home_io_control
}  // namespace esphome
