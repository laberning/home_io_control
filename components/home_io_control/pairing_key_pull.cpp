/// @file pairing_key_pull.cpp
/// @brief Key pull: ask a device for the key it holds and prove that it holds it.
/// @ingroup hioc_hub

#include "pairing_key_pull.h"

#include "decisions_attempts.h"
#include "decisions_pairing.h"
#include "log_frame.h"
#include "log_helpers.h"
#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_crypto.h"
#include "proto_device_types.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cstring>

namespace esphome {
namespace home_io_control {

namespace {
const char *const TAG = "home_io_control";
}  // namespace

KeyPull::KeyPull(RadioDriver **radio_ptr, const uint8_t *node_id, const uint8_t *system_key, const TuningConfig *tuning,
                 ExchangeEngine &engine, PairingTelemetry &telemetry)
    : radio_ptr_(radio_ptr),
      node_id_(node_id),
      system_key_(system_key),
      tuning_(tuning),
      engine_(engine),
      telemetry_(telemetry) {}

bool KeyPull::selected(const pairing::PairingContext &context) const {
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
  const bool pull = device_hands_out_key(context.device.type);
  ESP_LOGI(TAG, "Key transfer: auto -> %s (%s answered discovery as %s)", pull ? "pull" : "push",
           context.device_id.c_str(), format_device_kind(context.device.type, context.device.subtype).c_str());
  return pull;
}

bool KeyPull::wait_for_pulled_key_(pairing::PairingContext &context, uint32_t window_ms) {
  ListenSpec spec;
  spec.window_ms = window_ms;
  spec.policy = ListenPolicy::HOLD_REQUEST_CHANNEL;  // Unicast reply to a unicast request, as for 0x33.

  auto outcome =
      engine_.listen(spec, context.packet, context.rx, [&](const IoFrame *parsed, const RadioRxPacket & /*packet*/) {
        if (parsed == nullptr)
          return ReplyDisposition::IGNORE;
        const auto disposition = decisions::classify_key_pull_reply(*parsed, context.device.node_id, node_id_);
        if (disposition == decisions::PairingKeyPullReplyDisposition::IGNORE)
          return ReplyDisposition::IGNORE;
        const int16_t rssi = radio_()->get_last_capture().rssi_or_unknown();
        if (disposition == decisions::PairingKeyPullReplyDisposition::KEY) {
          this->telemetry_.record_rx(*parsed, rssi);
          return ReplyDisposition::ACCEPT;
        }
        this->telemetry_.record_rx_reject(*parsed, rssi);
        if (disposition == decisions::PairingKeyPullReplyDisposition::REFUSE) {
          note_key_refusal(this->telemetry_, context, *parsed, "Key pull");
          return ReplyDisposition::ABORT;
        }
        return ReplyDisposition::IGNORE;
      });
  return outcome == ListenOutcome::ACCEPTED;
}

KeyPull::Result KeyPull::pull(pairing::PairingContext &context) {
  ESP_LOGW(TAG, "Key pull: asking %s for its key", context.device_id.c_str());
  // One challenge for every try: the IV of the device's 0x32 is built from the request it answers, so
  // with the same request bytes a 0x32 that answers an earlier try late still decrypts to the key.
  uint8_t challenge[HMAC_SIZE];
  crypto::generate_challenge(challenge);
  // The request is the same on every try (one challenge), so it is built once and its transmit time
  // estimated once for the budget rule.
  if (!create_launch_key_transfer(context.req, node_id_, context.device.node_id, challenge)) {
    ESP_LOGW(TAG, "Key pull failed: could not build the key transfer request for %s", context.device_id.c_str());
    return Result::FAILED;
  }
  const uint16_t preamble = pairing_start_preamble(engine_, *tuning_, context.req);
  const decisions::AttemptPlan plan =
      engine_.plan_attempts(decisions::ReadKind::COMMAND, context.discovery_low_power, context.req, FREQ_CH2, preamble,
                            static_cast<uint16_t>(PAIRING_KEY_REPLY_TIMEOUT_MS));
  const ExchangeEngine::AttemptRun run{plan, engine_.tx_time_ms(context.req, FREQ_CH2, preamble)};
  const uint32_t begin_ms = millis();
  uint8_t tries = 0;
  const AttemptRunResult ran = engine_.run_attempts(run, [&](uint8_t try_number, uint16_t window_ms) {
    tries = try_number;
    enter_pairing_state(engine_, telemetry_, context, pairing::PairingState::TX_KEY_PULL, try_number, false);
    FrameLogHoldScope quiet_window;  // From before the 0x38 to the end of the wait for the 0x32 (ADR 0111).
    if (!engine_.transmit_frame(context.req, FREQ_CH2, preamble))
      return AttemptVerdict::AGAIN;
    context.state = pairing::PairingState::WAIT_KEY_PULL_REPLY;
    this->telemetry_.set_phase(context.state);
    const bool pulled = wait_for_pulled_key_(context, window_ms);
    quiet_window.release();
    if (pulled)
      return AttemptVerdict::DONE;
    if (context.key_refused)
      return AttemptVerdict::STOP;
    ESP_LOGI(TAG, "Key pull try %u: no key transfer (0x32) from %s within %u ms", try_number, context.device_id.c_str(),
             window_ms);
    return AttemptVerdict::AGAIN;
  });
  if (ran == AttemptRunResult::BUDGET_EXHAUSTED) {
    ESP_LOGI(TAG, "Key pull budget exhausted after %u tries (%" PRIu32 " of %u ms)", tries, millis() - begin_ms,
             tuning_->exchange_total_budget_ms);
  }
  const bool got_key = ran == AttemptRunResult::DONE;
  if (!got_key) {
    ESP_LOGW(TAG, "Key pull failed: %s sent no key", context.device_id.c_str());
    return Result::FAILED;
  }

  uint8_t key[AES_KEY_SIZE];
  if (!recover_key_from_pull(context.req, context.rx.data, key))
    return Result::FAILED;
  // The key check value names the key without revealing it, so two attempts or two devices can be
  // compared from the log alone.
  const std::string check_value = detail::format_key_check_value(key);
  // Our own key cannot come out of the decrypt by chance, so a match proves the device holds it.
  if (memcmp(key, system_key_, AES_KEY_SIZE) == 0) {
    ESP_LOGI(TAG, "Key pull check: %s -> key check value %s, the hub's own key", context.device_id.c_str(),
             check_value.c_str());
    return Result::OUR_KEY;
  }
  return prove_pulled_key_(context, key, check_value);
}

KeyPull::Result KeyPull::prove_pulled_key_(pairing::PairingContext &context, const uint8_t *key,
                                           const std::string &check_value) {
  context.state = pairing::PairingState::VERIFY_PULLED_KEY;
  this->telemetry_.set_phase(context.state);
  // The device's 0x3D over its own 0x32, under the key it sent, proves the decrypt and that it holds
  // the key (a chance match is 1 in 2^48). A lost answer is challenged again; a wrong one is final.
  // Every challenge repeats the same bytes, so a 0x3D that arrives in the next try's window still
  // verifies instead of reading as a wrong answer. The first challenge answers the 0x32 at once and
  // goes out with the response preamble; a repeat follows a pause, by which time a device that hops or
  // sleeps between frames may have left the channel, so it gets the 0x38's start preamble.
  uint8_t verify_challenge[HMAC_SIZE];
  crypto::generate_challenge(verify_challenge);
  const uint16_t repeat_preamble = pairing_start_preamble(engine_, *tuning_, context.req);
  ExchangeEngine::ChallengeResult proof = ExchangeEngine::ChallengeResult::NO_ANSWER;
  uint8_t challenges = 0;
  engine_.run_attempts(
      ExchangeEngine::fixed_attempts(EXCHANGE_RETRY_COUNT, EXCHANGE_RETRY_DELAY_MS),
      [&](uint8_t try_number, uint16_t /*window_ms*/) {
        challenges = try_number;
        const uint16_t challenge_preamble = try_number == 1 ? radio_()->response_preamble() : repeat_preamble;
        proof = engine_.challenge_sender(context.rx, FREQ_CH2, key, verify_challenge, challenge_preamble);
        return proof == ExchangeEngine::ChallengeResult::NO_ANSWER ? AttemptVerdict::AGAIN : AttemptVerdict::DONE;
      });
  if (proof == ExchangeEngine::ChallengeResult::VERIFIED) {
    ESP_LOGI(TAG, "Key pull check: %s -> key check value %s, proven by its 0x3D", context.device_id.c_str(),
             check_value.c_str());
    memcpy(context.pulled_key, key, AES_KEY_SIZE);
    context.has_pulled_key = true;
    return Result::OTHER_KEY;
  }
  this->telemetry_.set_key_pull_unproven();
  if (proof == ExchangeEngine::ChallengeResult::MISMATCH) {
    ESP_LOGW(TAG,
             "Key pull check: %s -> key check value %s, not proven: its 0x3D does not match. The key is not "
             "printed and must not be used",
             context.device_id.c_str(), check_value.c_str());
    return Result::FAILED;
  }
  // No answer at all is not a wrong answer: devices that answer a roll-call under the key can still
  // prove it (KeyHolderSearch).
  ESP_LOGW(TAG,
           "Key pull check: %s -> key check value %s, not proven: no 0x3D received after %u challenges. The key "
           "is not printed; a roll-call under it decides whether devices hold it",
           context.device_id.c_str(), check_value.c_str(), challenges);
  memcpy(context.pulled_key, key, AES_KEY_SIZE);
  context.has_pulled_key = true;
  return Result::UNPROVEN;
}

}  // namespace home_io_control
}  // namespace esphome
