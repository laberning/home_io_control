// GENERATED from bridle.ksy by bridle gen-cpp-protocol -- DO NOT EDIT.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bridle {

/// Protocol version this header was generated for (`-protocol-version` 0.9).
static constexpr uint8_t PROTO_MAJOR = 0;
static constexpr uint8_t PROTO_MINOR = 9;

/// The largest decoded message: msg_type, seq, body and CRC.
static constexpr size_t MAX_DECODED_BYTES = 255;

/// A command: host to co-processor (0x01-0x7F).
constexpr bool is_command(uint8_t type) { return type >= 0x01 && type <= 0x7F; }
/// A reply (0x80-0xBF): its body starts with `cmd_seq`.
constexpr bool is_reply(uint8_t type) { return type >= 0x80 && type <= 0xBF; }
/// An event (0xC0-0xFF): no `cmd_seq`.
constexpr bool is_event(uint8_t type) { return type >= 0xC0; }
/// Command types the spec retired: never reused, answered `unknown_type` like any undefined one.
static constexpr uint8_t RETIRED_MSG_TYPES[] = {0x0B};
/// A defined, non-retired command; anything else in the command range is `unknown_type`.
constexpr bool is_known_command(uint8_t type) {
  switch (type) {
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x09:
    case 0x0A:
      return true;
    default:
      return false;
  }
}

/// The specific reply a command draws besides `error`; 0 for a type that is not a command.
constexpr uint8_t reply_type_of(uint8_t command) {
  switch (command) {
    case 0x01:  // hello
      return 0x80;
    case 0x02:  // set_params
      return 0x81;
    case 0x03:  // rx_on
      return 0x82;
    case 0x04:  // rx_scan
      return 0x82;
    case 0x05:  // idle
      return 0x82;
    case 0x06:  // tx
      return 0x83;
    case 0x07:  // get_rssi
      return 0x84;
    case 0x08:  // get_stats
      return 0x85;
    case 0x09:  // soft_reset
      return 0x82;
    case 0x0A:  // enter_bootloader
      return 0x82;
    default:
      return 0;
  }
}

/// Index of each command field in its .ksy `seq` (0-based, every entry counted): the
/// `out_of_range` detail for a command other than set_params (README.md section 2.2).
namespace field {
namespace rx_on {
static constexpr uint8_t RADIO = 0;
static constexpr uint8_t FREQ_HZ = 1;
static constexpr uint8_t FORCE = 2;
}  // namespace rx_on
namespace rx_scan {
static constexpr uint8_t RADIO = 0;
static constexpr uint8_t DWELL_US = 1;
static constexpr uint8_t HOLD_US = 2;
static constexpr uint8_t NUM_FREQS_HZ = 3;
static constexpr uint8_t FREQS_HZ = 4;
}  // namespace rx_scan
namespace idle {
static constexpr uint8_t RADIO = 0;
}  // namespace idle
namespace tx {
static constexpr uint8_t RADIO = 0;
static constexpr uint8_t FREQ_HZ = 1;
static constexpr uint8_t THEN_RX = 2;
static constexpr uint8_t HAS_STROBE = 3;
static constexpr uint8_t AT_TIME = 4;
static constexpr uint8_t CCA = 5;
static constexpr uint8_t PREAMBLE_BYTES = 7;
static constexpr uint8_t T_MAIN_START_US = 8;
static constexpr uint8_t CCA_THRESHOLD_DBM = 9;
static constexpr uint8_t STROBE = 10;
static constexpr uint8_t LEN_PAYLOAD = 11;
static constexpr uint8_t PAYLOAD = 12;
}  // namespace tx
namespace get_rssi {
static constexpr uint8_t RADIO = 0;
}  // namespace get_rssi
namespace get_stats {
static constexpr uint8_t RESET_AFTER_READ = 0;
}  // namespace get_stats
namespace enter_bootloader {
static constexpr uint8_t MAGIC = 0;
}  // namespace enter_bootloader
}  // namespace field

