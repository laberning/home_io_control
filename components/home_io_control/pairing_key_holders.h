#pragma once

/// @file pairing_key_holders.h
/// @brief Key-holder search: find the devices that hold a key and register the ones that can be controlled.
/// @ingroup hioc_hub
///
/// After a key pull the attempt holds a key and does not yet know which devices use it. A device answers a
/// roll-call, or an authenticated read, only under the key it holds, so KeyHolderSearch asks under that key
/// (the heater probe first, then one roll-call sweep), logs one report and registers what answered until
/// reboot. How the attempt ends (paired, key received, failed) is PairingEngine's.
///
/// KeyHolderSearch reaches the radio only through ExchangeEngine and records into the PairingTelemetry it is
/// given; it neither knows the hub nor the pairing engine.

#include "device_registry.h"
#include "exchange_engine.h"
#include "hub_pairing.h"
#include "pairing_telemetry.h"
#include "radio_interface.h"
#include "roll_call.h"
#include "tuning_config.h"

#include <cstdint>

namespace esphome {
namespace home_io_control {

/// Finds the devices that hold a key and registers the ones a platform can drive.
/// @ingroup hioc_hub
class KeyHolderSearch {
 public:
  /// @brief What search_and_register() found under a key.
  struct Result {
    uint8_t answered{0};    ///< Devices that answered under the key (registered or not).
    uint8_t registered{0};  ///< Of those, the ones registered until reboot.
  };

  /// @param radio_ptr  Double pointer into the hub's `radio_` member — survives driver replacement in tests.
  /// @param node_id    Controller 3-byte node ID buffer, owned by the hub.
  /// @param system_key 16-byte AES system key buffer, owned by the hub.
  /// @param tuning     Tuning configuration, owned by the hub.
  /// @param engine     Shared exchange engine for transmit/receive operations.
  /// @param registry   Device registry the found devices are registered in.
  /// @param telemetry  Per-attempt telemetry recorder, owned by the hub.
  KeyHolderSearch(RadioDriver **radio_ptr, const uint8_t *node_id, const uint8_t *system_key,
                  const TuningConfig *tuning, ExchangeEngine &engine, DeviceRegistry &registry,
                  PairingTelemetry &telemetry);

  /// Non-copyable — stores pointers and references into hub member addresses.
  KeyHolderSearch(const KeyHolderSearch &) = delete;
  KeyHolderSearch &operator=(const KeyHolderSearch &) = delete;

  /// @brief Find the devices that hold `key`, log one report, register the ones that can be controlled.
  ///
  /// Asks the heater at the next address first (probe_heater_at_next_address_()); otherwise runs one
  /// roll-call sweep (run_roll_call_sweep()) with `key`. Registers every device found that has a platform
  /// and no YAML entry, until reboot (IoDevice::registered_by_pairing); a record an earlier pairing in this
  /// boot added is replaced. With a device key the report is build_pulled_key_report() (device log
  /// only, one banner, the responders carry the key as their own `system_key`); with the hub key it is
  /// build_hub_key_pull_report() (no key, no banner, the responders use the hub key).
  ///
  /// With `key_proven` false (a pulled key its device did not prove), the devices that answer are the
  /// proof: a device answers a roll-call, or the heater probe's authenticated read, only under the key it
  /// holds. If none answers, nothing is reported or registered and the key stays unprinted.
  /// @param context Pairing context of the attempt.
  /// @param key AES_KEY_SIZE bytes the roll-call is built with.
  /// @param device_key true when `key` is a key the device handed over, false for the hub's own.
  /// @param key_proven false when only the answers can prove `key` (a device key the device did not prove).
  /// @return How many devices answered, and how many of them were registered.
  Result search_and_register(pairing::PairingContext &context, const uint8_t *key, bool device_key, bool key_proven);

 private:
  /// @brief Ask the heater at the address after an Atlantic pairing node for its maximum setpoint.
  ///
  /// Runs only for a node decisions::heater_behind_pairing_node() names, and only when the
  /// YAML declares no device at that address. One authenticated register read under `key` (the full
  /// retry budget); an answer means the electrical heater with an adjustable setpoint is there, and it
  /// becomes the only entry of `state`, as if a roll-call had found it. The read goes out with `key` given to
  /// ExchangeEngine::send_and_receive(), so the registry is neither read nor written for it.
  /// @param context Pairing context of the attempt (the discovered node and its manufacturer).
  /// @param key AES_KEY_SIZE bytes the read is signed with.
  /// @param device_key true when `key` is a key the device handed over (the entry uses key slot 1).
  /// @param[out] state Filled with the heater when it answered; untouched otherwise.
  /// @return true when the heater answered.
  bool probe_heater_at_next_address_(const pairing::PairingContext &context, const uint8_t *key, bool device_key,
                                     ScanState &state);

  /// Convenience accessor returning the current radio driver (dereferences double pointer).
  [[nodiscard]] RadioDriver *radio_() const { return *radio_ptr_; }

  RadioDriver **radio_ptr_;
  const uint8_t *node_id_;
  const uint8_t *system_key_;
  const TuningConfig *tuning_;
  ExchangeEngine &engine_;
  DeviceRegistry &registry_;
  PairingTelemetry &telemetry_;
};

}  // namespace home_io_control
}  // namespace esphome
