/// @file fake_coprocessor.cpp
/// @brief A scripted Bridle co-processor for host tests (see fake_coprocessor.h).

#include "fake_coprocessor.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include "proto_frame.h"

using namespace esphome::home_io_control::bridle;

namespace {

/// A large enough buffer for any TLV list the fake builds.
constexpr size_t TLV_BUFFER = 256;

/// The bit number of a single-bit capability mask: what `unsupported` names for a missing capability.
constexpr uint16_t cap_bit(uint32_t mask) { return static_cast<uint16_t>(std::countr_zero(mask)); }

/// The preamble detector widths a `gfsk_raw` radio takes (spec §4).
constexpr bool valid_preamble_detect_bits(uint8_t bits) { return bits == 8 || bits == 16 || bits == 24 || bits == 32; }

/// The event-mask bit that admits an event type.
constexpr uint8_t EVENT_MASK_FRAME_START = event_mask::FRAME_START;

}  // namespace

FakeCoprocessor::FakeCoprocessor(FakeByteStream &stream, Config config) : stream_(stream), config_(std::move(config)) {
  descriptors_ = config_.radios.empty() ? std::vector<RadioDescriptor>{reference_802154_radio(0)} : config_.radios;
  reset_state_();
  stream_.set_pump([this] { pump_(); });
  stream_.set_write_handler([this](const uint8_t *data, size_t n) { on_host_bytes_(data, n); });
}

FakeCoprocessor::RadioDescriptor FakeCoprocessor::reference_802154_radio(uint8_t index) {
  RadioDescriptor d;
  d.radio = index;
  d.chip = static_cast<uint16_t>(ChipId::NRF52840);
  d.profiles = phy_profiles::IEEE802154_OQPSK_250K;
  d.band_min_hz = 2400000000u;
  d.band_max_hz = 2483500000u;
  d.tx_power_min_dbm = -40;
  d.tx_power_max_dbm = 8;
  d.max_payload = 125;
  d.max_preamble_bytes = 0;
  d.strobe_jitter_us = 2;
  d.caps = radio_caps::STROBE | radio_caps::SCHEDULED_TX | radio_caps::RX_SCAN | radio_caps::CCA;
  return d;
}

FakeCoprocessor::RadioDescriptor FakeCoprocessor::reference_gfsk_radio(uint8_t index) {
  RadioDescriptor d;
  d.radio = index;
  d.chip = static_cast<uint16_t>(ChipId::SX1262);
  d.profiles = phy_profiles::GFSK_RAW;
  d.band_min_hz = 863000000u;
  d.band_max_hz = 870000000u;
  d.tx_power_min_dbm = -9;
  d.tx_power_max_dbm = 22;
  d.max_payload = 147;
  d.max_preamble_bytes = GFSK_MAX_PREAMBLE_BYTES;
  d.strobe_jitter_us = 0;
  d.caps = radio_caps::RX_SCAN | radio_caps::CCA;
  return d;
}

bool FakeCoprocessor::configured_(const RadioState &r) {
  constexpr uint8_t ALL_GFSK_REQUIRED = 0x0F;  // bit rate, deviation, sync word, rx_len
  if (!r.profile_set)
    return false;
  return r.profile == PhyProfile::GFSK_RAW ? r.gfsk_set == ALL_GFSK_REQUIRED : r.sfd_set;
}

const FakeCoprocessor::RadioDescriptor *FakeCoprocessor::radio_(uint8_t index) const {
  for (const RadioDescriptor &d : descriptors_) {
    if (d.radio == index)
      return &d;
  }
  return nullptr;
}

void FakeCoprocessor::reset_state_() {
  radios_.assign(descriptors_.size(), RadioState{});
  for (size_t i = 0; i < radios_.size(); i++)
    radios_[i].tx_power_dbm = descriptors_[i].tx_power_max_dbm;
  event_mask_ = 0;  // every boot clears it (spec §4)
  counter_ = 0;
  tx_busy_until_us_ = 0;
  pending_.clear();
}

// === Scripting ===

void FakeCoprocessor::boot() {
  reset_state_();
  queue_ready_(0, now_us());
}

void FakeCoprocessor::reboot(ResetCause cause) {
  config_.reset_cause = cause;
  boot();
}