/// What every radio with profile `ieee802154_oqpsk_250k` does (the .ksy keys `-then-rx-turnaround-us`, `-preamble`).
namespace profile_ieee802154_oqpsk_250k {
static constexpr uint32_t THEN_RX_TURNAROUND_US = 300;  ///< `then_rx` bound
static constexpr uint32_t RX_ON_LATENCY_US = 1000;  ///< `rx_on` to listening
static constexpr uint32_t TX_LATENCY_US = 1000;  ///< `tx` to main frame
static constexpr uint32_t TX_CCA_LATENCY_US = 1000;  ///< `tx` with `cca`
static constexpr uint32_t GET_RSSI_REPLY_US = 650;  ///< `get_rssi`: command's last byte in to reply queued
static constexpr uint16_t STANDARD_PREAMBLE_BYTES = 4;
static constexpr bool PER_PACKET_PREAMBLE = false;
}  // namespace profile_ieee802154_oqpsk_250k

/// What every radio with profile `gfsk_raw` does (the .ksy keys `-then-rx-turnaround-us`, `-preamble`).
namespace profile_gfsk_raw {
static constexpr uint32_t THEN_RX_TURNAROUND_US = 1000;  ///< `then_rx` bound
static constexpr uint32_t RX_ON_LATENCY_US = 3000;  ///< `rx_on` to listening
static constexpr uint32_t TX_LATENCY_US = 1000;  ///< `tx` to main frame
static constexpr uint32_t TX_CCA_LATENCY_US = 3000;  ///< `tx` with `cca`
static constexpr uint32_t GET_RSSI_REPLY_US = 1100;  ///< `get_rssi`: command's last byte in to reply queued
static constexpr uint16_t STANDARD_PREAMBLE_BYTES = 8;
static constexpr bool PER_PACKET_PREAMBLE = true;
}  // namespace profile_gfsk_raw

// The always-present bytes of each reply and event body (a payload or text follows).
static constexpr size_t READY_BODY_FIXED_BYTES = 9;
static constexpr size_t PARAMS_ACK_BODY_FIXED_BYTES = 2;
static constexpr size_t ACK_BODY_FIXED_BYTES = 1;
static constexpr size_t TX_DONE_BODY_FIXED_BYTES = 14;
static constexpr size_t RSSI_BODY_FIXED_BYTES = 3;
static constexpr size_t STATS_BODY_FIXED_BYTES = 1;
static constexpr size_t ERROR_BODY_FIXED_BYTES = 4;
static constexpr size_t RX_START_BODY_FIXED_BYTES = 10;
static constexpr size_t RX_FRAME_BODY_FIXED_BYTES = 13;
static constexpr size_t RX_ABORT_BODY_FIXED_BYTES = 10;
static constexpr size_t RX_OVERFLOW_BODY_FIXED_BYTES = 2;
static constexpr size_t LOG_BODY_FIXED_BYTES = 1;
/// Bytes of a `radio` info TLV value (.ksy type `radio_descriptor`).
static constexpr size_t RADIO_DESCRIPTOR_BYTES = 24;
/// Longest `fw_version` info string the spec allows.
static constexpr size_t INFO_FW_VERSION_MAX_BYTES = 32;
/// Longest `board` info string the spec allows.
static constexpr size_t INFO_BOARD_MAX_BYTES = 32;

