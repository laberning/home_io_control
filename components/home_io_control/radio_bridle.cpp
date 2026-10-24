/// @file
/// @brief RadioBridle: the RadioDriver for a Bridle radio co-processor.
/// @ingroup hioc_radio

// IOHOME_BRIDLE is only visible after something pulls in esphome/core/defines.h (ESPHome codegen's
// cg.add_define() lands there, not as a compiler -D flag): this include must run before the #ifdef,
// or the file silently compiles to nothing in a real build although the host tests (which pass
// -DIOHOME_BRIDLE) are fine.
#include "esphome/core/hal.h"

#ifdef IOHOME_BRIDLE

#include "radio_bridle.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "proto_sizes.h"

namespace esphome::home_io_control {

static const char *const TAG = "home_io_control.bridle";

using namespace bridle;

namespace {

/// Copies `span` as text into `out` (truncated, always terminated).
void copy_text(char *out, size_t cap, ByteSpan span) {
  size_t const n = std::min(span.size, cap - 1);
  if (n > 0)
    std::memcpy(out, span.data, n);
  out[n] = '\0';
}

}  // namespace

// === Link ===

uint8_t RadioBridle::send_command_(const Body &body) {
  uint8_t const seq = this->next_seq_;
  this->next_seq_ = seq == UINT8_MAX ? 1 : static_cast<uint8_t>(seq + 1);  // 0 is never a command id
  uint8_t msg[MAX_MESSAGE_SIZE];
  uint8_t wire[MAX_WIRE_SIZE];
  size_t const msg_len = encode_message(seq, body, msg, sizeof(msg));
  size_t const n = msg_len == 0 ? 0 : frame_message(msg, msg_len, wire, sizeof(wire));
  if (n == 0) {
    ESP_LOGE(TAG, "command 0x%02X does not fit a message", msg_type_of(body));
    return 0;
  }
  this->trace_message_(TraceDirection::HOST_TO_COPROC, msg, msg_len);
  this->stream_->write(wire, n);
  this->last_command_sent_ms_ = millis();
  return seq;
}

void RadioBridle::emit_trace_line(const char *line) { ESP_LOGD(TAG, "%s", line); }

void RadioBridle::trace_message_(TraceDirection direction, const uint8_t *msg, size_t len) {
  if (!this->trace_)
    return;
  char line[bridle::TRACE_LINE_MAX];
  uint32_t const now = millis();
  for (size_t part = 0; part < bridle::trace_part_count(len); part++) {
    if (bridle::format_trace_part(line, sizeof(line), direction, now, msg, len, part) > 0)
      this->emit_trace_line(line);
  }
}

void RadioBridle::drain_() {
  uint8_t buf[DRAIN_CHUNK];
  size_t n;
  while ((n = this->stream_->read(buf, sizeof(buf))) > 0) {
    for (size_t i = 0; i < n; i++) {
      if (this->deframer_.feed(buf[i]) == Deframer::Event::MESSAGE)
        this->route_(this->deframer_.message(), this->deframer_.message_size());
    }
  }
}

void RadioBridle::note_event_counter_(const Message &m) {
  // The event counter is the second byte of every co-processor message (spec §2.2). A boot
  // `ready` is counter 0; any gap means events were lost.
  const auto *ready = std::get_if<Ready>(&m.body);
  // The `ready` that answers a taking `hello` restarts the counter at 0 as a boot does (spec §2.1); a later `ready`
  // reports the same cause until the next reboot but continues the counter.
  if (ready != nullptr && (ready->cmd_seq == 0 || (m.seq == 0 && is_link_takeover(*ready)))) {
    this->expected_counter_known_ = true;
  } else if (this->expected_counter_known_ && m.seq != this->expected_counter_) {
    this->counter_gaps_++;
    ESP_LOGW(TAG, "event counter jumped from %u to %u: events were lost", this->expected_counter_, m.seq);
    // Repeated gaps mean the link is losing events, which a resync cures (spec §7).
    uint32_t const now = millis();
    if (this->gap_count_ == GAP_LIMIT) {
      std::copy(this->gap_times_ms_ + 1, this->gap_times_ms_ + GAP_LIMIT, this->gap_times_ms_);
      this->gap_count_--;
    }
    this->gap_times_ms_[this->gap_count_++] = now;
    if (this->gap_count_ == GAP_LIMIT && now - this->gap_times_ms_[0] <= GAP_WINDOW_MS && this->link_running()) {
      this->gap_count_ = 0;
      this->begin_resync_("repeated event-counter gaps", false);
    }
  }
  this->expected_counter_ = static_cast<uint8_t>(m.seq + 1);
}

void RadioBridle::route_(const uint8_t *msg, size_t len) {
  this->trace_message_(TraceDirection::COPROC_TO_HOST, msg, len);
  this->last_rx_ms_ = millis();
  Message m;
  if (decode_message(msg, len, m) != DecodeStatus::OK) {
    ESP_LOGW(TAG, "dropping a co-processor message of type 0x%02X that does not fit its layout", msg[0]);
    return;
  }
  this->note_event_counter_(m);
  // A reply's body starts with `cmd_seq` (spec §2.2); the message type range says which messages are replies.
  if (is_reply(msg[0])) {
    if (len < 3)  // an undefined reply type carries no body to read `cmd_seq` from
      return;
    this->route_reply_(m, msg, len, msg[2]);
  } else {
    this->route_event_(m.body);
  }
}

void RadioBridle::route_reply_(const Message &m, const uint8_t *msg, size_t len, uint8_t seq) {
  if (std::holds_alternative<Ready>(m.body) &&
      (seq == 0 || (seq == this->hello_seq_ && this->link_ == Link::WAIT_READY))) {
    std::memcpy(this->ready_msg_, msg, len);  // a boot or reboot: the handshake or the link code takes it
    this->ready_msg_len_ = len;
    this->ready_pending_ = true;
    this->pending_count_ = 0;  // a reboot drops every command it had not answered
    return;
  }
  if (const auto *fault = std::get_if<Error>(&m.body); fault != nullptr && seq == 0) {
    // A radio fault outside a transmission answers no command; the radio is idle after it, so the
    // configuration is gone as far as the host can tell (spec §2.2).
    ESP_LOGW(TAG, "the co-processor reports a radio fault (error %u, detail %u); resyncing",
             static_cast<unsigned>(fault->code), fault->detail);
    this->begin_resync_("radio fault", false);
    return;
  }
  if (const auto *refusal = std::get_if<Error>(&m.body);
      refusal != nullptr && refusal->code == ErrorCode::LINK_IN_USE && seq != 0 && this->link_running()) {
    // Another host has taken the co-processor over (spec §2.1): a lost link, handled by a resync (spec §7).
    ESP_LOGW(TAG, "the co-processor answers link_in_use: another host has taken it over; resyncing");
    this->begin_resync_("link in use by another host", false);
    return;
  }
  if (const auto *stats = std::get_if<Stats>(&m.body)) {
    if (this->hb_pending_ && stats->cmd_seq == this->hb_seq_) {
      this->hb_pending_ = false;  // the co-processor answered: the link is alive
      if (stats->present > Stats::LOG_DROPPED) {
        this->log_dropped_ = stats->counters[Stats::LOG_DROPPED];
        this->have_stats_ = true;
      }
      return;
    }
  }
  // A reply belongs to whoever is waiting for its command, and to nobody once the wait is over.
  bool const mine = this->awaiting_ && seq == this->await_seq_;
  if (mine && msg[0] == static_cast<uint8_t>(this->await_type_)) {
    this->on_awaited_reply_(m, msg, len);
  } else if (mine && std::holds_alternative<Error>(m.body)) {
    const auto &e = std::get<Error>(m.body);
    this->last_error_code_ = e.code;
    this->last_error_detail_ = e.detail_dbm();
    this->await_errored_ = true;
    std::memcpy(this->reply_, msg, len);
    this->reply_len_ = len;
    this->await_done_ = true;
    if (e.code == ErrorCode::NOT_CONFIGURED)
      this->begin_resync_("radio not configured", false);
  } else if (!mine && this->on_pending_reply_(m, seq)) {
    return;
  } else {
    this->stale_replies_++;
    ESP_LOGD(TAG, "unmatched reply 0x%02X for command %u", msg[0], seq);
  }
}

void RadioBridle::on_awaited_reply_(const Message &m, const uint8_t *msg, size_t len) {
  if (const auto *done = std::get_if<TxDone>(&m.body)) {
    // Ordering rule: everything received before our own tx_done arrived before or during the
    // transmission and is not the reply to it.
    this->dropped_before_tx_done_ += static_cast<uint32_t>(this->fifo_count_);
    this->fifo_count_ = 0;
    this->fifo_head_ = 0;
    if (done->strobe_late > 0) {
      this->strobe_late_total_ += done->strobe_late;
      ESP_LOGW(TAG, "%u strobe frame(s) went out late", done->strobe_late);
    }
  }
  std::memcpy(this->reply_, msg, len);
  this->reply_len_ = len;
  this->await_done_ = true;
}

void RadioBridle::route_event_(const Body &body) {
  if (const auto *rx = std::get_if<Rx>(&body)) {
    this->on_rx_(*rx);
  } else if (const auto *fs = std::get_if<FrameStart>(&body)) {
    if (fs->radio == this->radio_) {
      this->frame_arriving_ = true;
      this->arriving_stage_ = fs->stage;
      this->note_reception_in_progress_();
    }
  } else if (const auto *abort = std::get_if<RxAbort>(&body)) {
    if (abort->radio == this->radio_) {
      this->frame_arriving_ = false;
      this->clear_reception_in_progress_();
    }
  } else if (const auto *overflow = std::get_if<RxOverflow>(&body)) {
    this->rx_overflow_reports_ += overflow->dropped;
    ESP_LOGW(TAG, "the co-processor dropped %u received frame(s): the host did not drain in time", overflow->dropped);
  } else if (const auto *log = std::get_if<Log>(&body)) {
    this->show_log_(*log);
  }
  // Any other event type is ignored (spec §2.2).
}

void RadioBridle::show_log_(const Log &log) {
  // Diagnostic only: shown, never acted on (spec §3.2).
  this->log_events_++;
  int const n = static_cast<int>(log.text.size);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — the text is ASCII bytes; %.*s bounds it.
  const char *text = reinterpret_cast<const char *>(log.text.data);
  switch (log.level) {
    case LogLevel::ERROR:
      ESP_LOGE(TAG, "[coproc] %.*s", n, text);
      break;
    case LogLevel::WARN:
      ESP_LOGW(TAG, "[coproc] %.*s", n, text);
      break;
    case LogLevel::INFO:
      ESP_LOGI(TAG, "[coproc] %.*s", n, text);
      break;
    default:
      ESP_LOGD(TAG, "[coproc] %.*s", n, text);
      break;
  }
}

RadioBridle::Wait RadioBridle::await_(uint8_t seq, MsgType expected, uint32_t timeout_ms) {
  if (seq == 0)
    return Wait::ERROR;
  this->awaiting_ = true;
  this->await_seq_ = seq;
  this->await_type_ = expected;
  this->await_done_ = false;
  this->await_errored_ = false;
  uint32_t const start = millis();
  while (!this->await_done_) {
    this->drain_();
    if (this->await_done_ || millis() - start >= timeout_ms || this->link_lost_())
      break;
    App.feed_wdt();
  }
  this->awaiting_ = false;
  if (!this->await_done_)
    return Wait::TIMEOUT;
  return this->reply_[0] == static_cast<uint8_t>(MsgType::ERROR) ? Wait::ERROR : Wait::REPLIED;
}

// === Unawaited commands ===

uint8_t RadioBridle::send_pending_(const Body &body, PendingKind kind) {
  if (this->pending_count_ == PENDING_DEPTH) {
    // Replies leave in command order, so the head's reply frees a slot; a head that stays unanswered is
    // a lost link, like a missing tx_done.
    uint32_t const start = millis();
    while (this->pending_count_ == PENDING_DEPTH && millis() - start < ACK_TIMEOUT_MS && this->link_running()) {
      this->drain_();
      App.feed_wdt();
    }
    if (this->pending_count_ == PENDING_DEPTH) {
      this->begin_resync_("pending commands unanswered", false);
      return 0;
    }
    if (!this->link_running())
      return 0;
  }
  uint8_t const seq = this->send_command_(body);
  if (seq == 0)
    return 0;
  this->pending_[this->pending_count_++] = PendingCommand{seq, kind, millis()};
  this->newest_receive_cmd_ = seq;
  return seq;
}

bool RadioBridle::on_pending_reply_(const Message &m, uint8_t seq) {
  const auto *error = std::get_if<Error>(&m.body);
  if (error == nullptr && !std::holds_alternative<Ack>(m.body))
    return false;
  size_t index = 0;
  while (index < this->pending_count_ && this->pending_[index].seq != seq)
    index++;
  if (index == this->pending_count_)
    return false;
  if (index != 0) {
    // The head was sent first and would have been answered first.
    ESP_LOGW(TAG, "the reply to command %u overtook command %u: a reply was lost", seq, this->pending_[0].seq);
    this->begin_resync_("a reply to an unawaited command was lost", false);
    return true;
  }
  PendingCommand const cmd = this->pending_[0];
  std::copy(this->pending_ + 1, this->pending_ + this->pending_count_, this->pending_);
  this->pending_count_--;
  if (error != nullptr)
    this->on_pending_refused_(cmd, *error);
  return true;
}

void RadioBridle::on_pending_refused_(const PendingCommand &cmd, const Error &error) {
  this->unawaited_errors_++;
  auto const code = static_cast<unsigned>(error.code);
  if (code < sizeof(this->refusal_logged_) * BITS_PER_BYTE && (this->refusal_logged_ & (1U << code)) == 0) {
    this->refusal_logged_ |= static_cast<uint16_t>(1U << code);
    ESP_LOGW(TAG, "the co-processor refused %s (error %u, detail %u)",
             cmd.kind == PendingKind::RX_SCAN ? "rx_scan" : (cmd.kind == PendingKind::RX_ON ? "rx_on" : "idle"), code,
             error.detail);
  }
  if (error.code == ErrorCode::NOT_CONFIGURED) {
    this->begin_resync_("radio not configured", false);
    return;
  }
  // Undo the optimistic state only when nothing newer has changed the radio since.
  if (cmd.seq != this->newest_receive_cmd_)
    return;
  if (cmd.kind == PendingKind::RX_SCAN) {
    this->scanning_ = false;
    this->rx_on_ = false;
    this->scan_refused_ = true;
    this->scan_refused_ms_ = millis();
  } else if (cmd.kind == PendingKind::RX_ON) {
    this->rx_on_ = false;
    this->rx_refused_ = true;
    this->rx_refused_ms_ = millis();
  }
}

// === Initialization ===

bool RadioBridle::init() {
  if (this->stream_ == nullptr) {
    this->fail_("no byte stream");
    return false;
  }
  if (this->rst_pin_ != nullptr) {
    this->rst_pin_->setup();
    this->rst_pin_->digital_write(true);
  }
  if (!this->handshake_()) {
    if (!this->busy_at_start_)
      return false;
    // Another link holds the co-processor: start down, and the retry takes it once that link lets go (spec §2.1).
    this->next_retry_ms_ = millis() + DOWN_RETRY_MS;
    this->set_link_(Link::DOWN, "co-processor busy on another link");
    return true;
  }
  if (!this->configure_())
    return false;
  this->link_ = Link::RUNNING;
  this->last_rx_ms_ = millis();
  this->last_command_sent_ms_ = millis();
  return true;
}

bool RadioBridle::handshake_() {
  // Spec §8.1: a reset pulse, then `ready` within 2 s. Without one, `hello`; if that fails too,
  // one more reset (never two pulses within a second: some bootloaders read that as "stay").
  bool refused = false;  // the co-processor answered `hello`, but with an `error`: the link works
  ErrorCode refusal = ErrorCode::UNKNOWN_TYPE;
  for (int attempt = 0; attempt < 2; attempt++) {
    this->ready_pending_ = false;
    this->expected_counter_known_ = false;
    this->reset_hardware_();
    uint32_t const start = millis();
    while (!this->ready_pending_ && millis() - start < READY_TIMEOUT_MS) {
      this->drain_();
      App.feed_wdt();
    }
    if (!this->ready_pending_) {
      ESP_LOGW(TAG, "no ready after the reset; asking with hello");
      uint8_t const seq = this->send_command_(Hello{});
      if (this->await_(seq, MsgType::READY, HELLO_TIMEOUT_MS) == Wait::REPLIED) {
        std::memcpy(this->ready_msg_, this->reply_, this->reply_len_);
        this->ready_msg_len_ = this->reply_len_;
        this->ready_pending_ = true;
      } else if (this->await_errored_) {
        refused = true;
        refusal = this->last_error_code_;
      }
    }
    if (this->ready_pending_) {
      this->ready_pending_ = false;
      Message m;
      if (decode_message(this->ready_msg_, this->ready_msg_len_, m) != DecodeStatus::OK ||
          !std::holds_alternative<Ready>(m.body)) {
        this->fail_("unreadable ready");
        return false;
      }
      return this->accept_ready_(std::get<Ready>(m.body), true);
    }
  }
  if (refused && refusal == ErrorCode::LINK_IN_USE) {
    // Spec §2.1: another link holds the co-processor and answers this one with `link_in_use` and nothing else.
    ESP_LOGE(TAG,
             "the co-processor refuses this link (link_in_use): another link holds it. It is freed when that "
             "link's USB port closes or when it stays silent for 60 s; this hub asks again every %" PRIu32 " s",
             DOWN_RETRY_MS / 1000);
    this->busy_at_start_ = true;
    return false;
  }
  if (refused) {
    ESP_LOGE(TAG, "the co-processor answered hello with error %u", static_cast<unsigned>(refusal));
    this->fail_("co-processor refused hello");
    return false;
  }
  ESP_LOGE(TAG, "the co-processor never answered: check the UART wiring, its power and the reset pin");
  this->fail_("co-processor not answering");
  return false;
}

bool RadioBridle::accept_ready_(const Ready &ready, bool latch_failure) {
  auto fail = [&](const char *reason) {
    if (latch_failure)
      this->fail_(reason);
  };
  // The host rule of spec §10.2: both numbers must match while the protocol is 0.y.
  if (!version_accepted(PROTO_MAJOR, PROTO_MINOR, ready.proto_major, ready.proto_minor)) {
    ESP_LOGE(TAG, "the co-processor speaks Bridle %u.%u, this firmware %u.%u: update one of them", ready.proto_major,
             ready.proto_minor, PROTO_MAJOR, PROTO_MINOR);
    fail("protocol version mismatch");
    return false;
  }
  ReadyInfo info;
  ReadyInfoStatus const status = parse_ready_info(ready.info, info);
  if (status != ReadyInfoStatus::OK) {
    ESP_LOGE(TAG, "the co-processor's ready lacks or damages: %s", ready_info_status_name(status));
    fail("ready lacks a required field");
    return false;
  }
  this->coproc_major_ = ready.proto_major;
  this->coproc_minor_ = ready.proto_minor;
  // Every `ready` reports `link_takeover` from a takeover until the co-processor's next reboot (spec §2.1).
  if (info.reset_cause == ResetCause::LINK_TAKEOVER)
    ESP_LOGI(TAG, "the co-processor was taken over from another link since its last reboot; configuring it");
  copy_text(this->fw_version_, sizeof(this->fw_version_), info.fw_version);
  copy_text(this->board_, sizeof(this->board_), info.board);
  info.fw_version = {};
  info.board = {};
  this->ready_info_ = info;

  const RadioDescriptor *chosen = nullptr;
  for (uint8_t i = 0; i < info.radio_count; i++) {
    const RadioDescriptor &d = info.radios[i];
    ESP_LOGI(TAG,
             "radio %u: chip 0x%04X, profiles 0x%04X, %" PRIu32 "-%" PRIu32
             " Hz, %d..%d dBm, payload %u, preamble up to %u, caps 0x%08" PRIX32,
             d.radio, d.chip, d.profiles, d.band_min_hz, d.band_max_hz, d.tx_power_min_dbm, d.tx_power_max_dbm,
             d.max_payload, d.max_preamble_bytes, d.caps);
    if (chosen == nullptr && this->phy_.radio_fits(d))
      chosen = &d;
  }
  if (chosen == nullptr) {
    ESP_LOGE(TAG, "none of the co-processor's radios offers the profile, band, capabilities, payload and preamble "
                  "length this band needs");
    fail("no suitable radio");
    return false;
  }
  this->radio_desc_ = *chosen;
  this->radio_ = chosen->radio;
  this->radio_caps_ = chosen->caps;
  this->max_payload_ = chosen->max_payload;
  this->applied_tx_power_dbm_ = tx_power_dbm_.value_or(chosen->tx_power_max_dbm);
  ESP_LOGI(TAG, "using radio %u of %s, firmware %s", this->radio_, this->board_, this->fw_version_);
  return true;
}

uint8_t RadioBridle::send_config_(bool device) {
  uint8_t buf[CONFIG_TLV_CAP];
  TlvWriter tlvs(buf, sizeof(buf));
  if (device) {
    // All bits are 0 after every boot; each event enabled here has a use: frame_start drives the
    // reception state, CRC-error rx fills the capture's crc_error, rx_abort ends a reception early,
    // log shows the co-processor's warnings and errors.
    tlvs.add_u8(static_cast<uint8_t>(ParamId::EVENT_MASK),
                event_mask::FRAME_START | event_mask::CRC_ERROR_RX | event_mask::RX_ABORT | event_mask::LOG);
  } else {
    // phy_profile first: it resets the radio's profile params to their defaults.
    tlvs.add_u8(static_cast<uint8_t>(ParamId::PHY_PROFILE), static_cast<uint8_t>(this->phy_.profile()));
    this->phy_.write_params(tlvs, this->tuning_);
    this->sent_params_len_ = this->build_params_(this->sent_params_, sizeof(this->sent_params_));
    tlvs.add_u8(static_cast<uint8_t>(ParamId::TX_POWER_DBM), static_cast<uint8_t>(this->applied_tx_power_dbm_));
  }
  SetParams cmd;
  cmd.radio = device ? DEVICE_SCOPE : this->radio_;
  cmd.tlvs = tlvs.span();
  return tlvs.ok() ? this->send_command_(cmd) : 0;
}

size_t RadioBridle::build_params_(uint8_t *buf, size_t cap) const {
  TlvWriter tlvs(buf, cap);
  this->phy_.write_params(tlvs, this->tuning_);
  return tlvs.ok() ? tlvs.size() : 0;
}

void RadioBridle::apply_tuning(const TuningConfig &tuning) {
  this->tuning_ = tuning;
  if (!this->link_running())
    return;  // the next configuration carries it
  uint8_t params[CONFIG_TLV_CAP];
  size_t const n = this->build_params_(params, sizeof(params));
  if (n == this->sent_params_len_ && std::memcmp(params, this->sent_params_, n) == 0)
    return;       // nothing the radio is tuned by changed
  SetParams cmd;  // the profile's params alone: `phy_profile` would reset them to their defaults
  cmd.radio = this->radio_;
  cmd.tlvs = {params, n};
  uint8_t const seq = n != 0 ? this->send_command_(cmd) : 0;
  Wait const wait = seq != 0 ? this->await_(seq, MsgType::PARAMS_ACK, ACK_TIMEOUT_MS) : Wait::ERROR;
  if (wait == Wait::REPLIED) {
    std::memcpy(this->sent_params_, params, n);
    this->sent_params_len_ = n;
    ESP_LOGI(TAG, "radio params re-sent for the new tuning");
    return;
  }
  if (wait == Wait::ERROR && seq != 0)
    this->config_reply_ok_(false);  // logs the code and the TLV the refusal names
  ESP_LOGW(TAG, "the co-processor did not take the new tuning; the radio keeps the old params");
}

bool RadioBridle::config_reply_ok_(bool device) {
  if (this->await_errored_) {
    // For param_range and unsupported `detail` is the TLV id that failed (spec §2.2); nothing was applied.
    ErrorCode const code = this->last_error_code_;
    if (code == ErrorCode::PARAM_RANGE || code == ErrorCode::UNSUPPORTED) {
      ESP_LOGE(TAG, "the %s configuration was refused: error %u names TLV 0x%02X", device ? "device" : "radio",
               static_cast<unsigned>(code), static_cast<unsigned>(this->last_error_detail_) & 0xFFU);
    } else {
      ESP_LOGE(TAG, "the %s configuration was refused: error %u (detail %d)", device ? "device" : "radio",
               static_cast<unsigned>(code), this->last_error_detail_);
    }
    return false;
  }
  Message m;
  ByteSpan value;
  if (!device && decode_message(this->reply_, this->reply_len_, m) == DecodeStatus::OK) {
    if (const auto *ack = std::get_if<ParamsAck>(&m.body)) {
      if (find_tlv(ack->applied, static_cast<uint8_t>(ParamId::TX_POWER_DBM), value) && value.size == 1) {
        this->applied_tx_power_dbm_ = static_cast<int8_t>(value.data[0]);
        ESP_LOGI(TAG, "TX power applied: %d dBm", this->applied_tx_power_dbm_);
      }
    }
  }
  return true;
}

bool RadioBridle::configure_() {
  for (bool const device : {true, false}) {
    uint8_t const seq = this->send_config_(device);
    Wait const wait = this->await_(seq, MsgType::PARAMS_ACK, ACK_TIMEOUT_MS);
    if (wait == Wait::TIMEOUT)
      ESP_LOGE(TAG, "the %s configuration was not acknowledged", device ? "device" : "radio");
    // An error ends the wait too; config_reply_ok_() is what logs its code and the TLV it names.
    if (wait == Wait::TIMEOUT || !this->config_reply_ok_(device)) {
      this->fail_(device ? "event mask refused" : "radio configuration refused");
      return false;
    }
  }
  this->scanning_ = false;
  this->rx_on_ = false;
  this->frame_arriving_ = false;
  return true;
}

// === Transmit ===

uint32_t RadioBridle::tx_air_time_us(uint8_t len, const RadioTxConfig &cfg) const {
  TxShape const shape = this->phy_.tx_shape(cfg.wake, cfg.preamble_len);
  size_t const wrapped = this->phy_.wrapped_size(len);
  // The command itself takes UART time before the radio starts: a strobe adds its own fields.
  size_t const command_bytes = 20 + wrapped + (shape.has_strobe ? 22 + shape.strobe.len_template : 0);
  return this->phy_.tx_air_time_us(shape, wrapped) + (static_cast<uint32_t>(command_bytes) * this->uart_byte_us_);
}

TxResult RadioBridle::send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) {
  if (!this->link_running() || len == 0)
    return TxResult::failed();
  uint8_t payload[MAX_MESSAGE_SIZE];
  size_t const wrapped = this->phy_.wrap(data, len, payload, sizeof(payload));
  if (wrapped == 0 || wrapped > this->max_payload_) {
    ESP_LOGW(TAG, "frame of %u bytes does not fit the radio's %u-byte payload", len, this->max_payload_);
    return TxResult::failed();
  }
  TxShape const shape = this->phy_.tx_shape(tx_config.wake, tx_config.preamble_len);
  Tx tx;
  tx.radio = this->radio_;
  tx.freq_hz = this->phy_.air_freq_hz(tx_config.freq_hz);
  tx.then_rx = true;
  tx.preamble_bytes = shape.preamble_bytes;
  tx.has_strobe = shape.has_strobe;
  tx.strobe = shape.strobe;
  tx.payload = {payload, wrapped};
  // Without the capability the frame goes out unchecked, as if no check had been asked for.
  if (tx_config.cca_threshold_dbm.has_value() && (this->radio_caps_ & radio_caps::CCA) != 0) {
    tx.cca = true;
    tx.cca_threshold_dbm = static_cast<int8_t>(std::clamp<int16_t>(*tx_config.cca_threshold_dbm, INT8_MIN, INT8_MAX));
  }
  uint8_t const seq = this->send_command_(tx);
  this->newest_receive_cmd_ = seq;  // a refusal of an earlier rx_on/rx_scan/idle no longer describes the radio
  // tx.freq_hz becomes the radio's frequency and a scan ends (spec §3.1), whatever the outcome.
  this->current_freq_ = tx_config.freq_hz;
  this->scanning_ = false;
  this->rx_on_ = tx.then_rx;
  if (seq == 0)
    return TxResult::failed();
  uint32_t const limit_ms = (this->tx_air_time_us(len, tx_config) / 1000) + TX_DONE_MARGIN_MS;
  switch (this->await_(seq, MsgType::TX_DONE, limit_ms)) {
    case Wait::REPLIED:
      return TxResult::ok();
    case Wait::ERROR: {
      Message m;
      if (decode_message(this->reply_, this->reply_len_, m) == DecodeStatus::OK) {
        if (const auto *e = std::get_if<Error>(&m.body)) {
          if (e->code == ErrorCode::CHANNEL_BUSY)
            return TxResult::channel_busy(e->detail_dbm());  // nothing was sent; queued frames stay
          ESP_LOGW(TAG, "tx refused: error %u detail %u", static_cast<unsigned>(e->code), e->detail);
        }
      }
      return TxResult::failed();
    }
    case Wait::TIMEOUT:
      if (!this->link_running())  // the wait ended because the link went down: that is already being handled
        return TxResult::failed();
      ESP_LOGW(TAG, "no tx_done within %" PRIu32 " ms", limit_ms);
      this->begin_resync_("no tx_done", false);
      return TxResult::failed();
  }
  return TxResult::failed();
}

