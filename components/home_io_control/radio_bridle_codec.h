/// @file
/// @brief Bridle wire codec: COBS framing, message structs and TLV lists.
/// @ingroup hioc_radio
///
/// Bridle is the protocol between the hub and a radio co-processor
/// ([specification](https://github.com/laberning/bridle/blob/bridle-v0.5.0/README.md)). This header is the byte-level
/// half of it: it turns messages into frames and frames into messages, and knows nothing of io-homecontrol, of any
/// radio or of the transport. Every code (message type, param id, error code, capability bit) comes from the generated
/// `radio_bridle_enums.h` (the generated header of a Bridle release, namespace `::bridle`); the link CRC is
/// `crc_ccitt()`.
///
/// Nothing here allocates. A decoded message refers to the buffer it was decoded from (payloads,
/// TLV lists and text are `ByteSpan` views), so it is valid only while that buffer is.
/// Compiled only under `IOHOME_BRIDLE`.

#pragma once

// IOHOME_BRIDLE is only visible after something pulls in esphome/core/defines.h (ESPHome codegen's
// cg.add_define() lands there, not as a compiler -D flag): this include must run before the #ifdef,
// or the file silently compiles to nothing in a real build although the host tests (which pass
// -DIOHOME_BRIDLE) are fine.
#include "esphome/core/hal.h"

#ifdef IOHOME_BRIDLE

#include <cstddef>
#include <cstdint>
#include <variant>

#include "radio_bridle_enums.h"