void FakeCoprocessor::inject_radio_fault(uint16_t detail) {
  Error fault;
  fault.cmd_seq = 0;
  fault.code = ErrorCode::RADIO_FAULT;
  fault.detail = detail;
  queue_(fault, now_us());
}

void FakeCoprocessor::skip_event_counter(uint8_t n) {
  Pending p;
  p.due_us = now_us();
  p.skip = n;
  insert_(std::move(p));
}

void FakeCoprocessor::reject_next(MsgType type, ErrorCode code, uint16_t detail, int radio) {
  rejections_.push_back({static_cast<uint8_t>(type), code, detail, radio});
}

void FakeCoprocessor::inject_rx(uint32_t freq_hz, const std::vector<uint8_t> &payload, CrcState crc, uint64_t delay_us,
                                uint8_t radio) {
  if (radio >= radios_.size() || radios_[radio].mode == Mode::IDLE)
    return;  // a radio that is not receiving reports nothing
  if (crc == CrcState::BAD && (event_mask_ & event_mask::CRC_ERROR_RX) == 0)
    return;
  Rx rx;
  rx.radio = radio;
  rx.t_us = static_cast<uint32_t>(now_us() + delay_us);
  rx.freq_hz = freq_hz;
  rx.rssi_dbm = rssi_dbm_;
  rx.crc = crc;
  rx.payload = {payload.data(), payload.size()};
  queue_(rx, now_us() + delay_us);
}

void FakeCoprocessor::inject_frame_start(uint32_t freq_hz, FrameStage stage, uint64_t delay_us, uint8_t radio) {
  if ((event_mask_ & EVENT_MASK_FRAME_START) == 0)
    return;
  FrameStart fs;
  fs.radio = radio;
  fs.t_us = static_cast<uint32_t>(now_us() + delay_us);
  fs.freq_hz = freq_hz;
  fs.stage = stage;
  queue_(fs, now_us() + delay_us);
}

void FakeCoprocessor::inject_rx_abort(uint32_t freq_hz, RxAbortReason reason, uint64_t delay_us, uint8_t radio) {
  if ((event_mask_ & event_mask::RX_ABORT) == 0)
    return;
  RxAbort abort;
  abort.radio = radio;
  abort.t_us = static_cast<uint32_t>(now_us() + delay_us);
  abort.freq_hz = freq_hz;
  abort.reason = reason;
  queue_(abort, now_us() + delay_us);
}

void FakeCoprocessor::inject_log(LogLevel level, const std::string &text, uint64_t delay_us) {
  if ((event_mask_ & event_mask::LOG) == 0)
    return;
  Log log;
  log.level = level;
  log.text = {reinterpret_cast<const uint8_t *>(text.data()), text.size()};
  queue_(log, now_us() + delay_us);
}

void FakeCoprocessor::inject_rx_overflow(uint16_t dropped, uint64_t delay_us) {
  RxOverflow overflow;
  overflow.dropped = dropped;
  queue_(overflow, now_us() + delay_us);
}

void FakeCoprocessor::inject_event(const Body &body, uint64_t delay_us) { queue_(body, now_us() + delay_us); }

void FakeCoprocessor::inject_rx_after_send(int send_index, uint64_t offset_us, uint32_t freq_hz,
                                           const std::vector<uint8_t> &payload) {
  TimedRelease anchor;
  anchor.anchor_send_index = send_index;
  anchor.offset_us = offset_us;
  timed_.push_back({anchor, freq_hz, payload});
}

void FakeCoprocessor::inject_rx_after_bytes(const std::vector<uint8_t> &anchor_payload, uint64_t offset_us,
                                            uint32_t freq_hz, const std::vector<uint8_t> &payload) {
  TimedRelease anchor;
  anchor.anchor_bytes = anchor_payload;
  anchor.offset_us = offset_us;
  timed_.push_back({anchor, freq_hz, payload});
}

void FakeCoprocessor::play_scenario(const Scenario &scenario, uint64_t t0_us) {
  scenario_mode_ = true;
  for (size_t i = 0; i < scenario.event_count; i++) {
    const ScenarioEvent &event = scenario.events[i];
    Pending p;
    p.due_us = t0_us + event.t_us;
    p.message.assign(event.message, event.message + event.message_len);
    if (event.answers_host_step >= 0) {
      p.scenario = &scenario;
      p.host_step = event.answers_host_step;
      p.commands_before = commands_.size();
    }
    insert_(std::move(p));
  }
}

