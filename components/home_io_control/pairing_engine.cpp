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
#include "log_helpers.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "proto_device_types.h"
#include "proto_heating.h"
#include "roll_call.h"
#include "tuning_config.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
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

/// Block for `ms`, feeding the watchdog between chunks of at most 1000 ms each — a single
/// multi-second delay() would trip the loop-task watchdog, and `pairing_key_init_delay_ms` allows
/// up to 10000. Used only by the discover-confirm step's post-step pause: every other wait in this
/// file is short enough (a few hundred ms) that App.feed_wdt() inside the retry loop is enough.
void delay_feeding_wdt(uint32_t ms) {
  while (ms > 0) {
    App.feed_wdt();
    const uint32_t chunk = ms < 1000 ? ms : 1000;
    delay(chunk);
    ms -= chunk;
  }
}

}  // namespace

// --- Constructor ---

PairingEngine::PairingEngine(RadioDriver **radio_ptr, const uint8_t *node_id, const uint8_t *system_key,
                             const TuningConfig *tuning, ExchangeEngine &engine, DeviceRegistry &registry,
                             PairingTelemetry &telemetry, const RecentOneWayPairingSighting &recent_oneway_sighting)
    : radio_ptr_(radio_ptr),
      node_id_(node_id),
      system_key_(system_key),
      tuning_(tuning),
      engine_(engine),
      registry_(registry),
      telemetry_(telemetry),
      recent_oneway_sighting_(recent_oneway_sighting) {}

uint16_t PairingEngine::pairing_start_preamble_(const IoFrame &frame) const {
  return std::min(engine_.request_preamble_for(frame), tuning_->pairing_discovery_preamble);
}

// --- Low-level waiters ---

/// Wait for a valid discovery response (0x29) within timeout_ms.
///
/// Listens with per-chip frequency hopping between slices. Distinguishes between
/// NO_RESPONSE (no packets at all) and INVALID (packets seen but none valid).
///
/// Frequency hopping: hops between the 2 non-request IO-homecontrol channels after each slice —
/// the request always goes out on FREQ_CH2 (see run_discovery_phase_()), and a discovery reply
/// essentially never lands back on that channel (see ListenPolicy's own doc comment for why), so
/// dwelling there is wasted listening time. Same policy as collect_broadcast_responses()'s
/// default. The slice length comes from
/// RadioDriver::hop_dwell_ms() (spec.dwell_ms left at 0, so listen() asks the driver). When
/// preamble or sync detection fires, the dwell extends by PREAMBLE_LINGER_DWELL_MS so the
/// incoming frame can complete without interruption.
decisions::PairingDiscoveryDisposition PairingEngine::wait_for_discovery_response_(uint32_t timeout_ms,
                                                                                   RadioRxPacket &packet,
                                                                                   IoFrame &response_frame) {
  ListenSpec spec;
  spec.window_ms = timeout_ms;
  // Skipping the request channel is the default because a broadcast's answers are almost never
  // on it (ListenPolicy's own doc has the 1-in-149 measurement). `all` is the escape hatch for a
  // device that has never answered at all: that measurement is drawn from responders we already
  // hear, so it cannot speak for one whose replies we might be missing entirely.
  spec.policy = tuning_->pairing_discovery_listen_channels == DiscoveryListenChannels::ALL
                    ? ListenPolicy::ROTATE_ALL_CHANNELS
                    : ListenPolicy::ROTATE_SKIPPING_REQUEST;
  spec.request_freq = FREQ_CH2;
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
/// `listen_for_key_confirm_()`. This loop runs on all three chips (it is called before the
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

/// Wait for one discover-confirm (0x2C) try's answer.
///
/// Listen policy alternates per try (`decisions::discover_confirm_try_rotates()`): the one
/// rotating try hedges against a 0x2D landing off the request channel — Somfy answers on the
/// request channel, VELUX is unmeasured — while the rest hold, matching every other unicast
/// pairing wait in this file (a 0x2D is a unicast reply to a unicast 0x2C). Only parsed frames are
/// recorded to telemetry, same as the other pairing waits.
decisions::PairingDiscoverConfirmDisposition PairingEngine::wait_for_discover_confirm_ack_(
    uint8_t try_index, pairing::PairingContext &context) {
  ListenSpec spec;
  spec.window_ms = PAIRING_DISCOVER_CONFIRM_ACK_TIMEOUT_MS;
  if (decisions::discover_confirm_try_rotates(try_index)) {
    spec.policy = ListenPolicy::ROTATE_ALL_CHANNELS;
    // Same shape as the discovery listen's rotating spec: a short rotating dwell needs the
    // preamble/sync linger guard so an incoming reply is not cut off mid-retune.
    spec.linger_on_preamble = true;
    spec.linger_dwell_ms = PREAMBLE_LINGER_DWELL_MS;
    spec.on_hop = [this]() { this->telemetry_.record_hop(); };
  } else {
    spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;
  }

  auto disposition = decisions::PairingDiscoverConfirmDisposition::IGNORE;
  auto outcome =
      engine_.listen(spec, context.packet, context.rx, [&](const IoFrame *parsed, const RadioRxPacket & /*packet*/) {
        if (parsed == nullptr)
          return ReplyDisposition::IGNORE;
        const int16_t rssi = radio_()->get_last_capture().rssi_dbm;
        disposition = decisions::classify_pairing_discover_confirm_reply(*parsed, context.device.node_id, node_id_);
        if (disposition == decisions::PairingDiscoverConfirmDisposition::IGNORE) {
          this->telemetry_.record_rx_reject(*parsed, rssi);
          return ReplyDisposition::IGNORE;
        }
        this->telemetry_.record_rx(*parsed, rssi);
        return ReplyDisposition::ACCEPT;
      });

  if (outcome != ListenOutcome::ACCEPTED)
    return decisions::PairingDiscoverConfirmDisposition::IGNORE;  // Timeout: no reply this try.
  return disposition;
}

/// One HOLD_REQUEST_CHANNEL listen for the key-transfer confirm wait — see the header doc for why
/// this is a shared helper rather than an inline lambda repeated at each of its two call sites.
///
/// Deliberately holds the request channel rather than hopping: a key confirm is a unicast reply to
/// a unicast request, and every measured unicast pairing reply — the 0x33 in the corpus pairing
/// captures on all three chips, every 0x3C, field-logged 0xFE error replies — came back on the
/// channel the request went out on. Only replies to *broadcasts* are measured off the request
/// channel, so there is nothing here to hop for, and hopping loses any device that answers later
/// than one slice (real devices answer some requests at 246+ ms). HOLD also does not slice the
/// wait: slicing exists so a hopping loop gets a chance to hop between dwells, and to keep the
/// watchdog fed during a long silent wait; neither applies here, and wait_for_packet() already
/// feeds the watchdog internally while it blocks.
decisions::PairingKeyConfirmDisposition PairingEngine::listen_for_key_confirm_(pairing::PairingContext &context,
                                                                               uint8_t try_number,
                                                                               bool after_challenge) {
  ListenSpec spec;
  spec.window_ms = PAIRING_KEY_CONFIRM_TIMEOUT_MS;
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;

  bool saw_any = false;
  auto disposition = decisions::PairingKeyConfirmDisposition::IGNORE;
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
        disposition = decisions::classify_pairing_key_confirm_reply(context.req, *parsed);
        if (disposition == decisions::PairingKeyConfirmDisposition::IGNORE)
          return ReplyDisposition::IGNORE;

        const int16_t rssi = radio_()->get_last_capture().rssi_dbm;
        if (disposition == decisions::PairingKeyConfirmDisposition::REFUSE) {
          this->telemetry_.record_rx_reject(*parsed, rssi);
          if (parsed->cmd == CMD_ERROR_RESP) {
            this->note_key_refusal_(context, *parsed, "Key transfer");
          } else {
            ESP_LOGW(TAG, "Key transfer: device responded with cmd=%s(0x%02X) (expected KEY_CONFIRM 0x33)",
                     command_name(parsed->cmd), parsed->cmd);
          }
          return ReplyDisposition::ABORT;
        }
        // CONFIRM (0x33) or CHALLENGE (a well-formed 0x3C): both are legitimate answers, not
        // rejections. No TX happens in this callback — a CHALLENGE is answered by the caller
        // after this listen ends, per the header doc.
        this->telemetry_.record_rx(*parsed, rssi);
        return disposition == decisions::PairingKeyConfirmDisposition::CONFIRM ? ReplyDisposition::ACCEPT
                                                                               : ReplyDisposition::ABORT;
      });

  if (outcome == ListenOutcome::TIMED_OUT) {
    ESP_LOGI(TAG, "Try %u%s: no response for key transfer (0x32) within %" PRIu32 " ms (saw_any=%d)", try_number,
             after_challenge ? " (post-challenge)" : "", PAIRING_KEY_CONFIRM_TIMEOUT_MS, saw_any);
    return decisions::PairingKeyConfirmDisposition::IGNORE;
  }
  return disposition;
}