// === Receive ===

void RadioBridle::on_rx_(const Rx &rx) {
  if (rx.radio != this->radio_)
    return;
  this->frame_arriving_ = false;
  this->clear_reception_in_progress_();
  if (rx.payload.size > MAX_RX_PSDU) {
    this->oversize_dropped_++;
    return;
  }
  // A frame that cannot be an io frame (the strobe frames of a wake-up train are 3 bytes) never
  // reaches the FIFO: a burst of them must not push a real frame out.
  uint8_t scratch[RADIO_PACKET_BUFFER_SIZE];
  if (this->phy_.unwrap(rx.payload.data, rx.payload.size, rx.crc, scratch, sizeof(scratch)).status ==
      UnwrapResult::Status::TOO_SHORT) {
    this->short_frames_++;
    return;
  }
  if (this->fifo_count_ == RX_FIFO_DEPTH) {
    // A newer frame is likelier to be the awaited reply than the oldest one.
    this->fifo_head_ = (this->fifo_head_ + 1) % RX_FIFO_DEPTH;
    this->fifo_count_--;
    this->fifo_overflows_++;
  }
  RxEntry &e = this->fifo_[(this->fifo_head_ + this->fifo_count_) % RX_FIFO_DEPTH];
  e.freq_hz = rx.freq_hz;
  e.rssi_dbm = rx.rssi_dbm;
  e.crc = rx.crc;
  e.len = static_cast<uint8_t>(rx.payload.size);
  if (rx.payload.size > 0)
    std::memcpy(e.data, rx.payload.data, rx.payload.size);
  this->fifo_count_++;
}

