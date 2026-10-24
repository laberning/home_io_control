/// @file io_blind_emulator.cpp
/// @brief The 2.4 GHz blind model (see the header).
/// @ingroup hioc_hub

#include "io_blind_emulator.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "proto_commands.h"
#include "proto_constants.h"
#include "proto_frame.h"
#include "radio_io2g4_phy.h"
#include "test_helpers.h"

namespace fullstack {

using namespace esphome::home_io_control;
// `bridle::` alone is ambiguous here: the global ::bridle (the Bridle release) is visible too.
namespace hioc_bridle = esphome::home_io_control::bridle;

namespace {

constexpr uint64_t PREAMBLE_AND_SFD_US = 160;  // 4 preamble bytes and the SFD at 32 µs
constexpr uint8_t STATUS_KIND = 0x60;          // byte 1 of the blind's status reply, resting or travelling
constexpr uint8_t STATUS_RESTING_FLAGS = 0x05;
constexpr uint8_t STATUS_MOVING_FLAGS = 0x04;
constexpr uint16_t RAW_CLOSED = 0xC800;
constexpr uint16_t RAW_STOP = static_cast<uint16_t>(POS_STOP) << 8;
// The data of the 0x04 that confirms an EXECUTE (a captured blind's answer: it carries no position the hub
// trusts, which the hub's own decoder knows).
constexpr uint8_t EXECUTE_ACK_DATA[8] = {0x54, 0x60, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00};

}  // namespace

uint64_t EmulatedReply::end_us() const {
  const uint64_t psdu = hioc_bridle::IO2G4_PSDU_HEADER_SIZE + io.size() + hioc_bridle::IO2G4_FCS_SIZE;
  return start_us + PREAMBLE_AND_SFD_US + (1 + psdu) * hioc_bridle::IO2G4_BYTE_US;
}

IoBlindEmulator::IoBlindEmulator(const Config &config)
    : config_(config),
      rest_raw_(static_cast<uint16_t>(std::lround(config.position_pct * 512.0))),
      next_challenge_(config.challenge_seed) {
  std::memcpy(commander_, config.commander, sizeof(commander_));
}

void IoBlindEmulator::settle_(uint64_t now_us) {
  if (journey_ && current_raw(now_us) == journey_->target_raw) {
    rest_raw_ = journey_->target_raw;
    journey_.reset();
  }
}

uint16_t IoBlindEmulator::current_raw(uint64_t now_us) const {
  if (!journey_)
    return rest_raw_;
  const double rate = RAW_CLOSED / config_.travel_s;  // raw units a second
  const auto travelled =
      static_cast<int64_t>(std::llround(static_cast<double>(now_us - journey_->since_us) / 1e6 * rate));
  const int64_t distance = std::abs(static_cast<int>(journey_->target_raw) - static_cast<int>(journey_->from_raw));
  if (travelled >= distance)
    return journey_->target_raw;
  return static_cast<uint16_t>(journey_->target_raw > journey_->from_raw ? journey_->from_raw + travelled
                                                                         : journey_->from_raw - travelled);
}

bool IoBlindEmulator::moving(uint64_t now_us) const { return journey_ && current_raw(now_us) != journey_->target_raw; }

std::vector<uint8_t> IoBlindEmulator::status_data(uint64_t now_us) const {
  const uint16_t current = current_raw(now_us);
  const bool travelling = moving(now_us);
  const uint16_t target = travelling ? journey_->target_raw : current;
  uint16_t remaining_s = 1;  // what a blind at rest reports
  if (travelling) {
    const double rate = RAW_CLOSED / config_.travel_s;
    remaining_s =
        static_cast<uint16_t>(std::ceil(std::abs(static_cast<int>(target) - static_cast<int>(current)) / rate));
  }
  std::vector<uint8_t> data = {travelling ? STATUS_MOVING_FLAGS : STATUS_RESTING_FLAGS,
                               STATUS_KIND,
                               static_cast<uint8_t>(target >> 8),
                               static_cast<uint8_t>(target),
                               static_cast<uint8_t>(current >> 8),
                               static_cast<uint8_t>(current),
                               static_cast<uint8_t>(remaining_s >> 8),
                               static_cast<uint8_t>(remaining_s)};
  data.insert(data.end(), commander_, commander_ + sizeof(commander_));
  data.insert(data.end(), {0x01, 0x00, 0x00});
  return data;
}

bool IoBlindEmulator::awake_(const HeardFrame &frame) const {
  if (moving(frame.start_us))
    return true;
  return last_addressed_us_ && frame.start_us - *last_addressed_us_ <= config_.awake_window_us;
}

EmulatedReply IoBlindEmulator::reply_(const HeardFrame &to, uint8_t cmd, const std::vector<uint8_t> &data,
                                      bool end) const {
  IoFrame request{};
  parse(to.io.data(), static_cast<uint8_t>(to.io.size()), request);
  IoFrame f{};
  init_frame(f, true, false, end, false);
  set_dst(f, request.src);
  set_src(f, config_.node);
  set_cmd(f, cmd, data.data(), static_cast<uint8_t>(data.size()));
  EmulatedReply reply;
  uint8_t buf[FRAME_MAX_SIZE];
  const uint8_t n = serialize(f, buf, sizeof(buf));
  reply.io.assign(buf, buf + n);
  reply.freq_hz = to.freq_hz;
  // The SFD of the reply ends `reply_sfd_after_request_us` after the request's end; the frame starts before that.
  reply.start_us = to.end_us + config_.reply_sfd_after_request_us - PREAMBLE_AND_SFD_US;
  return reply;
}

bool IoBlindEmulator::begin_move_(uint16_t main, uint64_t now_us) {
  const uint16_t current = current_raw(now_us);
  if (main == RAW_STOP) {
    rest_raw_ = current;
    journey_.reset();
    return true;
  }
  if (main > RAW_CLOSED)
    return false;  // favourite, unknown and the rest: no capture shows what a real blind does, so it does nothing
  rest_raw_ = current;
  journey_ = Journey{current, main, now_us};
  settle_(now_us);
  return true;
}

void IoBlindEmulator::set_off(uint16_t target_raw, uint64_t now_us) { begin_move_(target_raw, now_us); }

IoBlindEmulator::Config default_blind_config() {
  IoBlindEmulator::Config config;
  std::copy(std::begin(test::DST_ID), std::end(test::DST_ID), std::begin(config.node));
  std::copy(std::begin(test::TEST_SYSTEM_KEY), std::end(test::TEST_SYSTEM_KEY), std::begin(config.key));
  return config;
}

std::vector<EmulatedReply> IoBlindEmulator::hear(const HeardFrame &frame) {
  std::vector<EmulatedReply> out;
  IoFrame request{};
  if (!parse(frame.io.data(), static_cast<uint8_t>(frame.io.size()), request) ||
      std::memcmp(request.dst, config_.node, NODE_ID_SIZE) != 0)
    return out;  // not for this blind
  EmulatorEvent event;
  event.at_us = frame.start_us;
  event.wake_frames = frame.wake_frames;
  settle_(frame.start_us);

  if (config_.only_freq_hz != 0 && frame.freq_hz != config_.only_freq_hz) {
    event.kind = EmulatorEvent::Kind::LOST;
    events_.push_back(event);
    return out;
  }
  if (to_lose_ > 0) {
    to_lose_--;
    event.kind = EmulatorEvent::Kind::LOST;
    events_.push_back(event);
    return out;
  }
  if (is_start(request) && frame.wake_frames < config_.long_train_min_frames && !awake_(frame)) {
    event.kind = EmulatorEvent::Kind::SLEPT_THROUGH;
    events_.push_back(event);
    return out;
  }
  last_addressed_us_ = frame.start_us;

  if (request.cmd == CMD_PRIVATE) {
    out.push_back(reply_(frame, CMD_PRIVATE_RESP, status_data(frame.start_us), true));
    event.kind = EmulatorEvent::Kind::STATUS_ANSWERED;
    event.reply_gap_us = out.back().start_us - frame.end_us;
  } else if (request.cmd == CMD_EXECUTE) {
    OpenExchange open;
    open.request = frame.io;
    for (uint8_t &byte : open.challenge)
      byte = next_challenge_++;
    EmulatedReply challenge =
        reply_(frame, CMD_CHALLENGE_REQ, std::vector<uint8_t>(open.challenge, open.challenge + 6), false);
    open.challenge_end_us = challenge.end_us();
    open_ = open;
    out.push_back(std::move(challenge));
    event.kind = EmulatorEvent::Kind::EXECUTE_CHALLENGED;
    event.reply_gap_us = out.back().start_us - frame.end_us;
  } else if (request.cmd == CMD_CHALLENGE_RESP && open_ && request.data_len >= HMAC_SIZE) {
    // What the right answer is: the hub's own builder, over the EXECUTE we challenged.
    IoFrame challenged{};
    parse(open_->request.data(), static_cast<uint8_t>(open_->request.size()), challenged);
    IoFrame expected{};
    create_challenge_resp(expected, config_.node, request.src, open_->challenge, challenged, config_.key);
    const bool right = std::memcmp(expected.data, request.data, HMAC_SIZE) == 0;
    event.gap_us = frame.start_us - open_->challenge_end_us;
    if (event.gap_us > config_.answer_window_us) {
      event.kind = EmulatorEvent::Kind::ANSWER_TOO_LATE;
    } else if (!right) {
      event.kind = EmulatorEvent::Kind::ANSWER_WRONG;
    } else if (const auto main = static_cast<uint16_t>((challenged.data[EXECUTE_MAIN_BYTE_OFFSET] << 8) |
                                                       challenged.data[EXECUTE_MAIN_BYTE_OFFSET + 1]);
               !begin_move_(main, frame.start_us)) {
      event.kind = EmulatorEvent::Kind::UNEXPECTED;  // a main parameter the blind does not understand
    } else {
      event.kind = EmulatorEvent::Kind::ANSWER_ACCEPTED;
      std::memcpy(commander_, challenged.src, sizeof(commander_));
      accepted_++;
      out.push_back(reply_(frame, CMD_PRIVATE_RESP,
                           std::vector<uint8_t>(EXECUTE_ACK_DATA, EXECUTE_ACK_DATA + sizeof(EXECUTE_ACK_DATA)), true));
      event.reply_gap_us = out.back().start_us - frame.end_us;
    }
    open_.reset();
  } else {
    event.kind = EmulatorEvent::Kind::UNEXPECTED;
  }
  events_.push_back(event);
  return out;
}

}  // namespace fullstack
