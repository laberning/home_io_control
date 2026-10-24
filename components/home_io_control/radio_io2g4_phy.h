/// @file
/// @brief The 2.4 GHz io-homecontrol PHY profile: io frames in IEEE 802.15.4 PSDUs.
/// @ingroup hioc_radio
///
/// 2.4 GHz io-homecontrol carries the 868 MHz frames unchanged inside 802.15.4 O-QPSK 250 kb/s
/// PSDUs, behind the fixed header `01 69 6F` and with a start-of-frame delimiter of 0x56. There
/// is no preamble to lengthen, so a receiver that sleeps is woken by a train of short frames (a
/// strobe) ahead of the frame. This is the sibling of `radio_soft_phy.h`: no chip, no transport,
/// only the band. Compiled only under `IOHOME_BRIDLE`.

#pragma once

// IOHOME_BRIDLE is only visible after something pulls in esphome/core/defines.h (ESPHome codegen's
// cg.add_define() lands there, not as a compiler -D flag): this include must run before the #ifdef,
// or the file silently compiles to nothing in a real build although the host tests (which pass
// -DIOHOME_BRIDLE) are fine.
#include "esphome/core/hal.h"

#ifdef IOHOME_BRIDLE

#include "radio_bridle_phy.h"

namespace esphome::home_io_control::bridle {

// === Air interface ===

/// Start-of-frame delimiter of io-homecontrol on 802.15.4 (the standard's is 0xA7). Set through
/// the `sfd` param, so any radio with cap `custom_sfd` can receive it.
static constexpr uint8_t IO2G4_SFD = 0x56;
/// PSDU header in front of every io frame: identifies the payload as io-homecontrol.
static constexpr uint8_t IO2G4_PSDU_HEADER[3] = {0x01, 0x69, 0x6F};
/// Bytes of the PSDU header.
static constexpr size_t IO2G4_PSDU_HEADER_SIZE = sizeof(IO2G4_PSDU_HEADER);
/// Bytes of the 802.15.4 frame check sequence the radio appends and checks.
static constexpr size_t IO2G4_FCS_SIZE = 2;

/// The three channels of a 2.4 GHz io network: 802.15.4 channels 15, 20 and 25. Captured
/// exchanges ran on 15 and 20 and receivers scan all three, so no channel has a special role.
static constexpr uint32_t IO2G4_CH1_HZ = 2425000000;
static constexpr uint32_t IO2G4_CH2_HZ = 2450000000;  ///< Also the hub's TX channel (FREQ_CH2's counterpart).
static constexpr uint32_t IO2G4_CH3_HZ = 2475000000;

// === Airtime (IEEE 802.15.4 O-QPSK, 250 kb/s) ===

/// One byte on air: 8 bits at 250 kb/s.
static constexpr uint32_t IO2G4_BYTE_US = 32;
/// Bytes before the PSDU: 4 of preamble, 1 of SFD, 1 of PHR (length).
static constexpr size_t IO2G4_PHY_OVERHEAD_BYTES = 6;

// === Wake-up strobes (spec §5; measured in the corpus's 2.4 GHz captures) ===

// The long train is measured in `velux_klf200_probe_status_poll_timed_2g4` (a KLF 200) and
// `velux_klr200_probe_status_poll_timed_2g4` (a KLR 200 remote), both stamped at 1 µs.

/// Frames in the long train that wakes a duty-cycled receiver: counters 509 … 0, 510 ms in all. Both
/// controllers' trains have this many frames, and each started at counter 509.
static constexpr uint16_t IO2G4_LONG_STROBE_COUNT = 510;
/// Frames in the short train that finds a receiver scanning channels.
static constexpr uint16_t IO2G4_SHORT_STROBE_COUNT = 8;
/// Start-to-start distance of two strobe frames: 1000 µs (999 to 1001 µs in the captures), which is also
/// the counter's unit: a frame's counter is the milliseconds left before the main frame, minus one. The
/// period is held in ns because the spec's field is, and a period that is no whole number of µs would
/// drift on a µs grid. The short train's period is taken to be the same; no capture times it.
static constexpr uint32_t IO2G4_STROBE_PERIOD_NS = 1000000;
/// From the last strobe frame's start to the main frame's start: 1036 µs for the KLF 200 (1036 to
/// 1038 µs over 13 trains), 1025 µs for the KLR 200 (one train). The KLF 200's value is taken, as the
/// better-observed one.
static constexpr uint32_t IO2G4_STROBE_MAIN_GAP_NS = 1036000;
/// The strobe frame before its counter is written: `00 <counter, 2 bytes>`.
static constexpr uint8_t IO2G4_STROBE_TEMPLATE[3] = {0x00, 0x00, 0x00};

// === Idle scan ===

/// Dwell of the idle scan, per channel. A frame is caught only if the receiver has listened for its
/// preamble and SFD (about 128 µs of the 160) inside the dwell, less the 40 µs the radio takes to ramp
/// up; the wake frames of a short train come every 1000 µs. With 1000 µs, a window can fall where no
/// frame's sync fits, and the next windows (one cycle of three dwells, 3000 µs, later: that shifts
/// against the train by 72 µs at a spacing of 976.6 µs and not at all at 1000 µs) are then just as bad:
/// measured on the nRF52840 with the frames 1000 µs apart, 140 of 900 short trains (16 percent) were not
/// heard at all at 1000 µs, whatever the signal level (2.6 to 3.3 percent with the frames 976.6 µs apart).
/// A window longer than the strobe period plus the sync lead (about 1140 µs)
/// always holds a frame, and the train (8 ms before its main frame) spans a whole window plus a cycle:
/// at 1250 µs and at 1500 µs none of 900 trains was lost. 1500 µs leaves margin for a longer sync at a weak
/// signal. (A KLF 200 hops at about 1 ms; that is not what a co-processor with this receiver needs.)
static constexpr uint16_t IO2G4_SCAN_DWELL_US = 1500;
/// Hold after a frame: 8 ms. A scan that caught one wake frame stays for the rest of the train and the
/// frame, and for what follows: a device's reply starts about 2.5 ms after the request ends (3266 µs
/// from SFD to SFD in `velux_klf200_probe_status_poll_timed_2g4`, 2466 µs after the request's last
/// bit), and the pause between two frames of one exchange can be longer. A hold only delays the next
/// hop on a channel that is carrying traffic.
static constexpr uint16_t IO2G4_SCAN_HOLD_US = 8000;

/// @brief 2.4 GHz io-homecontrol over an 802.15.4 radio.
class Io2g4Phy : public BridlePhy {
 public:
  [[nodiscard]] PhyProfile profile() const override { return PhyProfile::IEEE802154_OQPSK_250K; }
  void write_params(TlvWriter &params, const TuningConfig &tuning) const override;
  /// An `ieee802154_oqpsk_250k` radio covering 2425 to 2475 MHz with caps `strobe` (the wake-up trains) and
  /// `rx_scan` (the idle scan a host cannot drive itself).
  [[nodiscard]] bool radio_fits(const RadioDescriptor &radio) const override;

