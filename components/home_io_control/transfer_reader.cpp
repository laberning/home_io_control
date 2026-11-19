/// @file transfer_reader.cpp
/// @brief TransferReader: the queue, the timeout chain and the rules for giving way.
/// @ingroup hioc_hub

#include "transfer_reader.h"

#include "esphome/core/hal.h"
#include "log_helpers.h"
#include "proto_timing.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace esphome {
namespace home_io_control {

namespace {
/// Name of the one timeout the read chain uses (a single chain: a step schedules only the next). set_timeout()
/// replaces a pending timeout of the same name, so two chains can never run side by side.
const char *const TIMEOUT_NAME = "transfer_read";
}  // namespace

// === Foreign transfers ===

void TransferReader::note_transfer_frame(const IoFrame &frame, uint32_t now) {
  if (frame.cmd < CMD_TRANSFER_OPEN || frame.cmd > CMD_UNKNOWN4A_RESP)
    return;
  // Our own never come back here (the radio does not hear its own transmissions, and the answers to our
  // requests are taken by the exchange). A frame addressed to us is a late answer to one of our requests; any
  // other belongs to another controller, one sharing our node ID included.
  if (std::memcmp(frame.dst, this->node_id_, NODE_ID_SIZE) == 0)
    return;
  this->drop_expired_foreign_(now);
  for (const uint8_t *endpoint : {frame.src, frame.dst}) {
    const std::string device_id = node_id_to_string(endpoint);
    // Only registered devices are tracked, so the list is bounded by the registry.
    if (this->hub_.find_device(device_id) == nullptr)
      continue;
    const auto known = std::find_if(this->foreign_.begin(), this->foreign_.end(),
                                    [&](const ForeignTransfer &entry) { return entry.device_id == device_id; });
    if (known != this->foreign_.end()) {
      known->heard_ms = now;
    } else {
      this->foreign_.push_back({device_id, now});
      ESP_LOGD(detail::TAG, "%s: another controller is using the data transport, reads wait", device_id.c_str());
    }
  }
}

void TransferReader::drop_expired_foreign_(uint32_t now) {
  // Unsigned subtraction: the elapsed time stays right across a millis() wrap.
  std::erase_if(this->foreign_,
                [now](const ForeignTransfer &entry) { return now - entry.heard_ms >= TRANSFER_FOREIGN_QUIET_MS; });
}

bool TransferReader::foreign_active_(const std::string &device_id) {
  this->drop_expired_foreign_(millis());
  return std::any_of(this->foreign_.begin(), this->foreign_.end(),
                     [&](const ForeignTransfer &entry) { return entry.device_id == device_id; });
}

bool TransferReader::transfer_open_to(const std::string &device_id) const {
  return this->session_.has_value() && this->session_->started() && this->reads_.front().device_id == device_id;
}

// === The queue ===

void TransferReader::read(const std::string &device_id, size_t recipe_index, uint16_t offset, TransferReadDone done) {
  const std::vector<uint8_t> none;
  if (recipe_index >= this->recipes_.size() || !transfer_recipe_allows_offset(this->recipes_[recipe_index], offset) ||
      this->hub_.find_device(device_id) == nullptr) {
    ESP_LOGW(detail::TAG, "%s: transfer read of recipe %u at offset 0x%04X refused (unknown recipe, offset or device)",
             device_id.c_str(), static_cast<unsigned>(recipe_index), static_cast<unsigned>(offset));
    if (done)
      done(TransferOutcome::REFUSED, none);
    return;
  }
  // Bounded: a caller queuing reads faster than they complete must not eat the heap.
  if (this->reads_.size() > QUEUE_MAX) {
    ESP_LOGW(detail::TAG, "%s: transfer read queue full, read of recipe %u skipped", device_id.c_str(),
             static_cast<unsigned>(recipe_index));
    if (done)
      done(TransferOutcome::ABANDONED, none);
    return;
  }
  this->reads_.push_back(Read{device_id, recipe_index, offset, std::move(done)});
  if (!this->session_.has_value() && !this->waiting_for_quiet_)
    this->start_next_();
}

void TransferReader::after_(uint32_t delay_ms, std::function<void()> step) {
  this->hub_.set_timeout(TIMEOUT_NAME, delay_ms, [this, step = std::move(step)]() mutable {
    // A timeout fires whatever the hub is doing; its own work (a command, pairing, a key extraction, a one-way
    // burst) keeps the radio, and this read waits behind it like a background poll.
    if (!this->hub_.background_radio_allowed()) {
      this->after_(TRANSFER_RADIO_HELD_RETRY_MS, std::move(step));
      return;
    }
    step();
  });
}

void TransferReader::start_next_() {
  this->waiting_for_quiet_ = false;
  if (this->reads_.empty()) {
    this->session_.reset();
    return;
  }
  // The first read whose device is quiet goes first: a device another controller keeps busy holds its own reads
  // only, and reads of devices that are not held stay first in, first out (ADR 0108).
  const auto ready = std::find_if(this->reads_.begin(), this->reads_.end(),
                                  [this](const Read &read) { return !this->foreign_active_(read.device_id); });
  if (ready == this->reads_.end()) {
    // Every queued device is held: choose again after a retry wait. No session exists meanwhile, so no read is
    // started and read() leaves the pending choice alone.
    this->session_.reset();
    this->waiting_for_quiet_ = true;
    this->after_(TRANSFER_FOREIGN_RETRY_MS, [this]() { this->start_next_(); });
    return;
  }
  std::rotate(this->reads_.begin(), ready, ready + 1);
  const Read &read = this->reads_.front();
  this->session_.emplace(this->recipes_[read.recipe_index], read.offset, this->recipes_);
  // Start on the next loop pass, never inside the caller (a sensor update or another read's callback): an
  // exchange started from there would run in the middle of the caller's own work. A read that follows another
  // keeps the step gap after the device's last answer.
  const uint32_t since_last_exchange = millis() - this->last_exchange_ms_;
  const uint32_t delay_ms =
      this->exchanged_ && since_last_exchange < TRANSFER_STEP_GAP_MS ? TRANSFER_STEP_GAP_MS - since_last_exchange : 0;
  this->after_(delay_ms, [this]() { this->step_(); });
}

// === One step ===

void TransferReader::step_() {
  if (!this->session_.has_value())
    return;  // Unreachable: a step is armed only for the front read, which owns the session.
  TransferSession &session = *this->session_;
  // A reference into reads_: finish_() erases it, so it is not used after a finish_() call.
  const Read &read = this->reads_.front();
  // Looked up on each step, not kept: the registry owns the device.
  const IoDevice *dev = this->hub_.find_device(read.device_id);
  if (dev == nullptr) {
    // A close would need a send path to a device the hub no longer records exchanges with; a transfer the device
    // accepted stays open until the device drops it (ADR 0108).
    ESP_LOGW(detail::TAG, "%s: device removed, transfer read dropped%s", read.device_id.c_str(),
             session.started() ? " (a transfer it accepted may be left open)" : "");
    this->finish_(TransferOutcome::ABANDONED);
    return;
  }
  if (this->foreign_active_(read.device_id)) {
    if (!session.started()) {
      // Nothing of ours is open: another controller took the device since this read was chosen. Choose again, which
      // lets a read of a quiet device go first or waits until this one is quiet.
      this->session_.reset();
      this->start_next_();
      return;
    }
    // The other controller started a transfer while ours is open: free the device at once (the next request is
    // the close) rather than interleave with it. Waiting would keep our handle open and the device busy.
    ESP_LOGW(detail::TAG, "%s: another controller is using the data transport, transfer read abandoned",
             read.device_id.c_str());
    session.abandon();
  }

  IoFrame request;
  IoFrame reply;
  if (!session.next_request(this->node_id_, dev->node_id, dev->low_power, request)) {
    this->finish_(session.outcome());
    return;
  }
  const RecordedAnswer answer = this->hub_.exchange(read.device_id, request, reply);
  this->last_exchange_ms_ = millis();
  this->exchanged_ = true;
  session.on_answer(answer == RecordedAnswer::REPLY ? &reply : nullptr, answer == RecordedAnswer::ACCEPTED_NO_REPLY);
  if (session.done()) {
    this->finish_(session.outcome());
    return;
  }
  // One gap after the device's last answer before the next request.
  this->after_(TRANSFER_STEP_GAP_MS, [this]() { this->step_(); });
}

void TransferReader::finish_(TransferOutcome outcome) {
  // Moved out before the callback: it may queue another read, which can move the queue in memory.
  const Read read = std::move(this->reads_.front());
  this->reads_.pop_front();
  const std::vector<uint8_t> data = this->session_.has_value() ? this->session_->data() : std::vector<uint8_t>{};
  ESP_LOGD(detail::TAG, "%s: transfer read of recipe %u at offset 0x%04X ended: %s (%u bytes)", read.device_id.c_str(),
           static_cast<unsigned>(read.recipe_index), static_cast<unsigned>(read.offset), transfer_outcome_name(outcome),
           static_cast<unsigned>(data.size()));
  this->session_.reset();
  if (read.done)
    read.done(outcome, data);
  // The callback may have queued a read, which started the chain itself (the session was reset).
  if (!this->session_.has_value() && !this->waiting_for_quiet_)
    this->start_next_();
}

}  // namespace home_io_control
}  // namespace esphome