/// Transmit the 0x32 key transfer and wait for 0x33 key confirm (with retry).
///
/// Only reached on slow-turnaround radios (`RadioDriver::has_fast_tx_rx_turnaround() == false`,
/// i.e. SX1262/LR1121): fast-turnaround radios (SX1276) catch the 0x33 through the standard
/// `ExchangeEngine::send_and_receive_()` / `wait_for_first_response_()` path instead and never
/// call this function — see `run_key_exchange_phase_()`. Uses the driver's response_preamble()
/// (drivers whose TX waveform needs more lock-on margin return a longer preamble). Retries up to
/// EXCHANGE_RETRY_COUNT times on timeout.
bool PairingEngine::wait_for_key_confirm_(pairing::PairingContext &context) {
  for (uint8_t tries = 0; tries < EXCHANGE_RETRY_COUNT; tries++) {
    if (tries > 0) {
      App.feed_wdt();
      delay(EXCHANGE_RETRY_DELAY_MS);
    }
    if (!engine_.transmit_frame(context.req, FREQ_CH2, radio_()->response_preamble()))
      continue;

    const uint8_t try_number = tries + 1;
    decisions::PairingKeyConfirmDisposition disposition = listen_for_key_confirm_(context, try_number, false);
    if (disposition == decisions::PairingKeyConfirmDisposition::CHALLENGE) {
      ESP_LOGI(TAG, "Key transfer: device challenged the key transfer, answering 0x3D");
      // "Rest of the window" would leave almost nothing after a late challenge plus the 0x3D TX,
      // so this is a fresh full window, same as ExchangeEngine::wait_for_final_response_() gets
      // after handle_authentication_(). A failed 0x3D build/TX leaves `disposition` at CHALLENGE,
      // which the checks below treat the same as a second challenge: this try ends without
      // confirming, and the next try re-sends 0x32.
      if (engine_.answer_challenge(context.req, context.resp, FREQ_CH2))
        disposition = listen_for_key_confirm_(context, try_number, true);
    }

    if (disposition == decisions::PairingKeyConfirmDisposition::CONFIRM)
      return true;
    if (disposition == decisions::PairingKeyConfirmDisposition::REFUSE)
      return false;  // An explicit refusal must not spend the remaining retries; a challenge is
                     // not a refusal, so neither branch above returns false for it.
    // IGNORE (timeout) or a second CHALLENGE in the same try: spend the next try.
  }
  return false;
}

/// Build CMD_KEY_TRANSFER against the challenge currently held in `context.rx.data` and wait for
/// the 0x33 confirm, routing through the fast- or slow-turnaround path. Shared by
/// run_key_exchange_phase_()'s first attempt and its slow-turnaround retry loop, which calls this
/// again against a freshly re-issued challenge (see that function's doc comment) rather than
/// discarding it.
bool PairingEngine::transfer_key_and_wait_confirm_(pairing::PairingContext &context) {
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
  // radios miss it while re-entering RX, so run_key_exchange_phase_()'s retry loop calls this
  // helper again instead, rather than using the dedicated wait loop directly.
  if (radio_()->has_fast_tx_rx_turnaround()) {
    // Key exchange is the one caller that genuinely needs the payload: without the 0x33 there is
    // no confirmation the device took the key, so an unconfirmed acceptance is not good enough.
    if (engine_.send_and_receive(context.req, context.resp, FREQ_CH2) != ExchangeOutcome::SUCCESS_WITH_RESPONSE)
      return false;
    if (context.resp.cmd == CMD_ERROR_RESP)
      this->note_key_refusal_(context, context.resp, "Key transfer");
    return frame_is_key_confirm(context.resp);
  }
  return wait_for_key_confirm_(context);
}

// --- Discovery metadata ---

/// Parse a discovery response frame into device metadata and ID.
///
/// Decodes node ID, device type, subtype, and the extended fields (manufacturer, backbone,
/// Multi Information Byte) via decode_discovery_response(), then emits pairing's diagnostic log
/// lines for whichever extended fields the payload actually included.
/// The inversion flag is derived from the type via `default_inverted_for_type()`.
DiscoveryResponseInfo PairingEngine::parse_device_from_discovery(const IoFrame &frame, IoDevice &device,
                                                                 std::string &device_id) {
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
               "Device %s reports low-power mode: add 'low_power: true' to its YAML entry. "
               "The pairing snippet below pre-fills it when one is printed.",
               device_id.c_str());
    }
  }
  return info;
}

// --- Phase helpers ---

