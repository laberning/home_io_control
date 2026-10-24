/// @file
/// @brief The band-specific half of a Bridle radio: how io frames are dressed for the air.
/// @ingroup hioc_radio
///
/// `RadioBridle` speaks the Bridle protocol to a co-processor and knows nothing of the band it
/// carries. Everything that depends on the band lives behind this interface: which PHY profile
/// and params the co-processor's radio gets, how an io frame becomes a radio payload and back,
/// how a wake-up level becomes a preamble or a strobe, and what the idle scan looks like. A
/// profile is stateless and injected by reference, so one profile serves any driver, a future
/// native driver included. Compiled only under `IOHOME_BRIDLE`.

#pragma once

// IOHOME_BRIDLE is only visible after something pulls in esphome/core/defines.h (ESPHome codegen's
// cg.add_define() lands there, not as a compiler -D flag): this include must run before the #ifdef,
// or the file silently compiles to nothing in a real build although the host tests (which pass
// -DIOHOME_BRIDLE) are fine.
#include "esphome/core/hal.h"

#ifdef IOHOME_BRIDLE

#include <cstddef>
#include <cstdint>

#include "proto_sizes.h"
#include "proto_timing.h"
#include "radio_bridle_codec.h"
#include "tuning_config.h"

namespace esphome::home_io_control::bridle {

/// How one frame is sent: the preamble length and the optional strobe of a `tx`.
struct TxShape {
  uint16_t preamble_bytes{0};  ///< The main frame's preamble; 0 = the profile's standard preamble.
  bool has_strobe{false};      ///< A timed burst of short frames precedes the main frame.
  StrobeSpec strobe{};         ///< Only with `has_strobe`.
};

/// What the co-processor's radio does while the hub is idle: cycle through a frequency list.
struct ScanSpec {
  uint16_t dwell_us{0};  ///< Time on each frequency.
  uint16_t hold_us{0};   ///< Stay on a frequency this long after a frame ends there.
  uint8_t num_freqs{0};
  uint32_t freqs_hz[MAX_SCAN_FREQS]{};
};

/// Result of `BridlePhy::unwrap`.
struct UnwrapResult {
  /// What became of the payload.
  enum class Status : uint8_t {
    OK,               ///< `frame_len` bytes of io frame were written.
    TOO_SHORT,        ///< Shorter than the smallest frame the profile can carry.
    BAD_CRC,          ///< The radio (or the profile) found the payload's checksum wrong.
    HEADER_MISMATCH,  ///< The profile's fixed header differs; `header` holds the bytes seen.
    TOO_LONG,         ///< The io frame does not fit the output buffer.
  };
  Status status{Status::TOO_SHORT};
  uint8_t frame_len{0};
  uint8_t header[3]{};  ///< The leading bytes seen on a `Status::HEADER_MISMATCH`.
};

/// @brief One band's rules for a Bridle radio.
class BridlePhy {
 public:
  virtual ~BridlePhy() = default;

  // === What the driver asks the co-processor for ===

  /// PHY profile the radio must support and be set to.
  [[nodiscard]] virtual PhyProfile profile() const = 0;
  /// Append the profile's params (everything after `phy_profile`, before the TX power).
  /// @param tuning The runtime tuning: a profile takes from it what the co-processor's radio is tuned by
  ///        (the receiver bandwidth of a `gfsk_raw` radio).
  virtual void write_params(TlvWriter &params, const TuningConfig &tuning) const = 0;
  /// Whether a radio can carry this band: it offers the profile, covers every channel, has the
  /// capabilities the band needs, takes a payload that holds the longest io frame, and (on a profile
  /// with a per-packet preamble) a preamble as long as the band's wake-up asks for.
  [[nodiscard]] virtual bool radio_fits(const RadioDescriptor &radio) const = 0;

  // === Frequencies ===

  /// Map the hub's logical channel (`FREQ_CH1` … `FREQ_CH3`) to the frequency on air.
  [[nodiscard]] virtual uint32_t air_freq_hz(uint32_t channel_hz) const = 0;
  /// Map a frequency on air back to the hub's logical channel; 0 when it is none of them.
  [[nodiscard]] virtual uint32_t channel_hz(uint32_t air_hz) const = 0;