bool RadioBridle::decode_entry_(const RxEntry &entry, RadioRxPacket &packet) {
  uint32_t const logical = this->phy_.channel_hz(entry.freq_hz);
  uint32_t const freq = logical != 0 ? logical : entry.freq_hz;
  uint8_t frame[RADIO_PACKET_BUFFER_SIZE];
  UnwrapResult const result = this->phy_.unwrap(entry.data, entry.len, entry.crc, frame, sizeof(frame));
  auto capture = [&](const uint8_t *io_frame, uint8_t io_len) {
    this->populate_capture_base_(false, freq, entry.rssi_dbm, entry.data,
                                 std::min<uint8_t>(entry.len, RADIO_PACKET_BUFFER_SIZE), io_frame, io_len);
    this->last_capture_.rx_done = true;
    this->last_capture_.reported_len = entry.len;
  };
  switch (result.status) {
    case UnwrapResult::Status::OK:
      packet.freq_hz = freq;
      packet.len = result.frame_len;
      std::memcpy(packet.data, frame, result.frame_len);
      capture(frame, result.frame_len);
      return true;
    case UnwrapResult::Status::BAD_CRC:
      this->bad_crc_frames_++;
      capture(nullptr, 0);
      this->last_capture_.crc_error = true;
      return false;
    case UnwrapResult::Status::HEADER_MISMATCH:
      // Possibly a manufacturer variant of the header; worth a line, not worth a failure.
      this->header_mismatches_++;
      ESP_LOGW(TAG, "frame on %" PRIu32 " Hz starts %02X %02X %02X, not the io header", entry.freq_hz, result.header[0],
               result.header[1], result.header[2]);
      capture(nullptr, 0);
      return false;
    case UnwrapResult::Status::TOO_LONG:
      this->oversize_dropped_++;
      return false;
    case UnwrapResult::Status::TOO_SHORT:
      return false;  // on_rx_() keeps these out of the FIFO; only a defensive branch remains
  }
  return false;
}