/// Log what discovery accepted: the device kind, and a warning when it answered as a remote controller.
static void log_discovered_device(const pairing::PairingContext &context) {
  if (context.discovery_metadata_complete) {
    ESP_LOGI(TAG, "Discovery: device %s: %s (%s)%s", context.device_id.c_str(),
             format_device_kind(context.device.type, context.device.subtype).c_str(),
             format_device_ids(context.device.type, context.device.subtype).c_str(),
             device_has_platform(context.device.type, context.device.subtype) ? "" : " (not supported yet)");
  }
  if (context.device.type == DeviceType::REMOTE_CONTROLLER) {
    ESP_LOGW(TAG,
             "Discovery: device %s answered as a remote controller (node type %u), not as an actuator. "
             "It may not accept a pushed key; the pairing advisor says what to try if the key step fails.",
             context.device_id.c_str(), static_cast<unsigned>(context.device.type));
  }
}

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
                                    tuning_->pairing_discovery_ack_capable, tuning_->pairing_discovery_payload_enabled,
                                    tuning_->pairing_discovery_payload, system_key_) ||
          !engine_.transmit_frame(context.req, FREQ_CH2, tuning_->pairing_discovery_preamble)) {
        return decisions::PairingDiscoveryDisposition::NO_RESPONSE;
      }

      context.state = pairing::PairingState::WAIT_DISCOVER_RESPONSE;
      engine_.record_debug(pairing_stage_name(context.state), attempt, false);
      this->telemetry_.set_phase(context.state);
      auto result = wait_for_discovery_response_(tuning_->pairing_discovery_wait_ms, context.packet, context.rx);
      if (result == decisions::PairingDiscoveryDisposition::ACCEPT) {
        const DiscoveryResponseInfo info = parse_device_from_discovery(context.rx, context.device, context.device_id);
        context.discovery_metadata_complete = context.rx.data_len >= DEVICE_METADATA_SIZE;
        context.discovery_low_power =
            info.has_extended && discovery_power_save_mode(info.flags) == POWER_SAVE_LOW_POWER;
        context.discovery_manufacturer = info.has_extended ? info.manufacturer : 0;
        this->telemetry_.set_discovered_device(context.device.node_id, context.device.type);
        log_discovered_device(context);
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

/// Discover-confirm step (0x2C → 0x2D): sent directly to the device discovery just found, once
/// per discover_and_pair() attempt, before the key-exchange retry loop begins (so a retry of that
/// loop never repeats this step).
///
/// Never fails a pairing attempt: `skip` mode, a timeout, and an explicit CMD_ERROR_RESP all still
/// let discover_and_pair() proceed to run_key_exchange_phase_() — this function only decides how
/// long that takes and what gets logged. CTRL1_ACK follows the *target's* power class, not the
/// mode alone: a low-power target's 0x2C carries `CTRL1_LOW_POWER` only in both `send` and
/// `send_with_ack` (every corpus hub's own shape for that class); only an always-alive target's
/// ACK bit changes between the two modes (see create_discover_confirm()'s doxygen for the
/// byte-shape cross-check against real captures).
///
/// `set_phase()` fires at most once per state for the whole step (not per try): the confirm step
/// can spend up to PAIRING_DISCOVER_CONFIRM_TRIES transmits, and PAIRING_TELEMETRY_MAX_EVENTS is a
/// fixed 32-event budget the pairing advisor scans — a per-try phase would spend events on exactly
/// the failure paths it is meant to diagnose. record_debug() (not telemetry) still runs every try.
pairing::DiscoverConfirmResult PairingEngine::run_discover_confirm_step_(pairing::PairingContext &context) {
  if (tuning_->pairing_discover_confirm == DiscoverConfirmMode::SKIP)
    return pairing::DiscoverConfirmResult::SKIPPED;

  const bool ack =
      tuning_->pairing_discover_confirm == DiscoverConfirmMode::SEND_WITH_ACK && !context.discovery_low_power;

  context.state = pairing::PairingState::TX_DISCOVER_CONFIRM;
  this->telemetry_.set_phase(context.state);

  bool wait_phase_started = false;
  pairing::DiscoverConfirmResult result = pairing::DiscoverConfirmResult::NO_REPLY;
  for (uint8_t try_index = 1; try_index <= PAIRING_DISCOVER_CONFIRM_TRIES; try_index++) {
    if (try_index > 1)
      App.feed_wdt();  // No extra retry gap: the 1.5 s windows already space the retransmissions.

    engine_.record_debug(pairing_stage_name(pairing::PairingState::TX_DISCOVER_CONFIRM), try_index, false);
    if (!create_discover_confirm(context.req, node_id_, context.device.node_id, context.discovery_low_power, ack) ||
        !engine_.transmit_frame(context.req, FREQ_CH2, pairing_start_preamble_(context.req))) {
      continue;  // A failed build/TX counts as a silent try, like wait_for_key_confirm_() does for 0x32.
    }

    context.state = pairing::PairingState::WAIT_DISCOVER_CONFIRM;
    if (!wait_phase_started) {
      this->telemetry_.set_phase(context.state);
      wait_phase_started = true;
    }
    engine_.record_debug(pairing_stage_name(context.state), try_index, false);

    const uint32_t wait_start_ms = millis();
    const decisions::PairingDiscoverConfirmDisposition disposition = wait_for_discover_confirm_ack_(try_index, context);

    if (disposition == decisions::PairingDiscoverConfirmDisposition::ACK) {
      ESP_LOGI(TAG, "Discover confirm: 0x2D from %s on freq=%" PRIu32 " after %" PRIu32 " ms (try %u/%u)",
               context.device_id.c_str(), context.packet.freq_hz, millis() - wait_start_ms, try_index,
               PAIRING_DISCOVER_CONFIRM_TRIES);
      result = pairing::DiscoverConfirmResult::ACKED;
      break;
    }
    if (disposition == decisions::PairingDiscoverConfirmDisposition::ERROR) {
      const uint8_t error_code = context.rx.data_len > 0 ? context.rx.data[0] : 0;
      ESP_LOGW(TAG, "Discover confirm: device %s answered 0x2C with error 0x%02X, continuing with key exchange",
               context.device_id.c_str(), error_code);
      result = pairing::DiscoverConfirmResult::ERROR_REPLY;
      break;
    }
    // IGNORE: timeout or an unrelated frame this try — spend the next try.
  }

  if (result == pairing::DiscoverConfirmResult::NO_REPLY) {
    ESP_LOGI(TAG, "Discover confirm: no 0x2D from %s after %u tries, continuing with key exchange",
             context.device_id.c_str(), PAIRING_DISCOVER_CONFIRM_TRIES);
  }

  // Applied for every result reaching here (i.e. every result except SKIPPED, which already
  // returned above), including NO_REPLY — one simple rule instead of a per-outcome one.
  if (tuning_->pairing_key_init_delay_ms > 0)
    delay_feeding_wdt(tuning_->pairing_key_init_delay_ms);

  return result;
}

/// Phase 2: authenticated key exchange (0x31 → 0x3C → 0x32 → 0x33).
///
/// Steps:
///   1. Transmit CMD_KEY_INIT (0x31), preamble per pairing_start_preamble_()
///   2. Wait for device challenge (0x3C)
///   3. Transmit CMD_KEY_TRANSFER (0x32) with encrypted system key
///   4. Wait for CMD_KEY_CONFIRM (0x33)
///
/// Step 4 depends on the driver's TX→RX turnaround: fast-turnaround radios await
/// the 0x33 through the standard send_and_receive() exchange; slow-turnaround
/// radios use the dedicated wait_for_key_confirm_() path with a key-init
/// re-trigger, because the 0x33 would otherwise arrive while the receiver is
/// still settling (see RadioDriver::has_fast_tx_rx_turnaround()).
///
/// The slow-turnaround re-trigger loop below (`for (int re = 0; ...)`) has two distinct outcomes
/// for its re-sent CMD_KEY_INIT, and they are handled differently:
///   - The device replies with CMD_KEY_CONFIRM (0x33) directly — it already had the key from the
///     first 0x32 and is just auto-confirming again. Nothing more to send.
///   - The device replies with a *fresh* CMD_CHALLENGE_REQ (0x3C) instead — proof it never received
///     the first 0x32 at all (a device that already holds the key doesn't re-challenge), so there
///     is no key transfer for it to confirm yet. The loop calls transfer_key_and_wait_confirm_()
///     again here, replaying CMD_KEY_TRANSFER against this new challenge, rather than discarding
///     the 0x3C and burning the retry for nothing — the device's next 0x31 would just produce
///     another fresh challenge either way, so retrying without resending 0x32 could never succeed.
///
/// That replay roughly doubles this function's worst-case blocking time when every wait times out
/// (approximately 3.5s -> 6.6s: two of the loop's iterations can now each wait out a full
/// key-transfer-and-confirm cycle instead of returning immediately on a missed 0x33). This is a
/// known, accepted trade-off: pairing already tolerates multi-second blocking exchanges (see
/// EXCHANGE_TOTAL_BUDGET_MS's own reasoning, proto_timing.h), and the alternative — leaving a
/// slow-turnaround device that never got its key stuck retrying forever — is worse than the
/// occasional slower failure path.
bool PairingEngine::run_key_exchange_phase_(pairing::PairingContext &context) {
  context.state = pairing::PairingState::TX_KEY_INIT;
  engine_.record_debug(pairing_stage_name(context.state), 1, false);
  this->telemetry_.set_phase(context.state);
  if (!create_key_init(context.key_init, node_id_, context.device.node_id) ||
      !engine_.transmit_frame(context.key_init, FREQ_CH2, pairing_start_preamble_(context.key_init))) {
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

  context.saw_key_challenge = true;
  // No challenge bytes here: the raw 0x3C payload plus the 0x3D response it provokes is a
  // known-plaintext/known-ciphertext pair under the system key (see redaction.h). The generic
  // frame-log helpers (log_frame()/log_component_capture()) already mask both commands.
  ESP_LOGI(TAG, "Challenge (0x3C) received: data_len=%u freq=%" PRIu32 " rssi=%d", context.rx.data_len,
           context.packet.freq_hz, radio_()->get_last_capture().rssi_dbm);

  bool key_ok = transfer_key_and_wait_confirm_(context);
  // Fast-turnaround radios catch the 0x33 through the standard exchange wait inside the helper
  // above and never reach this loop. Slow-turnaround radios miss it while re-entering RX, so on a
  // miss they re-send the key-init to trigger the device's auto-confirm. An explicit refusal is
  // the device's answer, not a missed confirm: re-sending the key-init after it only draws the same
  // refusal again (or, seen in the field, a refusal of a second key the device did not need).
  if (!radio_()->has_fast_tx_rx_turnaround()) {
    for (int re = 0; !key_ok && !context.key_refused && re < 2; re++) {
      ESP_LOGI(TAG, "Key confirm missed, re-sending key-init to trigger auto-confirm (attempt %d/2)", re + 1);
      App.feed_wdt();
      delay(EXCHANGE_RETRY_DELAY_MS);
      if (!engine_.transmit_frame(context.key_init, FREQ_CH2, pairing_start_preamble_(context.key_init)))
        continue;
      if (!wait_for_key_challenge_(PAIRING_KEY_CHALLENGE_TIMEOUT_MS, context.packet, context.rx,
                                   context.device.node_id))
        continue;
      if (context.rx.cmd == CMD_KEY_CONFIRM) {
        key_ok = true;
        continue;
      }
      context.saw_key_challenge = true;
      // A fresh CHALLENGE_REQ (0x3C), not a direct KEY_CONFIRM (0x33): the device never received
      // our first 0x32 in the first place, so there is no key for it to auto-confirm yet -- a
      // fresh 0x3C is exactly the signal that the right response is "resend 0x32 against this new
      // challenge", not "give up and burn the retry for nothing".
      key_ok = transfer_key_and_wait_confirm_(context);
    }
  }
  if (!key_ok) {
    ESP_LOGW(TAG, "Key exchange failed");
    return false;
  }
  return true;
}

/// Phase 3: send SetConfig1 (0x6F) once. Optional — see the header. Its preamble follows
/// pairing_start_preamble_(), like the other directed pairing start frames.
void PairingEngine::finalize_pairing_configuration_(pairing::PairingContext &context) {
  if (!create_set_config1(context.req, node_id_, context.device.node_id))
    return;
  // Its reply is never read: an explicit refusal and silence are equally harmless here.
  if (engine_.send_and_receive(context.req, context.resp, FREQ_CH2, PAIRING_SET_CONFIG1_MAX_TRIES,
                               pairing_start_preamble_(context.req)) == ExchangeOutcome::FAILED) {
    ESP_LOGI(TAG, "Pairing: no reply from %s to the optional SetConfig1 (0x6F); the pairing is complete",
             context.device_id.c_str());
  }
}

// --- Orchestrator ---

/// Pairing orchestrator — high-level three-phase flow.
///
/// Phase 1: run_discovery_phase_() finds a device in pairing mode.
/// Phase 2: run_key_exchange_phase_() performs authenticated key establishment.
/// Phase 3: finalize_pairing_configuration_() sends the optional SetConfig1 once.
///
/// On success the device is added to the registry and a YAML snippet is printed to the log.
/// The hub's thin wrapper manages the busy_ flag before and after this call.
bool PairingEngine::discover_and_pair() {
  this->telemetry_.begin();
  this->engine_.set_transmit_observer(&this->telemetry_);

  // Seed telemetry with a 1W pairing-gesture frame the hub overheard shortly before this call
  // (issue #27/#65): without this, a PROG press completed a few seconds before "Discover & Pair"
  // is pressed in the app is invisible to the advisor purely because the telemetry window opens
  // here, even though the radio heard it just fine — the false rf_silent advice this produced was
  // field-confirmed against a real trace (issue #27, miljaar).
  if (this->recent_oneway_sighting_.seen_ms != 0 &&
      (millis() - this->recent_oneway_sighting_.seen_ms) < PAIRING_RECENT_ONE_WAY_SIGHTING_WINDOW_MS) {
    this->telemetry_.record_recent_one_way_sighting(this->recent_oneway_sighting_);
  }

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

  if (tuning_->pairing_discovery_preamble < LONG_PREAMBLE) {
    ESP_LOGI(TAG, "Pairing: directed frames to %s use the %u-byte discovery preamble it just answered",
             context.device_id.c_str(), tuning_->pairing_discovery_preamble);
  }

  // Discover-confirm (0x2C -> 0x2D): once per attempt, before the key-exchange retry loop below,
  // so a retry of that loop never repeats it. Never fails the attempt — see the function's doc.
  this->run_discover_confirm_step_(context);

  // Phase 2: move the key, in the direction `pairing_key_transfer` selects (auto: the discovery
  // answer decides).
  if (key_pull_selected_(context)) {
    bool paired = false;
    if (this->run_pull_and_finish_(context, paired))
      return paired;
  } else if (!run_push_key_step_(context)) {
    this->finish_pairing_attempt_(context.key_refused ? PairingOutcome::KEY_TRANSFER_REFUSED
                                                      : PairingOutcome::KEY_EXCHANGE_FAILED);
    return false;
  }

  // Phase 3: optional SetConfig1. The 0x33 above completed the pairing.
  finalize_pairing_configuration_(context);

  context.state = pairing::PairingState::REGISTER_DEVICE;
  engine_.record_debug(pairing_stage_name(context.state), 1, true);
  this->telemetry_.set_phase(context.state);

  // The device just sent its 0x33, so it counts as heard from: its wake belief starts at "maybe
  // awake" for the follow-up exchanges rather than "asleep" (same stamp update_link_health() sets).
  context.device.last_seen_ms = millis();
  // A YAML entry for this node keeps its declared status; anything else lives until reboot.
  context.device.registered_by_pairing = registry_.get_declared(context.device_id) == nullptr;
  registry_.put(context.device_id, context.device);
  this->telemetry_.set_paired_device(context.device.node_id, context.device.type);

  const std::string type_diag = format_device_type_diagnostic(context.device.type);
  const std::string type_yaml = format_device_type_for_yaml(context.device.type);
  const std::string snippet = build_device_yaml_snippet(context.device.type, context.device.subtype, context.device_id,
                                                        context.discovery_metadata_complete, context.device.inverted,
                                                        context.discovery_low_power);

  if (snippet.empty()) {
    // metadata_complete is true here (build_device_yaml_snippet() never returns empty for
    // metadata_complete == false), so this is specifically "we know the type, but there's no
    // ESPHome platform for it yet" (device_has_platform()).
    ESP_LOGW(TAG,
             "Device %s paired successfully (%s), but no ESPHome platform drives it yet: type=%s "
             "class=%s subtype=%u.",
             context.device_id.c_str(), format_device_kind(context.device.type, context.device.subtype).c_str(),
             type_diag.c_str(), device_capability_class_name(context.device.type), context.device.subtype);
    ESP_LOGW(TAG,
             "No ready-to-paste YAML was generated. If you want to experiment manually, choose the most likely "
             "platform and set io_device_type: %s.",
             type_yaml.c_str());
    ESP_LOGW(TAG, "Please file a GitHub issue with this device type, subtype, model, and the pairing log so support "
                  "can be added.");

    context.state = pairing::PairingState::COMPLETE;
    engine_.record_debug(pairing_stage_name(context.state), 1, true);
    this->telemetry_.set_phase(context.state);
    this->finish_pairing_attempt_(PairingOutcome::PAIRED);
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
    this->finish_pairing_attempt_(PairingOutcome::PAIRED);
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
  this->finish_pairing_attempt_(PairingOutcome::PAIRED);
  return true;
}

bool PairingEngine::run_push_key_step_(pairing::PairingContext &context) {
  for (uint8_t ke_attempt = 0; ke_attempt < tuning_->pairing_key_exchange_retries; ke_attempt++) {
    if (ke_attempt > 0) {
      ESP_LOGI(TAG, "Retrying key exchange (attempt %d/%u)...", ke_attempt + 1, tuning_->pairing_key_exchange_retries);
      App.feed_wdt();
      delay(EXCHANGE_RETRY_DELAY_MS);
    }
    if (run_key_exchange_phase_(context))
      return true;
    // A refusal is final for this attempt: retrying the same push only repeats the question.
    if (context.key_refused)
      break;
  }
  // The device challenged our key-init, so it was talking to us; it may still have taken a key
  // whose confirm we lost. Without a challenge there is nothing to check.
  if (!context.saw_key_challenge)
    return false;
  return verify_device_holds_key_(context);
}

bool PairingEngine::verify_device_holds_key_(pairing::PairingContext &context) {
  context.state = pairing::PairingState::VERIFY_KEY_HELD;
  engine_.record_debug(pairing_stage_name(context.state), 1, false);
  this->telemetry_.set_phase(context.state);
  ESP_LOGI(TAG, "Key exchange: not confirmed; checking whether %s took the key anyway (authenticated name read)",
           context.device_id.c_str());
  if (!create_get_name(context.req, node_id_, context.device.node_id, context.discovery_low_power))
    return false;
  const ExchangeOutcome outcome = engine_.send_and_receive(
      context.req, context.resp, FREQ_CH2, PAIRING_KEY_VERIFY_MAX_TRIES, pairing_start_preamble_(context.req));
  const bool held = outcome == ExchangeOutcome::SUCCESS_WITH_RESPONSE && engine_.get_debug().saw_challenge &&
                    context.resp.cmd == CMD_GET_NAME_RESP;
  if (held) {
    ESP_LOGW(TAG,
             "Key exchange: %s never confirmed the key, but it answered an authenticated request with it. "
             "Treating it as paired.",
             context.device_id.c_str());
  } else if (outcome == ExchangeOutcome::SUCCESS_WITH_RESPONSE && context.resp.cmd == CMD_ERROR_RESP &&
             context.resp.data_len > 0) {
    // Refused outright: the device never got as far as checking a key.
    const uint8_t code = context.resp.data[0];
    ESP_LOGW(TAG,
             "Key exchange failed: %s refused the name read with 0x%02X %s (%s), so it does not act as a "
             "paired device",
             context.device_id.c_str(), code, command_result_name(code), command_result_description(code));
  } else {
    ESP_LOGW(TAG, "Key exchange failed: %s does not answer authenticated requests with our key",
             context.device_id.c_str());
  }
  return held;
}

void PairingEngine::note_key_refusal_(pairing::PairingContext &context, const IoFrame &reply, const char *step) {
  const uint8_t code = reply.data_len > 0 ? reply.data[0] : 0;
  context.key_refused = true;
  context.key_refusal_code = code;
  this->telemetry_.set_key_refusal(code);
  ESP_LOGW(TAG, "%s: device %s refused with 0x%02X %s (%s; code %u, state %u)", step, context.device_id.c_str(), code,
           command_result_name(code), command_result_description(code), error_resp_code(code), error_resp_state(code));
}

bool PairingEngine::wait_for_pulled_key_(pairing::PairingContext &context) {
  ListenSpec spec;
  spec.window_ms = PAIRING_KEY_PULL_REPLY_TIMEOUT_MS;
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;  // Unicast reply to a unicast request, as for 0x33.

  auto outcome =
      engine_.listen(spec, context.packet, context.rx, [&](const IoFrame *parsed, const RadioRxPacket & /*packet*/) {
        if (parsed == nullptr || memcmp(parsed->src, context.device.node_id, NODE_ID_SIZE) != 0 ||
            memcmp(parsed->dst, node_id_, NODE_ID_SIZE) != 0)
          return ReplyDisposition::IGNORE;
        const int16_t rssi = radio_()->get_last_capture().rssi_dbm;
        if (parsed->cmd == CMD_KEY_TRANSFER && parsed->data_len >= AES_KEY_SIZE) {
          this->telemetry_.record_rx(*parsed, rssi);
          return ReplyDisposition::ACCEPT;
        }
        this->telemetry_.record_rx_reject(*parsed, rssi);
        if (parsed->cmd == CMD_ERROR_RESP) {
          this->note_key_refusal_(context, *parsed, "Key pull");
          return ReplyDisposition::ABORT;
        }
        return ReplyDisposition::IGNORE;
      });
  return outcome == ListenOutcome::ACCEPTED;
}

bool PairingEngine::key_pull_selected_(const pairing::PairingContext &context) const {
  switch (tuning_->pairing_key_transfer) {
    case KeyTransferMode::PULL:
      return true;
    case KeyTransferMode::PUSH:
      return false;
    case KeyTransferMode::AUTO:
      break;
  }
  // A device that answers discovery as a remote controller (node type 1023) hands out its own key and
  // refuses a pushed one; every other device takes a push. No switch to the other direction inside the
  // attempt: a refusal repeats deterministically, and a retry only costs pairing time.
  const bool pull = context.device.type == DeviceType::REMOTE_CONTROLLER;
  ESP_LOGI(TAG, "Key transfer: auto -> %s (%s answered discovery as %s)", pull ? "pull" : "push",
           context.device_id.c_str(), format_device_kind(context.device.type, context.device.subtype).c_str());
  return pull;
}

uint8_t PairingEngine::roll_call_and_register_(pairing::PairingContext &context, const uint8_t *key, bool device_key) {
  // Every device that holds the key answers a roll-call built with it. For an Atlantic AGILIA radiator
  // that is the heater at the next address up, not the node that handed the key over.
  context.state = pairing::PairingState::SCAN_PULLED_KEY;
  engine_.record_debug(pairing_stage_name(context.state), 1, true);
  this->telemetry_.set_phase(context.state);
  ScanState state{};
  // An Atlantic radiator's heater answers a direct read every time but a roll-call only now and then
  // (about one request in four), so it is asked first; the roll-call is the fallback, and the only
  // search for any other device.
  if (!this->probe_heater_at_next_address_(context, key, device_key, state)) {
    uint8_t attempt = 0;
    const uint8_t total_attempts = count_enabled_passes(tuning_->scan_power_classes) * SCAN_CHANNEL_COUNT;
    ESP_LOGI(TAG, "Roll-call with key check value %s (%s %s handed over)", detail::format_key_check_value(key).c_str(),
             device_key ? "the key" : "the hub key, which", context.device_id.c_str());
    if (run_roll_call_sweep(engine_, node_id_, key, /*key_slot=*/device_key ? 1 : 0, *tuning_, state, attempt,
                            total_attempts) == RollCallAttemptResult::BUILD_FAILED) {
      ESP_LOGW(TAG, "Could not build the roll-call under the pulled key");
    }
  }

  // One report for the device log only: with a device key it carries the key, so the pairing summary, the
  // Last Pairing Result sensor and every Home Assistant event keep to the key check value. A hub-key report
  // carries no key and has no banner.
  const PulledKeyReportInfo info{context.device_id, context.device.type, context.device.subtype, key, system_key_};
  if (device_key) {
    ESP_LOGW(TAG, "========================================");
    detail::log_multiline_result(TAG, /*is_warning=*/true, /*prefix=*/"",
                                 build_pulled_key_report(info, state, registry_));
    ESP_LOGW(TAG, "========================================");
  } else if (state.count != 0) {
    detail::log_multiline_result(TAG, /*is_warning=*/false, /*prefix=*/"",
                                 build_hub_key_pull_report(info, state, registry_));
  }

  // Register each device the report offers a block for, so it can be controlled until the YAML is
  // flashed: with the key it answered under when that is a device key, with the hub key otherwise. A
  // device the YAML declares keeps its record; one an earlier pairing in this boot added is replaced, so
  // pairing it again counts as paired. One of a type no platform drives yet is reported but not registered.
  uint8_t registered = 0;
  for (uint8_t i = 0; i < state.count; i++) {
    const ScanResponder &responder = state.responders[i];
    const std::string responder_id = node_id_to_string(responder.src);
    if (registry_.get_declared(responder_id) != nullptr || !device_has_platform(responder.type, responder.subtype))
      continue;
    IoDevice device{};
    memcpy(device.node_id, responder.src, NODE_ID_SIZE);
    device.type = responder.type;
    device.subtype = responder.subtype;
    device.inverted = responder.inverted;
    device.low_power = responder.has_extended && discovery_power_save_mode(responder.flags) == POWER_SAVE_LOW_POWER;
    device.has_system_key = device_key;
    if (device_key)
      memcpy(device.system_key, key, AES_KEY_SIZE);
    device.last_seen_ms = millis();
    device.registered_by_pairing = true;
    registry_.put(responder_id, device);
    registered++;
    ESP_LOGI(TAG, "Pairing: registered %s (%s) until reboot; paste its block above to keep it", responder_id.c_str(),
             format_device_kind(responder.type, responder.subtype).c_str());
  }
  return registered;
}

bool PairingEngine::probe_heater_at_next_address_(const pairing::PairingContext &context, const uint8_t *key,
                                                  bool device_key, ScanState &state) {
  uint8_t heater[NODE_ID_SIZE];
  if (!decisions::pairing_node_has_heater_at_next_address(context.device.type, context.discovery_manufacturer) ||
      !decisions::next_node_id(context.device.node_id, heater))
    return false;
  const std::string heater_id = node_id_to_string(heater);
  // A YAML entry keeps its own key; the roll-call reports it against that.
  if (registry_.get_declared(heater_id) != nullptr)
    return false;

  // The exchange picks its key from the registry, so the candidate is in it for the read only; the
  // caller registers it properly if it answered. A record an earlier pairing in this boot left there is
  // put back afterwards.
  const IoDevice *earlier = registry_.get(heater_id);
  const bool had_earlier = earlier != nullptr;
  const IoDevice previous = had_earlier ? *earlier : IoDevice{};
  IoDevice candidate{};
  memcpy(candidate.node_id, heater, NODE_ID_SIZE);
  candidate.type = DeviceType::ELECTRICAL_HEATER;
  candidate.subtype = SUBTYPE_HEATER_WITH_SETPOINT;
  candidate.has_system_key = device_key;
  if (device_key)
    memcpy(candidate.system_key, key, AES_KEY_SIZE);
  candidate.registered_by_pairing = true;
  registry_.put(heater_id, candidate);

  // The maximum setpoint is a read-only register every such heater holds, so reading it changes nothing.
  uint8_t payload[HEATING_PAYLOAD_MAX_SIZE];
  const size_t payload_len = encode_heating_read(HeatingFunction::MAX_SETPOINT, payload);
  IoFrame request{};
  IoFrame response{};
  float max_setpoint_c = NAN;
  ESP_LOGI(TAG, "Heater probe: reading %s, the address after %s, under key check value %s", heater_id.c_str(),
           context.device_id.c_str(), detail::format_key_check_value(key).c_str());
  const bool answered =
      payload_len != 0 && create_write_private(request, node_id_, heater, /*low_power=*/false, payload, payload_len) &&
      engine_.send_and_receive(request, response, FREQ_CH2) == ExchangeOutcome::SUCCESS_WITH_RESPONSE &&
      decode_heating_reply(response, HeatingFunction::MAX_SETPOINT, max_setpoint_c);
  const int16_t rssi = radio_()->get_last_capture().rssi_dbm;
  if (had_earlier) {
    registry_.put(heater_id, previous);
  } else {
    registry_.remove_registered_by_pairing(heater_id);
  }
  if (!answered) {
    ESP_LOGI(TAG, "Heater probe: %s did not answer; looking for devices with a roll-call instead", heater_id.c_str());
    return false;
  }
  ESP_LOGI(TAG, "Heater probe: %s answered (maximum setpoint %.1f C); no roll-call needed", heater_id.c_str(),
           max_setpoint_c);

  // Only the register protocol answers this read, and only the heater with an adjustable setpoint
  // speaks it; manufacturer and flags come from no reply of its own, so the report omits them.
  ScanResponder &responder = state.responders[0];
  memcpy(responder.src, heater, NODE_ID_SIZE);
  responder.type = DeviceType::ELECTRICAL_HEATER;
  responder.subtype = SUBTYPE_HEATER_WITH_SETPOINT;
  responder.inverted = false;
  responder.rssi_dbm = rssi;
  responder.manufacturer = context.discovery_manufacturer;
  responder.flags = 0;
  responder.has_extended = false;
  responder.metadata_complete = true;
  responder.key_slot = device_key ? 1 : 0;
  state.count = 1;
  state.truncated = false;
  return true;
}

void PairingEngine::finish_paired_by_roll_call_(pairing::PairingContext &context) {
  // `node=` in the frozen result string stays the node that was discovered; the responders appear in
  // the log report only.
  this->telemetry_.set_paired_device(context.device.node_id, context.device.type);
  context.state = pairing::PairingState::COMPLETE;
  engine_.record_debug(pairing_stage_name(context.state), 1, true);
  this->telemetry_.set_phase(context.state);
  this->finish_pairing_attempt_(PairingOutcome::PAIRED);
}

bool PairingEngine::run_pull_and_finish_(pairing::PairingContext &context, bool &paired) {
  this->telemetry_.set_key_pull_attempted();
  const KeyPullResult pulled = run_key_pull_phase_(context);
  if (pulled == KeyPullResult::OTHER_KEY) {
    paired = this->register_pulled_key_devices_(context);
    return true;
  }
  if (pulled == KeyPullResult::FAILED) {
    this->finish_pairing_attempt_(context.key_refused ? PairingOutcome::KEY_TRANSFER_REFUSED
                                                      : PairingOutcome::KEY_EXCHANGE_FAILED);
    paired = false;
    return true;
  }
  // The device shares the hub's key. A device with a platform of its own is paired as after a push.
  if (device_has_platform(context.device.type, context.device.subtype))
    return false;
  // A node with no platform of its own (an AGILIA's remote-controller node): the devices that hold the
  // key are elsewhere, so look for them, as after a foreign key, and register the ones found with the
  // hub key. With none found the attempt ends here: the node itself cannot be controlled, and it
  // refuses the SetConfig1 the push path sends.
  if (this->roll_call_and_register_(context, system_key_, /*device_key=*/false) > 0) {
    this->finish_paired_by_roll_call_(context);
    paired = true;
    return true;
  }
  ESP_LOGW(TAG, "Pairing: %s holds this hub's key, but no device that holds it answered; nothing registered",
           context.device_id.c_str());
  this->finish_pairing_attempt_(PairingOutcome::HUB_KEY_RETURNED);
  paired = false;
  return true;
}

bool PairingEngine::register_pulled_key_devices_(pairing::PairingContext &context) {
  const uint8_t registered = this->roll_call_and_register_(context, context.pulled_key, /*device_key=*/true);
  memset(context.pulled_key, 0, AES_KEY_SIZE);
  context.has_pulled_key = false;
  if (registered == 0) {
    this->finish_pairing_attempt_(PairingOutcome::KEY_RECEIVED);
    return false;
  }
  this->finish_paired_by_roll_call_(context);
  return true;
}

PairingEngine::KeyPullResult PairingEngine::run_key_pull_phase_(pairing::PairingContext &context) {
  ESP_LOGW(TAG, "Key pull: asking %s for its key", context.device_id.c_str());
  uint8_t challenge[HMAC_SIZE];
  bool got_key = false;
  // The same channel rule as an exchange: a rotating band starts on the channel that last carried an
  // answer and moves on with every silent try; the proof goes out where the key came from.
  const bool rotate = radio_()->retries_rotate_channel();
  const uint32_t retry_gap_ms = radio_()->retry_gap_ms(*tuning_);
  uint32_t pull_freq = FREQ_CH2;
  for (uint8_t tries = 0; tries < EXCHANGE_RETRY_COUNT && !got_key && !context.key_refused; tries++) {
    if (tries > 0) {
      App.feed_wdt();
      delay(retry_gap_ms);
    }
    pull_freq = decisions::retry_frequency(rotate, FREQ_CH2, engine_.last_answered_freq(), tries);
    context.state = pairing::PairingState::TX_KEY_PULL;
    engine_.record_debug(pairing_stage_name(context.state), tries + 1, false);
    this->telemetry_.set_phase(context.state);
    // Fresh random bytes per try: the IV of the device's answer is built from the request it answers.
    crypto::generate_challenge(challenge);
    if (!create_launch_key_transfer(context.req, node_id_, context.device.node_id, challenge) ||
        !engine_.transmit_frame(context.req, pull_freq, pairing_start_preamble_(context.req)))
      continue;
    context.state = pairing::PairingState::WAIT_KEY_PULL_REPLY;
    this->telemetry_.set_phase(context.state);
    got_key = wait_for_pulled_key_(context);
    if (got_key)
      engine_.set_last_answered_freq(pull_freq);
    if (!got_key && !context.key_refused) {
      ESP_LOGI(TAG, "Key pull try %u: no key transfer (0x32) from %s within %" PRIu32 " ms", tries + 1,
               context.device_id.c_str(), PAIRING_KEY_PULL_REPLY_TIMEOUT_MS);
    }
  }
  if (!got_key) {
    ESP_LOGW(TAG, "Key pull failed: %s sent no key", context.device_id.c_str());
    return KeyPullResult::FAILED;
  }

  uint8_t key[AES_KEY_SIZE];
  if (!recover_key_from_pull(context.req, context.rx.data, key))
    return KeyPullResult::FAILED;
  // The key check value names the key without revealing it, so two attempts or two devices can be
  // compared from the log alone.
  const std::string check_value = detail::format_key_check_value(key);
  // Our own key cannot come out of the decrypt by chance, so a match proves the device holds it.
  if (memcmp(key, system_key_, AES_KEY_SIZE) == 0) {
    ESP_LOGI(TAG, "Key pull check: %s -> key check value %s, the hub's own key", context.device_id.c_str(),
             check_value.c_str());
    return KeyPullResult::OUR_KEY;
  }

  context.state = pairing::PairingState::VERIFY_PULLED_KEY;
  this->telemetry_.set_phase(context.state);
  // The device's 0x3D over its own 0x32, under the key it sent, proves the decrypt and that it holds
  // the key (a chance match is 1 in 2^48). A lost answer is challenged again; a wrong one is final.
  ExchangeEngine::ChallengeResult proof = ExchangeEngine::ChallengeResult::NO_ANSWER;
  uint8_t challenges = 0;
  while (challenges < EXCHANGE_RETRY_COUNT && proof == ExchangeEngine::ChallengeResult::NO_ANSWER) {
    if (challenges > 0) {
      App.feed_wdt();
      delay(retry_gap_ms);
    }
    challenges++;
    proof = engine_.challenge_sender(context.rx, pull_freq, key);
  }
  if (proof == ExchangeEngine::ChallengeResult::VERIFIED) {
    ESP_LOGI(TAG, "Key pull check: %s -> key check value %s, proven by its 0x3D", context.device_id.c_str(),
             check_value.c_str());
    memcpy(context.pulled_key, key, AES_KEY_SIZE);
    context.has_pulled_key = true;
    return KeyPullResult::OTHER_KEY;
  }
  this->telemetry_.set_key_pull_unproven();
  if (proof == ExchangeEngine::ChallengeResult::MISMATCH) {
    ESP_LOGW(TAG,
             "Key pull check: %s -> key check value %s, not proven: its 0x3D does not match. The key is not "
             "printed and must not be used",
             context.device_id.c_str(), check_value.c_str());
  } else {
    ESP_LOGW(TAG,
             "Key pull check: %s -> key check value %s, not proven: no 0x3D received after %u challenges. The key "
             "is not printed and must not be used",
             context.device_id.c_str(), check_value.c_str(), challenges);
  }
  return KeyPullResult::FAILED;
}

uint32_t PairingEngine::pull_key_worst_case_ms(const uint8_t controller[NODE_ID_SIZE]) {
  IoFrame request{};
  uint8_t challenge[HMAC_SIZE]{};
  uint32_t transmit_ms = 0;
  if (create_launch_key_transfer(request, node_id_, controller, challenge))
    transmit_ms = engine_.transmit_time_ms(request, FREQ_CH2, pairing_start_preamble_(request));
  const uint32_t gap_ms = radio_()->retry_gap_ms(*tuning_);
  const uint32_t pull_ms = EXCHANGE_RETRY_COUNT * (transmit_ms + PAIRING_KEY_PULL_REPLY_TIMEOUT_MS + gap_ms);
  const uint32_t proof_ms = EXCHANGE_RETRY_COUNT * (tuning_->exchange_response_wait_ms + gap_ms);
  return pull_ms + proof_ms;
}

PairingEngine::PullKeyReport PairingEngine::pull_key_from(const uint8_t controller[NODE_ID_SIZE]) {
  this->telemetry_.begin();
  this->telemetry_.set_key_pull_only();
  this->telemetry_.set_key_pull_attempted();
  this->engine_.set_transmit_observer(&this->telemetry_);

  pairing::PairingContext context;
  memcpy(context.device.node_id, controller, NODE_ID_SIZE);
  context.device.type = DeviceType::REMOTE_CONTROLLER;
  context.device_id = node_id_to_string(controller);
  this->telemetry_.set_discovered_device(controller, context.device.type);

  PullKeyReport report;
  const KeyPullResult pulled = run_key_pull_phase_(context);
  PairingOutcome outcome = PairingOutcome::KEY_EXCHANGE_FAILED;
  switch (pulled) {
    case KeyPullResult::OTHER_KEY: {
      report.outcome = PullKeyOutcome::PROVEN;
      outcome = PairingOutcome::KEY_RECEIVED;
      // The device log gets the key with its banner; the event and the pairing summary keep to the check value.
      const ScanState none{};
      const PulledKeyReportInfo info{context.device_id,  context.device.type, context.device.subtype,
                                     context.pulled_key, system_key_,         /*searched=*/false};
      report.log_report = build_pulled_key_report(info, none, registry_);
      break;
    }
    case KeyPullResult::OUR_KEY:
      report.outcome = PullKeyOutcome::OWN_KEY;
      outcome = PairingOutcome::HUB_KEY_RETURNED;
      break;
    case KeyPullResult::FAILED:
      if (context.key_refused) {
        report.outcome = PullKeyOutcome::REFUSED;
        report.has_refusal_code = true;
        report.refusal_code = context.key_refusal_code;
        outcome = PairingOutcome::KEY_TRANSFER_REFUSED;
      } else {
        report.outcome = this->telemetry_.key_pull_unproven() ? PullKeyOutcome::UNPROVEN : PullKeyOutcome::SILENT;
      }
      break;
  }
  memset(context.pulled_key, 0, AES_KEY_SIZE);
  context.has_pulled_key = false;
  this->finish_pairing_attempt_(outcome);

  advisor::PairingAdvice advice[advisor::PAIRING_ADVICE_MAX];
  const uint8_t advice_count = advisor::analyze_pairing_telemetry(this->telemetry_, this->node_id_, advice);
  for (uint8_t i = 0; i < advice_count; i++) {
    if (!report.message.empty())
      report.message += ' ';
    report.message += advisor::pairing_advice_message(advice[i]);
  }
  return report;
}

void PairingEngine::finish_pairing_attempt_(PairingOutcome outcome) {
  this->telemetry_.set_outcome(outcome);
  this->engine_.set_transmit_observer(nullptr);
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