void FakeCoprocessor::play_session(const Session &session) {
  scenario_mode_ = true;  // the recording supplies every reply
  playback_active_ = true;
  playback_start_us_ = now_us();
  playback_commands_before_ = commands_.size();
  playback_.clear();
  playback_commands_.clear();
  std::vector<uint32_t> command_ms;
  uint32_t const first_ms = session.message_count > 0 ? session.messages[0].host_ms : 0;
  for (size_t i = 0; i < session.message_count; i++) {
    const SessionMessage &m = session.messages[i];
    std::vector<uint8_t> bytes(m.bytes, m.bytes + m.len);
    if (m.from_host) {
      playback_commands_.push_back(bytes);
      command_ms.push_back(m.host_ms);
      continue;
    }
    PlaybackEvent event;
    event.anchor_command = static_cast<int>(playback_commands_.size()) - 1;
    uint32_t const anchor_ms = event.anchor_command >= 0 ? command_ms.back() : first_ms;
    event.gap_us = static_cast<uint64_t>(m.host_ms - anchor_ms) * 1000u;
    // A reply's first body byte is the cmd_seq of the command it answers (0: it answers none, as a boot `ready`).
    if (is_reply(bytes[0]) && bytes[2] != 0) {
      for (int c = static_cast<int>(playback_commands_.size()) - 1; c >= 0; c--) {
        if (playback_commands_[c][1] == bytes[2]) {
          event.reply_to_command = c;
          break;
        }
      }
    }
    event.message = std::move(bytes);
    playback_.push_back(std::move(event));
  }
}

void FakeCoprocessor::pump_playback_() {
  while (!playback_.empty()) {
    PlaybackEvent &event = playback_.front();
    uint64_t due = playback_start_us_ + event.gap_us;
    if (event.anchor_command >= 0) {
      size_t const index = playback_commands_before_ + static_cast<size_t>(event.anchor_command);
      if (index >= commands_.size())
        return;  // the host has not sent the command this message followed
      due = commands_[index].t_us + event.gap_us;
    }
    if (due > now_us())
      return;
    std::vector<uint8_t> msg = std::move(event.message);
    if (event.reply_to_command >= 0) {
      size_t const index = playback_commands_before_ + static_cast<size_t>(event.reply_to_command);
      if (index >= commands_.size())
        return;
      msg[2] = commands_[index].seq;
    }
    playback_.pop_front();
    if (silent_)
      continue;
    emit_(std::move(msg));
  }
}

void FakeCoprocessor::compare_playback_command_() {
  size_t const n = commands_.size() - 1 - playback_commands_before_;
  const std::vector<uint8_t> &got = commands_.back().message;
  auto describe = [](const std::vector<uint8_t> &m) {
    static constexpr char DIGITS[] = "0123456789abcdef";
    std::string text = "type 0x";
    text += DIGITS[m[0] >> 4];
    text += DIGITS[m[0] & 0x0F];
    text += " body ";
    for (size_t i = 2; i + 2 < m.size(); i++) {
      text += DIGITS[m[i] >> 4];
      text += DIGITS[m[i] & 0x0F];
    }
    return text;
  };
  if (n >= playback_commands_.size()) {
    playback_errors_.push_back("command " + std::to_string(n) + " is not in the recording: " + describe(got));
    return;
  }
  const std::vector<uint8_t> &want = playback_commands_[n];
  bool const same =
      got.size() == want.size() && got[0] == want[0] && std::equal(got.begin() + 2, got.end() - 2, want.begin() + 2);
  if (!same)
    playback_errors_.push_back("command " + std::to_string(n) + " differs\n  sent:     " + describe(got) +
                               "\n  recorded: " + describe(want));
}

std::vector<const FakeCoprocessor::RecordedCommand *> FakeCoprocessor::commands_of(MsgType type) const {
  std::vector<const RecordedCommand *> result;
  for (const RecordedCommand &c : commands_) {
    if (c.type == static_cast<uint8_t>(type))
      result.push_back(&c);
  }
  return result;
}

// === Event queue ===

void FakeCoprocessor::insert_(Pending event) {
  auto pos = std::upper_bound(pending_.begin(), pending_.end(), event.due_us,
                              [](uint64_t due, const Pending &p) { return due < p.due_us; });
  pending_.insert(pos, std::move(event));
}

