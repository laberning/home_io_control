#pragma once

/// @file oneway_key_adoption.h
/// @brief Opt-in, receive-only adoption of a 1W installation's controller key.
/// @ingroup hioc_hub
///
/// See oneway_key_adoption.cpp for the security framing. This header carries only the collaborator
/// class; IOHomeControlComponent owns one instance (hub_core.h) and forwards the three public
/// entry points to it.

#include "hub_hooks.h"
#include "proto_codecs.h"
#include "proto_device_model.h"
#include "proto_frame.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace home_io_control {

/// @brief Opt-in, receive-only listener that adopts an overheard 1W controller key.
///
/// Receive-only: nothing here transmits. While armed, an overheard CMD_ONEWAY_ADD_CONTROLLER
/// broadcast is decrypted and reported once, after which the listener disarms itself — one
/// adoption per arm. Constructed once by IOHomeControlComponent; non-copyable because it is wired
/// with an injected scheduling callback.
/// @ingroup hioc_hub
class OnewayKeyAdoption {
 public:
  /// @param schedule_auto_off Named-timeout scheduler for the 10-minute arm window (see NamedTimeoutFn).
  explicit OnewayKeyAdoption(NamedTimeoutFn schedule_auto_off) : schedule_auto_off_(std::move(schedule_auto_off)) {}

  /// Non-copyable — holds an injected callback and is owned by the hub.
  OnewayKeyAdoption(const OnewayKeyAdoption &) = delete;
  OnewayKeyAdoption &operator=(const OnewayKeyAdoption &) = delete;

  /// @brief Arm or disarm the 1W controller-key adoption listener.
  ///
  /// Arming resets any class observed in an earlier window and schedules a 10-minute auto-off.
  /// Disarming — manual, on successful adoption, or on auto-off — is immediate. See the class doc
  /// comment; this is the body that was IOHomeControlComponent::set_oneway_key_adoption_armed().
  /// @param armed Desired state.
  void set_armed(bool armed);

  /// @brief Whether the listener is currently armed.
  [[nodiscard]] bool armed() const { return this->armed_; }

  /// Register a callback invoked whenever the armed state changes (manual toggle, successful
  /// adoption, or auto-off), so the switch entity stays in sync when the listener disarms itself.
  /// @param cb Callable receiving the new armed state.
  void set_armed_callback(std::function<void(bool)> cb) { this->armed_callback_ = std::move(cb); }

  /// Remember the most recent 1W target device class observed from `info.src`, for the adoption
  /// report's `io_device_type` prefill. No-op unless armed and `info.target_type` is a real class.
  /// @param info Already-decoded 1W frame info (see decode_1w_frame()).
  void record_observed_class(const OneWayFrameInfo &info);

  /// Decode an inbound CMD_ONEWAY_ADD_CONTROLLER (0x30) while armed, report the result, and
  /// disarm. Returns nothing and never consumes the frame — the caller still runs it through the
  /// normal 1W logging path.
  /// @param frame Parsed inbound 1W frame.
  void try_adopt(const IoFrame &frame);

  /// Most recent 1W target device class observed while armed. Single-slot — this is a one-gesture
  /// flow, not a per-node registry — and reset on every arm so a stale observation from an earlier
  /// window never leaks into the next one. Feeds the `io_device_type` prefill in the report.
  struct ObservedClass {
    uint8_t node[NODE_ID_SIZE]{};
    DeviceType type{DeviceType::UNKNOWN};
    bool valid{false};
  };
  /// @return The most recent observed class (see ObservedClass).
  [[nodiscard]] const ObservedClass &last_observed_class() const { return this->observed_class_; }

 private:
  NamedTimeoutFn schedule_auto_off_;
  bool armed_{false};
  std::function<void(bool)> armed_callback_;
  ObservedClass observed_class_{};
};

namespace detail {

/// @brief Build the full 1W controller-key-adoption report: MAC-verification status, the
/// own-address transmission rationale, and the ready-to-paste `oneway_controllers:` YAML block.
///
/// Pure — takes already-decoded values, performs no I/O — so it is directly unit-testable
/// without a live radio or a captured log line (ESP_LOG's host stub discards its arguments).
/// This is the single intentional place `adopted.system_key` is formatted for display (via
/// format_key_hex(), log_helpers.h); the caller (OnewayKeyAdoption::try_adopt()) logs the returned
/// text through log_multiline_result() and nowhere else.
///
/// `node_id` is deliberately never mentioned as something to fill in — a later step derives one
/// from the hub's own node ID, and the report says so rather than asking the user to invent a
/// 3-byte address. The report also explains that the hub always transmits under its own address:
/// impersonating the sender would hijack that remote's rolling sequence counter and break it.
///
/// The emitted keys must track `ONEWAY_CONTROLLER_SCHEMA` (`__init__.py`) by hand — a newly
/// required schema key needs a matching line here too. `make yaml-emitter-sync`
/// (scripts/check-yaml-emitters.py) catches drift between the two statically; it does not tell
/// you what to add here.
///
/// @param adopted Decoded controller identity from decode_1w_add_controller() (proto_codecs.h).
/// @param observed_type_known True if this sender's other 1W traffic was observed while armed
/// (see OnewayKeyAdoption::record_observed_class()); false prints a commented-out
/// fallback pointing at the DEBUG log line that would reveal it instead.
/// @param observed_type The observed target class; only meaningful when observed_type_known.
/// @return Multi-line report text, ready to pass to log_multiline_result().
std::string build_oneway_adoption_report(const OneWayAdoptedKey &adopted, bool observed_type_known,
                                         DeviceType observed_type);

}  // namespace detail

}  // namespace home_io_control
}  // namespace esphome
