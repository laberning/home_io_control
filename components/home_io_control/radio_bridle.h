/// @file
/// @brief RadioBridle: the RadioDriver for a radio co-processor that speaks the Bridle protocol.
/// @ingroup hioc_radio
///
/// The hub drives this like any other radio, but the radio sits behind a serial link: commands go
/// out as Bridle messages, and everything the co-processor reports (frames, transmission results,
/// scan hits) comes back on the same stream as events. The driver is band-agnostic; everything
/// that depends on the band (PHY profile, framing, wake-up shape, idle scan) is a
/// `bridle::BridlePhy` injected by reference. Compiled only under `IOHOME_BRIDLE`.
///
/// Every command is answered. A command the driver waits for (`set_params`, `tx`, `get_rssi`, `hello`) ends
/// its wait on its reply or its `error`. One it does not wait for (`rx_on`, `rx_scan`, `idle`) is entered
/// in a small table of pending commands (`PENDING_DEPTH`) and its `ack` or `error` is matched against the
/// table's head when it arrives: replies leave the co-processor in command order (spec §2.2), so a reply
/// that names a later entry means the head's command was lost, and a head with no answer after
/// `ACK_TIMEOUT_MS` is a lost link. A refusal undoes the driver's optimistic state only while its command
/// is still the newest receive-state command, and is not retried before `SCAN_RETRY_MS`.
///
/// Receiving. One private drain (`drain_`) reads every byte the stream holds, deframes it and
/// routes each message: a reply to the command awaiting it, a received frame into a small FIFO,
/// `frame_start`/`rx_abort` into the reception state, `log` into the ESPHome log. Every getter
/// and every wait loop drains first, so the UART's receive buffer never has to hold more than one
/// `loop()` pass.
///
/// Ordering (spec §6). When `tx_done` of our own `tx` is routed, the FIFO is emptied: every `rx`
/// before it arrived before or during the transmission, which a half-duplex radio would have
/// lost, so a late reply to an earlier try can never be taken for the current one.

#pragma once

// IOHOME_BRIDLE is only visible after something pulls in esphome/core/defines.h (ESPHome codegen's
// cg.add_define() lands there, not as a compiler -D flag): this include must run before the #ifdef,
// or the file silently compiles to nothing in a real build although the host tests (which pass
// -DIOHOME_BRIDLE) are fine.
#include "esphome/core/hal.h"

#ifdef IOHOME_BRIDLE

#include <cstdint>
#include <optional>

#include "radio_bridle_codec.h"
#include "radio_bridle_phy.h"
#include "radio_bridle_trace.h"
#include "radio_interface.h"

namespace esphome::home_io_control {

using bridle::TraceDirection;

/// @brief A Bridle co-processor as a RadioDriver.
class RadioBridle : public RadioDriver {
 public:
  /// Most received frames held between two drains; when full the oldest is dropped.
  static constexpr size_t RX_FIFO_DEPTH = 8;
  /// Longest PSDU kept (IEEE 802.15.4's own limit); a longer payload is counted and dropped.
  static constexpr size_t MAX_RX_PSDU = 127;
  /// How long init() waits for a `ready`, and for the reply to a `hello`.
  static constexpr uint32_t READY_TIMEOUT_MS = 2000;
  static constexpr uint32_t HELLO_TIMEOUT_MS = 1000;
  /// How long a command may take to be answered (a configuration command, or the `ack` of an `rx_on`,
  /// `rx_scan` or `idle`).
  static constexpr uint32_t ACK_TIMEOUT_MS = 500;
  /// Most unawaited commands (`rx_on`, `rx_scan`, `idle`) in flight at once. The hub sends them from its own
  /// passes and drains the link only between passes: one pass sends at most four (a hop, a return to receive,
  /// a stop, a scan start), and twice that covers two passes without a drain.
  static constexpr size_t PENDING_DEPTH = 8;
  /// After a refused `rx_scan` or `rx_on` the driver does not send it again for this long: a refusal
  /// means the co-processor and the profile disagree, which a retry does not fix.
  static constexpr uint32_t SCAN_RETRY_MS = 5000;
  /// How long a `get_rssi` may take: the radio answers at once or says it is not receiving.
  static constexpr uint32_t RSSI_TIMEOUT_MS = 20;
  /// Margin on top of a transmission's own duration before send_packet() gives up on `tx_done`.
  static constexpr uint32_t TX_DONE_MARGIN_MS = 100;
  /// Bytes kept of the co-processor's firmware and board names.
  static constexpr size_t TEXT_CAP = 40;
  /// Bytes read from the stream at a time by the drain.
  static constexpr size_t DRAIN_CHUNK = 64;
  /// Room for the TLVs of one configuration command: the 868 MHz profile's eleven take 48 bytes with the
  /// profile and the TX power.
  static constexpr size_t CONFIG_TLV_CAP = 64;
  /// The level read_rssi() returns when the radio cannot say (dBm): below any real reading.
  static constexpr int16_t RSSI_NONE_DBM = -127;

