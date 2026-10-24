#pragma once

/// @file fake_coprocessor.h
/// @brief A scripted Bridle co-processor for host tests, sitting behind a FakeByteStream.
///
/// It decodes the host's commands with the production codec and answers the way a conforming
/// co-processor does (Bridle specification §2.2–§7): `ready`, `params_ack` with clamping, `tx` → `tx_done`
/// after the modelled air time, `ack` for the commands that have nothing to report, `error` in the
/// specified check order, atomic `set_params`, the `busy` rule, the event counter. Message
/// directions and command validity come from the generated header, never from a list kept here. Faults are scriptable
/// (silence, a counter gap, a corrupt frame, an unsolicited `ready`, `log`, `rx_overflow`, a busy channel). Scenario
/// mode plays the events of a spec scenario (bridle_scenarios.h), rewriting the event counter and the answered
/// `cmd_seq` to what the driver under test actually sent.
///
/// Time is the manual test clock: events are released when their due time has passed, on every
/// poll of the stream (FakeByteStream ticks the clock on empty polls).

#include <esphome/core/hal.h>  // test_clock

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "fake_byte_stream.h"
#include "radio_io2g4_phy.h"
#include "radio_io868_gfsk_phy.h"
#include "bridle_scenarios.h"
#include "sessions_generated.h"
#include "timed_release.h"

class FakeCoprocessor {
 public:
  using RadioDescriptor = esphome::home_io_control::bridle::RadioDescriptor;

  /// What the fake reports in `ready`.
  struct Config {
    std::vector<RadioDescriptor> radios;  ///< Empty: one 802.15.4 radio as the reference co-processor has it.
    uint8_t proto_major{esphome::home_io_control::bridle::PROTO_MAJOR};
    uint8_t proto_minor{esphome::home_io_control::bridle::PROTO_MINOR};
    uint16_t chip_id{static_cast<uint16_t>(esphome::home_io_control::bridle::ChipId::NRF52840)};
    std::string fw_version{"fake-0.0"};
    std::string board{"fake-board"};
    bool with_fw_version{true};
    bool with_reset_cause{true};
    bool with_rx_queue_depth{true};
    /// A reply scripted `offset` after a transmission is anchored at the end of that transmission, not at the
    /// start of its main frame: the SPI drivers' captures stamp a frame once it is out, so a capture of an
    /// 868 MHz exchange replays this way; the 2.4 GHz captures stamp the main frame's start.
    bool anchor_replies_at_tx_end{false};
    esphome::home_io_control::bridle::ResetCause reset_cause{esphome::home_io_control::bridle::ResetCause::PIN};
  };

  /// A host command the fake received.
  struct RecordedCommand {
    uint64_t t_us{0};
    uint8_t type{0};
    uint8_t seq{0};
    std::vector<uint8_t> message;  ///< The decoded message, CRC included.
  };

  FakeCoprocessor(FakeByteStream &stream, Config config);
  explicit FakeCoprocessor(FakeByteStream &stream) : FakeCoprocessor(stream, Config{}) {}

  /// The reference 802.15.4 radio: nRF52840, the capabilities of the reference co-processor (`strobe`,
  /// `scheduled_tx`, `rx_scan`, `cca`), TX power -40…+8 dBm, 125-byte payloads.
  [[nodiscard]] static RadioDescriptor reference_802154_radio(uint8_t index = 0);
  /// The SX1262 behind a Kite: `gfsk_raw` in 863-870 MHz, TX power -9…+22 dBm, 147-byte payloads,
  /// preambles up to 8191 bytes, capabilities `rx_scan` and `cca`; no strobe, no scheduled transmission.
  [[nodiscard]] static RadioDescriptor reference_gfsk_radio(uint8_t index = 0);
  /// Both radios of a dual-band board: 802.15.4 as radio 0, `gfsk_raw` as radio 1 (spec vector `ready_dual_band`).
  [[nodiscard]] static std::vector<RadioDescriptor> dual_band_radios() {
    return {reference_802154_radio(0), reference_gfsk_radio(1)};
  }
  /// The longest preamble a `gfsk_raw` radio sends, in bytes.
  static constexpr uint16_t GFSK_MAX_PREAMBLE_BYTES = 8191;

  // === Scripting ===

