/// @file
/// @brief RadioBridle: the RadioDriver for a radio co-processor that speaks the Bridle protocol.
/// @ingroup hioc_radio
///
/// The hub drives this like any other radio, but the radio sits behind a serial link: commands go
/// out as Bridle messages, and everything the co-processor reports (frames, transmission results,
/// scan hits) comes back on the same stream as events. The driver is band-agnostic; everything
/// that depends on the band (PHY profile, framing, wake-up shape, idle scan, and the band's own
/// description for the hub) is a `bridle::BridlePhy` injected by reference. Compiled only under
/// `IOHOME_BRIDLE`.
///
/// Every command is answered. A command the driver waits for (`set_params`, `tx`, `get_rssi`, `hello`) ends
/// its wait on its reply or its `error`. One it does not wait for (`rx_on`, `rx_scan`, `idle`) is entered
/// in a small table of pending commands (`PENDING_DEPTH`) and its `ack` or `error` is matched against the
/// table's head when it arrives: replies leave the co-processor in command order (spec §2.2), so a reply
/// that names a later entry means the head's command was lost, and a head with no answer after
/// `ACK_TIMEOUT_MS` is a lost link. A refusal undoes the driver's optimistic state only while its command
/// is still the newest receive-state command (or, while a `tx` is awaited, the one a refused `tx` restores), and is
/// not retried before `SCAN_RETRY_MS`.
///
/// Receiving. One private drain (`drain_`) reads the stream chunk by chunk, deframes it and
/// routes each message: a reply to the command awaiting it, a received frame into a small FIFO,
/// `frame_start`/`rx_abort` into the reception state, `log` into the log. Every line the driver writes
/// after its start-up, a trace line and the frame log's TX and RX lines (`log_frame()`, as the SPI drivers write
/// them) included, goes through the frame log's hold (`frame_log_line()`), so nothing
/// is printed while the hub waits for a reply it provoked (ADR 0111). Every getter
/// and every wait loop drains first, so the UART's receive buffer never has to hold more than one
/// `loop()` pass. A drain stops reading once it has spent `DRAIN_BUDGET_US` (checked between chunk
/// reads only; a chunk already read is routed completely) so the loop task returns however much a
/// message costs, and the rest waits for the next drain. Waits honour that unread input: a timeout is
/// declared only after a drain that emptied the stream (or `DRAIN_TIMEOUT_GRACE_MS` past the
/// deadline), and the link supervision's reply timeouts and the event-counter resync stand down while
/// the last drain was cut short, since a backlog on the host is not a fault of the link.
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
  /// How long a command may take to be answered (a configuration command, the `rssi` of a `get_rssi`, or the `ack`
  /// of an `rx_on`, `rx_scan` or `idle`). It is link supervision, not a bound on the co-processor's reading: it
  /// exceeds that bound plus the link's transit and the hub's own latency by two orders of magnitude (ADR 0134).
  static constexpr uint32_t ACK_TIMEOUT_MS = 500;
  /// Most unawaited commands (`rx_on`, `rx_scan`, `idle`) in flight at once. The hub sends them from its own
  /// passes and drains the link only between passes: one pass sends at most four (a hop, a return to receive,
  /// a stop, a scan start), and twice that covers two passes without a drain.
  static constexpr size_t PENDING_DEPTH = 8;
  /// After a refused `rx_scan` or `rx_on` the driver does not send it again for this long: a refusal
  /// means the co-processor and the profile disagree, which a retry does not fix.
  static constexpr uint32_t SCAN_RETRY_MS = 5000;
  /// Margin on top of a transmission's own duration before send_packet() gives up on `tx_done`.
  static constexpr uint32_t TX_DONE_MARGIN_MS = 100;
  /// Bytes kept of the co-processor's firmware and board names.
  static constexpr size_t TEXT_CAP = 40;
  /// Bytes read from the stream at a time by the drain.
  static constexpr size_t DRAIN_CHUNK = 64;
  /// Longest time one drain spends reading before it returns (µs). The hub's loop must return whatever
  /// a message costs: a busy neighbour can make the co-processor report a message per millisecond, and
  /// handling each one (a trace line blocks on the log UART) must not hold the loop task past the task
  /// watchdog. The budget is checked between reads, so one chunk of messages may overshoot it.
  static constexpr uint32_t DRAIN_BUDGET_US = 10000;
  /// How far past its deadline a wait keeps draining while input is still unread before it times out
  /// regardless, so a stream that never ends cannot hold a wait forever: two budgets.
  static constexpr uint32_t DRAIN_TIMEOUT_GRACE_MS = 2 * DRAIN_BUDGET_US / 1000;
  /// Least time between two warnings that drains are being cut short.
  static constexpr uint32_t DRAIN_CUT_WARN_INTERVAL_MS = 60000;
  /// Room for the TLVs of one configuration command: the 868 MHz profile's eleven take 48 bytes with the
  /// profile and the TX power.
  static constexpr size_t CONFIG_TLV_CAP = 64;

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
  /// Sends one `tx` (`then_rx`: the radio returns to receive by itself) and waits for its `tx_done`. The receiver runs
  /// when it returns, whatever the result: after a failed send the driver tells the radio to receive.
  TxResult send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) override;
  bool wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) override;
  bool check_for_packet(RadioRxPacket &packet) override;
  /// `get_rssi`: a fresh reading of the frequency the receiver is on (specification §3.1). An idle radio is
  /// told to receive first. Nothing when the link is down, a scan runs (a reading during a scan cannot be attributed
  /// to a channel), the co-processor answers `busy` (its own transmission, or a retune waiting behind a frame), or the
  /// reply does not come within ACK_TIMEOUT_MS (a command without a reply: the link is resynced, as for every other
  /// command), or the newest retune it depends on was refused (the radio then stays where it was, and the level read
  /// is of that channel, not of the one asked for); a `radio_fault` resets the link as it does wherever it comes. The
  /// hub does not ask again: the attempt that wanted the reading fails (ADR 0134).
  std::optional<int16_t> read_rssi() override;
  /// True unless the newest command of this driver already is a forced `rx_on` to @p freq_hz that the radio
  /// receives on: the recorded frequency says nothing about where the receiver is, because a scan hides its channel
  /// and an unforced retune to this very frequency may still wait behind a frame on the old one (spec §2.2), where a
  /// reading would be refused as `busy`. A forced `rx_on` replaces that waiting retune, so it is sent once per channel
  /// check, before its first reading, and again after any hop, scan, `idle` or send in between (ADR 0134); the
  /// command ids are compared, and a command sent since clears the check's id, so a wrapped id never stands in for it.
  [[nodiscard]] bool reading_needs_retune(uint32_t freq_hz) const override {
    bool const checking_here = this->reading_cmd_ != 0 && this->reading_cmd_ == this->newest_receive_cmd_ &&
                               this->rx_on_ && !this->scanning_ && freq_hz == this->current_freq_;
    return !checking_here;
  }
  bool is_sync_detected() override;
  bool is_preamble_detected() override;
  [[nodiscard]] bool reception_in_progress() override;
  /// Starts the profile's `rx_scan` once and returns true while it runs; false during the hold-off after a
  /// refusal, when the hub hops from the host instead (the radio is kept receiving for that).
  bool run_idle_scan() override;
  /// Records the frequency of an idle radio. A receiving radio gets an `rx_on` for a new frequency; a scanning
  /// one gets it for any, which ends the scan.
  void change_frequency(uint32_t freq_hz) override;
  /// Retunes with a forced `rx_on` (spec §3.1), sent even when the recorded frequency is @p freq_hz: the
  /// co-processor retunes at once, drops a frame in progress on another frequency (a preamble held for
  /// `sync_timeout_us` included) as `rx_abort(forced_retune)` and replaces an earlier retune that still waits, so
  /// the reading that follows is not refused as `busy`. Every other retune of this driver waits for the frame.
  void retune_for_reading(uint32_t freq_hz) override;
  /// `rx_on` on the current frequency; ends a scan.
  void set_mode_rx() override;
  /// `idle`; a frame being received is dropped without an `rx_abort`.
  void set_mode_standby() override;

  /// The `response_preamble_ms` tuning in programmed bytes of this band's radio (a profile with no per-packet
  /// preamble ignores it).
  [[nodiscard]] uint16_t response_preamble() const override {
    return this->preamble_bytes_for_ms(this->tuning_.response_preamble_ms);
  }
  /// The band the profile describes: where the channels lie on air and everything the hub's rules read from it.
  [[nodiscard]] const IoBand &band() const override { return this->phy_.band(); }
  /// Zero: the co-processor holds the reply to a `get_rssi` until the receiver has been listening on the new
  /// frequency, so the hub adds no settle of its own (specification §3.1).
  [[nodiscard]] uint32_t cca_settle_us() const override { return 0; }
  /// The profile's bit periods per preamble byte.
  [[nodiscard]] uint8_t preamble_bits_per_byte() const override { return this->phy_.preamble_bits_per_byte(); }
  /// Keeps the tuning the profile reads. When it changes a param of the radio's profile (the receiver
  /// bandwidth of a `gfsk_raw` radio) and the link is up, the radio-scope params go out again; the hub
  /// calls this between exchanges, so the command never lands inside this driver's own transmission
  /// (which the co-processor would answer `busy`).
  void apply_tuning(const TuningConfig &tuning) override;
  /// Whether the radio can send the wake-up @p tuning asks for: on a band whose wake-up is a long preamble
  /// (`low_power_wake_ms`), it must fit the radio's `max_preamble_bytes`. A wake-up is never shortened to fit; a
  /// tuning that does not fit is refused with a warning naming both numbers. Before the radio is chosen there is no
  /// limit to check against, so it accepts.
  [[nodiscard]] bool accepts_tuning(const TuningConfig &tuning) const override;
  [[nodiscard]] uint16_t hop_dwell_ms(const TuningConfig &tuning) const override {
    return this->phy_.hop_dwell_ms(tuning);
  }
  /// True: the co-processor's radio returns to receive by itself after a transmission (`then_rx`).
  [[nodiscard]] bool has_fast_tx_rx_turnaround() const override { return true; }
  [[nodiscard]] uint32_t tx_air_time_us(uint8_t len, const RadioTxConfig &cfg) const override;
  [[nodiscard]] const char *chip_name() const override { return "bridle"; }
  void dump_debug() override;

  /// Log every message sent and received as a trace line (spec §12.1), the recording format of
  /// session playback. The lines go through the frame log's hold.
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
  /// Where a trace line goes: `bridle::log_trace_line()`, the frame log's hold; a test overrides it to
  /// collect the lines. The line starts with the `[bridle]` marker, which readers look for after whatever
  /// prefix the log adds.
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
  /// The driver's own record of what the radio does, kept to put it back when a command is refused.
  struct RadioState {
    uint32_t freq_hz{0};            ///< Frequency the radio is tuned to (`current_freq_`).
    bool scanning{false};           ///< An `rx_scan` runs (`scanning_`).
    bool rx_on{false};              ///< The radio receives or scans (`rx_on_`).
    uint8_t newest_receive_cmd{0};  ///< Command whose refusal still describes the radio (`newest_receive_cmd_`).
  };
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
  /// Read the stream and route what it holds, until it is empty or `DRAIN_BUDGET_US` is spent. Sets
  /// `drain_cut_` to the result.
  /// @return true when the stream was emptied, false when the drain was cut short with input unread.
  bool drain_();
  /// Whether a wait that started at `start_ms` and may last `timeout_ms` has run out: the deadline has
  /// passed and either the last drain emptied the stream or `DRAIN_TIMEOUT_GRACE_MS` has also passed.
  [[nodiscard]] bool wait_expired_(uint32_t start_ms, uint32_t timeout_ms) const;
  /// Whether messages are queuing up on the host: the last drain was cut short, or the running one has
  /// spent its budget. A gap in the event counter then comes from the host falling behind.
  [[nodiscard]] bool host_backlog_() const;
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
  /// `cmd_seq` 0 (a radio fault outside a transmission), an `error(radio_fault)` that answers a command, or
  /// `not_configured` for any command, means the co-processor lost its configuration, so resync.
  void route_reply_(const bridle::Message &m, const uint8_t *msg, size_t len, uint8_t seq);
  /// An `error(radio_fault)`, unsolicited (@p seq 0) or answering command @p seq whether or not that command is still
  /// awaited: hands the waiting caller the refusal and starts the resync.
  /// @param fault The error.
  /// @param msg   The decoded message, kept for the waiting caller.
  /// @param len   Its length.
  /// @param seq   The `cmd_seq` it carries.
  void on_radio_fault_(const bridle::Error &fault, const uint8_t *msg, size_t len, uint8_t seq);
  /// End the running wait with @p error as the answer to the awaited command: what await_() reports as Wait::ERROR.
  /// @param error The `error` that answers it.
  /// @param msg   The decoded message, kept for the waiting caller.
  /// @param len   Its length.
  void end_await_with_error_(const bridle::Error &error, const uint8_t *msg, size_t len);
  void on_awaited_reply_(const bridle::Message &m, const uint8_t *msg, size_t len);
  void route_event_(const bridle::Body &body);
  void show_log_(const bridle::Log &log);
  /// Wait for the reply of type `expected` to command `seq`, draining as it waits.
  /// The wait ends on that reply or on an `error` for `seq`, whichever comes first.
  Wait await_(uint8_t seq, bridle::MsgType expected, uint32_t timeout_ms);

  // === Handshake and configuration ===

  bool handshake_();
  /// The one fit rule of the wake-up: whether the preamble of @p tuning's `low_power_wake_ms` is at most
  /// @p max_preamble_bytes. @p needed_bytes receives the preamble in programmed bytes (0 on a band whose wake-up is a
  /// strobe, which always fits).
  [[nodiscard]] bool wake_fits_(const TuningConfig &tuning, uint16_t max_preamble_bytes, uint32_t &needed_bytes) const;
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
  /// The one path of an awaited command whose reply did not come within @p waited_ms (a `tx` without its `tx_done`,
  /// a `get_rssi` without its `rssi`): a command or reply the link lost, so the link resyncs (spec §7). @p why names
  /// the missing reply ("no tx_done") in the log line and as the resync's reason. Nothing when the wait ended
  /// because the link already went down: that is being handled.
  void on_no_reply_(const char *why, uint32_t waited_ms);
  /// Keep the counters a heartbeat's `stats` reply carries for `dump_debug()`.
  void keep_heartbeat_stats_(const bridle::Stats &stats);
  void set_link_(Link state, const char *why);
  void attempt_failed_(const char *why);
  void finish_resync_();
  [[nodiscard]] bool link_lost_() const { return this->link_ != Link::INIT && this->link_ != Link::RUNNING; }

  // === Transmit ===

  /// The transmission itself: wrap the frame, send the `tx` and wait for its `tx_done`. send_packet() adds the
  /// receiver a failed send must leave running.
  TxResult transmit_(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config);

  // === Receive path ===

  void on_rx_(const bridle::Rx &rx);
  bool pop_packet_(RadioRxPacket &packet);
  bool decode_entry_(const RxEntry &entry, RadioRxPacket &packet);
  void ensure_rx_();
  /// True while a refused `rx_on` is not sent again (SCAN_RETRY_MS after the refusal).
  [[nodiscard]] bool rx_retry_held_() const;
  /// The retune of change_frequency() and retune_for_reading(): `rx_on` on a receiving or scanning radio, with
  /// @p force the frame in progress dropped at once; with @p force also on an idle radio.
  void retune_(uint32_t freq_hz, bool force);

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
  bool radio_chosen_{false};              ///< A radio fits the band and radio_desc_ describes it.
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
  RadioState tx_restore_{};        ///< What a refused `tx` puts back; kept current while the `tx` is awaited.
  bool tx_restore_active_{false};  ///< A `tx` is awaited, so a refusal of an older command lands in `tx_restore_`.
  uint8_t reading_cmd_{0};         ///< Command id of the forced `rx_on` of the current channel check (0 = none).
  uint8_t newest_receive_cmd_{0};  ///< Command id of the last rx_on, rx_scan, idle or tx sent (0 = none).
  uint8_t refused_cmd_{0};      ///< Command id of the last unawaited command refused; cleared when a newer one is sent.
  uint16_t refusal_logged_{0};  ///< Bit per error code already logged for a refused unawaited command.
  bool scan_refused_{false};    ///< An rx_scan was refused; not sent again before SCAN_RETRY_MS.
  uint32_t scan_refused_ms_{0};   ///< millis() of the rx_scan refusal.
  bool rx_refused_{false};        ///< An rx_on was refused; not sent again before SCAN_RETRY_MS.
  uint32_t rx_refused_ms_{0};     ///< millis() of the rx_on refusal.
  uint32_t unawaited_errors_{0};  ///< Errors received for unawaited commands.

  // Link supervision.
  Link link_{Link::INIT};
  bool trace_{false};
  bool drain_cut_{false};             ///< The last drain stopped at its budget with input unread.
  bool draining_{false};              ///< A drain is running (for host_backlog_()).
  uint32_t drain_start_us_{0};        ///< micros() at the start of the running drain.
  uint32_t drains_cut_{0};            ///< Drains cut short, for dump_debug().
  uint32_t last_cut_warn_ms_{0};      ///< millis() of the last cut-short warning.
  bool cut_warned_{false};            ///< A cut-short warning was logged (last_cut_warn_ms_ is valid).
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
  uint32_t rx_short_{0};  ///< Last `stats.rx_short` a heartbeat returned: refreshed with each `get_stats` reply.
  bool have_rx_short_{false};

  // Counters for dump_debug().
  uint32_t dropped_before_tx_done_{0};
  uint32_t fifo_overflows_{0};
  uint32_t oversize_dropped_{0};
  uint32_t header_mismatches_{0};
  uint32_t bad_crc_frames_{0};
  uint32_t short_frames_{0};
  uint32_t stale_replies_{0};
  uint32_t rssi_busy_{0};      ///< `get_rssi` answered `busy`: no reading, the attempt that asked fails.
  uint32_t rssi_no_reply_{0};  ///< `get_rssi` that got no reply within ACK_TIMEOUT_MS.
  uint32_t rssi_refused_{0};   ///< `get_rssi` answered with another `error` (a fault resets the link as well), or its
                               ///< level was dropped because the retune before it was refused.
  uint32_t counter_gaps_{0};
  uint32_t strobe_late_total_{0};
  uint32_t rx_overflow_reports_{0};
  uint32_t log_events_{0};
  bool expected_counter_known_{false};
  uint8_t expected_counter_{0};
};

}  // namespace esphome::home_io_control

#endif  // IOHOME_BRIDLE
