/// @file hub_pairing.cpp
/// @brief Thin hub wrapper delegating all pairing logic to PairingEngine.
/// @ingroup hioc_hub
///
/// All pairing implementation lives in pairing_engine.cpp. This file provides
/// only the IOHomeControlComponent::discover_and_pair() and
/// IOHomeControlComponent::pair_declared_device() implementations, which manage
/// the hub's busy_ flag and delegate to pairing_engine_.

#include "hub_pairing.h"
#include "hub_internal.h"

#include <algorithm>

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
  // Never 0, so "no attempt yet" stays distinguishable (see queue_discover_and_pair()).
  this->last_pairing_end_ms_ = std::max<uint32_t>(millis(), 1);
  // Only fire once an attempt actually ran (telemetry was populated) — not on the early-return
  // guard above, which would otherwise republish stale telemetry from a previous attempt.
  if (this->pairing_result_callback_)
    this->pairing_result_callback_();
  return ok;
}

/// Holds busy_ and stamps the attempt around PairingEngine::pair_declared_device(),
/// as discover_and_pair() does. A request refused before anything was sent stamps and publishes
/// nothing, so "Last Pairing Result" keeps the previous attempt's telemetry.
DeclaredPairingResult IOHomeControlComponent::pair_declared_device(const std::string &device_id) {
  if (!this->initialized_)
    return {.refusal = "hub is not initialized"};
  this->busy_ = true;
  const DeclaredPairingResult result = this->pairing_engine_.pair_declared_device(device_id);
  this->busy_ = false;
  if (!result.attempted())
    return result;
  this->last_pairing_end_ms_ = std::max<uint32_t>(millis(), 1);
  if (this->pairing_result_callback_)
    this->pairing_result_callback_();
  return result;
}

}  // namespace home_io_control
}  // namespace esphome
