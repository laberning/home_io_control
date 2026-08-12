#include "hub_internal.h"

#include "pairing_responder.h"
#include "proto_commands.h"
#include "proto_crypto.h"

#include <esp_random.h>

#include <cstdio>
#include <cstring>

/// @file hub_key_extraction.cpp
/// @brief "Accept Foreign Pairing (Key Extraction)" — device-role responder hub wiring.
/// @ingroup hioc_hub
///
/// Owns the impure side of the key-extraction feature: arming/disarming, throwaway node-ID
/// generation, the 10-minute auto-off timer, transmitting device-role replies, and the
/// security-sensitive result log block. The pure state-transition decisions live in
/// pairing_responder.h/.cpp; the three RX branches that call into this file are in
/// process_received_packet_() (hub_status.cpp).
///
/// @note Hardware-confirmed 2026-08-02: a full extraction (0x28 through 0x33) between two real
/// boards — SX1276 running this responder, SX1262 running this project's own PairingEngine as
/// the "hub" — recovered the hub's node_id/system_key byte-for-byte. That validates the crypto,
/// the state machine, and the radio wiring end-to-end on real RF hardware.
/// @warning What that test does NOT validate: compatibility with a genuine third-party hub
/// (Somfy TaHoma/Smoove, Velux KLF200, etc.). The device-role frames built here
/// (create_discover_resp(), create_challenge_req(), create_key_confirm()) were reverse-engineered
/// from this project's own encoder and a small number of captures. The self-test above
/// necessarily agrees with those conventions (it's the same codebase on both ends); a real hub's
/// exact requirements (discovery-response field completeness, retry cadence) may still differ.
/// recover_system_key_from_transfer()'s IV-derivation formula itself is now pinned against two
/// externally-captured known-answer key transfers (ProtoCrypto.CryptKeyMatchesDocumented*Capture
/// in proto_crypto_test.cpp), so that specific formula is no longer only self-derived — though
/// both captures are short requests and don't exercise construct_iv()'s 8-byte truncation window,
/// so a real hub sending a longer request is still an open question. Treat a recovered key as
/// unconfirmed until it has been verified against a real hub, or by successfully controlling a
/// device with it.

