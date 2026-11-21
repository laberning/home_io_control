/// @file oneway_transmitter.cpp
/// @brief One-way (1W) transmit collaborator.
/// @ingroup hioc_hub

#include "oneway_transmitter.h"

#include "proto_codecs.h"
#include "proto_commands.h"
#include "proto_device_kinds.h"
#include "proto_timing.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cmath>

namespace esphome {
namespace home_io_control {

namespace {

constexpr const char *const TAG = "home_io_control.oneway_tx";

/// Tenths per millisecond: the resolution of the TX log's preamble durations.
constexpr int32_t TENTHS_PER_MS = 10;

}  // namespace

OneWayCopyPreamble oneway_copy_preamble(OneWayCopyShape shape, const OneWayPreambleRules &rules) {
  if (shape.preamble == OneWayPreamble::WAKE)
    return {rules.bytes_for_ms(rules.wake_ms), rules.wake_ms};
  return {rules.bytes_for_ms(rules.start_ms), rules.start_ms};
}

std::string format_oneway_preamble_list(OneWayPowerClass power_class, const OneWayPreambleRules &rules) {
  std::string out;
  for (uint8_t copy = 0; copy < ONEWAY_BURST_REPEATS; copy++) {
    const OneWayCopyShape shape = oneway_burst_copy_shape(power_class, copy);
    if (copy > 0)
      out += '/';
    // Tenths of a millisecond: `start_preamble_ms` moves in 0.5 ms steps, and a whole-ms rounding
    // would log 3.5 ms as 4. A whole value prints without a decimal ("515/10/10/10").
    const auto tenths = static_cast<int32_t>(std::lround(oneway_copy_preamble(shape, rules).ms * TENTHS_PER_MS));
    out += std::to_string(tenths / TENTHS_PER_MS);
    if (tenths % TENTHS_PER_MS != 0) {
      out += '.';
      out += std::to_string(tenths % TENTHS_PER_MS);
    }
  }
  return out;
}

void OneWayTransmitter::setup() {
  for (const auto &identity : this->identities_.all())
    this->sequences_.add_identity(identity.node_id, identity.initial_sequence);
}

bool OneWayTransmitter::send_(const std::string &controller_id,
                              const std::function<bool(IoFrame &, const OneWayControllerIdentity &, uint16_t)> &build,
                              const char *explicit_intent, bool enrollment) {
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

  const bool transmitted =
      this->send_burst(frame, effective_power_class(*identity), explicit_intent[0] != '\0' ? explicit_intent : nullptr);

  const OneWayFrameInfo decoded = decode_1w_frame(frame);

  // 0x30/0x39 carry no intent decode_1w_frame() can read (it only understands
  // CMD_EXECUTE/CMD_ACTIVATE_MODE payloads) -- send_enrollment() etc. pass the label directly
  // rather than leave the "Last 1W Command" sensor blank on the one feature whose entire
  // diagnostic story is that sensor.
  std::string intent = explicit_intent;
  if (intent.empty() && decoded.has_intent)
    intent = decoded.intent;
  // The wire values of what went out, for the hub to follow. An enrollment frame reports none: its
  // follow-up commands (the VELUX registration STOP and DOWN) belong to the gesture and must not move a
  // device the hub follows.
  const OneWayFrameInfo wire = enrollment ? OneWayFrameInfo{} : decoded;
  // Always the identity's own class, never the frame's decoded destination: with
  // `execute_broadcast: all` the wire dst is `00 00 3F` -> DeviceType::UNKNOWN, which would render
  // "unknown" in the sensor (the sensor has no distinct "all" case the way the frame log does via
  // oneway_target_label()). The literal destination is already in the frame log.
  this->report_attempt_(controller_id, intent, identity->io_device_type, sequence, /*sequence_reserved=*/true,
                        transmitted, wire);
  return transmitted;
}

void OneWayTransmitter::report_attempt_(const std::string &controller_id, const std::string &intent,
                                        DeviceType target_type, uint16_t sequence, bool sequence_reserved,
                                        bool transmitted, const OneWayFrameInfo &wire) {
  if (!this->report_)
    return;
  OneWayCommandReport report{};
  report.controller_id = controller_id;
  report.intent = intent;
  report.target_type = target_type;
  report.sequence = sequence;
  report.sequence_reserved = sequence_reserved;
  report.transmitted = transmitted;
  report.wire = wire;
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
  // The caller's position is the user's (0 = open). An inverted kind wants wire 0 = closed, so the
  // value is mirrored here and only here. The intent label is computed from the user position
  // before mirroring, because decoding the built frame would report CLOSE for a user OPEN.
  // Spelled out rather than decoded from the wire byte: a user 50% would decode as FORCE_OPEN.
  char intent[ONEWAY_INTENT_BUFFER_SIZE]{};
  if (position == ONEWAY_POSITION_FULLY_OPEN) {
    snprintf(intent, sizeof(intent), "OPEN");
  } else if (position == ONEWAY_POSITION_FULLY_CLOSED) {
    snprintf(intent, sizeof(intent), "CLOSE");
  } else {
    snprintf(intent, sizeof(intent), "position %u%%", static_cast<unsigned>(position));
  }
  return this->send_(
      controller_id,
      [position](IoFrame &frame, const OneWayControllerIdentity &identity, uint16_t sequence) {
        // Out-of-range input is passed through so the builder refuses it, never wrapped by the mirror.
        const bool mirror = effective_inverted(identity) && position <= ONEWAY_POSITION_FULLY_CLOSED;
        const uint8_t wire_position = mirror ? static_cast<uint8_t>(ONEWAY_POSITION_FULLY_CLOSED - position) : position;
        return create_1w_execute_position(frame, identity.node_id, identity.io_device_type, wire_position, sequence,
                                          identity.system_key, effective_execute_acei(identity),
                                          identity.execute_broadcast_all);
      },
      intent);
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
  // The 1W pairing handshake is `0x39` immediately followed by `0x30`, both from the same controller, back to back
  // within one gesture -- a real Smoove capture landed them 128 ms apart, same burst (see
  // tests/corpus/captures/enrollment/somfy_smoove_enrollment_add_and_remove_controller_sx1276.yaml).
  // Both go to the all-devices broadcast (00 00 3F), as every real Somfy remote we hold sends them
  // (Smoove, Situo 5, Situo 1): only a receiver already in association mode reacts, so naming a
  // class adds nothing, and a class the receiver does not count itself in (a patio roof that is a
  // horizontal awning, not an awning) would never be reached. The identity's `io_device_type` still
  // addresses its commands.
  // `0x39` here carries only this identity's own `src` address, so it cannot name or displace a
  // different controller. A bench run on a Somfy Izymo showed that a receiver which already knows
  // the identity takes it off on the `0x39` and spends its PROG window on that change, so the
  // `0x30` that follows is not taken; on an identity the receiver does not know the `0x39` changes
  // nothing and the `0x30` pairs it.
  // The handling of an already-enrolled identity is an open decision (\todo on the declaration).
  const bool removed = this->send_(
      identity.id,
      [](IoFrame &frame, const OneWayControllerIdentity &id, uint16_t sequence) {
        return create_1w_remove_controller(frame, id.node_id, DeviceType::UNKNOWN, sequence, id.system_key);
      },
      "UNENROLL", /*enrollment=*/true);
  if (!removed) {
    ESP_LOGW(TAG, "1W tx: enrollment's 0x39 prelude did not reach the radio for '%s' -- trying 0x30 anyway",
             identity.id.c_str());
  }

  return this->send_(
      identity.id,
      [](IoFrame &frame, const OneWayControllerIdentity &id, uint16_t sequence) {
        return create_1w_add_controller(frame, id.node_id, DeviceType::UNKNOWN, id.manufacturer, sequence,
                                        id.system_key, id.enrollment_with_mac);
      },
      "ENROLL", /*enrollment=*/true);
}

bool OneWayTransmitter::send_velux_kli_enrollment_(const OneWayControllerIdentity &identity) {
  // The gesture a real KLI 310/313 PROG press produces (issue #74 capture + the KLI
  // manual):
  //   0x39 -> 00 00 3F  (clear self, to the all-devices address like the Somfy gesture's)
  //   0x30 -> each class in effective_enrollment_classes() (the profile's exterior-shading set unless
  //           `enrollment_classes:` overrides it) under one sequence (the class sweep)
  //   EXECUTE STOP, then EXECUTE DOWN, both -> 00 00 3F at the VELUX ACEI  (registration completion)
  const bool removed = this->send_(
      identity.id,
      [](IoFrame &frame, const OneWayControllerIdentity &id, uint16_t sequence) {
        return create_1w_remove_controller(frame, id.node_id, DeviceType::UNKNOWN, sequence, id.system_key);
      },
      "UNENROLL", /*enrollment=*/true);
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

  // STOP then DOWN, per the KLI manual's "press PAIR, then STOP then DOWN" registration-completion
  // step (the manual's own timing window is not established as a hard requirement here -- see ADR
  // 0032). Broadcast, the identity's effective ACEI, one sequence each. A partial miss
  // here only warns -- the sweep above is what registers us.
  const uint8_t acei = effective_execute_acei(identity);
  const bool stopped = this->send_(
      identity.id,
      [acei](IoFrame &frame, const OneWayControllerIdentity &id, uint16_t sequence) {
        return create_1w_execute_command(frame, id.node_id, id.io_device_type, CoverCommand::STOP, sequence,
                                         id.system_key, acei, /*broadcast_all=*/true);
      },
      "ENROLL STOP", /*enrollment=*/true);
  const bool lowered = this->send_(
      identity.id,
      [acei](IoFrame &frame, const OneWayControllerIdentity &id, uint16_t sequence) {
        // Raw wire value on purpose, not mirrored: this is the enrollment gesture a real KLI remote
        // produces, not a user position.
        return create_1w_execute_position(frame, id.node_id, id.io_device_type, ONEWAY_POSITION_FULLY_CLOSED, sequence,
                                          id.system_key, acei, /*broadcast_all=*/true);
      },
      "ENROLL DOWN", /*enrollment=*/true);
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
    if (this->send_burst(frame, effective_power_class(identity)))
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
        // All-devices broadcast, like the enrollment prelude: a real remote's 0x39 never names a class.
        return create_1w_remove_controller(frame, identity.node_id, DeviceType::UNKNOWN, sequence, identity.system_key);
      },
      "UNENROLL", /*enrollment=*/true);
}

