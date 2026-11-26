/// @file sim_bridge.cpp
/// @brief The simulator bridge (see the header).
/// @ingroup hioc_radio

#include "sim_bridge.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

#include "proto_frame.h"
#include "radio_bridle_codec.h"

namespace {

/// The bytes of a hex string (the simulator's lines carry at most 255 of them), through the project's own parser.
std::vector<uint8_t> from_hex(const std::string &hex) {
  std::vector<uint8_t> out(hex.size() / 2);
  if (hex.size() % 2 != 0 || out.size() > 255 ||
      !esphome::home_io_control::hex_to_bytes(hex, out.data(), static_cast<uint8_t>(out.size())))
    throw std::runtime_error("the simulator sent hex that is not a whole number of bytes: " + hex);
  return out;
}

// Lower-case hex of any length with no separators: the project's formatters (log_frame.h) write spaced upper-case
// text into a fixed buffer, which the control channel's `air` request cannot take.
std::string to_hex(const std::vector<uint8_t> &bytes) {
  static const char DIGITS[] = "0123456789abcdef";
  std::string out;
  for (uint8_t b : bytes) {
    out.push_back(DIGITS[b >> 4]);
    out.push_back(DIGITS[b & 0x0F]);
  }
  return out;
}

void write_all(int fd, const void *data, size_t n) {
  const auto *p = static_cast<const uint8_t *>(data);
  while (n > 0) {
    const ssize_t written = ::write(fd, p, n);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      throw std::runtime_error("the simulator went away while it was being written to");
    p += written;
    n -= static_cast<size_t>(written);
  }
}

}  // namespace

SimBridge::SimBridge(const std::string &sim_path) {
  if (!esphome::test_clock::is_manual())
    throw std::runtime_error("SimBridge needs the manual clock");
  if (::access(sim_path.c_str(), X_OK) != 0)
    throw std::runtime_error("the simulator binary is missing or not executable: " + sim_path + " (set BRIDLE_SIM)");
  // A write to a simulator that has gone must be an error the test sees, not a signal that ends the test binary.
  previous_sigpipe_ = ::signal(SIGPIPE, SIG_IGN);
  int control[2], reply[2], link0[2];
  if (::pipe(control) != 0 || ::pipe(reply) != 0 || ::pipe(link0) != 0)
    throw std::runtime_error("cannot make pipes");
  pid_ = ::fork();
  if (pid_ < 0)
    throw std::runtime_error("cannot start the simulator");
  if (pid_ == 0) {
    ::dup2(link0[0], STDIN_FILENO);
    ::close(link0[0]);
    ::close(link0[1]);
    ::close(control[1]);
    ::close(reply[0]);
    const std::string control_in = std::to_string(control[0]);
    const std::string control_out = std::to_string(reply[1]);
    ::execl(sim_path.c_str(), sim_path.c_str(), "--control-fd-in", control_in.c_str(), "--control-fd-out",
            control_out.c_str(), static_cast<char *>(nullptr));
    ::_exit(127);
  }
  ::close(control[0]);
  ::close(reply[1]);
  ::close(link0[0]);
  control_w_ = control[1];
  control_r_ = reply[0];
  link0_w_ = link0[1];
  for (int fd : {control_w_, control_r_, link0_w_})
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  origin_us_ = esphome::test_clock::state().now_us;
  stream_.set_pump([this] { pump(); });
  stream_.set_write_handler([this](const uint8_t *data, size_t n) { on_host_write_(data, n); });
  reset_.on_release = [this] { take_(request_("reset")); };
}

void SimBridge::terminate_simulator() {
  if (pid_ > 0) {
    ::kill(pid_, SIGKILL);
    int status = 0;
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
  }
}

SimBridge::~SimBridge() {
  // The simulator ends when its control channel closes.
  for (int fd : {control_w_, link0_w_, control_r_})
    if (fd >= 0)
      ::close(fd);
  if (pid_ > 0) {
    int status = 0;
    ::waitpid(pid_, &status, 0);
  }
  ::signal(SIGPIPE, previous_sigpipe_);
}

