/// @file hub_pairing.cpp
/// @brief Thin hub wrapper delegating all pairing logic to PairingEngine.
/// @ingroup hioc_hub
///
/// All pairing implementation lives in pairing_engine.cpp. This file provides
/// only the IOHomeControlComponent::discover_and_pair(), pair_declared_device() and pull_key_from()
/// implementations, which manage the hub's busy_ flag and delegate to pairing_engine_.

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
  // Only once an attempt actually ran (telemetry was populated) — not on the early-return guard
  // above, which would otherwise republish stale telemetry from a previous attempt.
  this->note_pairing_attempt_ended_();
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
  this->note_pairing_attempt_ended_();
  return result;
}

/// Holds busy_ and stamps the attempt around PairingEngine::pull_key_from(), as pair_declared_device() does:
/// a refused request stamps and publishes nothing.
PullKeyResult IOHomeControlComponent::pull_key_from(const uint8_t controller[NODE_ID_SIZE]) {
  if (!this->initialized_)
    return PullKeyResult::refused("hub is not initialized");
  this->busy_ = true;
  PullKeyResult result = this->pairing_engine_.pull_key_from(controller);
  this->busy_ = false;
  if (!result.attempted())
    return result;
  this->note_pairing_attempt_ended_();
  return result;
}

void IOHomeControlComponent::note_pairing_attempt_ended_() {
  // Never 0, so "no attempt yet" stays distinguishable (see queue_discover_and_pair()).
  this->last_pairing_end_ms_ = std::max<uint32_t>(millis(), 1);
  if (this->pairing_result_callback_)
    this->pairing_result_callback_();
}

}  // namespace home_io_control
}  // namespace esphome
