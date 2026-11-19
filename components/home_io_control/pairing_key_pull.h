#pragma once

/// @file pairing_key_pull.h
/// @brief Key pull: ask a device for the key it holds and prove that it holds it.
/// @ingroup hioc_hub
///
/// A device that hands out its own key (a remote controller answering discovery, or any device when
/// `pairing_key_transfer: pull` forces it) is asked with a key transfer request (0x38); it answers with its
/// key in a 0x32, and the hub challenges it (0x3C) to prove it holds that key (0x3D). KeyPull runs that
/// exchange and reports what the key is; what the attempt does with it (find the devices that hold it,
/// register them, end the attempt) is PairingEngine's and KeyHolderSearch's.
///
/// KeyPull holds the radio only through ExchangeEngine and records into the PairingTelemetry it is given;
/// it neither knows the hub nor the device registry.

#include "exchange_engine.h"
#include "hub_pairing.h"
#include "pairing_steps.h"
#include "pairing_telemetry.h"
#include "proto_frame.h"
#include "radio_interface.h"
#include "tuning_config.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace home_io_control {

/// Pulls a device's key and proves the device holds it.
/// @ingroup hioc_hub
class KeyPull {
 public:
  /// @brief What pull() ended with.
  enum class Result : uint8_t {
    FAILED,     ///< No key received, refused, or a different key the device did not prove (not printed).
    OUR_KEY,    ///< The device sent the hub's own key: it is paired with this hub.
    OTHER_KEY,  ///< The device sent a different key and proved it holds it (`context.pulled_key`).
    UNPROVEN,   ///< The device sent a different key and no 0x3D to our challenges (`context.pulled_key`,
                ///< not printed): only devices that answer a roll-call under it can prove it.
  };

  /// @param radio_ptr  Double pointer into the hub's `radio_` member — survives driver replacement in tests.
  /// @param node_id    Controller 3-byte node ID buffer, owned by the hub.
  /// @param system_key 16-byte AES system key buffer, owned by the hub.
  /// @param tuning     Tuning configuration, owned by the hub.
  /// @param engine     Shared exchange engine for transmit/receive operations.
  /// @param telemetry  Per-attempt telemetry recorder, owned by the hub.
  KeyPull(RadioDriver **radio_ptr, const uint8_t *node_id, const uint8_t *system_key, const TuningConfig *tuning,
          ExchangeEngine &engine, PairingTelemetry &telemetry);

  /// Non-copyable — stores pointers and references into hub member addresses.
  KeyPull(const KeyPull &) = delete;
  KeyPull &operator=(const KeyPull &) = delete;

  /// @brief Which direction this attempt moves the key in.
  ///
  /// `pull` and `push` force it; `auto` pulls when the device answered discovery as a remote
  /// controller (node type 1023) and pushes otherwise. Logs the `auto` decision.
  /// @param context Pairing context populated by the discovery phase.
  /// @return true to pull the device's key, false to push the hub's.
  [[nodiscard]] bool selected(const pairing::PairingContext &context) const;

  /// @brief Key pull (`pairing_key_transfer: pull`, or `auto` for a remote controller): 0x38 ->
  /// device's 0x32 -> our 0x3C -> device's 0x3D.
  ///
  /// Sends a key transfer request with 6 random bytes, waits for the device's 0x32 and decrypts the
  /// key (recover_key_from_pull()). The log gets one "Key pull check" line with the key's check value,
  /// never the key, so two attempts or two devices can be compared from the log alone.
  ///
  /// Every 0x38 try of the attempt carries the same challenge, so a 0x32 that answers an earlier try late
  /// still decrypts to the key. The tries follow the command plan of the device's power class
  /// (ExchangeEngine::plan_attempts()): to a mains device as many without a gap as fit
  /// `exchange_total_budget_ms` (at most EXCHANGE_MAX_ATTEMPTS), the early ones listening
  /// `exchange_early_response_wait_ms` and the last PAIRING_KEY_REPLY_TIMEOUT_MS; to a low-power
  /// one EXCHANGE_RETRY_COUNT with the retry gap, all of them kept whatever they cost (ADR 0095).
  /// A try of any other plan that could not finish inside the budget is not started
  /// (decisions::attempt_fits()).
  ///
  /// The hub's own key is accepted as is: no decrypt produces it by chance. A different key must be
  /// proven by the device's 0x3D to our challenge (ExchangeEngine::challenge_sender() under that key, up
  /// to EXCHANGE_RETRY_COUNT challenges while no 0x3D comes back; a 0x3D that does not verify ends it).
  /// The challenges repeat the same bytes, so a late 0x3D still verifies; the first goes out with the
  /// response preamble, as an immediate answer to the 0x32, the repeats with the 0x38's start preamble,
  /// since the device may no longer be listening on the channel by then. A proven key becomes
  /// `context.pulled_key` (OTHER_KEY); one with no 0x3D at all is kept there unprinted (UNPROVEN) for a
  /// roll-call to prove. The hub's own key is never replaced. Hardware-validated on an Atlantic AGILIA IO
  /// radiator (issue #165).
  /// @param context Pairing context populated by the discovery phase.
  /// @return See Result.
  Result pull(pairing::PairingContext &context);

 private:
  /// Wait for the device's 0x32 answer to our 0x38, holding the request channel.
  /// @param context   Pairing context; `context.rx` receives the accepted 0x32.
  /// @param window_ms How long to listen.
  /// @return true if a well-formed 0x32 from the device arrived; false on timeout or refusal
  ///         (a refusal also sets `context.key_refused`).
  bool wait_for_pulled_key_(pairing::PairingContext &context, uint32_t window_ms);

  /// Challenge the device to prove it holds `key`, the key it sent: the hub's own challenge repeated up
  /// to EXCHANGE_RETRY_COUNT times while no 0x3D comes back.
  /// @param context Pairing context; `context.req` is the 0x38, `context.rx` the device's 0x32.
  /// @param key AES_KEY_SIZE bytes the device sent.
  /// @param check_value The key's check value, for the log.
  /// @return OTHER_KEY when proven, UNPROVEN when no 0x3D came back, FAILED when a 0x3D did not match.
  Result prove_pulled_key_(pairing::PairingContext &context, const uint8_t *key, const std::string &check_value);

  /// Convenience accessor returning the current radio driver (dereferences double pointer).
  [[nodiscard]] RadioDriver *radio_() const { return *radio_ptr_; }

  RadioDriver **radio_ptr_;
  const uint8_t *node_id_;
  const uint8_t *system_key_;
  const TuningConfig *tuning_;
  ExchangeEngine &engine_;
  PairingTelemetry &telemetry_;
};

}  // namespace home_io_control
}  // namespace esphome
