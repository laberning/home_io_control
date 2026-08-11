/// @file oneway_transmitter.cpp
/// @brief One-way (1W) transmit collaborator.
/// @ingroup hioc_hub

#include "oneway_transmitter.h"

#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_timing.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

namespace {

constexpr const char *const TAG = "home_io_control.oneway_tx";

}  // namespace

void OneWayTransmitter::setup() {
  for (const auto &identity : this->identities_.all())
    this->sequences_.add_identity(identity.node_id, identity.initial_sequence);
}

bool OneWayTransmitter::send_(const std::string &controller_id,
                              const std::function<bool(IoFrame &, const OneWayControllerIdentity &, uint16_t)> &build) {
  const OneWayControllerIdentity *identity = this->identities_.get(controller_id);
  if (identity == nullptr) {
    ESP_LOGW(TAG, "1W tx: no controller identity '%s'", controller_id.c_str());
    this->report_failure_(controller_id, 0);
    return false;
  }

  // One sequence per logical command, reserved before anything is built or sent.
  uint16_t sequence = 0;
  if (!this->sequences_.next(identity->node_id, sequence)) {
    // The store logs why; it refuses rather than risk reusing a sequence.
    this->report_failure_(controller_id, 0);
    return false;
  }

  IoFrame frame{};
  if (!build(frame, *identity, sequence)) {
    // The sequence is spent either way. Skipping one is free; reusing it is not, so it is not
    // returned to the store.
    ESP_LOGW(TAG, "1W tx: could not build a frame for '%s'", controller_id.c_str());
    this->report_failure_(controller_id, sequence);
    return false;
  }

  const bool transmitted = this->send_burst(frame);
  if (this->report_) {
    const OneWayFrameInfo info = decode_1w_frame(frame);
    OneWayCommandReport report{};
    report.controller_id = controller_id;
    report.intent = info.has_intent ? info.intent : "";
    report.target_type = info.target_type;
    report.sequence = sequence;
    report.transmitted = transmitted;
    this->report_(report);
  }
  return transmitted;
}

void OneWayTransmitter::report_failure_(const std::string &controller_id, uint16_t sequence) {
  if (!this->report_)
    return;
  OneWayCommandReport report{};
  report.controller_id = controller_id;
  report.sequence = sequence;
  this->report_(report);
}

bool OneWayTransmitter::send_command(const std::string &controller_id, CoverCommand cmd) {
  return this->send_(controller_id, [cmd](IoFrame &frame, const OneWayControllerIdentity &identity, uint16_t sequence) {
    return create_1w_execute_command(frame, identity.node_id, identity.io_device_type, cmd, sequence,
                                     identity.system_key);
  });
}

bool OneWayTransmitter::send_position(const std::string &controller_id, uint8_t position) {
  return this->send_(controller_id,
                     [position](IoFrame &frame, const OneWayControllerIdentity &identity, uint16_t sequence) {
                       return create_1w_execute_position(frame, identity.node_id, identity.io_device_type, position,
                                                         sequence, identity.system_key);
                     });
}

bool OneWayTransmitter::send_burst(const IoFrame &frame) {
  // Logged once for the whole burst rather than once per copy: four log lines per button press
  // would suggest four commands, which is exactly the misreading the shared sequence exists to
  // prevent. Only frame-header facts are logged — never payload bytes — so this stays safe even
  // for the commands redaction.h flags as carrying key material.
  const OneWayFrameInfo info = decode_1w_frame(frame);
  if (info.has_intent) {
    ESP_LOGI(TAG, "1W tx: from %s to class %s intent %s (%ux)", node_id_to_string(frame.src).c_str(),
             device_type_name(info.target_type), info.intent, ONEWAY_BURST_REPEATS);
  } else {
    ESP_LOGI(TAG, "1W tx: from %s to class %s cmd 0x%02X (%ux)", node_id_to_string(frame.src).c_str(),
             device_type_name(info.target_type), frame.cmd, ONEWAY_BURST_REPEATS);
  }

  uint8_t sent = 0;
  for (uint8_t repeat = 0; repeat < ONEWAY_BURST_REPEATS; repeat++) {
    if (repeat > 0) {
      // The gap is part of the protocol, so it is taken before the copy rather than after the
      // last one — a trailing delay would hold the loop for nothing.
      App.feed_wdt();
      delay(ONEWAY_BURST_INTERVAL_MS);
    }
    if (this->transmit_(frame, FREQ_CH2, LONG_PREAMBLE)) {
      sent++;
    }
  }

  if (sent != ONEWAY_BURST_REPEATS) {
    // Worth a warning even when some copies made it: the burst is the reliability mechanism, so a
    // partial one is a degraded command, and nothing downstream will ever notice on its own.
    ESP_LOGW(TAG, "1W tx: only %u of %u copies transmitted", sent, ONEWAY_BURST_REPEATS);
  }
  return sent > 0;
}

}  // namespace home_io_control
}  // namespace esphome