void FakeCoprocessor::queue_(const Body &body, uint64_t due_us) {
  if (in_handler_ && scenario_mode_)
    return;
  uint8_t msg[MAX_MESSAGE_SIZE];
  size_t const n = encode_message(0, body, msg, sizeof(msg));
  if (n == 0)
    return;
  Pending p;
  p.due_us = due_us;
  p.message.assign(msg, msg + n);
  insert_(std::move(p));
}

void FakeCoprocessor::error_(uint8_t cmd_seq, ErrorCode code, uint16_t detail) {
  errors_sent_[static_cast<uint8_t>(code) & 0x0F]++;
  Error e;
  e.cmd_seq = cmd_seq;
  e.code = code;
  e.detail = detail;
  queue_(e, now_us());
}

bool FakeCoprocessor::resolve_scenario_reply_(const Pending &event, uint8_t &seq) const {
  const ScenarioHostStep &step = event.scenario->host[event.host_step];
  size_t ordinal = 0;
  for (int i = 0; i < event.host_step; i++) {
    if (event.scenario->host[i].msg_type == step.msg_type)
      ordinal++;
  }
  for (size_t i = event.commands_before; i < commands_.size(); i++) {
    if (commands_[i].type == step.msg_type && ordinal-- == 0) {
      seq = commands_[i].seq;
      return true;
    }
  }
  return false;
}

void FakeCoprocessor::release_timed_() {
  for (auto it = timed_.begin(); it != timed_.end();) {
    auto due = timeline_.release_us(it->anchor);
    if (!due.has_value()) {
      ++it;
      continue;
    }
    inject_rx(it->freq_hz, it->payload, CrcState::OK, *due > now_us() ? *due - now_us() : 0);
    it = timed_.erase(it);
  }
}

void FakeCoprocessor::ack_(uint8_t seq) {
  if (drop_next_ack_) {
    drop_next_ack_ = false;
    return;
  }
  Ack ack;
  ack.cmd_seq = seq;
  queue_(ack, now_us());
}

void FakeCoprocessor::pump_() {
  release_timed_();
  drain_due_();
  if (reboot_after_drain_ && (pending_.empty() || pending_.front().due_us > now_us())) {
    // The `ack` of a soft reset and everything due before it are out: the reboot follows (spec §3.1).
    reboot_after_drain_ = false;
    boot();
    drain_due_();
  }
  if (playback_active_)
    pump_playback_();
}

void FakeCoprocessor::drain_due_() {
  while (!pending_.empty() && pending_.front().due_us <= now_us()) {
    Pending &event = pending_.front();
    if (event.skip != 0) {
      counter_ = static_cast<uint8_t>(counter_ + event.skip);
      pending_.erase(pending_.begin());
      continue;
    }
    uint8_t reply_seq = 0;
    if (event.scenario != nullptr) {
      if (!resolve_scenario_reply_(event, reply_seq))
        return;  // the driver has not sent the command this reply answers yet; later events wait
      event.message[2] = reply_seq;
    }
    std::vector<uint8_t> msg = std::move(event.message);
    pending_.erase(pending_.begin());
    if (silent_)
      continue;
    emit_(std::move(msg));
  }
}

void FakeCoprocessor::emit_(std::vector<uint8_t> msg) {
  msg[1] = counter_++;
  uint16_t const crc = esphome::home_io_control::crc_ccitt(msg.data(), static_cast<uint8_t>(msg.size() - 2));
  msg[msg.size() - 2] = static_cast<uint8_t>(crc);
  msg[msg.size() - 1] = static_cast<uint8_t>(crc >> 8);
  if (corrupt_next_) {
    msg[msg.size() - 1] ^= 0xFF;
    corrupt_next_ = false;
  }
  uint8_t wire[MAX_WIRE_SIZE];
  size_t const w = frame_message(msg.data(), msg.size(), wire, sizeof(wire));
  stream_.push_rx(wire, w);
}

// === Host commands ===

