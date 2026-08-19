/// @file pairing_engine.cpp
/// @brief Device pairing orchestration — discovery, key exchange, and finalization.
/// @ingroup hioc_hub
///
/// Implements PairingEngine's three-phase pairing procedure and the low-level waiters.
/// All radio operations delegate to ExchangeEngine (which owns LBT and hop timing);
/// the PairingEngine focuses purely on protocol sequencing.
///
/// @todo Validate the full discovery and re-pair flow on freshly reset SX1276-backed devices.
/// @todo Validate the full discovery and re-pair flow on freshly reset SX1262-backed devices.
/// @todo Add first-class platform coverage for additional paired device classes once hardware is available.

#include "pairing_engine.h"

#include "hub_decisions.h"
#include "proto_commands.h"
#include "tuning_config.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cstring>

namespace esphome {
namespace home_io_control {

namespace {

const char *const TAG = "home_io_control";

/// Check if frame is a 0x33 key-confirm message.
bool frame_is_key_confirm(const IoFrame &frame) { return frame.cmd == CMD_KEY_CONFIRM; }

/// Log discovery-phase failure based on disposition.
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

// --- Constructor ---

PairingEngine::PairingEngine(RadioDriver **radio_ptr, const uint8_t *node_id, const uint8_t *system_key,
                             const TuningConfig *tuning, ExchangeEngine &engine, DeviceRegistry &registry,
                             PairingTelemetry &telemetry)
    : radio_ptr_(radio_ptr),
      node_id_(node_id),
      system_key_(system_key),
      tuning_(tuning),
      engine_(engine),
      registry_(registry),
      telemetry_(telemetry) {}

// --- Low-level waiters ---

/// Wait for a valid discovery response (0x29) within timeout_ms.
///
/// Listens with per-chip frequency hopping between slices. Distinguishes between
/// NO_RESPONSE (no packets at all) and INVALID (packets seen but none valid).
///
/// Frequency hopping: hops between the 3 IO-homecontrol channels after each slice.
/// The slice length comes from RadioDriver::hop_dwell_ms() (spec.dwell_ms left at 0, so
/// listen() asks the driver) — chips that retune slowly need a much longer dwell than
/// fast-hopping chips. When preamble or sync detection fires, the dwell extends by
/// PREAMBLE_LINGER_DWELL_MS so the incoming frame can complete without interruption.
decisions::PairingDiscoveryDisposition PairingEngine::wait_for_discovery_response_(uint32_t timeout_ms,
                                                                                   RadioRxPacket &packet,
                                                                                   IoFrame &response_frame) {
  ListenSpec spec;
  spec.window_ms = timeout_ms;
  spec.policy = ListenPolicy::ROTATE_ALL_CHANNELS;
  // dwell_ms left at 0: no measured reason to dwell differently from the roll-call, so listen()
  // asks the driver (radio_()->hop_dwell_ms(*tuning_)) the same way collect_broadcast_responses()
  // does.
  spec.linger_on_preamble = true;
  spec.linger_dwell_ms = PREAMBLE_LINGER_DWELL_MS;
  spec.on_hop = [this]() { this->telemetry_.record_hop(); };

  bool saw_traffic = false;
  auto outcome =
      engine_.listen(spec, packet, response_frame, [&](const IoFrame *parsed, const RadioRxPacket & /*packet*/) {
        saw_traffic = true;
        if (parsed == nullptr)
          return ReplyDisposition::IGNORE;
        const bool accepted = decisions::classify_pairing_discovery_response(*parsed, node_id_) ==
                              decisions::PairingDiscoveryDisposition::ACCEPT;
        this->record_discovery_rx_telemetry_(*parsed, accepted, radio_()->get_last_capture().rssi_dbm);
        return accepted ? ReplyDisposition::ACCEPT : ReplyDisposition::IGNORE;
      });

  if (outcome == ListenOutcome::ACCEPTED)
    return decisions::PairingDiscoveryDisposition::ACCEPT;
  return saw_traffic ? decisions::PairingDiscoveryDisposition::INVALID
                     : decisions::PairingDiscoveryDisposition::NO_RESPONSE;
}

/// Wait for a key-challenge (0x3C) or direct key-confirm (0x33) from target device.
///
/// During key exchange the device typically responds to 0x31 with a random 6-byte
/// challenge (0x3C). Some devices skip the challenge and send 0x33 directly —
/// indicating immediate key acceptance (observed mostly when the controller's
/// TX→RX turnaround is slow enough that the 0x3C is missed). Both are accepted.
///
/// Uses `ExchangeEngine::listen()` with `ListenPolicy::HOLD_REQUEST_CHANNEL`: this is a unicast
/// reply to a unicast request (the 0x31 key-init), and every measured unicast pairing reply came
/// back on the request channel, so there is nothing here to hop for — same reasoning as
/// `wait_for_key_confirm_()`. This loop runs on all three chips (it is called before the
/// `has_fast_tx_rx_turnaround()` branch in `run_key_exchange_phase_()`), unlike the dedicated
/// confirm wait, which only slow-turnaround radios reach.
bool PairingEngine::wait_for_key_challenge_(uint32_t timeout_ms, RadioRxPacket &packet, IoFrame &challenge_frame,
                                            const uint8_t device_node_id[NODE_ID_SIZE]) {
  ListenSpec spec;
  spec.window_ms = timeout_ms;
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;

  bool saw_traffic = false;
  auto outcome =
      engine_.listen(spec, packet, challenge_frame, [&](const IoFrame *parsed, const RadioRxPacket & /*packet*/) {
        saw_traffic = true;
        if (parsed == nullptr)
          return ReplyDisposition::IGNORE;
        const int16_t rssi = radio_()->get_last_capture().rssi_dbm;
        if (parsed->cmd == CMD_KEY_CONFIRM && memcmp(parsed->src, device_node_id, NODE_ID_SIZE) == 0 &&
            memcmp(parsed->dst, node_id_, NODE_ID_SIZE) == 0) {
          this->telemetry_.record_rx(*parsed, rssi);
          return ReplyDisposition::ACCEPT;
        }
        if (decisions::classify_pairing_key_challenge(*parsed, device_node_id, node_id_) !=
            decisions::PairingKeyChallengeDisposition::ACCEPT) {
          this->telemetry_.record_rx_reject(*parsed, rssi);
          return ReplyDisposition::IGNORE;
        }
        this->telemetry_.record_rx(*parsed, rssi);
        return ReplyDisposition::ACCEPT;
      });

  if (outcome == ListenOutcome::ACCEPTED)
    return true;
  ESP_LOGW(TAG, saw_traffic ? "Key exchange: no valid challenge received" : "Key exchange: no challenge received");
  return false;
}

/// Transmit the 0x32 key transfer and wait for 0x33 key confirm (with retry).
///
/// Only reached on slow-turnaround radios (`RadioDriver::has_fast_tx_rx_turnaround() == false`,
/// i.e. SX1262/LR1121): fast-turnaround radios (SX1276) catch the 0x33 through the standard
/// `ExchangeEngine::send_and_receive_()` / `wait_for_first_response_()` path instead and never
/// call this function — see `run_key_exchange_phase_()`. Uses `ExchangeEngine::listen()` with
/// `ListenPolicy::HOLD_REQUEST_CHANNEL` (does not hop, does not slice, see below) and the driver's
/// response_preamble() (drivers whose TX waveform needs more lock-on margin return a longer
/// preamble). Retries up to EXCHANGE_RETRY_COUNT times on timeout.
bool PairingEngine::wait_for_key_confirm_(pairing::PairingContext &context) {
  for (uint8_t tries = 0; tries < EXCHANGE_RETRY_COUNT; tries++) {
    if (tries > 0) {
      App.feed_wdt();
      delay(EXCHANGE_RETRY_DELAY_MS);
    }
    if (!engine_.transmit_frame(context.req, FREQ_CH2, radio_()->response_preamble()))
      continue;

    // Deliberately holds the request channel rather than hopping: a key confirm is a unicast
    // reply to a unicast request, and every measured unicast pairing reply — the 0x33 in the
    // corpus pairing captures on all three chips, every 0x3C, field-logged 0xFE error replies —
    // came back on the channel the request went out on. Only replies to *broadcasts* are measured
    // off the request channel. So there is nothing here to hop for, and hopping loses any device
    // that answers later than one slice (real devices answer some requests at 246+ ms). The
    // challenge wait above holds still for the same reason; the broadcast roll-call is the
    // opposite case and uses ListenPolicy::ROTATE_SKIPPING_REQUEST instead.
    //
    // HOLD also does not slice the wait: slicing exists so a hopping loop gets a chance to hop
    // between dwells, and to keep the watchdog fed during a long silent wait. Neither applies
    // here — there is nothing to hop for (above), and wait_for_packet() already feeds the
    // watchdog internally while it waits. Waiting the full remaining window in one call means
    // strictly fewer RX re-arm gaps than any slicing would, which matters most on exactly the
    // radios that reach this loop: has_fast_tx_rx_turnaround() routes fast-turnaround chips
    // (SX1276) through ExchangeEngine::wait_for_first_response_() instead, so only the
    // slow-turnaround chips (SX1262, LR1121) — the ones where a re-arm is most expensive — ever
    // wait here.
    ListenSpec spec;
    spec.window_ms = PAIRING_KEY_CONFIRM_TIMEOUT_MS;
    spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;

    bool saw_any = false;
    auto outcome =
        engine_.listen(spec, context.packet, context.resp, [&](const IoFrame *parsed, const RadioRxPacket &packet) {
          saw_any = true;
          ESP_LOGD(TAG, "Key confirm wait: got %u bytes on freq=%" PRIu32, packet.len, packet.freq_hz);
          if (parsed == nullptr) {
            ESP_LOGD(TAG, "Key confirm wait: parse failed");
            return ReplyDisposition::IGNORE;
          }
          ESP_LOGD(TAG, "Key confirm wait: parsed cmd=0x%02X src=%02X%02X%02X dst=%02X%02X%02X", parsed->cmd,
                   parsed->src[0], parsed->src[1], parsed->src[2], parsed->dst[0], parsed->dst[1], parsed->dst[2]);
          if (!decisions::frame_matches_exchange_endpoints(context.req, *parsed))
            return ReplyDisposition::IGNORE;
          const int16_t rssi = radio_()->get_last_capture().rssi_dbm;
          if (frame_is_key_confirm(*parsed)) {
            this->telemetry_.record_rx(*parsed, rssi);
            return ReplyDisposition::ACCEPT;
          }
          this->telemetry_.record_rx_reject(*parsed, rssi);
          ESP_LOGW(TAG, "Key transfer: device responded with cmd=%s(0x%02X) (expected KEY_CONFIRM 0x33)",
                   command_name(parsed->cmd), parsed->cmd);
          if (parsed->cmd == CMD_ERROR_RESP && parsed->data_len > 0)
            ESP_LOGW(TAG, "Key transfer: error code=0x%02X", parsed->data[0]);
          return ReplyDisposition::ABORT;
        });

    if (outcome == ListenOutcome::ACCEPTED)
      return true;
    if (outcome == ListenOutcome::ABORTED)
      return false;  // An explicit refusal must not spend the remaining retries.

    ESP_LOGI(TAG, "Try %d ended: no response for key transfer (0x32) within %" PRIu32 " ms (saw_any=%d)", tries + 1,
             PAIRING_KEY_CONFIRM_TIMEOUT_MS, saw_any);
  }
  return false;
}

// --- Discovery metadata ---

/// Parse a discovery response frame into device metadata and ID.
///
/// Decodes node ID, device type, subtype, and the extended fields (manufacturer, backbone,
/// Multi Information Byte) via decode_discovery_response(), then emits pairing's diagnostic log
/// lines for whichever extended fields the payload actually included.
/// The inversion flag is derived from the type via `default_inverted_for_type()`.
void PairingEngine::parse_device_from_discovery(const IoFrame &frame, IoDevice &device, std::string &device_id) {
  const DiscoveryResponseInfo info = decode_discovery_response(frame, device, device_id);

  if (frame.data_len > DISCOVERY_RESP_MANUFACTURER_OFFSET) {
    const char *mfr_name = manufacturer_name(info.manufacturer);
    ESP_LOGI(TAG, "Discovery: device %s manufacturer=%u (%s)", device_id.c_str(), info.manufacturer, mfr_name);
    if (info.manufacturer == 0 || info.manufacturer > MANUFACTURER_ID_MAX) {
      ESP_LOGW(TAG,
               "Unknown manufacturer ID %u reported by device %s. "
               "Please file a GitHub issue with this ID and your device model so support can be added.",
               info.manufacturer, device_id.c_str());
    }
  }
  if (frame.data_len > DISCOVERY_RESP_BACKBONE_OFFSET + NODE_ID_SIZE - 1) {
    ESP_LOGD(TAG, "Discovery: backbone=%02X%02X%02X", info.backbone[0], info.backbone[1], info.backbone[2]);
  }
  if (frame.data_len > DISCOVERY_RESP_FLAGS_OFFSET) {
    uint8_t const att = discovery_att_class(info.flags);
    uint8_t const power_save = discovery_power_save_mode(info.flags);
    ESP_LOGI(TAG, "Discovery: device %s turnaround=%s power_save=%s flags=0x%02X", device_id.c_str(),
             att_class_name(att), power_save_mode_name(power_save), info.flags);
    if (power_save == POWER_SAVE_LOW_POWER) {
      ESP_LOGI(TAG,
               "Device %s reports low-power mode. "
               "Consider adding 'low_power: true' to YAML if commands are unreliable.",
               device_id.c_str());
    }
  }
}

// --- Phase helpers ---

/// Phase 1: broadcast discovery command(s) and wait for a device response (0x29).
///
/// Sends each configured discovery command in order, waiting up to
/// `pairing_discovery_wait_ms` for a valid response after each TX.
/// Retries up to PAIRING_DISCOVERY_MAX_ATTEMPTS times per command.
decisions::PairingDiscoveryDisposition PairingEngine::run_discovery_phase_(pairing::PairingContext &context) {
  if (tuning_->pairing_discovery_initial_dwell_ms > 0) {
    ESP_LOGD(TAG, "Discovery: initial dwell %u ms", tuning_->pairing_discovery_initial_dwell_ms);
    delay(tuning_->pairing_discovery_initial_dwell_ms);
  }

  // Tracks whether any single attempt saw traffic that failed to classify as a valid discovery
  // response, so the final "gave up after retries" return can distinguish INVALID (something was
  // heard, just not a valid response) from NO_RESPONSE (nothing heard at all) instead of always
  // collapsing to NO_RESPONSE.
  bool saw_invalid = false;

  for (size_t command_index = 0; command_index < tuning_->pairing_discovery_commands.size(); ++command_index) {
    auto command = static_cast<uint8_t>(tuning_->pairing_discovery_commands[command_index]);
    const uint8_t *destination = resolve_discovery_destination(command, tuning_->pairing_discovery_destination_auto,
                                                               tuning_->pairing_discovery_destination.data());
    ESP_LOGD(TAG, "Discovery command %zu/%zu: cmd=0x%02X dst=%02X%02X%02X", command_index + 1,
             tuning_->pairing_discovery_commands.size(), command, destination[0], destination[1], destination[2]);

    for (uint8_t attempt = 1; attempt <= PAIRING_DISCOVERY_MAX_ATTEMPTS; ++attempt) {
      this->telemetry_.increment_discovery_attempt();
      context.state = pairing::PairingState::TX_DISCOVER;
      engine_.record_debug(pairing_stage_name(context.state), attempt, false);
      this->telemetry_.set_phase(context.state);
      if (!create_discovery_request(context.req, node_id_, command, destination, tuning_->pairing_discovery_low_power,
                                    tuning_->pairing_discovery_payload_enabled, tuning_->pairing_discovery_payload,
                                    system_key_) ||
          !engine_.transmit_frame(context.req, FREQ_CH2, LONG_PREAMBLE)) {
        return decisions::PairingDiscoveryDisposition::NO_RESPONSE;
      }

      context.state = pairing::PairingState::WAIT_DISCOVER_RESPONSE;
      engine_.record_debug(pairing_stage_name(context.state), attempt, false);
      this->telemetry_.set_phase(context.state);
      auto result = wait_for_discovery_response_(tuning_->pairing_discovery_wait_ms, context.packet, context.rx);
      if (result == decisions::PairingDiscoveryDisposition::ACCEPT) {
        parse_device_from_discovery(context.rx, context.device, context.device_id);
        context.discovery_metadata_complete = context.rx.data_len >= DEVICE_METADATA_SIZE;
        return result;
      }
      if (result == decisions::PairingDiscoveryDisposition::INVALID) {
        saw_invalid = true;
      }

      if (attempt < PAIRING_DISCOVERY_MAX_ATTEMPTS) {
        ESP_LOGI(TAG, "Discovery attempt %u/%u for cmd=0x%02X: no response, retrying...", attempt,
                 PAIRING_DISCOVERY_MAX_ATTEMPTS, command);
      }
    }
  }
  return saw_invalid ? decisions::PairingDiscoveryDisposition::INVALID
                     : decisions::PairingDiscoveryDisposition::NO_RESPONSE;
}

/// Phase 2: authenticated key exchange (0x31 → 0x3C → 0x32 → 0x33).
///
/// Steps:
///   1. Transmit CMD_KEY_INIT (0x31)
///   2. Wait for device challenge (0x3C)
///   3. Transmit CMD_KEY_TRANSFER (0x32) with encrypted system key
///   4. Wait for CMD_KEY_CONFIRM (0x33)
///
/// Step 4 depends on the driver's TX→RX turnaround: fast-turnaround radios await
/// the 0x33 through the standard send_and_receive() exchange; slow-turnaround
/// radios use the dedicated wait_for_key_confirm_() path with a key-init
/// re-trigger, because the 0x33 would otherwise arrive while the receiver is
/// still settling (see RadioDriver::has_fast_tx_rx_turnaround()).
bool PairingEngine::run_key_exchange_phase_(pairing::PairingContext &context) {
  context.state = pairing::PairingState::TX_KEY_INIT;
  engine_.record_debug(pairing_stage_name(context.state), 1, false);
  this->telemetry_.set_phase(context.state);
  if (!create_key_init(context.key_init, node_id_, context.device.node_id) ||
      !engine_.transmit_frame(context.key_init, FREQ_CH2, LONG_PREAMBLE)) {
    return false;
  }

  context.state = pairing::PairingState::WAIT_KEY_CHALLENGE;
  engine_.record_debug(pairing_stage_name(context.state), 1, true);
  this->telemetry_.set_phase(context.state);
  if (!wait_for_key_challenge_(PAIRING_KEY_CHALLENGE_TIMEOUT_MS, context.packet, context.rx, context.device.node_id)) {
    return false;
  }

  // Some devices send 0x33 directly after 0x31 without requiring 0x32.
  if (context.rx.cmd == CMD_KEY_CONFIRM) {
    ESP_LOGI(TAG, "Device accepted key immediately (0x33 without 0x32 exchange)");
    context.resp = context.rx;
    return true;
  }

  // No challenge bytes here: the raw 0x3C payload plus the 0x3D response it provokes is a
  // known-plaintext/known-ciphertext pair under the system key (see redaction.h). The generic
  // frame-log helpers (log_frame()/log_component_capture()) already mask both commands.
  ESP_LOGI(TAG, "Challenge (0x3C) received: data_len=%u freq=%" PRIu32 " rssi=%d", context.rx.data_len,
           context.packet.freq_hz, radio_()->get_last_capture().rssi_dbm);

  context.state = pairing::PairingState::TX_KEY_TRANSFER;
  engine_.record_debug(pairing_stage_name(context.state), 1, true);
  this->telemetry_.set_phase(context.state);
  if (!create_key_transfer(context.req, context.key_init, context.device.node_id, node_id_, system_key_,
                           context.rx.data)) {
    return false;
  }

  context.state = pairing::PairingState::WAIT_KEY_CONFIRM;
  engine_.record_debug(pairing_stage_name(context.state), 1, true);
  this->telemetry_.set_phase(context.state);
  // Fast-turnaround radios catch the 0x33 through the standard exchange wait. Slow-turnaround
  // radios miss it while re-entering RX, so they use the dedicated wait loop and, on a miss,
  // re-send the key-init to trigger the device's auto-confirm.
  bool key_ok = false;
  if (radio_()->has_fast_tx_rx_turnaround()) {
    // Key exchange is the one caller that genuinely needs the payload: without the 0x33 there is
    // no confirmation the device took the key, so an unconfirmed acceptance is not good enough.
    key_ok = engine_.send_and_receive(context.req, context.resp, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE &&
             frame_is_key_confirm(context.resp);
  } else {
    key_ok = wait_for_key_confirm_(context);
    for (int re = 0; !key_ok && re < 2; re++) {
      ESP_LOGI(TAG, "Key confirm missed, re-sending key-init to trigger auto-confirm (attempt %d/2)", re + 1);
      App.feed_wdt();
      delay(EXCHANGE_RETRY_DELAY_MS);
      if (!engine_.transmit_frame(context.key_init, FREQ_CH2, LONG_PREAMBLE))
        continue;
      if (wait_for_key_challenge_(PAIRING_KEY_CHALLENGE_TIMEOUT_MS, context.packet, context.rx,
                                  context.device.node_id) &&
          context.rx.cmd == CMD_KEY_CONFIRM) {
        key_ok = true;
      }
    }
  }
  if (!key_ok) {
    ESP_LOGW(TAG, "Key exchange failed");
    return false;
  }
  return true;
}

/// Phase 3: send SetConfig1 (0x6F) to enable automatic status updates. Best-effort.
bool PairingEngine::finalize_pairing_configuration_(pairing::PairingContext &context) {
  if (!create_set_config1(context.req, node_id_, context.device.node_id))
    return false;
  // Best-effort, and its reply is never read — an unconfirmed acceptance is a success here.
  return engine_.send_and_receive(context.req, context.resp, FREQ_CH2) != ExchangeOutcome::FAILED;
}

// --- Orchestrator ---

/// Pairing orchestrator — high-level three-phase flow.
///
/// Phase 1: run_discovery_phase_() finds a device in pairing mode.
/// Phase 2: run_key_exchange_phase_() performs authenticated key establishment.
/// Phase 3: finalize_pairing_configuration_() sends SetConfig1 (best-effort).
///
/// On success the device is added to the registry and a YAML snippet is printed to the log.
/// The hub's thin wrapper manages the busy_ flag before and after this call.
bool PairingEngine::discover_and_pair() {
  this->telemetry_.begin();
  this->engine_.set_pairing_telemetry(&this->telemetry_);

  ESP_LOGI(TAG, "Starting device discovery...");
  ESP_LOGI(TAG, "%s", tuning_config_full_snapshot(*tuning_).c_str());

  pairing::PairingContext context;

  // Phase 1: Discovery
  auto disc_disp = run_discovery_phase_(context);
  if (disc_disp != decisions::PairingDiscoveryDisposition::ACCEPT) {
    log_discovery_diagnostic(disc_disp);
    this->finish_pairing_attempt_(disc_disp == decisions::PairingDiscoveryDisposition::INVALID
                                      ? PairingOutcome::INVALID_RESPONSE
                                      : PairingOutcome::NO_RESPONSE);
    return false;
  }

  // Phase 2: Key exchange — retry up to the configured number of times.
  bool key_exchanged = false;
  for (uint8_t ke_attempt = 0; ke_attempt < tuning_->pairing_key_exchange_retries; ke_attempt++) {
    if (ke_attempt > 0) {
      ESP_LOGI(TAG, "Retrying key exchange (attempt %d/%u)...", ke_attempt + 1, tuning_->pairing_key_exchange_retries);
      App.feed_wdt();
      delay(EXCHANGE_RETRY_DELAY_MS);
    }
    if (run_key_exchange_phase_(context)) {
      key_exchanged = true;
      break;
    }
  }
  if (!key_exchanged) {
    this->finish_pairing_attempt_(PairingOutcome::KEY_EXCHANGE_FAILED);
    return false;
  }

  // Phase 3: Final configuration — best-effort SetConfig1. Pairing proceeds either way; the
  // result only affects which outcome telemetry reports (PAIRED vs. CONFIG_FAILED).
  const bool config_ok = finalize_pairing_configuration_(context);
  const PairingOutcome outcome = config_ok ? PairingOutcome::PAIRED : PairingOutcome::CONFIG_FAILED;

  context.state = pairing::PairingState::REGISTER_DEVICE;
  engine_.record_debug(pairing_stage_name(context.state), 1, true);
  this->telemetry_.set_phase(context.state);

  registry_.put(context.device_id, context.device);
  this->telemetry_.set_paired_device(context.device.node_id, context.device.type);

  const std::string type_diag = format_device_type_diagnostic(context.device.type);
  const std::string type_yaml = format_device_type_for_yaml(context.device.type);
  const std::string snippet = build_device_yaml_snippet(context.device.type, context.device.subtype, context.device_id,
                                                        context.discovery_metadata_complete, context.device.inverted);

  if (snippet.empty()) {
    // metadata_complete is true here (build_device_yaml_snippet() never returns empty for
    // metadata_complete == false), so this is specifically "we know the type, but there's no
    // ESPHome platform for it yet".
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

    context.state = pairing::PairingState::COMPLETE;
    engine_.record_debug(pairing_stage_name(context.state), 1, true);
    this->telemetry_.set_phase(context.state);
    this->finish_pairing_attempt_(outcome);
    return true;
  }

  if (!context.discovery_metadata_complete) {
    ESP_LOGW(TAG,
             "Device %s paired successfully, but the discovery response did not include type/subtype "
             "metadata, so the platform (cover/light/switch/lock) can't be determined automatically.",
             context.device_id.c_str());
    ESP_LOGI(TAG, "Add this to your YAML once you know what kind of device it is:\n%s", snippet.c_str());
    ESP_LOGW(TAG, "Please file a GitHub issue with the pairing log and device model so this discovery edge case can "
                  "be investigated.");

    context.state = pairing::PairingState::COMPLETE;
    engine_.record_debug(pairing_stage_name(context.state), 1, true);
    this->telemetry_.set_phase(context.state);
    this->finish_pairing_attempt_(outcome);
    return true;
  }

  ESP_LOGI(TAG, "Device %s paired successfully! Add this to your YAML:\n%s", context.device_id.c_str(),
           snippet.c_str());

  if (yaml_device_type_name(context.device.type) == nullptr) {
    ESP_LOGW(TAG,
             "This snippet uses the raw device type %s because the project does not yet expose a named YAML alias "
             "for %s.",
             type_yaml.c_str(), type_diag.c_str());
    ESP_LOGW(TAG, "Please file a GitHub issue with this type, subtype, device model, and the pairing log so support "
                  "can be added.");
  }

  context.state = pairing::PairingState::COMPLETE;
  engine_.record_debug(pairing_stage_name(context.state), 1, true);
  this->telemetry_.set_phase(context.state);
  this->finish_pairing_attempt_(outcome);
  return true;
}

void PairingEngine::finish_pairing_attempt_(PairingOutcome outcome) {
  this->telemetry_.set_outcome(outcome);
  this->engine_.set_pairing_telemetry(nullptr);
  this->telemetry_.log_summary();

  advisor::PairingAdvice advice[advisor::PAIRING_ADVICE_MAX];
  const uint8_t advice_count = advisor::analyze_pairing_telemetry(this->telemetry_, this->node_id_, advice);
  std::string advice_codes;
  for (uint8_t i = 0; i < advice_count; i++) {
    ESP_LOGW(TAG, "Pairing advisor: %s", advisor::pairing_advice_message(advice[i]).c_str());
    if (!advice_codes.empty())
      advice_codes += ',';
    advice_codes += advisor::pairing_advice_code_name(advice[i].code);
  }
  this->telemetry_.set_advice_codes(advice_codes);
}

void PairingEngine::record_discovery_rx_telemetry_(const IoFrame &frame, bool accepted, int16_t rssi) {
  if (accepted) {
    this->telemetry_.record_rx(frame, rssi);
  } else {
    this->telemetry_.record_rx_reject(frame, rssi);
  }
}

}  // namespace home_io_control
}  // namespace esphome
