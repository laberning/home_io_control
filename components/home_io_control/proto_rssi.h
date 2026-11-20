#pragma once

/// @file proto_rssi.h
/// @brief The one value that stands for "no signal strength" wherever an RSSI is passed on as a number.
/// @ingroup hioc_protocol
///
/// A received frame's RSSI is its own or unknown (ADR 0112),
/// and a device record has no sample until its first frame. The sentinel lives in a header of its own so
/// the radio interface can name it without pulling in the device model.

#include <cstdint>
#include <limits>

/// @brief Sentinel value meaning "no RSSI sample recorded yet" for `last_rssi_dbm`/`rssi_ema_scaled`,
/// and "this frame has no level of its own" wherever a received frame's RSSI is passed on as a plain
/// number (`RadioCaptureInfo::rssi_or_unknown()`). A real RSSI reading from these radios is always well
/// above `INT16_MIN` — and so is any real `rssi_ema_scaled` fixed-point value.
static constexpr int16_t RSSI_UNKNOWN_DBM = std::numeric_limits<int16_t>::min();