  /// Emit the unsolicited `ready` a boot produces (event counter 0).
  void boot();
  /// A reboot in the middle of a session: state is lost, `ready` follows with `cause`.
  void reboot(esphome::home_io_control::bridle::ResetCause cause);
  /// Advertise another protocol minor from the next `ready` on (a co-processor re-flashed meanwhile).
  void set_advertised_minor(uint8_t minor) { config_.proto_minor = minor; }
  /// Swallow the next `ack` the fake would send, as a command or reply lost on the link would.
  void drop_next_ack() { drop_next_ack_ = true; }
  /// Refuse the next `rx_scan` with `error(code, detail)` (default: `param_range` naming `dwell_us`).
  void refuse_next_rx_scan(
      esphome::home_io_control::bridle::ErrorCode code = esphome::home_io_control::bridle::ErrorCode::PARAM_RANGE,
      uint16_t detail = esphome::home_io_control::bridle::field::rx_scan::DWELL_US) {
    reject_next(esphome::home_io_control::bridle::MsgType::RX_SCAN, code, detail);
  }
  /// Emit an `error` with `cmd_seq` 0, as a radio fault outside a transmission is reported.
  void inject_radio_fault(uint16_t detail = 0);
  /// Answer every command with `error(link_in_use)` while @p refusing, as a co-processor does on a link that is not its
  /// host link (spec §2.1). Commands are still recorded.
  void set_link_in_use(bool refusing) { link_in_use_ = refusing; }
  /// Take the next `hello` as a takeover of the host link: the protocol state restarts without a reboot and the
  /// `ready` that answers carries event counter 0 and `reset_cause` `link_takeover` (spec §2.1).
  void take_over_on_next_hello() { take_over_on_hello_ = true; }
  /// Stop answering (and emitting anything); commands are still recorded.
  void set_silent(bool silent) { silent_ = silent; }
  /// Skip `n` event-counter values before the next emitted event, as lost events would.
  void skip_event_counter(uint8_t n);
  /// Corrupt the CRC of the next frame emitted.
  void corrupt_next_frame() { corrupt_next_ = true; }
  /// Answer the next command of `type` with `error(code, detail)` and otherwise ignore it. With @p radio >= 0 only
  /// a command whose first body byte (the radio of `set_params`, `rx_on`, ...) is that value is refused, so a
  /// radio-scope `set_params` can be refused without the device-scope one before it.
  void reject_next(esphome::home_io_control::bridle::MsgType type, esphome::home_io_control::bridle::ErrorCode code,
                   uint16_t detail = 0, int radio = -1);
  /// The energy `tx` with `cca` measures (default -100 dBm: a quiet channel).
  void set_channel_level_dbm(int16_t level) { channel_level_dbm_ = level; }
  /// The energy the next clear-channel checks measure, one queued level per check; the default
  /// applies once the queue is empty.
  void queue_channel_level_dbm(int16_t level) { channel_levels_.push_back(level); }
  /// Called for every accepted transmission with its payload (PSDU without FCS) and the manual-clock
  /// times its main frame starts and the transmission ends. A stand-in uses it to answer a frame.
  void set_tx_observer(std::function<void(const std::vector<uint8_t> &, uint64_t, uint64_t)> observer) {
    tx_observer_ = std::move(observer);
  }
  /// Report this many late strobe frames in the next `tx_done`.
  void set_next_strobe_late(uint16_t n) { next_strobe_late_ = n; }
  /// The RSSI a `get_rssi` reports while the radio receives.
  void set_rssi_dbm(int8_t rssi) { rssi_dbm_ = rssi; }
  /// Queue a received frame (payload including FCS) for `radio` at the current time plus `delay_us`.
  /// Events the host has not enabled (`frame_start`, a bad-CRC `rx`, `rx_abort`) are dropped, as a
  /// real co-processor does.
  void inject_rx(uint32_t freq_hz, const std::vector<uint8_t> &payload,
                 esphome::home_io_control::bridle::CrcState crc = esphome::home_io_control::bridle::CrcState::OK,
                 uint64_t delay_us = 0, uint8_t radio = 0);
  void inject_frame_start(uint32_t freq_hz, esphome::home_io_control::bridle::FrameStage stage, uint64_t delay_us = 0,
                          uint8_t radio = 0);
  void inject_rx_abort(uint32_t freq_hz, esphome::home_io_control::bridle::RxAbortReason reason, uint64_t delay_us = 0,
                       uint8_t radio = 0);
  void inject_log(esphome::home_io_control::bridle::LogLevel level, const std::string &text, uint64_t delay_us = 0);
  void inject_rx_overflow(uint16_t dropped, uint64_t delay_us = 0);
  /// Emit an arbitrary event after `delay_us` (event counter and, for a reply, `cmd_seq` are up to the caller).
  void inject_event(const esphome::home_io_control::bridle::Body &body, uint64_t delay_us = 0);
  /// A timed reply the way MockRadio releases one: `offset_us` after the main-frame start of the
  /// `send_index`-th transmission (0-based).
  void inject_rx_after_send(int send_index, uint64_t offset_us, uint32_t freq_hz, const std::vector<uint8_t> &payload);
  /// The same anchored at the first transmission whose payload equals `anchor_payload`.
  void inject_rx_after_bytes(const std::vector<uint8_t> &anchor_payload, uint64_t offset_us, uint32_t freq_hz,
                             const std::vector<uint8_t> &payload);

