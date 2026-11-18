#pragma once

/// @file hub_hooks.h
/// @brief Injected-capability callback aliases shared by the hub's collaborator objects.
/// @ingroup hioc_hub
///
/// Several hub collaborators need a *protected* hub capability — `Component::set_timeout()`
/// (protected in real ESPHome, only public in the host test stub),
/// `IOHomeControlComponent::transmit_frame_()`, `Component::warn_if_blocking_over_`. Rather than
/// granting `friend`, the hub injects a `std::function` for each, constructed as a lambda in its
/// own member-initializer list, which has the access. This header holds only those aliases so the
/// collaborator headers do not each redefine them. Dependency-light on purpose: it never includes
/// hub_core.h.

#include "proto_frame.h"

#include <cstdint>
#include <functional>
#include <string>

namespace esphome {
namespace home_io_control {

/// @brief Schedules a named, replace-on-same-name timeout on the hub's ESPHome scheduler.
/// @param name     Timer name; a later call with the same name replaces the pending callback.
/// @param delay_ms Delay before the callback runs.
/// @param callback Work to run in loop() context after the delay.
using NamedTimeoutFn = std::function<void(const char *name, uint32_t delay_ms, std::function<void()> callback)>;

/// @brief Schedules a numeric-id, replace-on-same-id timeout on the hub's ESPHome scheduler.
///
/// The id form of NamedTimeoutFn, for a timer keyed by a device's node address, where a name built per
/// device would dangle (`Component::set_timeout(const char *, ...)` keeps the caller's pointer).
/// @param id       Timer id; a later call with the same id replaces the pending callback.
/// @param delay_ms Delay before the callback runs.
/// @param callback Work to run in loop() context after the delay.
using NumericTimeoutFn = std::function<void(uint32_t id, uint32_t delay_ms, std::function<void()> callback)>;

/// @brief Puts a frame on air on a given channel via the hub's protected transmit_frame_().
/// @param frame    Frame to serialize and transmit.
/// @param freq_hz  RF channel frequency in Hz.
/// @param preamble Preamble length in bytes.
/// @return true if the frame reached the radio.
using TransmitFrameFn = std::function<bool(const IoFrame &frame, uint32_t freq_hz, uint16_t preamble)>;

/// @brief Raises the hub's "operation took a long time" warning threshold for a blocking radio
/// excursion — writes `Component::warn_if_blocking_over_`, which is protected on ESPHome's
/// `Component`. Wired to a lambda in the hub's initializer list, which has protected access.
using BeginBlockingExcursionFn = std::function<void()>;

/// @brief Runs one authenticated request/answer exchange on the command channel with the hub's shared exchange
/// bookkeeping (exchange counters, link health of every ending, result code of an error reply, device event
/// reports) and no poll backoff.
/// @param device_id Hexadecimal node ID string of the device the request goes to.
/// @param request   Request frame to send.
/// @param response  Receives the device's reply.
/// @return true when a reply frame, an error reply included, is in @p response; false for silence or an acceptance
///         without a reply.
using RecordedExchangeFn = std::function<bool(const std::string &device_id, const IoFrame &request, IoFrame &response)>;

}  // namespace home_io_control
}  // namespace esphome
