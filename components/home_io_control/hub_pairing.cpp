/// @file hub_pairing.cpp
/// @brief Thin hub wrapper delegating all pairing logic to PairingEngine.
/// @ingroup hioc_hub
///
/// All pairing implementation lives in pairing_engine.cpp. This file provides
/// only the IOHomeControlComponent::discover_and_pair() implementation which
/// manages the hub's busy_ flag and delegates to pairing_engine_.

#include "hub_pairing.h"
#include "hub_internal.h"

namespace esphome {
namespace home_io_control {

/// Discover and pair a device in pairing mode.
///
/// The hub wrapper manages the busy_ flag so loop() stops hopping and polling
/// while the blocking pairing exchange is in progress. All protocol logic is
/// in PairingEngine::discover_and_pair().
bool IOHomeControlComponent::discover_and_pair() {
  if (!this->initialized_)
    return false;
  this->busy_ = true;
  bool const ok = this->pairing_engine_.discover_and_pair();
  this->busy_ = false;
  // Only fire once an attempt actually ran (telemetry was populated) — not on the early-return
  // guard above, which would otherwise republish stale telemetry from a previous attempt.
  if (this->pairing_result_callback_)
    this->pairing_result_callback_();
  return ok;
}

}  // namespace home_io_control
}  // namespace esphome
