/// @file oneway_sequence_store.cpp
/// @brief Persistent rolling-sequence counters for one-way (1W) transmit.
/// @ingroup hioc_hub

#include "oneway_sequence_store.h"

#include "esphome/core/log.h"

#include <cstring>

namespace esphome {
namespace home_io_control {

namespace {

constexpr const char *const TAG = "home_io_control.oneway_seq";

/// FNV-1 parameters, matching the hash ESPHome uses for its own preference keys.
constexpr uint32_t FNV1_OFFSET_BASIS = 2166136261U;
constexpr uint32_t FNV1_PRIME = 16777619U;

/// Preference key for an identity's counter.
///
/// Derived from the node address alone, so it is stable across builds, across firmware versions
/// and across YAML edits that reorder identities — the persisted counter must follow the address
/// it belongs to, because that is what the receiving device tracks. An index- or name-derived key
/// would silently hand an identity somebody else's counter after a reorder or a rename.
uint32_t preference_key(const uint8_t node_id[NODE_ID_SIZE]) {
  // FNV-1 over the three address bytes, with a fixed prefix so this component's keys cannot
  // collide with an entity's.
  uint32_t hash = FNV1_OFFSET_BASIS;
  hash = (hash ^ static_cast<uint8_t>('1')) * FNV1_PRIME;
  hash = (hash ^ static_cast<uint8_t>('W')) * FNV1_PRIME;
  for (uint8_t i = 0; i < NODE_ID_SIZE; i++)
    hash = (hash ^ node_id[i]) * FNV1_PRIME;
  return hash;
}

/// Persist a reservation and commit it to flash before returning.
///
/// save() only stages the write — on ESP32 nothing reaches NVS until sync() commits it. Without
/// the sync the reservation would be lost by exactly the reboot it exists to survive.
bool persist(ESPPreferenceObject &pref, uint16_t value) {
  if (!pref.save(&value))
    return false;
  return global_preferences->sync();
}

}  // namespace

void OneWaySequenceStore::add_identity(const uint8_t node_id[NODE_ID_SIZE], uint16_t initial_sequence) {
  if (this->find_(node_id) != nullptr)
    return;  // Registering an address twice would give one transmitter two counters.

  Counter counter{};
  memcpy(counter.node_id, node_id, NODE_ID_SIZE);
  counter.pref = global_preferences->make_preference<uint16_t>(preference_key(node_id));

  uint16_t persisted = 0;
  if (counter.pref.load(&persisted)) {
    // Resume from whichever is *higher*. Forward is always safe, backward never is, so a YAML
    // edit can push a desynced counter ahead — which is what `initial_sequence` is for — but can
    // never drag a live one back into replay territory. seed() is the deliberate way to go back.
    // Plain numeric comparison, not ring arithmetic: as a config escape hatch, predictable beats
    // clever, and a user who needs to cross the wrap point should use seed().
    counter.next = persisted > initial_sequence ? persisted : initial_sequence;
  } else {
    counter.next = initial_sequence;
  }
  // Nothing is reserved yet: the first next() call writes before it hands anything out.
  counter.reserved = counter.next;

  this->counters_.push_back(counter);
}

bool OneWaySequenceStore::next(const uint8_t node_id[NODE_ID_SIZE], uint16_t &out) {
  Counter *counter = this->find_(node_id);
  if (counter == nullptr)
    return false;

  if (counter->next == counter->reserved) {
    // Block exhausted (or never reserved). Persist the end of the next block before handing out
    // anything inside it. Unsigned wraparound is intended: the counter is a 16-bit ring.
    const auto new_reservation = static_cast<uint16_t>(counter->next + ONEWAY_SEQUENCE_STRIDE);
    if (!persist(counter->pref, new_reservation)) {
      // Refusing here is the point: transmitting an unreserved sequence risks reusing it after a
      // reboot, and a device that sees a replay ignores every later command too.
      ESP_LOGE(TAG, "Could not persist a 1W sequence reservation; refusing to transmit");
      return false;
    }
    counter->reserved = new_reservation;
  }

  out = counter->next;
  counter->next = static_cast<uint16_t>(counter->next + 1);
  return true;
}

bool OneWaySequenceStore::seed(const uint8_t node_id[NODE_ID_SIZE], uint16_t value) {
  Counter *counter = this->find_(node_id);
  if (counter == nullptr)
    return false;

  if (!persist(counter->pref, value))
    return false;
  counter->next = value;
  // Nothing beyond `value` is reserved, so the next call re-reserves from here.
  counter->reserved = value;
  return true;
}

bool OneWaySequenceStore::peek(const uint8_t node_id[NODE_ID_SIZE], uint16_t &out) const {
  const Counter *counter = this->find_(node_id);
  if (counter == nullptr)
    return false;
  out = counter->next;
  return true;
}

OneWaySequenceStore::Counter *OneWaySequenceStore::find_(const uint8_t node_id[NODE_ID_SIZE]) {
  for (auto &counter : this->counters_) {
    if (memcmp(counter.node_id, node_id, NODE_ID_SIZE) == 0)
      return &counter;
  }
  return nullptr;
}

const OneWaySequenceStore::Counter *OneWaySequenceStore::find_(const uint8_t node_id[NODE_ID_SIZE]) const {
  for (const auto &counter : this->counters_) {
    if (memcmp(counter.node_id, node_id, NODE_ID_SIZE) == 0)
      return &counter;
  }
  return nullptr;
}

}  // namespace home_io_control
}  // namespace esphome
