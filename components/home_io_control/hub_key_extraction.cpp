#include "hub_internal.h"

#include "pairing_responder.h"
#include "proto_commands.h"
#include "proto_crypto.h"

#include <esp_random.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>

/// @file hub_key_extraction.cpp
/// @brief "Recover System Key" (key extraction) — device-role responder hub wiring.
/// @ingroup hioc_hub
///
/// Owns the impure side of the key-extraction feature: arming/disarming, throwaway node-ID
/// generation, the 10-minute auto-off timer, the post-extraction grace window, transmitting
/// device-role replies, and the security-sensitive result log block. The pure state-transition
/// decisions live in pairing_responder.h/.cpp; the six RX branches
/// (0x28/0x2C/0x31/0x32/0x36/0x3C) dispatch through try_handle_key_extraction_frame_(), called
/// from process_received_packet_() (hub_status.cpp).
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
/// (Somfy TaHoma/Smoove, Velux KLF200, etc.). That self-test necessarily agrees with this
/// codebase's own conventions — it is the same encoder on both ends — so it is blind to two
/// things: a device-role frame that is self-consistent but wrong on air, and a protocol step a
/// real hub requires that this project's own controller role never sends. Both are real failure
/// modes against real hubs; see tests/corpus/captures/issues/issue_45_*_key_extraction_stall.yaml.
/// The device-role builders (create_discover_resp(), create_challenge_req_device_role(),
/// create_key_confirm(), create_discover_confirm_ack()) are each pinned against a real device's
/// captured framing by tests/corpus_device_role_builder_test.cpp — except
/// create_discover_resp()'s flags/timestamp bytes, which remain placeholders.
/// recover_system_key_from_transfer()'s IV-derivation formula is independently pinned against two
/// externally-captured known-answer key transfers (ProtoCrypto.CryptKeyMatchesDocumented*Capture
/// in proto_crypto_test.cpp), so that formula does not rest on this codebase's own conventions —
/// though both captures are short requests and don't exercise construct_iv()'s 8-byte truncation
/// window, so a real hub sending a longer request is an open question. Treat a recovered key as
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

