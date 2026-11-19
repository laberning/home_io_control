/// @file transfer_session.cpp
/// @brief TransferSession: the request and answer order of one read of a device object, and what each answer means.
/// @ingroup hioc_hub

#include "transfer_session.h"

#include "proto_commands.h"

namespace esphome {
namespace home_io_control {

const char *transfer_outcome_name(TransferOutcome outcome) {
  switch (outcome) {
    case TransferOutcome::READ:
      return "read";
    case TransferOutcome::EMPTY:
      return "empty";
    case TransferOutcome::DECLINED:
      return "declined";
    case TransferOutcome::NO_SUCH_OBJECT:
      return "no such object";
    case TransferOutcome::REFUSED:
      return "refused";
    case TransferOutcome::MALFORMED:
      return "malformed answer";
    case TransferOutcome::TOO_LONG:
      return "too long";
    case TransferOutcome::INCOMPLETE:
      return "incomplete";
    case TransferOutcome::NO_ANSWER:
      return "no answer";
    case TransferOutcome::NO_DATA:
      return "accepted, no answer";
    case TransferOutcome::ABANDONED:
      return "abandoned";
  }
  return "unknown";
}

// === Requests ===

bool TransferSession::next_request(const uint8_t *own, const uint8_t *dst, bool low_power, IoFrame &request) {
  switch (this->phase_) {
    case Phase::OPEN: {
      const TransferObject &object = this->recipe_->opens[this->step_].object;
      if (!create_transfer_open(request, own, dst, object, this->offset_, low_power, this->allowlist_)) {
        this->finish_(TransferOutcome::REFUSED);
        return false;
      }
      this->started_ = true;
      return true;
    }
    case Phase::BLOCKS:
      return create_transfer_read(request, own, dst, this->open_.handle, this->chunk_, low_power);
    case Phase::CLOSE:
      return create_transfer_read(request, own, dst, this->open_.handle, TRANSFER_CHUNK_CLOSE, low_power);
    case Phase::DONE:
      return false;
  }
  return false;
}

// === Answers ===

void TransferSession::on_answer(const IoFrame *reply, bool request_accepted) {
  switch (this->phase_) {
    case Phase::OPEN:
      this->on_open_answer_(reply, request_accepted);
      break;
    case Phase::BLOCKS:
      this->on_block_answer_(reply, request_accepted);
      break;
    case Phase::CLOSE:
      // The answer to the close is not checked: the read ends either way.
      this->finish_(this->outcome_);
      break;
    case Phase::DONE:
      break;
  }
}

void TransferSession::on_open_answer_(const IoFrame *reply, bool request_accepted) {
  this->ended_on_accepted_silence_ = reply == nullptr && request_accepted;
  TransferOpenReply answer;
  const bool well_formed = reply != nullptr && decode_transfer_open_reply(*reply, answer);
  if (well_formed) {
    this->open_ = answer;
    this->open_answered_ = true;
  }
  const bool has_data = well_formed && answer.ok() && answer.length > 0;
  if (!has_data) {
    this->end_open_without_data_(reply, well_formed);
    return;
  }
  // An object far longer than the recipe expects means a misreading: do not spend the shared band on it. The
  // open was accepted, so the transfer is still closed.
  if (answer.length > this->recipe_->max_bytes) {
    this->close_then_(TransferOutcome::TOO_LONG);
    return;
  }
  this->data_.reserve(answer.length);
  this->chunk_ = transfer_chunk_count(answer.length);
  this->phase_ = Phase::BLOCKS;
}

void TransferSession::end_open_without_data_(const IoFrame *reply, bool well_formed) {
  const bool last_step = this->step_ + 1 >= this->recipe_->opens.size();
  // A preparing step is expected to come back empty, and whatever it did say, the next open follows.
  if (!last_step && this->recipe_->opens[this->step_].expect_empty) {
    this->step_++;
    return;
  }
  TransferOutcome outcome = this->ended_on_accepted_silence_ ? TransferOutcome::NO_DATA : TransferOutcome::NO_ANSWER;
  if (reply != nullptr && !well_formed) {
    outcome = reply->cmd == CMD_ERROR_RESP ? TransferOutcome::NO_SUCH_OBJECT : TransferOutcome::MALFORMED;
  } else if (well_formed) {
    const bool nothing_to_read = this->open_.status == TRANSFER_STATUS_NOTHING_TO_READ && this->open_.length == 0;
    if (this->open_.ok() || nothing_to_read) {
      outcome = TransferOutcome::EMPTY;
    } else if (this->open_.status == TRANSFER_STATUS_NO_SUCH_OBJECT) {
      outcome = TransferOutcome::NO_SUCH_OBJECT;
    } else {
      outcome = TransferOutcome::DECLINED;
    }
  }
  // A device that now and then answers an open empty gets the whole open sequence once more.
  if (outcome == TransferOutcome::EMPTY && this->recipe_->retry_empty_open && !this->retried_) {
    this->retried_ = true;
    this->step_ = 0;
    return;
  }
  this->finish_(outcome);
}

void TransferSession::on_block_answer_(const IoFrame *reply, bool request_accepted) {
  this->ended_on_accepted_silence_ = reply == nullptr && request_accepted;
  const uint8_t *block = nullptr;
  uint8_t block_len = 0;
  // The blocks arrive counting down but their data runs forward; a block that echoes another chunk is a late
  // answer and ends the read rather than being spliced in. The size check caps the total at the recipe's limit
  // even if the blocks disagree with the length the open announced.
  if (reply == nullptr || !decode_transfer_block(*reply, this->open_.handle, this->chunk_, block, block_len) ||
      this->data_.size() + block_len > this->recipe_->max_bytes) {
    this->close_then_(TransferOutcome::INCOMPLETE);
    return;
  }
  this->data_.insert(this->data_.end(), block, block + block_len);
  this->chunk_--;
  if (this->chunk_ == 0) {
    // The blocks are all in; the read is complete only if they add up to the length the open announced.
    this->close_then_(this->data_.size() == this->open_.length ? TransferOutcome::READ : TransferOutcome::INCOMPLETE);
  }
}

// === Ending ===

void TransferSession::abandon() {
  switch (this->phase_) {
    case Phase::OPEN:
      this->finish_(TransferOutcome::ABANDONED);
      break;
    case Phase::BLOCKS:
      this->close_then_(TransferOutcome::ABANDONED);
      break;
    case Phase::CLOSE:
      // The close is next anyway and the read already has its ending: data read in full stays READ.
    case Phase::DONE:
      break;
  }
}

void TransferSession::close_then_(TransferOutcome outcome) {
  this->outcome_ = outcome;
  this->phase_ = Phase::CLOSE;
}

void TransferSession::finish_(TransferOutcome outcome) {
  this->outcome_ = outcome;
  this->phase_ = Phase::DONE;
}

}  // namespace home_io_control
}  // namespace esphome