bool RadioBridle::pop_packet_(RadioRxPacket &packet) {
  while (this->fifo_count_ > 0) {
    RxEntry const entry = this->fifo_[this->fifo_head_];
    this->fifo_head_ = (this->fifo_head_ + 1) % RX_FIFO_DEPTH;
    this->fifo_count_--;
    if (this->decode_entry_(entry, packet))
      return true;
  }
  return false;
}

void RadioBridle::ensure_rx_() {
  if (!this->link_running() || this->rx_on_ || this->scanning_)
    return;
  if (this->rx_refused_ && millis() - this->rx_refused_ms_ < SCAN_RETRY_MS)
    return;  // a refused rx_on is not repeated at once
  this->set_mode_rx();
}

bool RadioBridle::check_for_packet(RadioRxPacket &packet) {
  this->prepare_nonblocking_receive_(packet);
  this->poll_link_();
  return this->pop_packet_(packet);
}

bool RadioBridle::wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) {
  this->prepare_blocking_receive_(packet);
  this->ensure_rx_();
  uint32_t const start = millis();
  do {
    this->poll_link_();
    if (this->pop_packet_(packet)) {
      this->last_capture_.blocking_wait = true;
      return true;
    }
    if (timeout_ms == 0)
      break;
    App.feed_wdt();
  } while (millis() - start < timeout_ms);
  return false;
}

