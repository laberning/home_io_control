#pragma once

/// @file test_bands.h
/// @brief A band that is not 868 MHz, for tests that must show the hub reads the radio's band and
/// tests no band of its own.
/// @ingroup hioc_hub

#include "proto_band.h"

namespace esphome {
namespace home_io_control {
namespace test_bands {

/// Air frequencies of the second band's channels, invented: none is a frequency of a real radio band
/// and each lies 25 MHz from the next, so a Wi-Fi centre placed on one channel (the 15 MHz guard of
/// decisions::WIFI_CHANNEL_GUARD_HZ) leaves the other two outside.
static constexpr uint32_t SECOND_BAND_CH1_HZ = 2415000000U;
static constexpr uint32_t SECOND_BAND_CH2_HZ = 2440000000U;
static constexpr uint32_t SECOND_BAND_CH3_HZ = 2465000000U;

/// A second band with invented numbers (a test fixture, not a real band, and not a value any radio
/// carries): other air frequencies, every channel may wake a low-power receiver, a looser fallback
/// busy limit, short reply windows and a short gap, battery-powered unless declared, and an answer
/// reported late after 40 ms. Every field differs from IO_868_BAND, so a rule that still reads an
/// 868 MHz constant shows.
inline IoBand second_band() {
  IoBand band;
  band.air_hz = {SECOND_BAND_CH2_HZ, SECOND_BAND_CH1_HZ, SECOND_BAND_CH3_HZ};  // CH2, CH1, CH3
  band.wake_up_channels = ChannelSet::all();
  band.busy_limit_until_quiet_level_dbm = -70;
  band.start_reply_window_ms = 80;
  band.low_power_reply_window_ms = 70;
  band.retry_gap_ms = 45;
  band.low_power_by_default = true;
  band.late_answer_warn_ms = 40;
  return band;
}

}  // namespace test_bands
}  // namespace home_io_control
}  // namespace esphome