/// .ksy enum `msg_type`.
enum class MsgType : uint8_t {
  HELLO = 0x01,  ///< Ask for `ready`
  SET_PARAMS = 0x02,  ///< Apply settings to one radio, or in device scope with radio `0xFF`, all or nothing (TLV list, §4)
  RX_ON = 0x03,  ///< Receive on a radio on a frequency in Hz; a different frequency is a retune
  RX_SCAN = 0x04,  ///< Receive on a radio while cycling through a frequency list (radio cap `rx_scan`)
  IDLE = 0x05,  ///< Stop a radio
  TX = 0x06,  ///< Transmit a payload, optionally after a strobe or with a given preamble length (§5)
  GET_RSSI = 0x07,  ///< A fresh reading of the signal level on the frequency a receiving radio listens on, taken after the command arrives (§3.1); `error(not_receiving)` while it is idle, `error(radio_busy)` while it transmits or a command waits behind a frame or a transmission (§2.2)
  GET_STATS = 0x08,  ///< Read the device-scope counters
  SOFT_RESET = 0x09,  ///< Reboot the co-processor; it answers `ack` first, then the boot `ready` follows. A boot `ready` with `reset_cause` `soft` confirms a `soft_reset` whether or not its `ack` was seen
  ENTER_BOOTLOADER = 0x0A,  ///< Reboot into the bootloader (device cap `bootloader`); it answers `ack` first. The body is a fixed 4-byte magic, so a corrupted command can't trigger it.
  READY = 0x80,  ///< Identity (`mcu`), protocol version, device capabilities, info TLVs including one `radio` descriptor per radio. Sent unsolicited after every boot (`cmd_seq` 0), and in reply to `hello`. `chip` values are registered in the `.ksy` enum, append-only; a new port requests one through a pull request to this package.
  PARAMS_ACK = 0x81,  ///< The settings as applied (e.g. a clamped TX power, a rounded RX bandwidth)
  ACK = 0x82,  ///< The command was accepted and has nothing to report
  TX_DONE = 0x83,  ///< A `tx` finished: main-frame start/end time, strobe frames sent and late. A `tx` whose clear-channel check failed gets `error(channel_busy)` instead.
  RSSI = 0x84,  ///< Reply to `get_rssi`
  STATS = 0x85,  ///< Device-scope counters. They are append-only; a reader takes as many as the body holds.
  ERROR = 0x88,  ///< `cmd_seq`, code, detail
  RX_START = 0xC0,  ///< A frame is arriving: `stage` `sync` (sync word / SFD seen) or `preamble` (preamble detected). Only if enabled in `event_mask`.
  RX_FRAME = 0xC1,  ///< A received frame: radio, frequency, RSSI, LQI, FCS state, payload (§4 per profile). Frames with a bad FCS are sent only if enabled in `event_mask`.
  RX_ABORT = 0xC2,  ///< A reception started but did not complete
  RX_OVERFLOW = 0xC3,  ///< Reception events (`rx_start`, `rx_frame`, `rx_abort`) dropped because the host did not drain the event queue; `dropped` says how many (§6)
  LOG = 0xC4,  ///< Debug text with a level (only if enabled in `event_mask`). Diagnostic only: the host never acts on its content. Lowest priority, dropped first (§6).
};

/// .ksy enum `param_id`.
enum class ParamId : uint8_t {
  SFD = 0x01,  ///< u8. Required.
  TX_POWER_DBM = 0x02,  ///< s8; clamped to the radio's range, and the applied value is echoed
  EVENT_MASK = 0x03,  ///< Bits: 0 `rx_start`, 1 `rx_frame` with a bad FCS, 2 `rx_abort`, 3 `log` (release builds send only levels `error` and `warn`, §3.2). After every boot all bits are 0: none of these events is sent until the host enables it.
  PHY_PROFILE = 0x04,  ///< u8, one of the radio's profiles (table below). The first TLV of a `set_params` that carries it: it resets the radio's profile params to their defaults.
  BITRATE_BPS = 0x07,  ///< u32. Required.
  FDEV_HZ = 0x08,  ///< u32, frequency deviation. Required.
  RX_BW_HZ = 0x09,  ///< u32, double-sided; the radio applies the nearest supported bandwidth not below it, and echoes that value. Choose at least `bitrate_bps` + 2 × `fdev_hz` plus the frequency error between the two ends.
  PULSE_SHAPE = 0x0A,  ///< u8, table below. Default `none`.
  SYNC_WORD = 0x0B,  ///< 1–8 bytes, sent and matched as given, first byte first. Required.
  RX_CAPTURE_BYTES = 0x0C,  ///< u8: bytes captured after the sync word, 1…`max_payload_bytes`. Required.
  PREAMBLE_DETECT_BITS = 0x0D,  ///< u8: preamble bits the radio needs before it looks for the sync word (8, 16, 24 or 32), fewer than the sync word has; a radio answers `out_of_range` for a value that is not. Default 8.
  SYNC_TIMEOUT_US = 0x0E,  ///< u32: after a preamble is detected, how long to wait for the sync word before `rx_abort(no_sync_after_preamble)`. 0 = the longest wait the radio supports, at least 1 s, so longer than any preamble it sends (a chip that cannot see a preamble end has no better bound). Default 0.
  RX_MIN_PSDU = 0x0F,  ///< u8, 0…127 (a larger value is `out_of_range`): a received frame whose PHR length (the PSDU length including the FCS) is below this produces no `rx_start`, `rx_frame` or `rx_abort`, whatever `event_mask` says, and is counted in `stats.rx_short`; it still counts as a frame for `rx_scan` (§3.1). Default 0: every frame is reported.
};