constexpr uint32_t KEY_EXTRACTION_POST_EXTRACT_GRACE_MS = 60000;  ///< One minute.
// TODO(hardware-verify): no timing data exists for the 0x33->0x36 gap on real hardware (the only
// capture of that gap is an untimed SPI trace). One minute is deliberately generous rather than
// tight: the window is inert by construction (every pre-EXTRACTED guard in pairing_responder.cpp
// rejects EXTRACTED/SENT_ADDRESS_RESP, so nothing new can start inside it), it is hard-capped by
// the 10-minute auto-off timer above, and the only cost of overshooting is that the HA switch
// reports "still listening" for longer. Undershooting, by contrast, silently drops a slow hub's
// address-verification round — the exact failure this feature exists to fix. Not measured.
constexpr const char *KEY_EXTRACTION_GRACE_TIMER_NAME = "key_extraction_post_extract_grace";

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
           "mode now.",
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
  if (frame.cmd == CMD_DISCOVER_CONFIRM) {
    this->handle_key_extraction_discover_confirm_(frame);
    return true;
  }
  if (frame.cmd == CMD_KEY_INIT) {
    this->handle_key_extraction_key_init_(frame);
    return true;
  }
  if (frame.cmd == CMD_KEY_TRANSFER) {
    this->handle_key_extraction_key_transfer_(frame);
    return true;
  }
  if (frame.cmd == CMD_ADDRESS_REQ) {
    this->handle_key_extraction_address_req_(frame);
    return true;
  }
  if (frame.cmd == CMD_CHALLENGE_REQ) {
    this->handle_key_extraction_address_challenge_(frame);
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

void IOHomeControlComponent::handle_key_extraction_discover_confirm_(const IoFrame &frame) {
  if (!pairing_responder::on_discover_confirm(this->key_extraction_ctx_))
    return;

  IoFrame resp;
  if (!create_discover_confirm_ack(resp, this->key_extraction_ctx_.throwaway_id, frame.src)) {
    ESP_LOGW(detail::TAG, "Key extraction: failed to build discovery-confirm ack");
    return;
  }
  this->broadcast_key_extraction_reply_(resp);
  ESP_LOGI(detail::TAG, "Key extraction: acknowledged discovery confirm from hub %s",
           node_id_to_string(frame.src).c_str());
}

void IOHomeControlComponent::handle_key_extraction_key_init_(const IoFrame &frame) {
  uint8_t candidate_challenge[HMAC_SIZE];
  crypto::generate_challenge(candidate_challenge);
  if (!pairing_responder::on_key_init(this->key_extraction_ctx_, candidate_challenge, frame.src))
    return;

  IoFrame resp;
  if (!create_challenge_req_device_role(resp, frame.src, this->key_extraction_ctx_.throwaway_id,
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

  // Log before the grace window can disarm us: disarm resets key_extraction_ctx_, which is where
  // the recovered key and the hub's real node ID live.
  this->log_key_extraction_result_();
  ESP_LOGI(detail::TAG,
           "Key extraction: still listening for up to %" PRIu32
           " more seconds in case the hub verifies this device's address (CMD_ADDRESS_REQ/0x36) — leave the "
           "switch on until it turns off on its own.",
           KEY_EXTRACTION_POST_EXTRACT_GRACE_MS / 1000);
  // Don't disarm immediately: some hubs (Velux KLR200) follow the key exchange with an address
  // request (0x36) and a challenge (0x3C) verifying it, and disarming here would make the
  // responder deaf to that round before it can happen. A second hub attempting to pair mid-window
  // still cannot succeed and produce a second, confusing log block — on_discover_request() and
  // every other pure guard in pairing_responder.cpp reject EXTRACTED/SENT_ADDRESS_RESP, so the
  // state machine refuses to go backwards; the grace timer below disarms once no further progress
  // is seen, instead of doing it at once.
  this->arm_post_extraction_grace_();
}

void IOHomeControlComponent::handle_key_extraction_address_req_(const IoFrame &frame) {
  // Our throwaway ID is not a secret -- it went out in clear in our own 0x29/0x37 -- so the dst
  // check in try_handle_key_extraction_frame_() alone doesn't establish this frame actually came
  // from the hub we exchanged keys with. hub_node_id was captured from the 0x31 that started this
  // attempt (pairing_responder::on_key_init()); anything else claiming our throwaway ID as dst is
  // not that hub and gets no reply, closing an otherwise-unbounded loop an onlooker could drive to
  // keep re-arming the grace window for as long as the arm cycle lasts.
  if (memcmp(frame.src, this->key_extraction_ctx_.hub_node_id, NODE_ID_SIZE) != 0)
    return;
  if (!pairing_responder::on_address_req(this->key_extraction_ctx_))
    return;
  IoFrame resp;
  if (!create_address_resp_device_role(resp, this->key_extraction_ctx_.throwaway_id, frame.src)) {
    ESP_LOGW(detail::TAG, "Key extraction: failed to build address response");
    return;
  }
  this->broadcast_key_extraction_reply_(resp);
  this->arm_post_extraction_grace_();  // Hub is still progressing — push the disarm back out.
  ESP_LOGI(detail::TAG, "Key extraction: answered address request from hub %s", node_id_to_string(frame.src).c_str());
}

void IOHomeControlComponent::handle_key_extraction_address_challenge_(const IoFrame &frame) {
  // Hub-identity guard, mirroring handle_key_extraction_address_req_()'s: only the hub we actually
  // exchanged keys with may drive this round.
  if (memcmp(frame.src, this->key_extraction_ctx_.hub_node_id, NODE_ID_SIZE) != 0)
    return;
  // Length guard, mirroring handle_key_extraction_key_transfer_()'s: frame.data is passed straight
  // into a `const uint8_t challenge[HMAC_SIZE]` parameter, so a short 0x3C would silently
  // authenticate over stale bytes left in IoFrame::data from a previous parse.
  if (frame.data_len < HMAC_SIZE) {
    ESP_LOGW(detail::TAG, "Key extraction: address challenge payload too short (%u bytes)", frame.data_len);
    return;
  }
  if (!pairing_responder::on_address_challenge(this->key_extraction_ctx_))
    return;
  // Rebuild the 0x37 we last sent — deterministic from ctx, nothing stored across the two calls.
  // Only origin.cmd/origin.data/origin.data_len feed the transcript, so the dst passed here is
  // irrelevant to the HMAC; the real builder is used anyway so the transcript cannot drift if the
  // 0x37 payload ever changes.
  IoFrame our_address_resp;
  if (!create_address_resp_device_role(our_address_resp, /*own=*/this->key_extraction_ctx_.throwaway_id,
                                       /*dst=*/frame.src))
    return;
  IoFrame resp;
  if (!create_challenge_resp_device_role(resp, /*dst=*/frame.src, /*src=*/this->key_extraction_ctx_.throwaway_id,
                                         frame.data, our_address_resp, this->key_extraction_ctx_.recovered_key)) {
    ESP_LOGW(detail::TAG, "Key extraction: failed to build address challenge response");
    return;
  }
  this->broadcast_key_extraction_reply_(resp);
  ESP_LOGI(detail::TAG, "Key extraction: answered address challenge from hub %s", node_id_to_string(frame.src).c_str());
  // Do NOT disarm here — re-arm the grace window instead and stay in SENT_ADDRESS_RESP so a
  // retried 0x3C is answered (on_address_challenge() deliberately never advances state).
  this->arm_post_extraction_grace_();
}

void IOHomeControlComponent::arm_post_extraction_grace_() {
  // Same replace-on-reschedule idiom as KEY_EXTRACTION_TIMEOUT_NAME's 10-minute timer above —
  // every new sign of hub progress (0x36 received, 0x3D sent, and this same call at extraction
  // time) pushes the disarm back out, so a slow multi-retry hub isn't cut off mid-round. That
  // 10-minute timer is deliberately neither cancelled nor extended here, so it still bounds the
  // whole arm cycle: no amount of grace-window re-arming can keep the responder listening past the
  // 10 minutes the switch entity documents.
  this->set_timeout(KEY_EXTRACTION_GRACE_TIMER_NAME, KEY_EXTRACTION_POST_EXTRACT_GRACE_MS, [this]() {
    // Only a still-running post-extraction cycle may be disarmed from here. Checking DISARMED
    // alone is NOT enough: a user who toggles the switch off and back on inside the grace window
    // leaves this callback pending against a brand-new, unrelated arm cycle (set_timeout() only
    // replaces a *pending* timer of the same name, and re-arming schedules the 10-minute auto-off
    // timer, not this one) — and that new cycle is ARMED_IDLE, not DISARMED, so a DISARMED-only
    // guard would let a stale callback kill it.
    // The converse worry — that the new cycle reaches EXTRACTED (a state this guard accepts)
    // before the stale callback fires — cannot happen: reaching EXTRACTED calls this function,
    // whose set_timeout() replaces the pending callback under the same name. The stale timer is
    // destroyed exactly when the state becomes acceptable to it, so the only window it can fire in
    // is one this guard rejects.
    const auto state = this->key_extraction_ctx_.state;
    if (state != pairing_responder::ResponderState::EXTRACTED &&
        state != pairing_responder::ResponderState::SENT_ADDRESS_RESP)
      return;
    ESP_LOGI(detail::TAG, "Key extraction: post-extraction grace window elapsed (stage=%s). Disarming.",
             pairing_responder::responder_stage_name(state));
    this->set_key_extraction_armed(false);
  });
}

// TODO(hardware-verify): an authenticated read-back to the foreign hub using the recovered key,
// to confirm it before trusting it. recover_system_key_from_transfer()'s IV-derivation formula is
// independently pinned against externally-captured known-answer key transfers (see the file-level
// @warning above), but nothing here confirms this specific extraction talks to a real third-party
// hub correctly — the single highest-risk unverified piece of this feature. That read-back subflow
// is deliberately not implemented: doubling the protocol-speculation surface for a feature that
// already ships marked experimental is not worth it for a second unverified vendor-hub
// interaction. The key is still always printed (gating it on an equally-unverified secondary check
// risks hiding a correct key), but the log below says so.
void IOHomeControlComponent::log_key_extraction_result_() {
  // Deliberate, explicit exception to redaction.h's masking — see that file and README.md's
  // "Reporting Unsupported Devices" section, which already warns about pairing logs and the
  // shared TRANSFER_KEY in almost identical terms. Do NOT route this through the generic
  // frame-log helpers (log_frame()/log_component_capture()); those must keep masking 0x32.
  //
  // Logged line-by-line via log_multiline_result(), not as one ESP_LOGW("%s", ...) call: a single
  // call silently truncates at ESPHome's 512-byte log buffer -- see that function's doxygen
  // (hub_internal.h) for the root cause, and build_oneway_adoption_report()'s caller for the
  // identical reasoning on the 1W path.
  ESP_LOGW(detail::TAG, "========================================");
  detail::log_multiline_result(detail::TAG, /*is_warning=*/true, /*prefix=*/"",
                               detail::build_key_extraction_report(this->key_extraction_ctx_.hub_node_id,
                                                                   this->key_extraction_ctx_.recovered_key));
  ESP_LOGW(detail::TAG, "========================================");
}

}  // namespace home_io_control
}  // namespace esphome