void FakeCoprocessor::on_host_bytes_(const uint8_t *data, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (deframer_.feed(data[i]) != Deframer::Event::MESSAGE)
      continue;
    RecordedCommand rec;
    rec.t_us = now_us();
    rec.message.assign(deframer_.message(), deframer_.message() + deframer_.message_size());
    rec.type = rec.message[0];
    rec.seq = rec.message[1];
    commands_.push_back(rec);
    if (playback_active_)
      compare_playback_command_();
    if (silent_)
      continue;
    in_handler_ = true;
    Message msg;
    if (link_in_use_) {
      error_(rec.seq, ErrorCode::LINK_IN_USE);
    } else if (!is_known_command(rec.type)) {
      error_(rec.seq, ErrorCode::UNKNOWN_TYPE);
    } else if (decode_message(rec.message.data(), rec.message.size(), msg) != DecodeStatus::OK) {
      error_(rec.seq, ErrorCode::BAD_LENGTH);
    } else if (auto reject = std::find_if(rejections_.begin(), rejections_.end(),
                                          [&](const Rejection &r) {
                                            return r.type == rec.type && (r.radio < 0 || (rec.message.size() > 2 &&
                                                                                          rec.message[2] == r.radio));
                                          });
               reject != rejections_.end()) {
      error_(rec.seq, reject->code, reject->detail);
      rejections_.erase(reject);
    } else {
      handle_(msg);
    }
    in_handler_ = false;
  }
}

void FakeCoprocessor::handle_(const Message &msg) {
  uint8_t const seq = msg.seq;
  // Host commands carry a radio index first; answer no_such_radio before anything else about it.
  auto radio_of = [&](uint8_t index) -> RadioState * {
    if (radio_(index) == nullptr) {
      error_(seq, ErrorCode::NO_SUCH_RADIO, index);
      return nullptr;
    }
    return &radios_[index];
  };
  if (std::holds_alternative<Hello>(msg.body)) {
    if (take_over_on_hello_) {
      take_over_on_hello_ = false;
      reset_state_();
      config_.reset_cause = ResetCause::LINK_TAKEOVER;
    }
    queue_ready_(seq, now_us());
  } else if (std::holds_alternative<SoftReset>(msg.body)) {
    ack_(seq);
    reboot_after_drain_ = true;
    config_.reset_cause = ResetCause::SOFT;
  } else if (const auto *cmd = std::get_if<SetParams>(&msg.body)) {
    handle_set_params_(*cmd, seq);
  } else if (const auto *on = std::get_if<RxOn>(&msg.body)) {
    if (RadioState *r = radio_of(on->radio)) {
      const RadioDescriptor &d = descriptors_[on->radio];
      if (!configured_(*r)) {
        error_(seq, ErrorCode::NOT_CONFIGURED);
      } else if (on->freq_hz < d.band_min_hz || on->freq_hz > d.band_max_hz) {
        error_(seq, ErrorCode::PARAM_RANGE, field::rx_on::FREQ_HZ);
      } else {
        r->freq_hz = on->freq_hz;  // a retune; any scan ends
        r->mode = Mode::RX;
        ack_(seq);
      }
    }
  } else if (const auto *idle = std::get_if<Idle>(&msg.body)) {
    if (RadioState *r = radio_of(idle->radio)) {
      if (!configured_(*r)) {
        error_(seq, ErrorCode::NOT_CONFIGURED);
      } else {
        r->mode = Mode::IDLE;
        ack_(seq);
      }
    }
  } else if (const auto *rssi = std::get_if<GetRssi>(&msg.body)) {
    if (RadioState *r = radio_of(rssi->radio)) {
      if (!configured_(*r)) {
        error_(seq, ErrorCode::NOT_CONFIGURED);
      } else if (transmitting()) {
        error_(seq, ErrorCode::BUSY);
      } else if (r->mode == Mode::IDLE) {
        error_(seq, ErrorCode::NOT_RECEIVING);
      } else {
        Rssi reply;
        reply.cmd_seq = seq;
        reply.radio = rssi->radio;
        reply.rssi_dbm = rssi_dbm_;
        queue_(reply, now_us());
      }
    }
  } else if (std::holds_alternative<GetStats>(msg.body)) {
    Stats stats;
    stats.cmd_seq = seq;
    stats.present = Stats::COUNT;
    stats.counters[Stats::TX_OK] = tx_ok_;
    stats.counters[Stats::LINK_ERR] = deframer_.counters().total();
    stats.counters[Stats::UPTIME_MS] = static_cast<uint32_t>(now_us() / 1000);
    queue_(stats, now_us());
  } else if (const auto *scan = std::get_if<RxScan>(&msg.body)) {
    if (RadioState *r = radio_of(scan->radio)) {
      if (!(descriptors_[scan->radio].caps & radio_caps::RX_SCAN)) {
        error_(seq, ErrorCode::UNSUPPORTED, cap_bit(radio_caps::RX_SCAN));
      } else if (!configured_(*r)) {
        error_(seq, ErrorCode::NOT_CONFIGURED);
      } else if (scan->dwell_us < 200) {
        error_(seq, ErrorCode::PARAM_RANGE, field::rx_scan::DWELL_US);
      } else if (scan->num_freqs == 0 || scan->num_freqs > MAX_SCAN_FREQS) {
        error_(seq, ErrorCode::PARAM_RANGE, field::rx_scan::NUM_FREQS_HZ);
      } else {
        r->scan = *scan;
        r->freq_hz = scan->freqs_hz[0];
        r->mode = Mode::SCAN;
        ack_(seq);
      }
    }
  } else if (const auto *tx = std::get_if<Tx>(&msg.body)) {
    handle_tx_(*tx, seq);
  } else if (std::holds_alternative<EnterBootloader>(msg.body)) {
    ack_(seq);  // then a real co-processor reboots into its bootloader and says nothing more
  } else {
    error_(seq, ErrorCode::UNKNOWN_TYPE);
  }
}