  // === Link supervision (spec §7 and the numbers the spec leaves to the host) ===

  /// Time without a command sent after which the host sends `get_stats`, and how long that may go unanswered. The
  /// co-processor lets another host take the link after 60 s without a command (spec §2.1), so the host keeps a
  /// command flowing even while events arrive.
  static constexpr uint32_t HEARTBEAT_SILENCE_MS = 30000;
  static constexpr uint32_t HEARTBEAT_REPLY_MS = 100;
  /// Event-counter gaps within GAP_WINDOW_MS that count as a lost link (a lone gap is one lost event).
  static constexpr size_t GAP_LIMIT = 3;
  static constexpr uint32_t GAP_WINDOW_MS = 10000;
  /// Least time between two reset pulses of resyncs (the spec's floor is one second).
  static constexpr uint32_t RESYNC_MIN_INTERVAL_MS = 5000;
  /// Failed resyncs within FAILURE_WINDOW_MS that put the link down, and how often a down link tries again.
  static constexpr size_t FAILURE_LIMIT = 3;
  static constexpr uint32_t FAILURE_WINDOW_MS = 60000;
  static constexpr uint32_t DOWN_RETRY_MS = 60000;

  /// The state of the link to the co-processor.
  enum class Link : uint8_t {
    INIT,           ///< Before init() finished: nothing is supervised yet.
    RUNNING,        ///< Configured and answering.
    WAIT_RESET,     ///< A resync is due: the reset pulse waits for its earliest time.
    WAIT_READY,     ///< Reset done, waiting for the co-processor's `ready`.
    CONFIG_DEVICE,  ///< Event mask sent, waiting for its acknowledgement.
    CONFIG_RADIO,   ///< Radio parameters sent, waiting for their acknowledgement.
    DOWN,           ///< Repeated resyncs failed: sending fails at once; one attempt every DOWN_RETRY_MS.
  };

  /// Microseconds one byte takes on a UART at `baud` with 8N1 framing (a start bit, eight data bits
  /// and a stop bit: ten bit times), rounded up: 10 µs at 1 Mbaud, 87 µs at 115200.
  static constexpr uint32_t byte_time_us(uint32_t baud) {
    constexpr uint32_t bits_per_byte = 10;  // start + 8 data + stop
    constexpr uint32_t us_per_s = 1000000;
    return (bits_per_byte * us_per_s + baud - 1U) / baud;
  }

  /// @param stream The link to the co-processor; not owned.
  /// @param phy The band's profile; not owned, must outlive the driver.
  /// @param rst_pin The co-processor's active-low reset input.
  /// @param tx_power_dbm Requested TX power; unset asks for the radio's own maximum. The
  ///        co-processor clamps it and echoes what it applied.
  RadioBridle(ByteStreamAccess *stream, const bridle::BridlePhy &phy, InternalGPIOPin *rst_pin,
              std::optional<int8_t> tx_power_dbm)
      : RadioDriver(rst_pin),
        stream_(stream),
        phy_(phy),
        tx_power_dbm_(tx_power_dbm),
        uart_byte_us_(byte_time_us(stream != nullptr ? stream->baud_rate() : ByteStreamAccess::DEFAULT_BAUD_RATE)) {}