  /// `FREQ_CH1` … `FREQ_CH3` → 2425 / 2450 / 2475 MHz. An unknown value maps to 2450 MHz.
  [[nodiscard]] uint32_t air_freq_hz(uint32_t channel_hz) const override;
  [[nodiscard]] uint32_t channel_hz(uint32_t air_hz) const override;

  /// Prepends the header `01 69 6F`; the radio appends the FCS.
  [[nodiscard]] size_t wrap(const uint8_t *frame, uint8_t len, uint8_t *out, size_t cap) const override;
  [[nodiscard]] size_t wrapped_size(uint8_t frame_len) const override { return IO2G4_PSDU_HEADER_SIZE + frame_len; }
  /// Checks the header and strips it and the FCS. A payload whose radio FCS check failed is
  /// `BAD_CRC`; a header mismatch returns the 3 bytes so the driver can log a manufacturer variant.
  [[nodiscard]] UnwrapResult unwrap(const uint8_t *payload, size_t len, CrcState crc, uint8_t *out,
                                    size_t cap) const override;
  /// `NONE`: no strobe. `SHORT`: 8 frames. `LONG`: 510 frames. The preamble stays the standard
  /// one, so `preamble_len` plays no part.
  [[nodiscard]] TxShape tx_shape(TxWake wake, uint16_t preamble_len) const override;
  [[nodiscard]] uint32_t tx_air_time_us(const TxShape &shape, size_t wrapped_len) const override;

  [[nodiscard]] ScanSpec idle_scan() const override;
  [[nodiscard]] FrameStage frame_arriving_stage() const override { return FrameStage::SYNC; }

  [[nodiscard]] uint16_t hop_dwell_ms(const TuningConfig &tuning) const override { return tuning.io2g4_hop_dwell_ms; }
  [[nodiscard]] int16_t cca_threshold_dbm(const TuningConfig &tuning) const override {
    return tuning.io2g4_lbt_rssi_threshold_dbm;
  }
  /// True: every captured 2.4 GHz request is a start frame with the low-power flag, and a receiver
  /// at rest ignores a train without it.
  [[nodiscard]] bool low_power_by_default() const override { return true; }
  /// `tuning.io2g4_exchange_start_response_wait_ms`: a device replies within a few milliseconds.
  [[nodiscard]] uint32_t start_response_wait_ms(const TuningConfig &tuning) const override {
    return tuning.io2g4_exchange_start_response_wait_ms;
  }
  /// `tuning.io2g4_exchange_retry_gap_ms`.
  [[nodiscard]] uint32_t retry_gap_ms(const TuningConfig &tuning) const override {
    return tuning.io2g4_exchange_retry_gap_ms;
  }
  /// True: a device answers on the request's channel, and the channels fade independently.
  [[nodiscard]] bool retries_rotate_channel() const override { return true; }
};

/// The one 2.4 GHz profile; profiles are stateless, so drivers share it.
[[nodiscard]] const Io2g4Phy &io2g4_phy();

}  // namespace esphome::home_io_control::bridle

#endif  // IOHOME_BRIDLE