void FakeCoprocessor::handle_set_params_(const SetParams &cmd, uint8_t seq) {
  if (cmd.radio != DEVICE_SCOPE && radio_(cmd.radio) == nullptr) {
    error_(seq, ErrorCode::NO_SUCH_RADIO, cmd.radio);
    return;
  }
  // Atomic (spec §4): every TLV is checked in order against a staged copy; the first failure refuses the
  // whole command and nothing is applied.
  uint8_t applied[TLV_BUFFER];
  TlvWriter out(applied, sizeof(applied));
  TlvReader in(cmd.tlvs);
  uint8_t id;
  ByteSpan value;
  RadioState staged;
  if (cmd.radio != DEVICE_SCOPE)
    staged = radios_[cmd.radio];
  uint8_t staged_event_mask = event_mask_;
  bool first = true;
  bool profile_seen = false;
  auto refuse = [&](ErrorCode code, uint16_t detail) { error_(seq, code, detail); };
  while (in.next(id, value)) {
    bool const is_profile = id == static_cast<uint8_t>(ParamId::PHY_PROFILE);
    if (is_profile && (!first || profile_seen)) {
      refuse(ErrorCode::PARAM_RANGE, id);  // `phy_profile` is the first TLV, and only once
      return;
    }
    first = false;
    if (cmd.radio == DEVICE_SCOPE) {
      if (id == static_cast<uint8_t>(ParamId::EVENT_MASK) && value.size == 1) {
        staged_event_mask = value.data[0];
        out.add(id, value.data, value.size);
        continue;
      }
      refuse(ErrorCode::UNSUPPORTED, id);
      return;
    }
    const RadioDescriptor &d = descriptors_[cmd.radio];
    switch (static_cast<ParamId>(id)) {
      case ParamId::PHY_PROFILE:
        profile_seen = true;
        if (value.size == 1 && (d.profiles & (1u << value.data[0])) != 0) {
          staged.profile_set = true;
          staged.profile = static_cast<PhyProfile>(value.data[0]);
          staged.sfd_set = false;  // setting the profile resets the profile params
          staged.gfsk_set = 0;
          out.add(id, value.data, value.size);
          break;
        }
        refuse(ErrorCode::PARAM_RANGE, id);
        return;
      case ParamId::SFD:
        if (!staged.profile_set || staged.profile != PhyProfile::IEEE802154_OQPSK_250K) {
          refuse(ErrorCode::UNSUPPORTED, id);
          return;
        }
        if (value.size != 1) {
          refuse(ErrorCode::PARAM_RANGE, id);
          return;
        }
        staged.sfd_set = true;
        out.add(id, value.data, value.size);
        break;
      case ParamId::BITRATE_BPS:
      case ParamId::FDEV_HZ:
      case ParamId::RX_BW_HZ:
      case ParamId::PULSE_SHAPE:
      case ParamId::SYNC_WORD:
      case ParamId::RX_LEN:
      case ParamId::PREAMBLE_DETECT_BITS:
      case ParamId::SYNC_TIMEOUT_US: {
        // gfsk_raw params: accepted and echoed on a radio staged to that profile; the four required ones are
        // remembered.
        if (!staged.profile_set || staged.profile != PhyProfile::GFSK_RAW) {
          refuse(ErrorCode::UNSUPPORTED, id);
          return;
        }
        if ((static_cast<ParamId>(id) == ParamId::RX_LEN &&
             (value.size != 1 || value.data[0] == 0 || value.data[0] > d.max_payload)) ||
            (static_cast<ParamId>(id) == ParamId::PREAMBLE_DETECT_BITS &&
             (value.size != 1 || !valid_preamble_detect_bits(value.data[0])))) {
          refuse(ErrorCode::PARAM_RANGE, id);
          return;
        }
        constexpr uint8_t REQUIRED_BIT_FOR[] = {0x01, 0x02, 0x00,
                                                0x00, 0x04, 0x08};  // bitrate, fdev, then rx_bw.. rx_len
        uint8_t const param =
            static_cast<uint8_t>(static_cast<ParamId>(id)) - static_cast<uint8_t>(ParamId::BITRATE_BPS);
        if (param < sizeof(REQUIRED_BIT_FOR))
          staged.gfsk_set = static_cast<uint8_t>(staged.gfsk_set | REQUIRED_BIT_FOR[param]);
        out.add(id, value.data, value.size);
        break;
      }
      case ParamId::TX_POWER_DBM:
        if (value.size != 1) {
          refuse(ErrorCode::PARAM_RANGE, id);
          return;
        }
        staged.tx_power_dbm = std::clamp(static_cast<int8_t>(value.data[0]), d.tx_power_min_dbm, d.tx_power_max_dbm);
        out.add_u8(id, static_cast<uint8_t>(staged.tx_power_dbm));  // the applied value is echoed
        break;
      default:
        refuse(ErrorCode::UNSUPPORTED, id);
        return;
    }
  }
  if (cmd.radio != DEVICE_SCOPE && transmitting()) {
    set_params_during_tx_++;
    refuse(ErrorCode::BUSY, 0);  // only when every TLV passed; device scope is never busy
    return;
  }
  if (cmd.radio == DEVICE_SCOPE)
    event_mask_ = staged_event_mask;
  else
    radios_[cmd.radio] = staged;
  ParamsAck ack;
  ack.cmd_seq = seq;
  ack.radio = cmd.radio;
  ack.applied = out.span();
  queue_(ack, now_us());
}