namespace esphome {
namespace home_io_control {

namespace {

constexpr uint32_t KEY_EXTRACTION_AUTO_OFF_MS = 10 * 60 * 1000;  ///< Arm window: 10 minutes.
// TODO(hardware-verify): confirm a real hub's pairing flow doesn't validate the advertised
// manufacturer/type against a known-device allowlist before completing key exchange —
// Somfy/roller-shutter is a plausible but unconfirmed default.
constexpr uint8_t KEY_EXTRACTION_MANUFACTURER_ID = MANUFACTURER_SOMFY;  ///< Plausible, widely-supported default.
constexpr DeviceType KEY_EXTRACTION_ADVERTISED_TYPE = DeviceType::ROLLER_SHUTTER;  ///< Plausible default device type.
constexpr uint8_t KEY_EXTRACTION_ADVERTISED_SUBTYPE = 0;
constexpr uint8_t KEY_EXTRACTION_ID_GEN_MAX_ATTEMPTS = 16;  ///< Collision-retry budget for the throwaway node ID.
constexpr const char *KEY_EXTRACTION_TIMEOUT_NAME = "key_extraction_auto_off";
constexpr uint32_t RANDOM_LOW_BYTE_MASK = 0xFF;  ///< Isolates one random byte from esp_random()'s 32-bit output.

/// Format a 16-byte key as an uppercase hex string. The one deliberate place system-key bytes
/// are formatted for display — see log_key_extraction_result_() and redaction.h.
std::string format_key_hex(const uint8_t key[AES_KEY_SIZE]) {
  std::string out;
  out.reserve(AES_KEY_SIZE * 2);
  char byte_buf[3];
  for (uint8_t i = 0; i < AES_KEY_SIZE; i++) {
    snprintf(byte_buf, sizeof(byte_buf), "%02X", key[i]);
    out += byte_buf;
  }
  return out;
}

}  // namespace

void IOHomeControlComponent::generate_key_extraction_throwaway_id_(uint8_t out[NODE_ID_SIZE]) {
  for (uint8_t attempt = 0; attempt < KEY_EXTRACTION_ID_GEN_MAX_ATTEMPTS; attempt++) {
    for (uint8_t i = 0; i < NODE_ID_SIZE; i++)
      out[i] = static_cast<uint8_t>(esp_random() & RANDOM_LOW_BYTE_MASK);
    if (!stored_node_id_is_valid(out))
      continue;
    if (memcmp(out, this->node_id_, NODE_ID_SIZE) == 0)
      continue;
    if (memcmp(out, BROADCAST_DISCOVER, NODE_ID_SIZE) == 0 || memcmp(out, BROADCAST_DISCOVER_ALT, NODE_ID_SIZE) == 0)
      continue;
    if (this->registry_.get(node_id_to_string(out)) != nullptr)
      continue;
    return;
  }
  // Every attempt collided (astronomically unlikely for a 3-byte space against a handful of
  // reserved/registered IDs) — fall through and use the last-generated candidate rather than
  // leaving the buffer stale; a false collision here only degrades to "discovery/key-init from
  // the colliding real device also gets intercepted," not a crash or security issue.
}

void IOHomeControlComponent::set_key_extraction_armed(bool armed) {
  if (!armed) {
    if (this->key_extraction_ctx_.state == pairing_responder::ResponderState::DISARMED)
      return;
    this->key_extraction_ctx_ = pairing_responder::ResponderContext{};
    ESP_LOGI(detail::TAG, "Key extraction: disarmed");
    if (this->key_extraction_armed_callback_)
      this->key_extraction_armed_callback_(false);
    return;
  }

  this->key_extraction_ctx_ = pairing_responder::ResponderContext{};
  this->generate_key_extraction_throwaway_id_(this->key_extraction_ctx_.throwaway_id);
  this->key_extraction_ctx_.advertised_type = KEY_EXTRACTION_ADVERTISED_TYPE;
  this->key_extraction_ctx_.advertised_subtype = KEY_EXTRACTION_ADVERTISED_SUBTYPE;
  this->key_extraction_ctx_.state = pairing_responder::ResponderState::ARMED_IDLE;

  ESP_LOGW(detail::TAG,
           "Key extraction: ARMED for 10 minutes, throwaway ID %s. Put your existing hub into pairing/add-device "
           "mode now. Not yet confirmed against a third-party hub — see docs for details.",
           node_id_to_string(this->key_extraction_ctx_.throwaway_id).c_str());

  this->set_timeout(KEY_EXTRACTION_TIMEOUT_NAME, KEY_EXTRACTION_AUTO_OFF_MS, [this]() {
    // Guards against a stale timeout firing after a manual disarm/re-arm already ran; this hub's
    // set_timeout() replaces any pending callback with the same name, but the check is cheap
    // insurance and documents the intent either way.
    if (this->key_extraction_ctx_.state == pairing_responder::ResponderState::DISARMED)
      return;
    if (this->key_extraction_ctx_.state == pairing_responder::ResponderState::ARMED_IDLE) {
      ESP_LOGW(detail::TAG, "Key extraction: window expired, no pairing attempt seen. Disarming.");
    } else {
      ESP_LOGW(detail::TAG, "Key extraction: window expired while in progress (reached stage=%s). Disarming.",
               pairing_responder::responder_stage_name(this->key_extraction_ctx_.state));
    }
    this->set_key_extraction_armed(false);
  });

  if (this->key_extraction_armed_callback_)
    this->key_extraction_armed_callback_(true);
}

bool IOHomeControlComponent::try_handle_key_extraction_frame_(const IoFrame &frame) {
  if (this->key_extraction_ctx_.state == pairing_responder::ResponderState::DISARMED)
    return false;

  if (frame.cmd == CMD_DISCOVER_REQ) {
    this->handle_key_extraction_discover_(frame);
    return true;
  }
  if (memcmp(frame.dst, this->key_extraction_ctx_.throwaway_id, NODE_ID_SIZE) != 0)
    return false;
  if (frame.cmd == CMD_KEY_INIT) {
    this->handle_key_extraction_key_init_(frame);
    return true;
  }
  if (frame.cmd == CMD_KEY_TRANSFER) {
    this->handle_key_extraction_key_transfer_(frame);
    return true;
  }
  return false;
}

void IOHomeControlComponent::broadcast_key_extraction_reply_(const IoFrame &frame) {
  // Broadcast on all 3 channels like the CMD_STATUS_UPDATE_RESP ack in hub_status.cpp: we don't
  // know which channel the foreign hub is listening on after transmitting its own frame. Use the
  // driver's own response_preamble() (12 bytes for SX1276, 8 for SX1262) rather than a flat
  // SHORT_PREAMBLE(8) or LONG_PREAMBLE(1024) constant — this is the same chip-tuned "reply, not a
  // cold start" preamble ExchangeEngine/PairingEngine already use for every other reply in this
  // codebase (see exchange_engine.cpp, pairing_engine.cpp), and it exists for exactly this
  // problem: long enough that a channel-hopping receiver reliably lands on it, short enough that
  // 3 sequential channel transmissions don't block the main loop for the better part of a second
  // (hardware-confirmed 2026-08-02: LONG_PREAMBLE on all 3 channels blocked long enough to blow
  // through the hub's tight per-try wait windows and broke both directions).
  const uint16_t preamble = this->radio_->response_preamble();
  this->transmit_frame_(frame, FREQ_CH1, preamble);
  this->transmit_frame_(frame, FREQ_CH2, preamble);
  this->transmit_frame_(frame, FREQ_CH3, preamble);
}

void IOHomeControlComponent::handle_key_extraction_discover_(const IoFrame &frame) {
  if (!pairing_responder::on_discover_request(this->key_extraction_ctx_))
    return;

  IoFrame resp;
  if (!create_discover_resp(resp, this->key_extraction_ctx_.throwaway_id, frame.src,
                            this->key_extraction_ctx_.advertised_type, this->key_extraction_ctx_.advertised_subtype,
                            KEY_EXTRACTION_MANUFACTURER_ID)) {
    ESP_LOGW(detail::TAG, "Key extraction: failed to build discovery response");
    return;
  }
  this->broadcast_key_extraction_reply_(resp);
  ESP_LOGI(detail::TAG, "Key extraction: replied to discovery from hub %s with throwaway ID %s",
           node_id_to_string(frame.src).c_str(), node_id_to_string(this->key_extraction_ctx_.throwaway_id).c_str());
}

void IOHomeControlComponent::handle_key_extraction_key_init_(const IoFrame &frame) {
  uint8_t candidate_challenge[HMAC_SIZE];
  crypto::generate_challenge(candidate_challenge);
  if (!pairing_responder::on_key_init(this->key_extraction_ctx_, candidate_challenge, frame.src))
    return;

  IoFrame resp;
  if (!create_challenge_req(resp, frame.src, this->key_extraction_ctx_.throwaway_id,
                            this->key_extraction_ctx_.challenge)) {
    ESP_LOGW(detail::TAG, "Key extraction: failed to build challenge request");
    return;
  }
  this->broadcast_key_extraction_reply_(resp);
  ESP_LOGI(detail::TAG, "Key extraction: sent challenge to hub %s", node_id_to_string(frame.src).c_str());
}

void IOHomeControlComponent::handle_key_extraction_key_transfer_(const IoFrame &frame) {
  if (frame.data_len < AES_KEY_SIZE) {
    ESP_LOGW(detail::TAG, "Key extraction: key-transfer payload too short (%u bytes)", frame.data_len);
    return;
  }
  if (!pairing_responder::on_key_transfer(this->key_extraction_ctx_, frame.data))
    return;

  IoFrame resp;
  if (create_key_confirm(resp, this->key_extraction_ctx_.throwaway_id, frame.src)) {
    this->broadcast_key_extraction_reply_(resp);
  } else {
    ESP_LOGW(detail::TAG, "Key extraction: failed to build key confirm");
  }

  // Log before disarming: disarm resets key_extraction_ctx_, which is where the recovered key
  // and the hub's real node ID live.
  this->log_key_extraction_result_();
  // Immediately disarm: a second hub attempting to pair mid-window must not also succeed and
  // produce a second, confusing log block (see the feature plan's rollout notes).
  this->set_key_extraction_armed(false);
}

// TODO(hardware-verify): the feature plan's §3/§6 step 8 recommends an authenticated read-back
// to the foreign hub using the recovered key before trusting it. recover_system_key_from_transfer()'s
// IV-derivation formula is now pinned against externally-captured known-answer key transfers (see
// the file-level @warning above), so that piece is no longer just self-consistent — but nothing
// here confirms this specific extraction talks to a real third-party hub correctly, which is now
// the single highest-risk unverified piece of this feature. That read-back subflow was
// deliberately deferred here: it would require carving a narrow exception into
// is_exchange_internal_command()'s 0x3C/0x3D early-drop (hub_status.cpp) for a second unverified
// vendor-hub interaction, doubling the protocol-speculation surface for a feature that already
// ships marked experimental. The key is still always printed (gating it on an equally-unverified
// secondary check risks hiding a correct key), but the log below says so.
void IOHomeControlComponent::log_key_extraction_result_() {
  const std::string node_id_str = node_id_to_string(this->key_extraction_ctx_.hub_node_id);
  const std::string key_str = format_key_hex(this->key_extraction_ctx_.recovered_key);
  // Deliberate, explicit exception to redaction.h's masking — see that file and README.md's
  // "Reporting Unsupported Devices" section, which already warns about pairing logs and the
  // shared TRANSFER_KEY in almost identical terms. Do NOT route this through the generic
  // frame-log helpers (log_frame()/log_component_capture()); those must keep masking 0x32.
  ESP_LOGW(detail::TAG, "========================================");
  ESP_LOGW(detail::TAG, "SYSTEM KEY EXTRACTED -- DO NOT SHARE YOUR SYSTEM KEY");
  ESP_LOGW(detail::TAG, "Anyone with this key and node_id can control every device on this installation.");
  ESP_LOGW(detail::TAG, "This exchange has not been independently confirmed against your specific hub -- test");
  ESP_LOGW(detail::TAG, "this key (e.g. by controlling a device with it) before relying on it.");
  ESP_LOGW(detail::TAG, "Copy the block below into a new hub's YAML.");
  ESP_LOGW(detail::TAG, "========================================");
  ESP_LOGW(detail::TAG, "home_io_control:");
  ESP_LOGW(detail::TAG, "  node_id: \"%s\"", node_id_str.c_str());
  ESP_LOGW(detail::TAG, "  system_key: \"%s\"", key_str.c_str());
  ESP_LOGW(detail::TAG, "========================================");
}

}  // namespace home_io_control
}  // namespace esphome
