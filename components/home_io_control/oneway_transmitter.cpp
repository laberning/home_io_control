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
                              const std::function<bool(IoFrame &, const OneWayControllerIdentity &, uint16_t)> &build,
                              const char *explicit_intent) {
  const OneWayControllerIdentity *identity = this->identities_.get(controller_id);
  if (identity == nullptr) {
    ESP_LOGW(TAG, "1W tx: no controller identity '%s'", controller_id.c_str());
    this->report_failure_(controller_id, 0, /*sequence_reserved=*/false);
    return false;
  }

  // One sequence per logical command, reserved before anything is built or sent.
  uint16_t sequence = 0;
  if (!this->sequences_.next(identity->node_id, sequence)) {
    // The store logs why; it refuses rather than risk reusing a sequence.
    this->report_failure_(controller_id, 0, /*sequence_reserved=*/false);
    return false;
  }

  IoFrame frame{};
  if (!build(frame, *identity, sequence)) {
    // The sequence is spent either way. Skipping one is free; reusing it is not, so it is not
    // returned to the store.
    ESP_LOGW(TAG, "1W tx: could not build a frame for '%s'", controller_id.c_str());
    this->report_failure_(controller_id, sequence, /*sequence_reserved=*/true);
    return false;
  }

  const bool transmitted = this->send_burst(frame);
  if (this->report_) {
    OneWayCommandReport report{};
    report.controller_id = controller_id;
    if (explicit_intent[0] != '\0') {
      // 0x30/0x39 carry no intent decode_1w_frame() can read (it only understands
      // CMD_EXECUTE/CMD_ACTIVATE_MODE payloads) -- send_enrollment()/send_unenrollment() supply
      // the label directly rather than leave the "Last 1W Command" sensor blank on the one
      // feature whose entire diagnostic story is that sensor.
      report.intent = explicit_intent;
      report.target_type = identity->io_device_type;
    } else {
      const OneWayFrameInfo info = decode_1w_frame(frame);
      report.intent = info.has_intent ? info.intent : "";
      report.target_type = info.target_type;
    }
    report.sequence = sequence;
    report.sequence_reserved = true;
    report.transmitted = transmitted;
    this->report_(report);
  }
  return transmitted;
}

void OneWayTransmitter::report_failure_(const std::string &controller_id, uint16_t sequence, bool sequence_reserved) {
  if (!this->report_)
    return;
  OneWayCommandReport report{};
  report.controller_id = controller_id;
  report.sequence = sequence;
  report.sequence_reserved = sequence_reserved;
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

bool OneWayTransmitter::send_enrollment(const std::string &controller_id) {
  // The documented 1W pairing handshake (reference/iown-homecontrol/docs/linklayer.md:396, "1W
  // Discovery") is `0x39` immediately followed by `0x30`, both from the same controller, back to
  // back within one gesture -- a real Smoove capture landed them 128 ms apart, same burst (see
  // tests/corpus/captures/somfy_awning/oneway_add_and_remove_controller_sx1276.yaml and
  // analysis/completed/oneway_1w_support_plan.md Step 13). `0x39` here carries only this
  // identity's own `src` address, so on the wire it can only mean "clear my own prior entry
  // before I re-register" -- it cannot name or displace a different controller. Sending it right
  // before `0x30` clears a stale slot from an earlier enrollment attempt under this identity,
  // which a bare `0x30` re-add is not guaranteed to overwrite.
  const bool removed = this->send_(
      controller_id,
      [](IoFrame &frame, const OneWayControllerIdentity &identity, uint16_t sequence) {
        return create_1w_remove_controller(frame, identity.node_id, identity.io_device_type, sequence,
                                           identity.system_key);
      },
      "UNENROLL");
  if (!removed) {
    ESP_LOGW(TAG, "1W tx: enrollment's 0x39 prelude did not reach the radio for '%s' -- trying 0x30 anyway",
             controller_id.c_str());
  }

  return this->send_(
      controller_id,
      [](IoFrame &frame, const OneWayControllerIdentity &identity, uint16_t sequence) {
        return create_1w_add_controller(frame, identity.node_id, identity.io_device_type, identity.manufacturer,
                                        sequence, identity.system_key, identity.enrollment_with_mac);
      },
      "ENROLL");
}

bool OneWayTransmitter::send_unenrollment(const std::string &controller_id) {
  return this->send_(
      controller_id,
      [](IoFrame &frame, const OneWayControllerIdentity &identity, uint16_t sequence) {
        return create_1w_remove_controller(frame, identity.node_id, identity.io_device_type, sequence,
                                           identity.system_key);
      },
      "UNENROLL");
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