int16_t RadioBridle::read_rssi() {
  if (!this->link_running())
    return RSSI_NONE_DBM;
  uint8_t const seq = this->send_command_(GetRssi{this->radio_});
  if (this->await_(seq, MsgType::RSSI, RSSI_TIMEOUT_MS) != Wait::REPLIED)
    return RSSI_NONE_DBM;  // not_receiving, busy or no answer: no value, and no reason to resync
  Message m;
  if (decode_message(this->reply_, this->reply_len_, m) == DecodeStatus::OK) {
    if (const auto *r = std::get_if<Rssi>(&m.body))
      return r->rssi_dbm;
  }
  return RSSI_NONE_DBM;
}

bool RadioBridle::is_sync_detected() {
  this->poll_link_();
  return this->frame_arriving_ && this->arriving_stage_ == FrameStage::SYNC;
}

bool RadioBridle::is_preamble_detected() {
  this->poll_link_();
  return this->frame_arriving_ && this->arriving_stage_ == FrameStage::PREAMBLE;
}

bool RadioBridle::reception_in_progress() {
  this->poll_link_();
  return this->frame_arriving_ || RadioDriver::reception_in_progress();
}

// === Tuning and modes ===

void RadioBridle::change_frequency(uint32_t freq_hz) {
  bool const changed = freq_hz != this->current_freq_;
  this->current_freq_ = freq_hz;
  // An idle radio only records the frequency: the next receive starts on it. A receiving or scanning one
  // retunes with `rx_on`, which also ends a scan (even one on this frequency).
  if (!this->link_running() || !(this->rx_on_ || this->scanning_) || (!changed && !this->scanning_))
    return;
  RxOn rx_on;
  rx_on.radio = this->radio_;
  rx_on.freq_hz = this->phy_.air_freq_hz(freq_hz);
  this->send_pending_(rx_on, PendingKind::RX_ON);  // answered with an ack that is only matched, never awaited
  this->scanning_ = false;
  this->rx_on_ = true;
}