/// .ksy enum `info_id`.
enum class InfoId : uint8_t {
  FW_VERSION = 0x01,  ///< ASCII
  BOARD = 0x02,  ///< ASCII
  RADIO = 0x03,  ///< Radio descriptor, one TLV per radio (layout in the table above)
  EVENT_QUEUE_DEPTH = 0x04,  ///< u8, the largest number of event entries (`rx_start`, `rx_frame`, `rx_abort`, `log` and `rx_overflow`) the co-processor keeps; fewer are kept while replies wait unread, each reply waiting unread taking the place of one event, so that every accepted command has a slot for its reply whatever the host leaves unread (§6)
  RESET_CAUSE = 0x05,  ///< u8: 0 power-on, 1 pin, 2 watchdog, 3 soft, 4 brown-out, 5 link takeover
};

/// .ksy enum `phy_profile`.
enum class PhyProfile : uint8_t {
  IEEE802154_OQPSK_250K = 0x00,  ///< TX: PSDU without FCS (≤ 125 B). RX: PSDU including the received FCS.
  GFSK_RAW = 0x01,  ///< TX: the bytes to send after the sync word, as given. RX: exactly `rx_capture_bytes` bytes after the sync word, whatever their meaning.
};

/// .ksy enum `pulse_shape`.
enum class PulseShape : uint8_t {
  NONE = 0x00,
  GAUSSIAN_BT_0_3 = 0x01,
  GAUSSIAN_BT_0_5 = 0x02,
  GAUSSIAN_BT_0_7 = 0x03,
  GAUSSIAN_BT_1_0 = 0x04,
};

/// .ksy enum `rx_stage`.
enum class RxStage : uint8_t {
  SYNC = 0x00,
  PREAMBLE = 0x01,
};

/// .ksy enum `fcs_state`.
enum class FcsState : uint8_t {
  NOT_CHECKED = 0x00,
  OK = 0x01,
  BAD = 0x02,
};

/// .ksy enum `chip`.
enum class Chip : uint16_t {
  NRF52840 = 0x01,
  NRF52833 = 0x02,
  NRF5340 = 0x03,
  EFR32MG21 = 0x10,
  EFR32MG24 = 0x11,
  ESP32H2 = 0x20,
  SX1262 = 0x100,
};

/// .ksy enum `rx_abort_reason`.
enum class RxAbortReason : uint8_t {
  NO_END_AFTER_SYNC = 0x01,  ///< A frame whose sync word or SFD was seen but whose end was not, or a frame the radio could not deliver intact
  BAD_PHR = 0x02,
  TX_STARTED = 0x03,
  FORCED_RETUNE = 0x04,
  NO_SYNC_AFTER_PREAMBLE = 0x05,
};

/// .ksy enum `error_code`.
enum class ErrorCode : uint8_t {
  UNKNOWN_TYPE = 0x01,  ///< —
  MALFORMED = 0x02,  ///< — (a well-framed command whose body does not match its type's layout, §2.2)
  OUT_OF_RANGE = 0x03,  ///< The TLV id for `set_params`, otherwise the field index (§2.2)
  RADIO_BUSY = 0x04,  ///< — (the radio is transmitting and the command would change or use it: a second `tx`, a radio-scope `set_params`; or `get_rssi` during a transmission or while a command waits behind a frame or a transmission, §2.2)
  NOT_CONFIGURED = 0x05,  ///< — (`tx`/`rx_on`/`rx_scan`/`get_rssi` before the radio's profile and its required params were set)
  UNSUPPORTED = 0x06,  ///< The TLV id for `set_params`, otherwise the bit of the missing capability
  RADIO_FAULT = 0x07,  ///< Chip-specific code (also: a `get_rssi` the radio cannot take a reading for, §3.1). The radio is idle afterwards, whether the error is unsolicited or answers a command.
  PAYLOAD_TOO_LONG = 0x08,  ///< — (longer than the radio's `max_payload_bytes`)
  NO_SUCH_RADIO = 0x09,  ///< The radio index
  LINK_IN_USE = 0x0A,  ///< — (a command on a link other than the host link, §2.1)
  CHANNEL_BUSY = 0x0B,  ///< Measured level in dBm, as a signed 16-bit value (`tx` with `cca`; nothing was sent)
  NOT_RECEIVING = 0x0C,  ///< — (`get_rssi` on a configured radio that is neither receiving nor transmitting)
};

