/// @file
/// @brief The 868 MHz io-homecontrol PHY profile on a `gfsk_raw` radio.
/// @ingroup hioc_radio

// IOHOME_BRIDLE is only visible after something pulls in esphome/core/defines.h (ESPHome codegen's
// cg.add_define() lands there, not as a compiler -D flag): this include must run before the #ifdef,
// or the file silently compiles to nothing in a real build although the host tests (which pass
// -DIOHOME_BRIDLE) are fine.
#include "esphome/core/hal.h"

#ifdef IOHOME_BRIDLE

#include "radio_io868_gfsk_phy.h"

#include <cmath>
#include <cstring>

#include "proto_frame.h"
#include "proto_sizes.h"

namespace esphome::home_io_control::bridle {

namespace {

/// Hertz in a kilohertz: the bandwidth tunable is a kHz option, the `rx_bw_hz` param is Hz.
constexpr float HZ_PER_KHZ = 1000.0F;

}  // namespace

void Io868GfskPhy::write_params(TlvWriter &params, const TuningConfig &tuning) const {
  // The order of vector `set_params_io868`: the required params, then the optional ones.
  params.add_u32(static_cast<uint8_t>(ParamId::BITRATE_BPS), IO868_BITRATE_BPS);
  params.add_u32(static_cast<uint8_t>(ParamId::FDEV_HZ), IO868_FDEV_HZ);
  params.add_u32(static_cast<uint8_t>(ParamId::RX_BW_HZ),
                 static_cast<uint32_t>(std::lround(sx1262_bandwidth_to_khz(tuning.sx1262_rx_bandwidth) * HZ_PER_KHZ)));
  params.add_u8(static_cast<uint8_t>(ParamId::PULSE_SHAPE), static_cast<uint8_t>(PulseShape::GAUSSIAN_BT_1_0));
  params.add(static_cast<uint8_t>(ParamId::SYNC_WORD), IO868_SYNC_WORD, sizeof(IO868_SYNC_WORD));
  params.add_u8(static_cast<uint8_t>(ParamId::RX_LEN), IO868_RX_LEN);
  params.add_u8(static_cast<uint8_t>(ParamId::PREAMBLE_DETECT_BITS), IO868_PREAMBLE_DETECT_BITS);
  params.add_u32(static_cast<uint8_t>(ParamId::SYNC_TIMEOUT_US), IO868_SYNC_TIMEOUT_US);
}

bool Io868GfskPhy::radio_fits(const RadioDescriptor &radio) const {
  // Everything else the band needs (then_rx, the preamble and sync events, a per-packet preamble) is
  // what every `gfsk_raw` radio does; rx_scan is the idle scan a host cannot drive itself, and the
  // preamble capacity is the longest wake-up, checked here rather than at the first wake-up.
  return this->offers_profile_and_payload_(radio) && radio.band_min_hz <= FREQ_CH1 && radio.band_max_hz >= FREQ_CH3 &&
         (radio.caps & radio_caps::RX_SCAN) != 0 && radio.max_preamble_bytes >= LOW_POWER_WAKE_PREAMBLE_MAX;
}

uint32_t Io868GfskPhy::air_freq_hz(uint32_t channel_hz) const {
  switch (channel_hz) {
    case FREQ_CH1:
    case FREQ_CH2:
    case FREQ_CH3:
      return channel_hz;
    default:
      return FREQ_CH2;  // the TX channel; a value the hub never produces still lands on a real one
  }
}

uint32_t Io868GfskPhy::channel_hz(uint32_t air_hz) const {
  return air_hz == FREQ_CH1 || air_hz == FREQ_CH2 || air_hz == FREQ_CH3 ? air_hz : 0;
}

size_t Io868GfskPhy::wrap(const uint8_t *frame, uint8_t len, uint8_t *out, size_t cap) const {
  if (len == 0)
    return 0;
  // The CRC is appended before the UART coding, so it occupies two cells of its own. FRAME_MAX_WIRE_SIZE
  // already holds a frame, its trailer and its CRC.
  uint8_t with_crc[FRAME_MAX_WIRE_SIZE] = {0};
  if (static_cast<size_t>(len) + FRAME_CRC_SIZE > sizeof(with_crc))
    return 0;
  std::memcpy(with_crc, frame, len);
  const uint16_t crc = crc_ccitt(frame, len);
  with_crc[len] = crc & UINT8_MAX;  // low byte first, as the frame's own CRC field
  with_crc[len + 1] = (crc >> BITS_PER_BYTE) & UINT8_MAX;
  const size_t needed = wrapped_size(len);
  if (cap < needed)
    return 0;
  return uart_encode_packet(with_crc, len + FRAME_CRC_SIZE, out, static_cast<uint8_t>(cap));
}

UnwrapResult Io868GfskPhy::unwrap(const uint8_t *payload, size_t len, CrcState /*crc*/, uint8_t *out,
                                  size_t cap) const {
  UnwrapResult result;
  if (len == 0 || len > UINT8_MAX) {
    result.status = UnwrapResult::Status::TOO_SHORT;
    return result;
  }
  // The raw bytes have no guaranteed cell alignment: the probe tries every one and accepts only a
  // CRC-valid frame of a command the hub knows.
  const UartProbeResult probe = find_uart_probe(payload, static_cast<uint8_t>(len));
  if (!probe.valid) {
    result.status = UnwrapResult::Status::BAD_CRC;
    return result;
  }
  if (probe.frame_len > cap) {
    result.status = UnwrapResult::Status::TOO_LONG;
    return result;
  }
  std::memcpy(out, probe.decoded + probe.frame_start, probe.frame_len);
  result.frame_len = probe.frame_len;
  result.status = UnwrapResult::Status::OK;
  return result;
}

TxShape Io868GfskPhy::tx_shape(TxWake /*wake*/, uint16_t preamble_len) const {
  TxShape shape;  // the wake-up is the preamble the hub chose; 0 asks for the short continuation preamble
  shape.preamble_bytes = preamble_len != 0 ? preamble_len : SHORT_PREAMBLE;
  return shape;
}

uint32_t Io868GfskPhy::tx_air_time_us(const TxShape &shape, size_t wrapped_len) const {
  return soft_phy_air_time_us(shape.preamble_bytes + IO868_SYNC_WORD_BYTES + static_cast<uint32_t>(wrapped_len));
}

ScanSpec Io868GfskPhy::idle_scan() const {
  ScanSpec scan;
  scan.dwell_us = IO868_SCAN_DWELL_US;
  scan.hold_us = 0;
  scan.num_freqs = 3;
  scan.freqs_hz[0] = FREQ_CH1;
  scan.freqs_hz[1] = FREQ_CH2;
  scan.freqs_hz[2] = FREQ_CH3;
  return scan;
}

const Io868GfskPhy &io868_gfsk_phy() {
  static const Io868GfskPhy PHY;
  return PHY;
}

}  // namespace esphome::home_io_control::bridle

#endif  // IOHOME_BRIDLE