void RadioBridle::set_mode_rx() {
  if (!this->link_running())
    return;
  RxOn rx_on;
  rx_on.radio = this->radio_;
  rx_on.freq_hz = this->phy_.air_freq_hz(this->current_freq_);
  this->send_pending_(rx_on, PendingKind::RX_ON);
  this->rx_on_ = true;
  this->scanning_ = false;
}

void RadioBridle::set_mode_standby() {
  if (!this->link_running())
    return;
  this->send_pending_(Idle{this->radio_}, PendingKind::IDLE);
  this->rx_on_ = false;
  this->scanning_ = false;
  // `idle` drops a frame being received without an rx_abort, so no frame_start is left open.
  this->frame_arriving_ = false;
  this->clear_reception_in_progress_();
}

bool RadioBridle::run_idle_scan() {
  this->poll_link_();  // the periodic work of the link rides on this pass (see the class docs)
  if (!this->link_running())
    return false;
  if (this->scan_refused_ && millis() - this->scan_refused_ms_ < SCAN_RETRY_MS) {
    // The co-processor refused the scan: the hub hops from the host until the hold-off ends, which needs the
    // radio receiving (change_frequency() only records the frequency of an idle one).
    this->ensure_rx_();
    return false;
  }
  if (!this->scanning_) {
    ScanSpec const spec = this->phy_.idle_scan();
    RxScan scan;
    scan.radio = this->radio_;
    scan.dwell_us = spec.dwell_us;
    scan.hold_us = spec.hold_us;
    scan.num_freqs = spec.num_freqs;
    std::copy(spec.freqs_hz, spec.freqs_hz + spec.num_freqs, scan.freqs_hz);
    this->scan_refused_ = false;
    this->send_pending_(scan, PendingKind::RX_SCAN);
    this->scanning_ = true;
    this->rx_on_ = true;
  }
  return true;
}

