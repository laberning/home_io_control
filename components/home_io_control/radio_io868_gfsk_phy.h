/// @file
/// @brief The 868 MHz io-homecontrol PHY profile on a `gfsk_raw` radio: io frames UART-coded by the host.
/// @ingroup hioc_radio
///
/// A Bridle `gfsk_raw` radio has no framing: it sends a preamble, a sync word and the bytes it is given,
/// and receives exactly `rx_len` bytes behind the sync word. 868 MHz io-homecontrol is that waveform
/// with the host's own coding on top: every byte of the frame and of its CRC in a 10-bit UART cell,
/// behind the sync word `57 FD 99`. This is the same coding the software PHY (`radio_soft_phy.h`)
/// applies for the SX1262 and LR1121 drivers, so a frame from this profile is on air what theirs is.
/// A long preamble wakes a duty-cycled receiver, which is why the profile needs `per_packet_preamble`.
/// This is the sibling of `radio_io2g4_phy.h`: no chip, no transport, only the band, which here is the
/// 868 MHz band every other driver has (`IO_868_BAND`). Compiled only under `IOHOME_BRIDLE`.

#pragma once

// IOHOME_BRIDLE is only visible after something pulls in esphome/core/defines.h (ESPHome codegen's
// cg.add_define() lands there, not as a compiler -D flag): this include must run before the #ifdef,
// or the file silently compiles to nothing in a real build although the host tests (which pass
// -DIOHOME_BRIDLE) are fine.
#include "esphome/core/hal.h"

#ifdef IOHOME_BRIDLE

#include "radio_bridle_phy.h"
#include "radio_soft_phy.h"

namespace esphome::home_io_control::bridle {

// === Modem (spec §5 example, vector `set_params_io868`) ===

/// Line rate of 868 MHz io-homecontrol, in bit/s: the one every 868 MHz driver programs.
static constexpr uint32_t IO868_BITRATE_BPS = IO868_LINE_RATE_BPS;
/// Frequency deviation, in Hz.
static constexpr uint32_t IO868_FDEV_HZ = 19200;
/// Sync word, first byte first: the UART-coded `55 FF 33` read six bits into its first cell, which the
/// SX1262 and LR1121 drivers program too (see `radio_sx1262.cpp`, "Sync word").
static constexpr uint8_t IO868_SYNC_WORD[IO868_SYNC_WORD_BYTES] = {0x57, 0xFD, 0x99};
/// Raw bytes the radio captures behind the sync word: the software PHY's probe length, 48 bytes
/// (a 12-byte frame and its CRC take 20; the longest frame, 49 bytes of coded cells, still fits).
static constexpr uint8_t IO868_RX_LEN = 48;
/// Preamble bits the radio needs before it looks for the sync word. 16, as the LR1121 driver uses (validated
/// against real devices): at 8 an SX1262 on a quiet bench reported about 3.8 false preambles a second and a
/// receiver in a false preamble missed frames; at 16 it reported none in 30 s and received every test frame.
/// Spec §4 allows 8 or 16 here (fewer bits than the 24-bit sync word).
static constexpr uint8_t IO868_PREAMBLE_DETECT_BITS = 16;
/// How long the radio waits for the sync word after a preamble, in µs. Longer than the 213 ms of a 1024-byte
/// wake-up preamble, so a genuine wake-up is one reception: a receiver that gives up after 20 ms, hops on in its
/// idle scan and misses the sync word received 38 of 60 such frames, one that waits 250 ms 59 of 60 (SX1262 bench).
/// The price is the receiver parked on each false preamble (2 to 4 a minute at 16-bit detection) for a quarter of a
/// second.
static constexpr uint32_t IO868_SYNC_TIMEOUT_US = 250000;

// === Preamble ===

/// Bit periods one preamble byte occupies on air: the radio sends a raw byte stream, 8 bits a byte, like the
/// software PHY (`SOFT_PHY_PREAMBLE_BITS_PER_BYTE`).
static constexpr uint8_t IO868_PREAMBLE_BITS_PER_BYTE = SOFT_PHY_PREAMBLE_BITS_PER_BYTE;
/// The preamble capacity the Bridle specification requires of a radio that carries this band: 4096 bytes. A radio
/// with at least that is accepted; whether the configured wake-up fits that radio's own capacity is checked against
/// the radio (`RadioBridle::accepts_tuning()`), never by a larger requirement here.
static constexpr uint16_t IO868_MIN_PREAMBLE_CAPACITY_BYTES = 4096;

// === Idle scan ===

/// Dwell of the idle scan, per channel, in µs: the hub's own hop time.
static constexpr uint16_t IO868_SCAN_DWELL_US = HOP_TIME_US;

/// @brief 868 MHz io-homecontrol over a `gfsk_raw` radio.
class Io868GfskPhy : public BridlePhy {
 public:
  [[nodiscard]] PhyProfile profile() const override { return PhyProfile::GFSK_RAW; }
  /// The modem of vector `set_params_io868`: bit rate, deviation, the receiver bandwidth of
  /// `tuning.sx1262_rx_bandwidth` (the far end is an SX1262 whose bandwidth matters the same way),
  /// Gaussian BT 1.0, the sync word, `rx_len`, the detector, the sync timeout.
  void write_params(TlvWriter &params, const TuningConfig &tuning) const override;
  /// A `gfsk_raw` radio covering 868.25 to 869.85 MHz with cap `rx_scan` (the idle scan a host cannot drive
  /// itself) and a preamble of at least `IO868_MIN_PREAMBLE_CAPACITY_BYTES` bytes (what the specification requires for
  /// this band).
  [[nodiscard]] bool radio_fits(const RadioDescriptor &radio) const override;