/// .ksy enum `reset_cause`.
enum class ResetCause : uint8_t {
  POWER_ON = 0x00,
  PIN = 0x01,
  WATCHDOG = 0x02,
  SOFT = 0x03,
  BROWN_OUT = 0x04,
  LINK_TAKEOVER = 0x05,
};

/// .ksy enum `log_level`.
enum class LogLevel : uint8_t {
  ERROR = 0x01,
  WARN = 0x02,
  INFO = 0x03,
  DEBUG = 0x04,
};

/// Bit masks of .ksy `event_mask` (8 bits; bits not listed are reserved and sent as 0).
namespace event_mask {
static constexpr uint8_t RX_START = uint8_t(1) << 0;
static constexpr uint8_t RX_FCS_BAD = uint8_t(1) << 1;
static constexpr uint8_t RX_ABORT = uint8_t(1) << 2;
static constexpr uint8_t LOG = uint8_t(1) << 3;
}  // namespace event_mask

/// Bit masks of .ksy `rx_on_flags` (8 bits; bits not listed are reserved and sent as 0).
namespace rx_on_flags {
static constexpr uint8_t FORCE = uint8_t(1) << 0;  ///< Retune immediately, aborting a reception in progress. Without it a retune waits for the current frame.
}  // namespace rx_on_flags

/// Bit masks of .ksy `tx_flags` (8 bits; bits not listed are reserved and sent as 0).
namespace tx_flags {
static constexpr uint8_t THEN_RX = uint8_t(1) << 0;  ///< After the main frame, return to RX on the same frequency without host involvement.
static constexpr uint8_t HAS_STROBE = uint8_t(1) << 1;
static constexpr uint8_t AT_TIME = uint8_t(1) << 2;  ///< Requires radio cap at_time.
static constexpr uint8_t CCA = uint8_t(1) << 3;  ///< Check the channel before sending (radio cap cca); see cca_threshold_dbm.
}  // namespace tx_flags

/// Bit masks of .ksy `get_stats` (8 bits; bits not listed are reserved and sent as 0).
namespace get_stats {
static constexpr uint8_t RESET_AFTER_READ = uint8_t(1) << 0;
}  // namespace get_stats

/// Bit masks of .ksy `device_caps` (32 bits; bits not listed are reserved and sent as 0).
namespace device_caps {
static constexpr uint32_t BOOTLOADER = uint32_t(1) << 0;  ///< `enter_bootloader` supported
static constexpr uint32_t CONCURRENT_RADIOS = uint32_t(1) << 1;  ///< Radios can receive at the same time; otherwise a command to one radio idles the others
static constexpr uint32_t USB_CDC = uint32_t(1) << 2;  ///< The protocol is also answered on a USB CDC-ACM port (§2.1)
}  // namespace device_caps

/// Bit masks of .ksy `radio_caps` (32 bits; bits not listed are reserved and sent as 0).
namespace radio_caps {
static constexpr uint32_t STROBE = uint32_t(1) << 0;  ///< `tx` supports a strobe
static constexpr uint32_t AT_TIME = uint32_t(1) << 1;  ///< `tx.at_time` supported
static constexpr uint32_t RX_SCAN = uint32_t(1) << 2;  ///< `rx_scan` supported
static constexpr uint32_t CCA = uint32_t(1) << 3;  ///< `tx.cca` supported: a clear-channel check on the target frequency right before sending
}  // namespace radio_caps

/// Bit masks of .ksy `phy_profiles` (16 bits; bits not listed are reserved and sent as 0).
namespace phy_profiles {
static constexpr uint16_t IEEE802154_OQPSK_250K = uint16_t(1) << 0;
static constexpr uint16_t GFSK_RAW = uint16_t(1) << 1;
}  // namespace phy_profiles

}  // namespace bridle