// === Link supervision ===

void RadioBridle::poll_link_() {
  this->drain_();
  this->supervise_();
}

namespace {
const char *link_name(RadioBridle::Link state) {
  switch (state) {
    case RadioBridle::Link::INIT:
      return "init";
    case RadioBridle::Link::RUNNING:
      return "running";
    case RadioBridle::Link::WAIT_RESET:
      return "waiting to reset";
    case RadioBridle::Link::WAIT_READY:
      return "waiting for ready";
    case RadioBridle::Link::CONFIG_DEVICE:
      return "configuring the device";
    case RadioBridle::Link::CONFIG_RADIO:
      return "configuring the radio";
    case RadioBridle::Link::DOWN:
      return "down";
  }
  return "?";
}
}  // namespace

void RadioBridle::set_link_(Link state, const char *why) {
  if (state != this->link_)
    ESP_LOGW(TAG, "link %s -> %s: %s", link_name(this->link_), link_name(state), why);
  this->link_ = state;
}

void RadioBridle::begin_resync_(const char *why, bool skip_reset) {
  if (!this->link_running())
    return;
  this->resyncs_++;
  this->pending_count_ = 0;
  this->hb_pending_ = false;
  this->awaiting_ = false;
  this->retrying_from_down_ = false;
  this->hello_seq_ = 0;
  // At most one pulse per RESYNC_MIN_INTERVAL_MS, and never two within the spec's second.
  uint32_t const now = millis();
  this->resync_at_ms_ = this->pulsed_once_ ? std::max(now, this->last_pulse_ms_ + RESYNC_MIN_INTERVAL_MS) : now;
  if (skip_reset) {
    this->deadline_ms_ = now + READY_TIMEOUT_MS;
    this->set_link_(Link::WAIT_READY, why);
  } else {
    this->set_link_(Link::WAIT_RESET, why);
  }
}

void RadioBridle::attempt_failed_(const char *why) {
  this->awaiting_ = false;
  uint32_t const now = millis();
  if (this->retrying_from_down_) {
    this->retrying_from_down_ = false;
    this->next_retry_ms_ = now + DOWN_RETRY_MS;
    this->set_link_(Link::DOWN, why);
    return;
  }
  // Keep the failures of the last minute; the limit-th of them puts the link down.
  size_t kept = 0;
  for (size_t i = 0; i < this->failure_count_; i++) {
    if (now - this->failure_times_ms_[i] <= FAILURE_WINDOW_MS)
      this->failure_times_ms_[kept++] = this->failure_times_ms_[i];
  }
  this->failure_count_ = kept;
  this->failure_times_ms_[this->failure_count_++] = now;
  if (this->failure_count_ >= FAILURE_LIMIT) {
    this->failure_count_ = 0;
    this->next_retry_ms_ = now + DOWN_RETRY_MS;
    this->set_link_(Link::DOWN, why);
    return;
  }
  this->resync_at_ms_ = std::max(now, this->last_pulse_ms_ + RESYNC_MIN_INTERVAL_MS);
  this->set_link_(Link::WAIT_RESET, why);
}

void RadioBridle::finish_resync_() {
  this->awaiting_ = false;
  this->failure_count_ = 0;
  this->retrying_from_down_ = false;
  // The radio is configured but idle: the hub's next wait or idle pass restores receive on the recorded
  // frequency, and the scan.
  this->pending_count_ = 0;
  this->rx_on_ = false;
  this->scanning_ = false;
  this->frame_arriving_ = false;
  this->clear_reception_in_progress_();
  this->fifo_count_ = 0;
  this->fifo_head_ = 0;
  this->last_rx_ms_ = millis();
  this->set_link_(Link::RUNNING, "link restored");
}

void RadioBridle::supervise_() {
  switch (this->link_) {
    case Link::INIT:
      return;
    case Link::RUNNING:
      this->supervise_running_();
      return;
    case Link::WAIT_RESET:
      this->supervise_wait_reset_();
      return;
    case Link::WAIT_READY:
      this->supervise_wait_ready_();
      return;
    case Link::CONFIG_DEVICE:
    case Link::CONFIG_RADIO:
      this->supervise_config_();
      return;
    case Link::DOWN:
      this->supervise_down_();
      return;
  }
}

void RadioBridle::supervise_running_() {
  uint32_t const now = millis();
  if (this->ready_pending_) {
    // A `ready` nobody asked for: the co-processor rebooted. Its cause says why; every radio must
    // be configured again (the event mask is 0 after a boot).
    this->ready_pending_ = false;
    Message m;
    if (decode_message(this->ready_msg_, this->ready_msg_len_, m) == DecodeStatus::OK) {
      ReadyInfo info;
      const Ready *ready = std::get_if<Ready>(&m.body);
      if (ready != nullptr && parse_ready_info(ready->info, info) == ReadyInfoStatus::OK) {
        ESP_LOGW(TAG, "the co-processor rebooted (reset cause %u); configuring it again",
                 static_cast<unsigned>(info.reset_cause));
      }
    }
    this->begin_resync_("unsolicited ready", true);
    this->ready_pending_ = true;  // the WAIT_READY step takes it
    return;
  }
  if (this->pending_count_ > 0 && now - this->pending_[0].sent_ms >= ACK_TIMEOUT_MS) {
    this->begin_resync_("no ack for an unawaited command", false);
    return;
  }
  if (this->hb_pending_) {
    if (now - this->hb_sent_ms_ >= HEARTBEAT_REPLY_MS) {
      this->hb_pending_ = false;
      this->begin_resync_("no answer to the heartbeat", false);
    }
  } else if (now - this->last_command_sent_ms_ >= HEARTBEAT_SILENCE_MS) {
    this->hb_seq_ = this->send_command_(GetStats{});
    this->hb_pending_ = this->hb_seq_ != 0;
    this->hb_sent_ms_ = now;
  }
}

