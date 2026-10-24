/// @file
/// @brief The 2.4 GHz io-homecontrol PHY profile.
/// @ingroup hioc_radio

// IOHOME_BRIDLE is only visible after something pulls in esphome/core/defines.h (ESPHome codegen's
// cg.add_define() lands there, not as a compiler -D flag): this include must run before the #ifdef,
// or the file silently compiles to nothing in a real build although the host tests (which pass
// -DIOHOME_BRIDLE) are fine.
#include "esphome/core/hal.h"

#ifdef IOHOME_BRIDLE

#include "radio_io2g4_phy.h"

#include <cstring>

#include "proto_sizes.h"

namespace esphome::home_io_control::bridle {

namespace {

/// Smallest PSDU that can hold an io frame: header, the shortest frame, the FCS.
constexpr size_t MIN_PSDU_SIZE = IO2G4_PSDU_HEADER_SIZE + FRAME_MIN_SIZE + IO2G4_FCS_SIZE;

/// Ceiling division of nanoseconds into microseconds.
constexpr uint64_t NS_PER_US = 1000;
constexpr uint64_t ns_to_us_ceil(uint64_t ns) { return (ns + NS_PER_US - 1) / NS_PER_US; }

}  // namespace

void Io2g4Phy::write_params(TlvWriter &params, const TuningConfig & /*tuning*/) const {
  // The SFD is what tells io-homecontrol frames from standard 802.15.4 traffic.
  params.add_u8(static_cast<uint8_t>(ParamId::SFD), IO2G4_SFD);
}

bool Io2g4Phy::radio_fits(const RadioDescriptor &radio) const {
  // The io SFD, the FCS and a `then_rx` fast enough for a reply ~300 µs after the frame are what every
  // 802.15.4 radio does; strobe: the wake-up trains; rx_scan: the idle scan a host cannot drive itself.
  constexpr uint32_t required_caps = radio_caps::STROBE | radio_caps::RX_SCAN;
  return this->offers_profile_and_payload_(radio) && radio.band_min_hz <= IO2G4_CH1_HZ &&
         radio.band_max_hz >= IO2G4_CH3_HZ && (radio.caps & required_caps) == required_caps;
}

uint32_t Io2g4Phy::air_freq_hz(uint32_t channel_hz) const {
  switch (channel_hz) {
    case FREQ_CH1:
      return IO2G4_CH1_HZ;
    case FREQ_CH2:
      return IO2G4_CH2_HZ;
    case FREQ_CH3:
      return IO2G4_CH3_HZ;
    default:
      return IO2G4_CH2_HZ;  // the TX channel; a value the hub never produces still lands on a real one
  }
}

uint32_t Io2g4Phy::channel_hz(uint32_t air_hz) const {
  switch (air_hz) {
    case IO2G4_CH1_HZ:
      return FREQ_CH1;
    case IO2G4_CH2_HZ:
      return FREQ_CH2;
    case IO2G4_CH3_HZ:
      return FREQ_CH3;
    default:
      return 0;
  }
}

size_t Io2g4Phy::wrap(const uint8_t *frame, uint8_t len, uint8_t *out, size_t cap) const {
  size_t const total = IO2G4_PSDU_HEADER_SIZE + len;
  if (cap < total)
    return 0;
  std::memcpy(out, IO2G4_PSDU_HEADER, IO2G4_PSDU_HEADER_SIZE);
  if (len > 0)
    std::memcpy(out + IO2G4_PSDU_HEADER_SIZE, frame, len);
  return total;
}

UnwrapResult Io2g4Phy::unwrap(const uint8_t *payload, size_t len, CrcState crc, uint8_t *out, size_t cap) const {
  UnwrapResult result;
  if (len < MIN_PSDU_SIZE) {
    result.status = UnwrapResult::Status::TOO_SHORT;
    return result;
  }
  if (crc == CrcState::BAD) {
    result.status = UnwrapResult::Status::BAD_CRC;
    return result;
  }
  if (std::memcmp(payload, IO2G4_PSDU_HEADER, IO2G4_PSDU_HEADER_SIZE) != 0) {
    result.status = UnwrapResult::Status::HEADER_MISMATCH;
    std::memcpy(result.header, payload, IO2G4_PSDU_HEADER_SIZE);
    return result;
  }
  size_t const frame_len = len - IO2G4_PSDU_HEADER_SIZE - IO2G4_FCS_SIZE;
  if (frame_len > cap || frame_len > UINT8_MAX) {
    result.status = UnwrapResult::Status::TOO_LONG;
    return result;
  }
  std::memcpy(out, payload + IO2G4_PSDU_HEADER_SIZE, frame_len);
  result.frame_len = static_cast<uint8_t>(frame_len);
  result.status = UnwrapResult::Status::OK;
  return result;
}

TxShape Io2g4Phy::tx_shape(TxWake wake, uint16_t preamble_len) const {
  TxShape shape;  // preamble_bytes 0: the standard 4-byte preamble is the only one 802.15.4 has
  if (wake == TxWake::NONE)
    return shape;
  StrobeSpec &strobe = shape.strobe;
  shape.has_strobe = true;
  strobe.count = wake == TxWake::LONG ? IO2G4_LONG_STROBE_COUNT : IO2G4_SHORT_STROBE_COUNT;
  strobe.period_ns = IO2G4_STROBE_PERIOD_NS;
  strobe.main_gap_ns = IO2G4_STROBE_MAIN_GAP_NS;
  strobe.counter_offset = 1;
  strobe.counter_width = 2;
  strobe.counter_unit_ns = IO2G4_STROBE_PERIOD_NS;
  strobe.counter_adjust = -1;  // the frame k slots before the main frame counts k - 1 down to 0
  strobe.len_template = sizeof(IO2G4_STROBE_TEMPLATE);
  std::memcpy(strobe.template_bytes, IO2G4_STROBE_TEMPLATE, sizeof(IO2G4_STROBE_TEMPLATE));
  return shape;
}

uint32_t Io2g4Phy::tx_air_time_us(const TxShape &shape, size_t wrapped_len) const {
  // First strobe frame's start to the main frame's start, then the main frame itself.
  uint64_t lead_ns = 0;
  if (shape.has_strobe && shape.strobe.count > 0)
    lead_ns = (static_cast<uint64_t>(shape.strobe.count - 1) * shape.strobe.period_ns) + shape.strobe.main_gap_ns;
  uint64_t const main_bytes = IO2G4_PHY_OVERHEAD_BYTES + wrapped_len + IO2G4_FCS_SIZE;
  uint64_t const main_us = main_bytes * IO2G4_BYTE_US;
  return static_cast<uint32_t>(ns_to_us_ceil(lead_ns + (main_us * NS_PER_US)));
}

ScanSpec Io2g4Phy::idle_scan() const {
  ScanSpec scan;
  scan.dwell_us = IO2G4_SCAN_DWELL_US;
  scan.hold_us = IO2G4_SCAN_HOLD_US;
  scan.num_freqs = 3;
  scan.freqs_hz[0] = IO2G4_CH1_HZ;
  scan.freqs_hz[1] = IO2G4_CH2_HZ;
  scan.freqs_hz[2] = IO2G4_CH3_HZ;
  return scan;
}

const Io2g4Phy &io2g4_phy() {
  static const Io2g4Phy PHY;
  return PHY;
}

}  // namespace esphome::home_io_control::bridle

#endif  // IOHOME_BRIDLE