  // === Scenario mode ===

  /// Queue the events of `scenario`, each due `t0_us` + its scenario time, and let the scenario
  /// supply every reply from now on (the fake still tracks state and records commands, but answers
  /// none itself). A reply is released once the driver has sent the command its host step stands
  /// for (the n-th command of that type since now), and carries that command's `seq`.
  void play_scenario(const esphome::home_io_control::bridle::Scenario &scenario, uint64_t t0_us);

  // === Playback mode ===

  /// Replay a recorded session: every co-processor message of the recording is emitted, in order,
  /// anchored to the recorded host command it followed and released at that command's actual
  /// arrival time plus the recorded gap (messages before the first command: `now` plus their gap).
  /// A reply carries the `seq` of the command it answered, the event counter is the fake's own.
  /// Every command the host sends is compared with the recording by type and body (never `seq`
  /// or CRC: a host that retries a different number of times shifts every later `seq`); a
  /// difference is kept in @ref playback_errors. Like a scenario, a session supplies every reply.
  void play_session(const esphome::home_io_control::bridle::Session &session);
  /// What differed from the recording, one line each with both messages; empty while it matches.
  [[nodiscard]] const std::vector<std::string> &playback_errors() const { return playback_errors_; }
  /// Recorded co-processor messages not emitted yet.
  [[nodiscard]] size_t playback_events_pending() const { return playback_.size(); }
  /// Every recorded message has been emitted and the host has sent every recorded command.
  [[nodiscard]] bool playback_finished() const {
    return playback_.empty() && commands_.size() - playback_commands_before_ >= playback_commands_.size();
  }

  // === What the host did ===

  [[nodiscard]] const std::vector<RecordedCommand> &commands() const { return commands_; }
  /// The recorded commands of one message type, in order.
  [[nodiscard]] std::vector<const RecordedCommand *> commands_of(esphome::home_io_control::bridle::MsgType type) const;
  /// Every transmission's payload (PSDU without FCS) with the manual-clock time its main frame started.
  [[nodiscard]] const SendTimeline &send_timeline() const { return timeline_; }
  /// The frames the deframer dropped (bad COBS, CRC or length).
  [[nodiscard]] uint32_t link_errors() const { return deframer_.counters().total(); }
  /// Whether the fake is transmitting at the current time.
  /// (Never in scenario or playback mode: the recording says when a transmission ended.)
  [[nodiscard]] bool transmitting() const { return !scenario_mode_ && now_us() < tx_busy_until_us_; }
  /// Radio 0's current frequency, and whether it is receiving or scanning.
  [[nodiscard]] uint32_t freq_hz() const { return radios_.empty() ? 0 : radios_[0].freq_hz; }
  [[nodiscard]] bool receiving() const { return !radios_.empty() && radios_[0].mode != Mode::IDLE; }
  [[nodiscard]] bool scanning() const { return !radios_.empty() && radios_[0].mode == Mode::SCAN; }
  [[nodiscard]] uint8_t event_mask() const { return event_mask_; }
  /// How many `error` replies the fake has sent, by code.
  [[nodiscard]] uint32_t errors_sent(esphome::home_io_control::bridle::ErrorCode code) const {
    return errors_sent_[static_cast<uint8_t>(code) & 0x0F];
  }
  /// Whether a set_params landed during a transmission (it must have been answered `busy`).
  [[nodiscard]] uint32_t set_params_during_tx() const { return set_params_during_tx_; }
  /// The last params_ack's applied TX power for radio `radio`.
  [[nodiscard]] int8_t tx_power_dbm(uint8_t radio = 0) const { return radios_[radio].tx_power_dbm; }
  /// The scan radio 0 was last given.
  [[nodiscard]] const esphome::home_io_control::bridle::RxScan &scan() const { return radios_[0].scan; }

  static uint64_t now_us() { return esphome::test_clock::state().now_us; }