void RadioBridle::supervise_wait_reset_() {
  if (millis() < this->resync_at_ms_)
    return;
  this->ready_pending_ = false;
  this->expected_counter_known_ = false;
  this->reset_hardware_();  // the one blocking step: a 10 ms pulse and 10 ms of settling
  this->last_pulse_ms_ = millis();
  this->pulsed_once_ = true;
  this->deadline_ms_ = this->last_pulse_ms_ + READY_TIMEOUT_MS;
  this->hello_seq_ = 0;
  if (this->rst_pin_ == nullptr) {
    // No reset line: only a `hello` can take the co-processor back from another host, and its `ready` answers it
    // (spec §7).
    this->hello_seq_ = this->send_command_(Hello{});
  }
  this->set_link_(Link::WAIT_READY, "reset pulsed");
}

void RadioBridle::start_awaiting_config_(uint8_t seq) {
  this->awaiting_ = seq != 0;
  this->await_seq_ = seq;
  this->await_type_ = MsgType::PARAMS_ACK;
  this->await_done_ = false;
  this->await_errored_ = false;
  this->deadline_ms_ = millis() + ACK_TIMEOUT_MS;
}

void RadioBridle::supervise_wait_ready_() {
  if (!this->ready_pending_) {
    if (millis() < this->deadline_ms_)
      return;
    if (this->hello_seq_ == 0) {
      // No boot `ready` after the pulse: the reset line may not reach the co-processor, or another host's claim
      // was released without a reset. Ask with `hello`, as the start-up handshake does (spec §8.1, §2.1).
      this->hello_seq_ = this->send_command_(Hello{});
      this->deadline_ms_ = millis() + HELLO_TIMEOUT_MS;
      if (this->hello_seq_ != 0)
        return;
    }
    this->attempt_failed_("no ready after the reset");
    return;
  }
  this->ready_pending_ = false;
  Message m;
  const Ready *ready = decode_message(this->ready_msg_, this->ready_msg_len_, m) == DecodeStatus::OK
                           ? std::get_if<Ready>(&m.body)
                           : nullptr;
  // A co-processor that comes back different (version, radios) is not usable: down, not failed.
  if (ready == nullptr || !this->accept_ready_(*ready, false)) {
    this->attempt_failed_("the ready after the reset was not acceptable");
    return;
  }
  this->start_awaiting_config_(this->send_config_(true));
  this->set_link_(Link::CONFIG_DEVICE, "ready accepted");
}

void RadioBridle::supervise_config_() {
  bool const device = this->link_ == Link::CONFIG_DEVICE;
  if (!this->await_done_) {
    if (millis() >= this->deadline_ms_)
      this->attempt_failed_("configuration not acknowledged");
    return;
  }
  this->awaiting_ = false;
  if (!this->config_reply_ok_(device)) {
    this->attempt_failed_("configuration refused");
  } else if (device) {
    this->start_awaiting_config_(this->send_config_(false));
    this->set_link_(Link::CONFIG_RADIO, "device configured");
  } else {
    this->finish_resync_();
  }
}

void RadioBridle::supervise_down_() {
  uint32_t const now = millis();
  if (now < this->next_retry_ms_)
    return;
  this->retrying_from_down_ = true;
  this->resync_at_ms_ = std::max(now, this->pulsed_once_ ? this->last_pulse_ms_ + RESYNC_MIN_INTERVAL_MS : now);
  this->set_link_(Link::WAIT_RESET, "retrying a down link");
}

// === Diagnostics ===

void RadioBridle::dump_debug() {
  ESP_LOGCONFIG(TAG, "  Bridle co-processor: %s, firmware %s", this->board_, this->fw_version_);
  ESP_LOGCONFIG(TAG, "    Protocol %u.%u, radio %u (chip 0x%04X), payload limit %u, caps 0x%08" PRIX32, PROTO_MAJOR,
                PROTO_MINOR, this->radio_, this->radio_desc_.chip, this->max_payload_, this->radio_caps_);
  ESP_LOGCONFIG(TAG, "    Preamble up to %u bytes, strobe jitter %u us", this->radio_desc_.max_preamble_bytes,
                this->radio_desc_.strobe_jitter_us);
  ESP_LOGCONFIG(TAG, "    TX power applied: %d dBm", this->applied_tx_power_dbm_);
  if (this->ready_info_.has_rx_queue_depth)
    ESP_LOGCONFIG(TAG, "    Co-processor RX queue: %u events", this->ready_info_.rx_queue_depth);
  ESP_LOGCONFIG(TAG, "    Reset cause: %u", static_cast<unsigned>(this->ready_info_.reset_cause));
  ESP_LOGCONFIG(TAG, "    Link: %s, resyncs: %" PRIu32, link_name(this->link_), this->resyncs_);
  if (this->have_stats_)
    ESP_LOGCONFIG(TAG, "    Log events dropped by the co-processor: %" PRIu32, this->log_dropped_);
  ESP_LOGCONFIG(TAG, "    Link errors: %" PRIu32 " (bad COBS %" PRIu32 ", CRC %" PRIu32 ", length %" PRIu32 ")",
                this->deframer_.counters().total(), this->deframer_.counters().bad_cobs,
                this->deframer_.counters().bad_crc, this->deframer_.counters().bad_length);
  ESP_LOGCONFIG(TAG, "    Event counter gaps: %" PRIu32 ", co-processor overflow reports: %" PRIu32,
                this->counter_gaps_, this->rx_overflow_reports_);
  ESP_LOGCONFIG(TAG,
                "    Frames dropped: before tx_done %" PRIu32 ", FIFO full %" PRIu32 ", oversize %" PRIu32
                ", header mismatch %" PRIu32 ", bad CRC %" PRIu32 ", too short %" PRIu32,
                this->dropped_before_tx_done_, this->fifo_overflows_, this->oversize_dropped_, this->header_mismatches_,
                this->bad_crc_frames_, this->short_frames_);
  ESP_LOGCONFIG(TAG,
                "    Late strobe frames: %" PRIu32 ", stale replies: %" PRIu32
                ", errors to unawaited commands: %" PRIu32 ", log events: %" PRIu32,
                this->strobe_late_total_, this->stale_replies_, this->unawaited_errors_, this->log_events_);
}

}  // namespace esphome::home_io_control

#endif  // IOHOME_BRIDLE
