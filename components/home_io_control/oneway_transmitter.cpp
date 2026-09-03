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

  // 0x30/0x39 carry no intent decode_1w_frame() can read (it only understands
  // CMD_EXECUTE/CMD_ACTIVATE_MODE payloads) -- send_enrollment() etc. pass the label directly
  // rather than leave the "Last 1W Command" sensor blank on the one feature whose entire
  // diagnostic story is that sensor.
  std::string intent = explicit_intent;
  if (intent.empty()) {
    const OneWayFrameInfo info = decode_1w_frame(frame);
    if (info.has_intent)
      intent = info.intent;
  }
  // Always the identity's own class, never the frame's decoded destination: with
  // `execute_broadcast: all` the wire dst is `00 00 3F` -> DeviceType::UNKNOWN, which would render
  // "unknown" in the TX log and the sensor. The literal destination is already in the frame log.
  this->report_attempt_(controller_id, intent, identity->io_device_type, sequence, /*sequence_reserved=*/true,
                        transmitted);
  return transmitted;
}

void OneWayTransmitter::report_attempt_(const std::string &controller_id, const std::string &intent,
                                        DeviceType target_type, uint16_t sequence, bool sequence_reserved,
                                        bool transmitted) {
  if (!this->report_)
    return;
  OneWayCommandReport report{};
  report.controller_id = controller_id;
  report.intent = intent;
  report.target_type = target_type;
  report.sequence = sequence;
  report.sequence_reserved = sequence_reserved;
  report.transmitted = transmitted;
  this->report_(report);
}

void OneWayTransmitter::report_failure_(const std::string &controller_id, uint16_t sequence, bool sequence_reserved) {
  this->report_attempt_(controller_id, "", DeviceType::UNKNOWN, sequence, sequence_reserved, /*transmitted=*/false);
}

bool OneWayTransmitter::send_command(const std::string &controller_id, CoverCommand cmd) {
  return this->send_(controller_id, [cmd](IoFrame &frame, const OneWayControllerIdentity &identity, uint16_t sequence) {
    return create_1w_execute_command(frame, identity.node_id, identity.io_device_type, cmd, sequence,
                                     identity.system_key, effective_execute_acei(identity),
                                     identity.execute_broadcast_all);
  });
}

bool OneWayTransmitter::send_position(const std::string &controller_id, uint8_t position) {
  return this->send_(
      controller_id, [position](IoFrame &frame, const OneWayControllerIdentity &identity, uint16_t sequence) {
        return create_1w_execute_position(frame, identity.node_id, identity.io_device_type, position, sequence,
                                          identity.system_key, effective_execute_acei(identity),
                                          identity.execute_broadcast_all);
      });
}

bool OneWayTransmitter::send_enrollment(const std::string &controller_id) {
  const OneWayControllerIdentity *identity = this->identities_.get(controller_id);
  if (identity == nullptr) {
    ESP_LOGW(TAG, "1W tx: no controller identity '%s'", controller_id.c_str());
    this->report_failure_(controller_id, 0, /*sequence_reserved=*/false);
    return false;
  }
  if (resolve_oneway_wire_profile(identity->manufacturer).enroll_gesture == EnrollGesture::VELUX_KLI)
    return this->send_velux_kli_enrollment_(*identity);
  return this->send_somfy_enrollment_(*identity);
}

bool OneWayTransmitter::send_somfy_enrollment_(const OneWayControllerIdentity &identity) {
  // The documented 1W pairing handshake (reference/iown-homecontrol/docs/linklayer.md:396, "1W
  // Discovery") is `0x39` immediately followed by `0x30`, both from the same controller, back to
  // back within one gesture -- a real Smoove capture landed them 128 ms apart, same burst (see
  // tests/corpus/captures/enrollment/somfy_smoove_enrollment_add_and_remove_controller_sx1276.yaml and
  // analysis/completed/oneway_1w_support_plan.md Step 13). `0x39` here carries only this
  // identity's own `src` address, so on the wire it can only mean "clear my own prior entry
  // before I re-register" -- it cannot name or displace a different controller. Sending it right
  // before `0x30` clears a stale slot from an earlier enrollment attempt under this identity,
  // which a bare `0x30` re-add is not guaranteed to overwrite.
  const bool removed = this->send_(
      identity.id,
      [](IoFrame &frame, const OneWayControllerIdentity &id, uint16_t sequence) {
        return create_1w_remove_controller(frame, id.node_id, id.io_device_type, sequence, id.system_key);
      },
      "UNENROLL");
  if (!removed) {
    ESP_LOGW(TAG, "1W tx: enrollment's 0x39 prelude did not reach the radio for '%s' -- trying 0x30 anyway",
             identity.id.c_str());
  }

  return this->send_(
      identity.id,
      [](IoFrame &frame, const OneWayControllerIdentity &id, uint16_t sequence) {
        return create_1w_add_controller(frame, id.node_id, id.io_device_type, id.manufacturer, sequence, id.system_key,
                                        id.enrollment_with_mac);
      },
      "ENROLL");
}