 private:
  enum class Mode : uint8_t { IDLE, RX, SCAN };
  struct RadioState {
    bool profile_set{false};
    esphome::home_io_control::bridle::PhyProfile profile{
        esphome::home_io_control::bridle::PhyProfile::IEEE802154_OQPSK_250K};
    bool sfd_set{false};
    uint8_t gfsk_set{0};  ///< Which of the four required `gfsk_raw` params were set since the profile: bits 0-3.
    uint32_t freq_hz{0};
    Mode mode{Mode::IDLE};
    int8_t tx_power_dbm{0};
    esphome::home_io_control::bridle::RxScan scan{};
  };
  /// One message waiting for its time.
  struct Pending {
    uint64_t due_us{0};
    std::vector<uint8_t> message;  ///< Decoded, CRC included; counter and reply seq filled in on release.
    // Scenario replies: the host step they stand for.
    const esphome::home_io_control::bridle::Scenario *scenario{nullptr};
    int host_step{-1};
    size_t commands_before{0};
    uint8_t skip{0};  ///< A message-less entry that skips this many counter values when it comes due.
  };

  void pump_();
  /// Emit every queued message that is due.
  void drain_due_();
  void on_host_bytes_(const uint8_t *data, size_t n);
  void handle_(const esphome::home_io_control::bridle::Message &msg);
  void error_(uint8_t cmd_seq, esphome::home_io_control::bridle::ErrorCode code, uint16_t detail = 0);
  void queue_(const esphome::home_io_control::bridle::Body &body, uint64_t due_us);
  void queue_ready_(uint8_t cmd_seq, uint64_t due_us);
  void reset_state_();
  /// Whether `r` has every param its profile requires before `tx` and `rx_on` (spec §4).
  [[nodiscard]] static bool configured_(const RadioState &r);
  void handle_set_params_(const esphome::home_io_control::bridle::SetParams &cmd, uint8_t seq);
  /// Queue an `ack` for command `seq` (unless the script drops it).
  void ack_(uint8_t seq);
  /// The first failure of a staged `set_params`, or nothing when every TLV passes.
  struct ParamFailure {
    esphome::home_io_control::bridle::ErrorCode code{esphome::home_io_control::bridle::ErrorCode::PARAM_RANGE};
    uint16_t detail{0};
  };
  void handle_tx_(const esphome::home_io_control::bridle::Tx &cmd, uint8_t seq);
  bool resolve_scenario_reply_(const Pending &event, uint8_t &seq) const;
  void pump_playback_();
  void emit_(std::vector<uint8_t> message);
  void compare_playback_command_();
  void insert_(Pending event);
  void release_timed_();
  [[nodiscard]] const RadioDescriptor *radio_(uint8_t index) const;

  FakeByteStream &stream_;
  Config config_;
  std::vector<RadioDescriptor> descriptors_;
  std::vector<RadioState> radios_;
  esphome::home_io_control::bridle::Deframer deframer_;
  std::vector<Pending> pending_;
  std::vector<RecordedCommand> commands_;
  SendTimeline timeline_;
  uint8_t counter_{0};
  uint8_t event_mask_{0};
  bool silent_{false};
  bool link_in_use_{false};
  bool take_over_on_hello_{false};
  bool scenario_mode_{false};  ///< A scenario supplies the replies; the handlers only track state.
  bool in_handler_{false};
  bool corrupt_next_{false};
  bool drop_next_ack_{false};
  bool reboot_after_drain_{false};  ///< A soft reset waits until the `ack` and everything due before it is out.
  uint32_t errors_sent_[16]{};
  int16_t channel_level_dbm_{-100};
  uint16_t next_strobe_late_{0};
  int8_t rssi_dbm_{-60};
  uint64_t tx_busy_until_us_{0};
  uint32_t set_params_during_tx_{0};
  uint32_t tx_ok_{0};
  struct Rejection {
    uint8_t type;
    esphome::home_io_control::bridle::ErrorCode code;
    uint16_t detail;
    int radio;  ///< Only commands whose first body byte is this; -1 = any.
  };
  std::vector<Rejection> rejections_;
  std::deque<int16_t> channel_levels_;
  std::function<void(const std::vector<uint8_t> &, uint64_t, uint64_t)> tx_observer_;
  /// A reply anchored at a transmission that has not happened yet.
  struct TimedRx {
    TimedRelease anchor;
    uint32_t freq_hz;
    std::vector<uint8_t> payload;
  };
  std::vector<TimedRx> timed_;

  /// One co-processor message of a recorded session.
  struct PlaybackEvent {
    std::vector<uint8_t> message;
    int anchor_command{-1};    ///< The recorded host command it followed (-1: before any).
    uint64_t gap_us{0};        ///< Recorded gap after that command (or after the session's first message).
    int reply_to_command{-1};  ///< The recorded host command it answers, when it is a reply.
  };
  std::deque<PlaybackEvent> playback_;
  std::vector<std::vector<uint8_t>> playback_commands_;  ///< The recording's host commands, in order.
  size_t playback_commands_before_{0};
  uint64_t playback_start_us_{0};
  bool playback_active_{false};
  std::vector<std::string> playback_errors_;
};