  // === Frames ===

  /// Dress an io frame (no io CRC) as a radio payload.
  /// @return Payload length, or 0 when `cap` is too small.
  [[nodiscard]] virtual size_t wrap(const uint8_t *frame, uint8_t len, uint8_t *out, size_t cap) const = 0;
  /// Payload length `wrap` produces for a `frame_len`-byte io frame.
  [[nodiscard]] virtual size_t wrapped_size(uint8_t frame_len) const = 0;
  /// Undress a received payload into an io frame, checking what the radio did not.
  /// @param crc What the radio said about its checksum.
  [[nodiscard]] virtual UnwrapResult unwrap(const uint8_t *payload, size_t len, CrcState crc, uint8_t *out,
                                            size_t cap) const = 0;
  /// Preamble and strobe that give `wake` the reach it asks for.
  /// @param preamble_len The preamble the hub chose, for profiles whose wake-up is a preamble.
  [[nodiscard]] virtual TxShape tx_shape(TxWake wake, uint16_t preamble_len) const = 0;
  /// Time the radio spends transmitting a `wrapped_len`-byte payload with `shape`, in µs.
  [[nodiscard]] virtual uint32_t tx_air_time_us(const TxShape &shape, size_t wrapped_len) const = 0;

  // === Listening ===

  /// The scan the co-processor runs while the hub is idle.
  [[nodiscard]] virtual ScanSpec idle_scan() const = 0;
  /// Which `frame_start` stage means "a frame is arriving" on this profile.
  [[nodiscard]] virtual FrameStage frame_arriving_stage() const = 0;

  // === Band defaults the hub would otherwise take from a 868 MHz tuning field ===

  /// Per-channel dwell for a rotating listen (see RadioDriver::hop_dwell_ms()).
  [[nodiscard]] virtual uint16_t hop_dwell_ms(const TuningConfig &tuning) const = 0;
  /// Clear-channel threshold (see RadioDriver::lbt_threshold_dbm()).
  [[nodiscard]] virtual int16_t cca_threshold_dbm(const TuningConfig &tuning) const = 0;
  /// Whether the band's devices are duty-cycled receivers unless configured otherwise (see
  /// RadioDriver::low_power_by_default()).
  [[nodiscard]] virtual bool low_power_by_default() const { return false; }
  /// Reply window for a start frame (see RadioDriver::start_response_wait_ms()).
  [[nodiscard]] virtual uint32_t start_response_wait_ms(const TuningConfig &tuning) const {
    return tuning.exchange_start_response_wait_ms;
  }
  /// Pause between the tries of an exchange (see RadioDriver::retry_gap_ms()).
  [[nodiscard]] virtual uint32_t retry_gap_ms(const TuningConfig & /*tuning*/) const { return EXCHANGE_RETRY_DELAY_MS; }
  /// Whether a retry moves to another channel (see RadioDriver::retries_rotate_channel()).
  [[nodiscard]] virtual bool retries_rotate_channel() const { return false; }
  /// Preamble for continuation frames (see RadioDriver::response_preamble()).
  [[nodiscard]] virtual uint16_t response_preamble(const TuningConfig & /*tuning*/) const { return SHORT_PREAMBLE; }
  /// Preamble for a directed start frame (see RadioDriver::default_start_preamble()).
  [[nodiscard]] virtual uint16_t default_start_preamble() const { return NORMAL_START_PREAMBLE; }

 protected:
  /// The checks every band shares: the radio offers `profile()` and its payload holds the longest io
  /// frame with the profile's 3-byte header.
  [[nodiscard]] bool offers_profile_and_payload_(const RadioDescriptor &radio) const {
    uint32_t const profile_bit = 1U << static_cast<unsigned>(this->profile());
    return (radio.profiles & profile_bit) != 0 && radio.max_payload >= 3 + FRAME_MAX_DECLARED_SIZE;
  }
};

}  // namespace esphome::home_io_control::bridle

#endif  // IOHOME_BRIDLE