bool OneWayTransmitter::send_velux_kli_enrollment_(const OneWayControllerIdentity &identity) {
  // The gesture a real KLI 310/313 PROG press produces (issue #74 capture + samr037/iohc-flipper
  // tx_runner.c + the KLI manual, see analysis/velux_vs_somfy_1w_frame_differences.md §6):
  //   0x39 -> 00 00 3F  (clear self; VELUX broadcasts it, unlike Somfy's typed 0x39)
  //   0x30 -> each of {roller_shutter, awning, dual_shutter} under one sequence  (the class sweep)
  //   EXECUTE STOP, then EXECUTE DOWN, both -> 00 00 3F at the VELUX ACEI  (registration completion)
  const bool removed = this->send_(
      identity.id,
      [](IoFrame &frame, const OneWayControllerIdentity &id, uint16_t sequence) {
        return create_1w_remove_controller(frame, id.node_id, DeviceType::UNKNOWN, sequence, id.system_key);
      },
      "UNENROLL");
  if (!removed) {
    ESP_LOGW(TAG, "1W tx: VELUX enrollment's 0x39 prelude did not reach the radio for '%s' -- continuing",
             identity.id.c_str());
  }

  const bool enrolled = this->send_enroll_sweep_(identity, effective_enrollment_classes(identity));
  if (!enrolled) {
    // Nothing registered us, so the STOP+DOWN would be an unprovoked close broadcast to every 1W
    // device on the network holding the key. Skip it.
    ESP_LOGW(TAG, "1W tx: VELUX 0x30 sweep transmitted nothing for '%s' -- skipping the STOP+DOWN follow-up",
             identity.id.c_str());
    return false;
  }

  // STOP then DOWN, per the KLI manual's "then STOP then DOWN within 3 seconds". Broadcast, the
  // identity's effective ACEI, one sequence each. A partial miss here only warns -- the sweep
  // above is what registers us.
  const uint8_t acei = effective_execute_acei(identity);
  const bool stopped = this->send_(
      identity.id,
      [acei](IoFrame &frame, const OneWayControllerIdentity &id, uint16_t sequence) {
        return create_1w_execute_command(frame, id.node_id, id.io_device_type, CoverCommand::STOP, sequence,
                                         id.system_key, acei, /*broadcast_all=*/true);
      },
      "ENROLL STOP");
  const bool lowered = this->send_(
      identity.id,
      [acei](IoFrame &frame, const OneWayControllerIdentity &id, uint16_t sequence) {
        return create_1w_execute_position(frame, id.node_id, id.io_device_type, ONEWAY_POSITION_FULLY_CLOSED, sequence,
                                          id.system_key, acei, /*broadcast_all=*/true);
      },
      "ENROLL DOWN");
  if (!stopped || !lowered) {
    ESP_LOGW(TAG, "1W tx: VELUX enrollment's STOP+DOWN follow-up did not fully reach the radio for '%s'",
             identity.id.c_str());
  }
  return true;  // the sweep registered us; STOP+DOWN are completion, not the credential
}

bool OneWayTransmitter::send_enroll_sweep_(const OneWayControllerIdentity &identity,
                                           const std::array<DeviceType, 3> &classes) {
  // One sequence for the whole sweep -- every 0x30 in it carries the same value, matching a real
  // KLI remote and how send_() treats a burst's copies as one logical command.
  uint16_t sequence = 0;
  if (!this->sequences_.next(identity.node_id, sequence)) {
    this->report_failure_(identity.id, 0, /*sequence_reserved=*/false);
    return false;
  }

  bool any_transmitted = false;
  for (const DeviceType target_type : classes) {
    if (target_type == DeviceType::UNKNOWN)
      continue;
    IoFrame frame{};
    if (!create_1w_add_controller(frame, identity.node_id, target_type, identity.manufacturer, sequence,
                                  identity.system_key, identity.enrollment_with_mac)) {
      ESP_LOGW(TAG, "1W tx: could not build a 0x30 for class 0x%02X on '%s'", static_cast<unsigned>(target_type),
               identity.id.c_str());
      continue;
    }
    if (this->send_burst(frame))
      any_transmitted = true;
  }

  this->report_attempt_(identity.id, "ENROLL", identity.io_device_type, sequence, /*sequence_reserved=*/true,
                        any_transmitted);
  return any_transmitted;
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