bool OneWayTransmitter::send_burst(const IoFrame &frame, OneWayPowerClass power_class, const char *intent_label) {
  // Logged once for the whole burst rather than once per copy: four log lines per button press
  // would suggest four commands, which is exactly the misreading the shared sequence exists to
  // prevent. Only frame-header facts are logged — never payload bytes — so this stays safe even
  // for the commands redaction.h flags as carrying key material.
  const OneWayFrameInfo info = decode_1w_frame(frame);
  // Same vocabulary as the RX line (hub_internal.h's log_1w_remote_frame()): "all" for the
  // all-devices broadcast, else the target class's bare name -- one decision, made once, by
  // oneway_target_label() (proto_codecs.h), so the two paths cannot render the same address
  // differently. "to <class>" is a fair claim here and only here: we choose the class we transmit
  // to (enrollment_classes). The RX line says dst-class= instead, because a received class says
  // nothing about what the sending remote drives.
  const char *target = oneway_target_label(info);
  // Built from the same oneway_burst_copy_shape() the transmit loop below calls, so this can never
  // claim a preamble shape the burst does not actually send.
  const OneWayPreambleRules rules{this->tuning_->start_preamble_ms, this->tuning_->low_power_wake_ms,
                                  this->preamble_bytes_};
  const std::string preambles = format_oneway_preamble_list(power_class, rules);
  if (info.has_intent || intent_label != nullptr) {
    ESP_LOGI(TAG, "1W tx: from %s to %s intent %s (%ux, preamble %s ms)", node_id_to_string(frame.src).c_str(), target,
             intent_label != nullptr ? intent_label : info.intent, ONEWAY_BURST_REPEATS, preambles.c_str());
  } else {
    ESP_LOGI(TAG, "1W tx: from %s to %s cmd 0x%02X (%ux, preamble %s ms)", node_id_to_string(frame.src).c_str(), target,
             frame.cmd, ONEWAY_BURST_REPEATS, preambles.c_str());
  }

  uint8_t sent = 0;
  for (uint8_t copy = 0; copy < ONEWAY_BURST_REPEATS; copy++) {
    // No wait between copies, as a real remote sends them; only the channel check inside
    // transmit_ (ADR 0096) can delay one. The watchdog is fed per copy, as the burst blocks.
    App.feed_wdt();
    // The transmitter owns CTRL1's low-power bit and the preamble, per copy -- the frame builders
    // always hand back ctrl1=0 (proto_commands.cpp), and this clears the bit unconditionally on a
    // NORMAL-shaped copy too, whatever the builder produced. Everything else about the copy
    // (cmd/data/sequence/MAC) stays exactly what the caller built, per send_burst()'s own contract.
    const OneWayCopyShape shape = oneway_burst_copy_shape(power_class, copy);
    IoFrame copy_frame = frame;
    if (shape.low_power_flag) {
      copy_frame.ctrl1 |= CTRL1_LOW_POWER;
    } else {
      copy_frame.ctrl1 &= static_cast<uint8_t>(~CTRL1_LOW_POWER);
    }
    const OneWayCopyPreamble preamble = oneway_copy_preamble(shape, rules);
    if (this->transmit_(copy_frame, FREQ_CH2, preamble.bytes)) {
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