void FakeCoprocessor::handle_tx_(const Tx &cmd, uint8_t seq) {
  if (radio_(cmd.radio) == nullptr) {
    error_(seq, ErrorCode::NO_SUCH_RADIO, cmd.radio);
    return;
  }
  const RadioDescriptor &d = descriptors_[cmd.radio];
  RadioState &r = radios_[cmd.radio];
  // `unsupported` names the bit of the missing capability (spec §2.2).
  if (cmd.has_strobe && !(d.caps & radio_caps::STROBE)) {
    error_(seq, ErrorCode::UNSUPPORTED, cap_bit(radio_caps::STROBE));
    return;
  }
  if (cmd.cca && !(d.caps & radio_caps::CCA)) {
    error_(seq, ErrorCode::UNSUPPORTED, cap_bit(radio_caps::CCA));
    return;
  }
  if (cmd.at_time && !(d.caps & radio_caps::SCHEDULED_TX)) {
    error_(seq, ErrorCode::UNSUPPORTED, cap_bit(radio_caps::SCHEDULED_TX));
    return;
  }
  if (!configured_(r)) {
    error_(seq, ErrorCode::NOT_CONFIGURED);
    return;
  }
  // The profile decides which preambles exist: 802.15.4 has only its standard one, a per-packet profile any
  // length up to the radio's `max_preamble_bytes`.
  uint16_t const longest_preamble = r.profile == PhyProfile::GFSK_RAW ? d.max_preamble_bytes : 0;
  if (cmd.preamble_bytes > longest_preamble) {
    error_(seq, ErrorCode::PARAM_RANGE, field::tx::PREAMBLE_BYTES);
    return;
  }
  if (cmd.payload.size > d.max_payload) {
    error_(seq, ErrorCode::PAYLOAD_TOO_LONG);
    return;
  }
  if (transmitting()) {
    error_(seq, ErrorCode::BUSY);
    return;
  }
  if (cmd.has_strobe && (cmd.strobe.count < 1 || cmd.strobe.count > 1024 || cmd.strobe.len_template < 1 ||
                         cmd.strobe.len_template > MAX_STROBE_TEMPLATE)) {
    error_(seq, ErrorCode::PARAM_RANGE, field::tx::STROBE);
    return;
  }
  r.freq_hz = cmd.freq_hz;  // tx.freq_hz becomes the radio's frequency (spec §3.1); a scan ends
  int16_t level = channel_level_dbm_;
  if (cmd.cca && !channel_levels_.empty()) {
    level = channel_levels_.front();
    channel_levels_.pop_front();
  }
  if (cmd.cca && level >= cmd.cca_threshold_dbm) {
    r.mode = cmd.then_rx ? Mode::RX : Mode::IDLE;
    error_(seq, ErrorCode::CHANNEL_BUSY, static_cast<uint16_t>(level));
    return;
  }
  TxShape shape;
  // 0 = the profile's standard preamble (the generated `-preamble` of the profile).
  shape.preamble_bytes = cmd.preamble_bytes != 0 || r.profile != PhyProfile::GFSK_RAW
                             ? cmd.preamble_bytes
                             : profile_gfsk_raw::STANDARD_PREAMBLE_BYTES;
  shape.has_strobe = cmd.has_strobe;
  shape.strobe = cmd.strobe;
  uint32_t const air_us = r.profile == PhyProfile::GFSK_RAW ? io868_gfsk_phy().tx_air_time_us(shape, cmd.payload.size)
                                                            : io2g4_phy().tx_air_time_us(shape, cmd.payload.size);
  uint64_t const lead_us =
      cmd.has_strobe
          ? ((cmd.strobe.count - 1) * static_cast<uint64_t>(cmd.strobe.period_ns) + cmd.strobe.main_gap_ns) / 1000
          : 0;
  uint64_t const start = now_us() + lead_us;
  uint64_t const end = now_us() + air_us;
  timeline_.record_at(cmd.payload.data, cmd.payload.size, config_.anchor_replies_at_tx_end ? end : start);
  tx_busy_until_us_ = end;
  r.mode = cmd.then_rx ? Mode::RX : Mode::IDLE;
  tx_ok_++;
  TxDone done;
  done.cmd_seq = seq;
  done.radio = cmd.radio;
  done.t_main_start_us = static_cast<uint32_t>(start);
  done.t_main_end_us = static_cast<uint32_t>(end);
  done.strobe_sent = cmd.has_strobe ? cmd.strobe.count : 0;
  done.strobe_late = next_strobe_late_;
  next_strobe_late_ = 0;
  queue_(done, end);
  if (tx_observer_)
    tx_observer_(std::vector<uint8_t>(cmd.payload.data, cmd.payload.data + cmd.payload.size), start, end);
}