  /// `IO_868_BAND`: the channels are the frequencies on air.
  [[nodiscard]] const IoBand &band() const override { return IO_868_BAND; }

  /// Appends the io CRC to the frame and UART-codes the whole, exactly as `SoftPhyDriverBase::send_packet()`
  /// does before it writes to the chip.
  [[nodiscard]] size_t wrap(const uint8_t *frame, uint8_t len, uint8_t *out, size_t cap) const override;
  [[nodiscard]] size_t wrapped_size(uint8_t frame_len) const override {
    return soft_phy_raw_bytes_for_frame(frame_len);
  }
  /// Finds a CRC-valid io frame in the `rx_len` raw bytes at any bit alignment. Noise, or a frame of a
  /// command the hub does not know, is `BAD_CRC`; the radio itself checks nothing.
  [[nodiscard]] UnwrapResult unwrap(const uint8_t *payload, size_t len, CrcState crc, uint8_t *out,
                                    size_t cap) const override;
  /// The preamble the hub chose, and no strobe: a wake-up on this band is a long preamble.
  [[nodiscard]] TxShape tx_shape(TxWake wake, uint16_t preamble_len) const override;
  /// Preamble, sync word and the coded frame, each byte 8 bit times: what `io868_tx_air_time_us()` gives.
  [[nodiscard]] uint32_t tx_air_time_us(const TxShape &shape, size_t wrapped_len) const override;

  /// The three channels, `HOP_TIME_US` each, no hold: a frame holds the scan by itself.
  [[nodiscard]] ScanSpec idle_scan() const override;
  /// The radio reports a preamble before its sync word, so a frame is arriving from the preamble on.
  [[nodiscard]] FrameStage frame_arriving_stage() const override { return FrameStage::PREAMBLE; }

  [[nodiscard]] uint16_t hop_dwell_ms(const TuningConfig &tuning) const override {
    return tuning.sx1262_discovery_hop_slice_ms;
  }
  /// True: a wake-up is a long preamble.
  [[nodiscard]] bool wake_up_is_preamble() const override { return true; }
  /// `IO868_PREAMBLE_BITS_PER_BYTE`.
  [[nodiscard]] uint8_t preamble_bits_per_byte() const override { return IO868_PREAMBLE_BITS_PER_BYTE; }
};

/// The one 868 MHz `gfsk_raw` profile; profiles are stateless, so drivers share it.
[[nodiscard]] const Io868GfskPhy &io868_gfsk_phy();

}  // namespace esphome::home_io_control::bridle

#endif  // IOHOME_BRIDLE