  bool init() override;
  TxResult send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) override;
  bool wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) override;
  bool check_for_packet(RadioRxPacket &packet) override;
  /// `get_rssi`; the idle floor (see the constant below) when the link is down, the radio is not receiving or
  /// the reply does not come.
  int16_t read_rssi() override;
  bool is_sync_detected() override;
  bool is_preamble_detected() override;
  [[nodiscard]] bool reception_in_progress() override;
  /// Starts the profile's `rx_scan` once and returns true while it runs; false during the hold-off after a
  /// refusal, when the hub hops from the host instead (the radio is kept receiving for that).
  bool run_idle_scan() override;
  /// Records the frequency of an idle radio. A receiving radio gets an `rx_on` for a new frequency; a scanning
  /// one gets it for any, which ends the scan.
  void change_frequency(uint32_t freq_hz) override;
  /// The band profile's frequency for one of the hub's channels (2425/2450/2475 MHz on `io_2g4`); any
  /// other value comes back unchanged.
  [[nodiscard]] uint32_t air_frequency_hz(uint32_t channel_hz) const override;
  /// `rx_on` on the current frequency; ends a scan.
  void set_mode_rx() override;
  /// `idle`; a frame being received is dropped without an `rx_abort`.
  void set_mode_standby() override;

  [[nodiscard]] uint16_t response_preamble() const override { return this->phy_.response_preamble(this->tuning_); }
  /// Keeps the tuning the profile reads. When it changes a param of the radio's profile (the receiver
  /// bandwidth of a `gfsk_raw` radio) and the link is up, the radio-scope params go out again; the hub
  /// calls this between exchanges, so the command never lands inside this driver's own transmission
  /// (which the co-processor would answer `busy`).
  void apply_tuning(const TuningConfig &tuning) override;
  [[nodiscard]] uint16_t default_start_preamble() const override { return this->phy_.default_start_preamble(); }
  [[nodiscard]] uint16_t hop_dwell_ms(const TuningConfig &tuning) const override {
    return this->phy_.hop_dwell_ms(tuning);
  }
  [[nodiscard]] int16_t lbt_threshold_dbm(const TuningConfig &tuning) const override {
    return this->phy_.cca_threshold_dbm(tuning);
  }
  [[nodiscard]] bool low_power_by_default() const override { return this->phy_.low_power_by_default(); }
  [[nodiscard]] uint32_t start_response_wait_ms(const TuningConfig &tuning) const override {
    return this->phy_.start_response_wait_ms(tuning);
  }
  [[nodiscard]] uint32_t retry_gap_ms(const TuningConfig &tuning) const override {
    return this->phy_.retry_gap_ms(tuning);
  }
  [[nodiscard]] bool retries_rotate_channel() const override { return this->phy_.retries_rotate_channel(); }
  /// True: the co-processor's radio returns to receive by itself after a transmission (`then_rx`).
  [[nodiscard]] bool has_fast_tx_rx_turnaround() const override { return true; }
  [[nodiscard]] uint32_t tx_air_time_us(uint8_t len, const RadioTxConfig &cfg) const override;
  [[nodiscard]] const char *chip_name() const override { return "bridle"; }
  void dump_debug() override;

  /// Log every message sent and received as a trace line (spec §12.1), the recording format of
  /// session playback.
  void set_trace(bool enabled) { this->trace_ = enabled; }
  /// Whether the connected co-processor defines what was added in protocol minor `since_minor` of this major
  /// (spec §10.2): ask before sending a param or message that has a `-since`.
  [[nodiscard]] bool coproc_supports(uint8_t since_minor) const {
    return bridle::coproc_supports(this->coproc_major_, this->coproc_minor_, bridle::PROTO_MAJOR, since_minor);
  }
  /// Whether the link is configured and answering.
  [[nodiscard]] bool link_running() const { return this->link_ == Link::RUNNING; }
  [[nodiscard]] Link link_state() const { return this->link_; }

 protected:
  /// Where a trace line goes; a test overrides it (the host log stub discards output). The line
  /// starts with the `[bridle]` marker, which readers look for after whatever prefix the log adds.
  virtual void emit_trace_line(const char *line);
  /// Frames received but not yet handed to the hub.
  struct RxEntry {
    uint32_t freq_hz{0};  ///< Frequency on air.
    int8_t rssi_dbm{0};
    bridle::CrcState crc{bridle::CrcState::NOT_CHECKED};
    uint8_t len{0};
    uint8_t data[MAX_RX_PSDU]{};
  };
  /// Outcome of waiting for a reply.
  enum class Wait : uint8_t { REPLIED, ERROR, TIMEOUT };

  // === Link ===

  /// Send `body` as a command; returns its command id (never 0).
  uint8_t send_command_(const bridle::Body &body);
  /// What an unawaited command was, for undoing the driver's optimistic state when it is refused.
  enum class PendingKind : uint8_t { RX_ON, RX_SCAN, IDLE };
  /// An unawaited command waiting for its `ack` or `error`.
  struct PendingCommand {
    uint8_t seq{0};                        ///< Command id the reply is matched against.
    PendingKind kind{PendingKind::RX_ON};  ///< Which command it was, for the optimistic state to undo on refusal.
    uint32_t sent_ms{0};                   ///< millis() at the send, for the ACK_TIMEOUT_MS check.
  };
  /// Send `body` without waiting for its reply and enter it in the pending table. When the table is full the
  /// send waits (draining, feeding the watchdog) for the head's reply up to `ACK_TIMEOUT_MS`, then resyncs.
  /// @return The command id, 0 when nothing was sent.
  uint8_t send_pending_(const bridle::Body &body, PendingKind kind);
  /// An `ack` or `error` for a command in the pending table; false when `seq` is not in it.
  bool on_pending_reply_(const bridle::Message &m, uint8_t seq);
  /// A refused pending command: log it once per code and undo the optimistic state where it applies.
  void on_pending_refused_(const PendingCommand &cmd, const bridle::Error &error);
  /// Read everything the stream holds and route it.
  void drain_();
  /// drain_() plus link supervision. Every public entry the hub calls on an idle or waiting pass
  /// uses this, so the periodic work of the link (heartbeat, resync, the down retry) needs no timer
  /// of its own.
  void poll_link_();
  void supervise_();
  void supervise_running_();
  void supervise_wait_reset_();
  void supervise_wait_ready_();
  void supervise_config_();
  void supervise_down_();
  /// Arm the async wait for the acknowledgement of config command `seq`.
  void start_awaiting_config_(uint8_t seq);
  void trace_message_(TraceDirection direction, const uint8_t *msg, size_t len);
  /// Route one deframed message.
  void route_(const uint8_t *msg, size_t len);
  void note_event_counter_(const bridle::Message &m);
  /// Route a reply: to the awaited config command, a pending command, or the resync path. An `error` with
  /// `cmd_seq` 0 (a radio fault outside a transmission), or `not_configured` for any command, means the
  /// co-processor lost its configuration, so resync.
  void route_reply_(const bridle::Message &m, const uint8_t *msg, size_t len, uint8_t seq);
  void on_awaited_reply_(const bridle::Message &m, const uint8_t *msg, size_t len);
  void route_event_(const bridle::Body &body);
  void show_log_(const bridle::Log &log);
  /// Wait for the reply of type `expected` to command `seq`, draining as it waits.
  /// The wait ends on that reply or on an `error` for `seq`, whichever comes first.
  Wait await_(uint8_t seq, bridle::MsgType expected, uint32_t timeout_ms);

  // === Handshake and configuration ===

  bool handshake_();
  /// Check a `ready` and pick the radio. At init a defect latches the driver failed; during a resync
  /// it is only logged (the caller takes the link down instead).
  bool accept_ready_(const bridle::Ready &ready, bool latch_failure);
  bool configure_();
  /// Send the event mask (device) or the radio parameters; returns the command id (0 on failure).
  uint8_t send_config_(bool device);
  /// The profile's params for the current tuning, without `phy_profile` and the TX power, into `buf`.
  /// @return Their length in bytes, 0 when they do not fit.
  size_t build_params_(uint8_t *buf, size_t cap) const;
  /// Check the answer to the last config command, take the applied TX power from it and log a refusal with
  /// the TLV it names.
  bool config_reply_ok_(bool device);

  // === Resync (non-blocking) ===

  /// A lost link: reset the co-processor and configure it again, one step per poll.
  void begin_resync_(const char *why, bool skip_reset);
  void set_link_(Link state, const char *why);
  void attempt_failed_(const char *why);
  void finish_resync_();
  [[nodiscard]] bool link_lost_() const { return this->link_ != Link::INIT && this->link_ != Link::RUNNING; }

  // === Receive path ===

  void on_rx_(const bridle::Rx &rx);
  bool pop_packet_(RadioRxPacket &packet);
  bool decode_entry_(const RxEntry &entry, RadioRxPacket &packet);
  void ensure_rx_();

  ByteStreamAccess *stream_;
  const bridle::BridlePhy &phy_;
  std::optional<int8_t> tx_power_dbm_;
  uint32_t uart_byte_us_;  ///< Microseconds one byte takes on the link (8N1), from the stream's baud rate.
  uint8_t
      sent_params_[CONFIG_TLV_CAP]{};  ///< The profile's params as last sent, to tell whether a tuning changes them.
  size_t sent_params_len_{0};          ///< Bytes in @ref sent_params_.
  TuningConfig
      tuning_{};  ///< The runtime tuning the profile reads (`apply_tuning()`); defaults until the hub applies it.
  bridle::Deframer deframer_;

  // Selected radio and what its descriptor said.
  uint8_t radio_{0};
  uint32_t radio_caps_{0};
  uint8_t max_payload_{0};
  bridle::RadioDescriptor radio_desc_{};  ///< The chosen radio's descriptor, for dump_debug().
  int8_t applied_tx_power_dbm_{0};
  bridle::ReadyInfo ready_info_{};  ///< The scalars of the last accepted `ready` (its spans are not kept).
  char fw_version_[TEXT_CAP]{};
  char board_[TEXT_CAP]{};
  uint8_t ready_msg_[bridle::MAX_MESSAGE_SIZE]{};  ///< An unsolicited `ready` waiting to be accepted.
  size_t ready_msg_len_{0};
  bool ready_pending_{false};

  // Command bookkeeping: one command waits for its reply at a time.
  uint8_t next_seq_{1};
  bool awaiting_{false};
  uint8_t await_seq_{0};
  bridle::MsgType await_type_{bridle::MsgType::HELLO};
  bool await_done_{false};
  bool await_errored_{false};
  uint8_t reply_[bridle::MAX_MESSAGE_SIZE]{};
  size_t reply_len_{0};
  bridle::ErrorCode last_error_code_{bridle::ErrorCode::UNKNOWN_TYPE};
  int16_t last_error_detail_{0};

  // Reception state.
  RxEntry fifo_[RX_FIFO_DEPTH]{};
  size_t fifo_head_{0};
  size_t fifo_count_{0};
  bool frame_arriving_{false};
  bridle::FrameStage arriving_stage_{bridle::FrameStage::SYNC};
  bool rx_on_{false};
  bool scanning_{false};

  // Unawaited commands.
  PendingCommand pending_[PENDING_DEPTH]{};
  size_t pending_count_{0};
  uint8_t newest_receive_cmd_{0};  ///< Command id of the last rx_on, rx_scan, idle or tx sent (0 = none).
  uint16_t refusal_logged_{0};     ///< Bit per error code already logged for a refused unawaited command.
  bool scan_refused_{false};       ///< An rx_scan was refused; not sent again before SCAN_RETRY_MS.
  uint32_t scan_refused_ms_{0};    ///< millis() of the rx_scan refusal.
  bool rx_refused_{false};         ///< An rx_on was refused; not sent again before SCAN_RETRY_MS.
  uint32_t rx_refused_ms_{0};      ///< millis() of the rx_on refusal.
  uint32_t unawaited_errors_{0};   ///< Errors received for unawaited commands.

  // Link supervision.
  Link link_{Link::INIT};
  bool trace_{false};
  uint32_t last_rx_ms_{0};            ///< When the co-processor last sent anything.
  uint32_t last_command_sent_ms_{0};  ///< When the host last sent any command: the co-processor's claim clock.
  /// Protocol major version the co-processor reported in its last accepted `ready`.
  uint8_t coproc_major_{bridle::PROTO_MAJOR};
  /// Protocol minor version the co-processor reported in its last accepted `ready`.
  uint8_t coproc_minor_{bridle::PROTO_MINOR};
  uint8_t hello_seq_{0};       ///< The `hello` a resync without a reset line waits on (0: none).
  bool busy_at_start_{false};  ///< The start-up `hello` got `link_in_use`: init() starts the link down.
  bool hb_pending_{false};     ///< A heartbeat `get_stats` waits for its reply.
  uint8_t hb_seq_{0};
  uint32_t hb_sent_ms_{0};
  uint32_t gap_times_ms_[GAP_LIMIT]{};
  size_t gap_count_{0};
  uint32_t failure_times_ms_[FAILURE_LIMIT]{};
  size_t failure_count_{0};
  uint32_t last_pulse_ms_{0};  ///< When the last resync pulsed the reset line.
  bool pulsed_once_{false};
  uint32_t resync_at_ms_{0};   ///< Earliest time of the next reset pulse.
  uint32_t deadline_ms_{0};    ///< End of the current wait (`ready`, an acknowledgement).
  uint32_t next_retry_ms_{0};  ///< When a down link tries again.
  bool retrying_from_down_{false};
  uint32_t resyncs_{0};
  uint32_t log_dropped_{0};  ///< Last `stats.log_dropped` a heartbeat returned.
  bool have_stats_{false};

  // Counters for dump_debug().
  uint32_t dropped_before_tx_done_{0};
  uint32_t fifo_overflows_{0};
  uint32_t oversize_dropped_{0};
  uint32_t header_mismatches_{0};
  uint32_t bad_crc_frames_{0};
  uint32_t short_frames_{0};
  uint32_t stale_replies_{0};
  uint32_t counter_gaps_{0};
  uint32_t strobe_late_total_{0};
  uint32_t rx_overflow_reports_{0};
  uint32_t log_events_{0};
  bool expected_counter_known_{false};
  uint8_t expected_counter_{0};
};

}  // namespace esphome::home_io_control

#endif  // IOHOME_BRIDLE
