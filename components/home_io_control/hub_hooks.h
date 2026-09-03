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

namespace esphome {
namespace home_io_control {

/// @brief Schedules a named, replace-on-same-name timeout on the hub's ESPHome scheduler.
/// @param name     Timer name; a later call with the same name replaces the pending callback.
/// @param delay_ms Delay before the callback runs.
/// @param callback Work to run in loop() context after the delay.
using NamedTimeoutFn = std::function<void(const char *name, uint32_t delay_ms, std::function<void()> callback)>;

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

}  // namespace home_io_control
}  // namespace esphome