std::vector<std::string> SimBridge::request_(const std::string &line) {
  const std::string text = line + "\n";
  write_all(control_w_, text.data(), text.size());
  std::vector<std::string> data;
  while (true) {
    const size_t newline = reply_buffer_.find('\n');
    if (newline == std::string::npos) {
      char buffer[8192];
      const ssize_t n = ::read(control_r_, buffer, sizeof(buffer));
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        throw std::runtime_error("the simulator ended while answering \"" + line + "\"");
      reply_buffer_.append(buffer, static_cast<size_t>(n));
      continue;
    }
    const std::string reply = reply_buffer_.substr(0, newline);
    reply_buffer_.erase(0, newline + 1);
    if (reply == "ok")
      return data;
    if (reply.rfind("error", 0) == 0)
      throw std::runtime_error("\"" + line + "\": " + reply);
    data.push_back(reply);
  }
}

void SimBridge::take_(const std::vector<std::string> &lines) {
  namespace bridle = esphome::home_io_control::bridle;
  bool wrote = false;
  for (const std::string &line : lines) {
    std::istringstream in(line);
    std::string kind;
    in >> kind;
    if (kind == "now") {
      in >> sim_now_us_;
    } else if (kind == "frame") {
      size_t link = 0;
      uint64_t t_us = 0;
      std::string hex;
      in >> link >> t_us >> hex;
      if (link != 0)
        continue;
      const std::vector<uint8_t> message = from_hex(hex);
      uint8_t wire[bridle::MAX_WIRE_SIZE];
      const size_t n = bridle::frame_message(message.data(), message.size(), wire, sizeof(wire));
      stream_.push_rx(wire, n);
      wrote = true;
    }
  }
  if (wrote)
    refresh_air_();
}

void SimBridge::refresh_air_() {
  if (!air_handler_)
    return;
  const std::vector<AirTx> log = air_log();
  if (log.size() <= air_seen_)
    return;
  const size_t first_new = air_seen_;
  air_seen_ = log.size();
  air_handler_(log, first_new);
}

std::vector<SimBridge::AirTx> SimBridge::air_log() {
  std::vector<AirTx> out;
  for (const std::string &line : request_("air_log")) {
    std::istringstream in(line);
    std::string kind, psdu_hex;
    AirTx tx;
    unsigned sfd = 0;
    in >> kind >> tx.freq_hz >> tx.start_us >> tx.sync_us >> tx.end_us >> sfd >> psdu_hex;
    if (kind != "tx")
      continue;
    tx.sfd = static_cast<uint8_t>(sfd);
    tx.psdu = from_hex(psdu_hex);
    out.push_back(std::move(tx));
  }
  return out;
}

void SimBridge::inject_air(uint32_t freq_hz, uint64_t start_us, const std::vector<uint8_t> &psdu, int level_dbm) {
  std::ostringstream line;
  line << "air {\"freq_hz\":" << freq_hz << ",\"t_us\":" << start_us << ",\"psdu_hex\":\"" << to_hex(psdu)
       << "\",\"level_dbm\":" << level_dbm << "}";
  take_(request_(line.str()));
}

void SimBridge::advance_(uint64_t us) { take_(request_("advance " + std::to_string(us) + " stop")); }

void SimBridge::write_to_sim_(const std::vector<uint8_t> &bytes) { write_all(link0_w_, bytes.data(), bytes.size()); }

void SimBridge::flush_due_writes_() {
  while (!held_.empty() && held_.front().due_us <= sim_now_us_) {
    write_to_sim_(held_.front().bytes);
    held_.pop_front();
  }
}

void SimBridge::on_host_write_(const uint8_t *data, size_t n) {
  pump();  // the bytes arrive at the simulator's present, which is the test's present
  const std::vector<uint8_t> bytes(data, data + n);
  if (uplink_delay_us_ == 0 && held_.empty()) {
    write_to_sim_(bytes);
    return;
  }
  const uint64_t due = std::max(sim_now_us_ + uplink_delay_us_, held_.empty() ? 0 : held_.back().due_us);
  held_.push_back({due, bytes});
}

void SimBridge::pump() {
  if (pumping_)
    return;
  pumping_ = true;
  const uint64_t target = esphome::test_clock::state().now_us - origin_us_;
  while (true) {
    flush_due_writes_();
    if (sim_now_us_ >= target)
      break;
    uint64_t until = target;
    if (!held_.empty())
      until = std::min(until, held_.front().due_us);
    advance_(until - sim_now_us_);
  }
  pumping_ = false;
}