void FakeCoprocessor::queue_ready_(uint8_t cmd_seq, uint64_t due_us) {
  uint8_t info[TLV_BUFFER];
  TlvWriter w(info, sizeof(info));
  if (config_.with_fw_version)
    w.add(static_cast<uint8_t>(InfoId::FW_VERSION), reinterpret_cast<const uint8_t *>(config_.fw_version.data()),
          config_.fw_version.size());
  if (!config_.board.empty())
    w.add(static_cast<uint8_t>(InfoId::BOARD), reinterpret_cast<const uint8_t *>(config_.board.data()),
          config_.board.size());
  for (const RadioDescriptor &d : descriptors_) {
    w.add_radio(d);
  }
  if (config_.with_rx_queue_depth)
    w.add_u8(static_cast<uint8_t>(InfoId::RX_QUEUE_DEPTH), 16);
  if (config_.with_reset_cause)
    w.add_u8(static_cast<uint8_t>(InfoId::RESET_CAUSE), static_cast<uint8_t>(config_.reset_cause));
  Ready ready;
  ready.cmd_seq = cmd_seq;
  ready.proto_major = config_.proto_major;
  ready.proto_minor = config_.proto_minor;
  ready.chip_id = config_.chip_id;
  ready.caps = 0;
  ready.info = w.span();
  queue_(ready, due_us);
}