namespace esphome::home_io_control::bridle {

// The release's generated names (`MsgType`, `field::tx::…`, `is_reply()`) are used here and by every includer as
// `esphome::home_io_control::bridle::X`.
using namespace ::bridle;

// === Sizes (spec §2.2) ===

/// Longest decoded message: `msg_type · seq · body · crc`.
static constexpr size_t MAX_MESSAGE_SIZE = 255;
/// Shortest decoded message: `msg_type · seq · crc` with an empty body.
static constexpr size_t MIN_MESSAGE_SIZE = 4;
/// Longest body.
static constexpr size_t MAX_BODY_SIZE = MAX_MESSAGE_SIZE - MIN_MESSAGE_SIZE;
/// Longest frame on the wire: a maximum message plus COBS overhead plus the `0x00` delimiter.
static constexpr size_t MAX_WIRE_SIZE = 258;
/// Radio index that addresses the device instead of one radio in `set_params`.
static constexpr uint8_t DEVICE_SCOPE = 0xFF;
/// Most frequencies one `rx_scan` may list (spec §3.1); a longer list is a `param_range` error.
static constexpr size_t MAX_SCAN_FREQS = 16;
/// Longest strobe template (spec §5).
static constexpr size_t MAX_STROBE_TEMPLATE = 32;
/// Most radios a `ready` descriptor list is read for; further radios are ignored.
static constexpr size_t MAX_RADIOS = 8;
/// Fixed magic of `enter_bootloader` (`.ksy` `enter_bootloader.magic`), so a corrupted command
/// cannot reboot the co-processor into its bootloader.
static constexpr uint8_t ENTER_BOOTLOADER_MAGIC[4] = {0xAD, 0x10, 0x07, 0xB0};

/// A read-only run of bytes inside a buffer owned by someone else.
struct ByteSpan {
  const uint8_t *data{nullptr};
  size_t size{0};
};

// === COBS ===

/// Result of `cobs_decode` when the input is not a valid COBS block.
static constexpr int COBS_MALFORMED = -1;
/// Result of `cobs_decode` when the decoded bytes do not fit the output buffer.
static constexpr int COBS_TOO_LONG = -2;

/// @brief COBS-encode `n` bytes (no delimiter appended).
/// @return Encoded length, or 0 when `cap` is too small.
size_t cobs_encode(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

/// @brief COBS-decode `n` bytes (the delimiter already removed).
/// @return Decoded length, `COBS_MALFORMED` or `COBS_TOO_LONG`.
int cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

// === Message bodies ===
//
// One struct per message type, named after its `.ksy` type. Host commands come first. `TYPE` is
// the wire message type, so encode() never takes the type as a second argument.

/// `hello` (0x01): ask for `ready`. Empty body.
struct Hello {
  static constexpr MsgType TYPE = MsgType::HELLO;
};
/// `set_params` (0x02): settings for one radio or, with `DEVICE_SCOPE`, for the device.
struct SetParams {
  static constexpr MsgType TYPE = MsgType::SET_PARAMS;
  uint8_t radio{0};
  ByteSpan tlvs{};  ///< `id · len · value` entries; build with `TlvWriter`.
};
/// `rx_on` (0x03): receive on a frequency; a different one than the radio is on is a retune.
struct RxOn {
  static constexpr MsgType TYPE = MsgType::RX_ON;
  uint8_t radio{0};
  uint32_t freq_hz{0};
  bool force{false};  ///< Retune at once instead of after the frame being received.
};
/// `rx_scan` (0x04): receive while cycling through a frequency list.
struct RxScan {
  static constexpr MsgType TYPE = MsgType::RX_SCAN;
  uint8_t radio{0};
  uint16_t dwell_us{0};  ///< Time on each frequency, at least 200.
  uint16_t hold_us{0};   ///< Stay on a frequency this long after a frame ends there.
  uint8_t num_freqs{0};  ///< Frequencies on the wire; only the first `MAX_SCAN_FREQS` are kept.
  uint32_t freqs_hz[MAX_SCAN_FREQS]{};
};
/// `idle` (0x05): stop a radio.
struct Idle {
  static constexpr MsgType TYPE = MsgType::IDLE;
  uint8_t radio{0};
};
/// The timed burst of short frames sent before a `tx` main frame (spec §5).
struct StrobeSpec {
  uint16_t count{0};                              ///< Frames in the burst, 1…1024.
  uint32_t period_ns{0};                          ///< Start-to-start distance of two frames.
  uint32_t main_gap_ns{0};                        ///< From the last strobe frame's start to the main frame's start.
  uint8_t counter_offset{0};                      ///< Where the down-counter is written into the template.
  uint8_t counter_width{0};                       ///< Counter bytes, big-endian.
  uint32_t counter_unit_ns{0};                    ///< Counter step: the time to the main frame in these units.
  int16_t counter_adjust{0};                      ///< Added to the counter.
  uint8_t len_template{0};                        ///< Template bytes, 1…`MAX_STROBE_TEMPLATE`.
  uint8_t template_bytes[MAX_STROBE_TEMPLATE]{};  ///< The strobe frame before the counter is written.
};
/// `tx` (0x06): send one payload, optionally after a strobe.
struct Tx {
  static constexpr MsgType TYPE = MsgType::TX;
  uint8_t radio{0};
  uint32_t freq_hz{0};
  bool then_rx{false};          ///< Return to receive after the main frame.
  bool at_time{false};          ///< Start the main frame at `t_main_us`.
  bool cca{false};              ///< Check the channel first, against `cca_threshold_dbm`.
  bool has_strobe{false};       ///< `strobe` is present.
  uint16_t preamble_bytes{0};   ///< 0 = the profile's standard preamble.
  uint32_t t_main_us{0};        ///< Co-processor time; only with `at_time`.
  int8_t cca_threshold_dbm{0};  ///< Only with `cca`.
  StrobeSpec strobe{};          ///< Only with `has_strobe`.
  ByteSpan payload{};
};
/// `get_rssi` (0x07): instantaneous RSSI; only while the radio receives.
struct GetRssi {
  static constexpr MsgType TYPE = MsgType::GET_RSSI;
  uint8_t radio{0};
};
/// `get_stats` (0x08): read the device counters.
struct GetStats {
  static constexpr MsgType TYPE = MsgType::GET_STATS;
  bool reset_after_read{false};
};
/// `soft_reset` (0x09): reboot the co-processor. Empty body.
struct SoftReset {
  static constexpr MsgType TYPE = MsgType::SOFT_RESET;
};
/// `enter_bootloader` (0x0A): reboot into the bootloader. The body is `ENTER_BOOTLOADER_MAGIC`.
struct EnterBootloader {
  static constexpr MsgType TYPE = MsgType::ENTER_BOOTLOADER;
};

/// `ready` (0x80): identity, version, capabilities and info TLVs (see `parse_ready_info`).
struct Ready {
  static constexpr MsgType TYPE = MsgType::READY;
  uint8_t cmd_seq{0};  ///< 0 when unsolicited (after a boot).
  uint8_t proto_major{PROTO_MAJOR};
  uint8_t proto_minor{PROTO_MINOR};
  uint16_t chip_id{0};  ///< A `ChipId` value (kept raw so an unknown chip still decodes).
  uint32_t caps{0};     ///< `device_caps` bits.
  ByteSpan info{};      ///< `id · len · value` entries.
};
/// `ack` (0x82): the answer to a command that has nothing to report.
struct Ack {
  static constexpr MsgType TYPE = MsgType::ACK;
  uint8_t cmd_seq{0};
};
/// `params_ack` (0x81): the settings as applied.
struct ParamsAck {
  static constexpr MsgType TYPE = MsgType::PARAMS_ACK;
  uint8_t cmd_seq{0};
  uint8_t radio{0};
  ByteSpan applied{};  ///< `id · len · value` entries.
};
/// `frame_start` (0xC0): a frame is arriving.
struct FrameStart {
  static constexpr MsgType TYPE = MsgType::FRAME_START;
  uint8_t radio{0};
  uint32_t t_us{0};
  uint32_t freq_hz{0};
  FrameStage stage{FrameStage::SYNC};
};
/// `rx` (0xC1): a received frame.
struct Rx {
  static constexpr MsgType TYPE = MsgType::RX;
  uint8_t radio{0};
  uint32_t t_us{0};
  uint32_t freq_hz{0};
  int8_t rssi_dbm{0};
  uint8_t lqi{0};
  CrcState crc{CrcState::NOT_CHECKED};
  ByteSpan payload{};  ///< 802.15.4: the PSDU including the received FCS.
};
/// `rx_abort` (0xC2): a reception began but did not complete.
struct RxAbort {
  static constexpr MsgType TYPE = MsgType::RX_ABORT;
  uint8_t radio{0};
  uint32_t t_us{0};
  uint32_t freq_hz{0};
  RxAbortReason reason{RxAbortReason::NO_END_AFTER_SYNC};
};
/// `tx_done` (0x83): a `tx` finished.
struct TxDone {
  static constexpr MsgType TYPE = MsgType::TX_DONE;
  uint8_t cmd_seq{0};
  uint8_t radio{0};
  uint32_t t_main_start_us{0};
  uint32_t t_main_end_us{0};
  uint16_t strobe_sent{0};
  uint16_t strobe_late{0};  ///< Strobe frames skipped for being more than half a period late.
};
/// `rssi` (0x84): reply to `get_rssi`.
struct Rssi {
  static constexpr MsgType TYPE = MsgType::RSSI;
  uint8_t cmd_seq{0};
  uint8_t radio{0};
  int8_t rssi_dbm{0};
};
/// `stats` (0x85): device counters. They are append-only, so a reader takes as many as it gets.
struct Stats {
  static constexpr MsgType TYPE = MsgType::STATS;
  /// Index of each counter in `counters`, in wire order.
  enum Counter : uint8_t {
    RX_OK,
    RX_CRC_ERR,
    RX_ABORT,
    RX_OVERFLOW,
    TX_OK,
    TX_ERR,
    LINK_ERR,
    UPTIME_MS,
    LOG_DROPPED,
    COUNT
  };
  uint8_t cmd_seq{0};
  uint8_t present{0};  ///< How many leading counters the body held.
  uint32_t counters[COUNT]{};
};
/// `error` (0x88): a command was refused.
struct Error {
  static constexpr MsgType TYPE = MsgType::ERROR;
  uint8_t cmd_seq{0};
  ErrorCode code{ErrorCode::UNKNOWN_TYPE};
  uint16_t detail{0};  ///< Meaning depends on `code`; see `detail_dbm`.
  /// `detail` as the signed level of `channel_busy`.
  [[nodiscard]] int16_t detail_dbm() const { return static_cast<int16_t>(detail); }
};
/// `log` (0xC4): debug text. The host never acts on it.
struct Log {
  static constexpr MsgType TYPE = MsgType::LOG;
  LogLevel level{LogLevel::INFO};
  ByteSpan text{};
};
/// `rx_overflow` (0xC3): received frames were dropped because the host did not drain.
struct RxOverflow {
  static constexpr MsgType TYPE = MsgType::RX_OVERFLOW;
  uint16_t dropped{0};
};
/// A message of a type this codec does not know. Its body is kept raw so it can be answered
/// (`error(unknown_type)`), traced and skipped.
struct Unknown {
  uint8_t msg_type{0};
  ByteSpan body{};
};

/// Every message body the codec handles.
using Body =
    std::variant<Unknown, Hello, SetParams, RxOn, RxScan, Idle, Tx, GetRssi, GetStats, SoftReset, EnterBootloader,
                 Ready, ParamsAck, Ack, FrameStart, Rx, RxAbort, TxDone, Rssi, Stats, Error, Log, RxOverflow>;

/// One decoded message: the frame header's `seq` plus the typed body.
struct Message {
  uint8_t seq{0};  ///< Host: command id (never 0). Co-processor: event counter.
  Body body;
};

/// The wire `msg_type` byte of `body`.
[[nodiscard]] uint8_t msg_type_of(const Body &body);

// === Encoding and decoding ===

/// Outcome of `decode_message`.
enum class DecodeStatus : uint8_t {
  OK,          ///< `out` holds the message (possibly an `Unknown` one).
  BAD_LENGTH,  ///< The message is too short, or the body does not match its type's layout.
};

/// @brief Encode `body` as a message: `msg_type · seq · body · crc`.
/// @return Message length, or 0 when `cap` is too small or a field cannot be encoded.
size_t encode_message(uint8_t seq, const Body &body, uint8_t *out, size_t cap);

/// @brief Encode `body` as a frame on the wire: COBS of the message, then the `0x00` delimiter.
/// @return Frame length, or 0 when `cap` is too small or a field cannot be encoded.
size_t encode_frame(uint8_t seq, const Body &body, uint8_t *out, size_t cap);

/// @brief Wrap an already encoded message (CRC included) as a wire frame.
/// @return Frame length, or 0 when `cap` is too small.
size_t frame_message(const uint8_t *msg, size_t n, uint8_t *out, size_t cap);

/// @brief Decode one message. The CRC is not checked here: `Deframer` does that.
///
/// The spec's rule for a command whose body does not fit its layout applies to every message: a
/// short body, a TLV that runs past the end, a wrong magic, or (while the protocol is 0.y) bytes
/// left over are all `DecodeStatus::BAD_LENGTH`. An unknown type is not an error.
[[nodiscard]] DecodeStatus decode_message(const uint8_t *msg, size_t n, Message &out);

/// Whether the trailing CRC-16/KERMIT of `msg` matches its other bytes.
[[nodiscard]] bool message_crc_ok(const uint8_t *msg, size_t n);

// === Streaming deframer ===

/// @brief Turns a byte stream into messages one byte at a time.
///
/// Invalid frames (bad COBS, bad CRC, bad length) are dropped and counted, as the spec requires
/// (`stats.link_err`); the deframer resynchronises on the next delimiter and never allocates.
class Deframer {
 public:
  /// What one byte did.
  enum class Event : uint8_t {
    NONE,     ///< The frame is still incomplete, or an empty frame ended.
    MESSAGE,  ///< A valid message is ready: see `message`.
    DROPPED,  ///< An invalid frame ended and was counted.
  };
  /// Frames dropped so far, by reason.
  struct Counters {
    uint32_t bad_cobs{0};
    uint32_t bad_crc{0};
    uint32_t bad_length{0};
    [[nodiscard]] uint32_t total() const { return bad_cobs + bad_crc + bad_length; }
  };

  /// Feed the next received byte.
  Event feed(uint8_t byte);
  /// The last valid message (`msg_type · seq · body · crc`); valid until the next feed().
  [[nodiscard]] const uint8_t *message() const { return this->msg_; }
  /// Length of `message`.
  [[nodiscard]] size_t message_size() const { return this->msg_len_; }
  /// Frames dropped since construction.
  [[nodiscard]] const Counters &counters() const { return this->counters_; }

 private:
  Event finish_frame_();

  uint8_t wire_[MAX_WIRE_SIZE]{};
  size_t wire_len_{0};
  bool overflow_{false};  ///< The frame outgrew the wire buffer; dropped at its delimiter.
  uint8_t msg_[MAX_MESSAGE_SIZE]{};
  size_t msg_len_{0};
  Counters counters_{};
};

// === The radio descriptor ===

/// One radio of the co-processor (`info_id::radio`).
struct RadioDescriptor {
  uint8_t radio{0};
  uint16_t chip{0};      ///< A `ChipId` value: the radio's own chip.
  uint16_t profiles{0};  ///< `phy_profiles` bits.
  uint32_t band_min_hz{0};
  uint32_t band_max_hz{0};
  int8_t tx_power_min_dbm{0};
  int8_t tx_power_max_dbm{0};
  uint8_t max_payload{0};
  uint16_t max_preamble_bytes{0};  ///< Longest `tx.preamble_bytes` on a per-packet-preamble profile; 0 without.
  uint16_t strobe_jitter_us{0};    ///< Worst-case strobe slot error; 0 without cap `strobe`.
  uint32_t caps{0};                ///< `radio_caps` bits.
};

// === TLV lists (spec §4) ===

/// @brief Walks an `id · len · value` list.
class TlvReader {
 public:
  explicit TlvReader(ByteSpan list) : list_(list) {}
  /// Next entry. Returns false at the end of the list, or at an entry that runs past it
  /// (`malformed`).
  bool next(uint8_t &id, ByteSpan &value);
  /// Whether the walk stopped at an entry that runs past the end of the list.
  [[nodiscard]] bool malformed() const { return this->malformed_; }

 private:
  ByteSpan list_;
  size_t pos_{0};
  bool malformed_{false};
};

/// Whether every entry of `list` fits inside it.
[[nodiscard]] bool tlv_list_valid(ByteSpan list);
/// Find the first entry with `id`; false when there is none.
[[nodiscard]] bool find_tlv(ByteSpan list, uint8_t id, ByteSpan &value);

/// @brief Builds an `id · len · value` list in a caller-owned buffer.
class TlvWriter {
 public:
  TlvWriter(uint8_t *buf, size_t cap) : buf_(buf), cap_(cap) {}
  /// Append an entry with a raw value.
  TlvWriter &add(uint8_t id, const uint8_t *value, size_t n);
  /// Append a one-byte entry.
  TlvWriter &add_u8(uint8_t id, uint8_t value);
  /// Append a little-endian two-byte entry.
  TlvWriter &add_u16(uint8_t id, uint16_t value);
  /// Append a little-endian four-byte entry.
  TlvWriter &add_u32(uint8_t id, uint32_t value);
  /// Append a radio descriptor entry (`info_id::radio`).
  TlvWriter &add_radio(const RadioDescriptor &radio);
  /// Bytes written.
  [[nodiscard]] size_t size() const { return this->size_; }
  /// False once an entry did not fit; the list is then incomplete.
  [[nodiscard]] bool ok() const { return this->ok_; }
  /// The list written so far.
  [[nodiscard]] ByteSpan span() const { return {this->buf_, this->size_}; }

 private:
  uint8_t *buf_;
  size_t cap_;
  size_t size_{0};
  bool ok_{true};
};

// === The `ready` descriptor ===

/// What a `ready` says about the co-processor, read from its info TLVs.
struct ReadyInfo {
  ByteSpan fw_version{};
  ByteSpan board{};  ///< Empty when the co-processor sent none.
  RadioDescriptor radios[MAX_RADIOS]{};
  uint8_t radio_count{0};
  bool has_rx_queue_depth{false};
  uint8_t rx_queue_depth{0};
  ResetCause reset_cause{ResetCause::POWER_ON};
};

/// Result of `parse_ready_info`.
enum class ReadyInfoStatus : uint8_t {
  OK,
  BAD_TLV,              ///< A TLV has the wrong length for its id.
  MISSING_FW_VERSION,   ///< `fw_version` is required.
  MISSING_RADIO,        ///< At least one `radio` is required.
  MISSING_RESET_CAUSE,  ///< `reset_cause` is required.
};

/// The TLV id a non-OK status names, for log lines ("ready lacks radio").
[[nodiscard]] const char *ready_info_status_name(ReadyInfoStatus status);

/// @brief Read the info TLVs of a `ready`. Unknown ids are skipped; the first defect is reported.
[[nodiscard]] ReadyInfoStatus parse_ready_info(ByteSpan info, ReadyInfo &out);

/// Whether a `ready` is the answer to a `hello` that took the host link over (`reset_cause` `link_takeover`, spec
/// §2.1): the co-processor did not reboot, but its protocol state and event counter restarted.
[[nodiscard]] bool is_link_takeover(const Ready &ready);

// === The version rule (spec §10.2) ===

/// Whether a host speaking protocol `host_major.host_minor` accepts a co-processor that reports
/// `coproc_major.coproc_minor`: while the major version is 0 both numbers must be equal, from 1 on the majors must be
/// equal and any minor, older or newer, is accepted. The versions are parameters so the 1.x rule is testable before
/// a 1.0 exists.
[[nodiscard]] constexpr bool version_accepted(uint8_t host_major, uint8_t host_minor, uint8_t coproc_major,
                                              uint8_t coproc_minor) {
  if (host_major != coproc_major)
    return false;
  return host_major == 0 ? host_minor == coproc_minor : true;
}

/// Whether a co-processor reporting `coproc_major.coproc_minor` defines what was added in `since_major.since_minor`
/// (a `-since` of the `.ksy`): the same major and a minor that is not older. A host asks before it sends a param or a
/// message that has a `-since`.
[[nodiscard]] constexpr bool coproc_supports(uint8_t coproc_major, uint8_t coproc_minor, uint8_t since_major,
                                             uint8_t since_minor) {
  return coproc_major == since_major && coproc_minor >= since_minor;
}

}  // namespace esphome::home_io_control::bridle

#endif  // IOHOME_BRIDLE
