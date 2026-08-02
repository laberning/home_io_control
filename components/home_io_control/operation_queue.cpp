/// @file operation_queue.cpp
/// @brief OperationQueue implementation.
/// @ingroup hioc_hub

#include "operation_queue.h"

#include <algorithm>

namespace esphome {
namespace home_io_control {

bool OperationQueue::is_background_op(PendingOperationType t) {
  return t == PendingOperationType::REQUEST_STATUS || t == PendingOperationType::REQUEST_NAME;
}

void OperationQueue::push_control_(PendingOperation op) {
  queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                              [&op](const PendingOperation &existing) {
                                return existing.type == PendingOperationType::REQUEST_STATUS &&
                                       existing.device_id == op.device_id;
                              }),
               queue_.end());
  auto it =
      std::find_if(queue_.begin(), queue_.end(), [](const PendingOperation &o) { return is_background_op(o.type); });
  queue_.insert(it, std::move(op));
}

bool OperationQueue::enqueue_set_position(const std::string &device_id, uint8_t position) {
  for (auto &op : queue_) {
    if (op.type == PendingOperationType::SET_TILT && op.device_id == device_id) {
      // SET_TILT stores tilt in op.position (the only field available on standalone tilt ops).
      uint8_t const pending_tilt = op.position;
      op.type = PendingOperationType::SET_POSITION_AND_TILT;
      op.position = position;
      op.tilt = pending_tilt;
      return true;
    }
  }
  push_control_({PendingOperationType::SET_POSITION, device_id, position});
  return false;
}

bool OperationQueue::enqueue_set_tilt(const std::string &device_id, uint8_t tilt_percent) {
  for (auto &op : queue_) {
    if (op.type == PendingOperationType::SET_POSITION && op.device_id == device_id) {
      op.type = PendingOperationType::SET_POSITION_AND_TILT;
      op.tilt = tilt_percent;
      // op.position already holds the correct position from the original SET_POSITION enqueue.
      return true;
    }
  }
  push_control_({PendingOperationType::SET_TILT, device_id, tilt_percent});
  return false;
}

void OperationQueue::enqueue_set_position_and_tilt(const std::string &device_id, uint8_t position,
                                                   uint8_t tilt_percent) {
  push_control_({PendingOperationType::SET_POSITION_AND_TILT, device_id, position, tilt_percent});
}

void OperationQueue::enqueue_device_command(const std::string &device_id, CoverCommand cmd) {
  PendingOperation op{};
  op.type = PendingOperationType::DEVICE_COMMAND;
  op.device_id = device_id;
  op.command = cmd;
  push_control_(std::move(op));
}

void OperationQueue::enqueue_set_light_position(const std::string &device_id, uint8_t position) {
  push_control_({PendingOperationType::SET_LIGHT_STATE, device_id, position});
}

void OperationQueue::enqueue_set_light_state(const std::string &device_id, bool on) {
  this->enqueue_set_light_position(device_id, on ? BINARY_ENTITY_ON_POSITION : BINARY_ENTITY_OFF_POSITION);
}

void OperationQueue::enqueue_set_lock_state(const std::string &device_id, bool locked) {
  push_control_({PendingOperationType::SET_LOCK_STATE, device_id,
                 locked ? BINARY_ENTITY_OFF_POSITION : BINARY_ENTITY_ON_POSITION});
}

void OperationQueue::enqueue_set_switch_state(const std::string &device_id, bool on) {
  push_control_(
      {PendingOperationType::SET_SWITCH_STATE, device_id, on ? BINARY_ENTITY_ON_POSITION : BINARY_ENTITY_OFF_POSITION});
}

bool OperationQueue::enqueue_request_status(const std::string &device_id) {
  for (const auto &op : queue_) {
    if (op.type == PendingOperationType::REQUEST_STATUS && op.device_id == device_id)
      return false;
  }
  queue_.push_back({PendingOperationType::REQUEST_STATUS, device_id, 0});
  return true;
}

bool OperationQueue::enqueue_request_name(const std::string &device_id) {
  for (const auto &op : queue_) {
    if (op.type == PendingOperationType::REQUEST_NAME && op.device_id == device_id)
      return false;
  }
  queue_.push_back({PendingOperationType::REQUEST_NAME, device_id, 0});
  return true;
}

bool OperationQueue::enqueue_discover_and_pair() {
  for (const auto &op : queue_) {
    if (op.type == PendingOperationType::DISCOVER_AND_PAIR)
      return false;
  }
  queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                              [](const PendingOperation &op) {
                                return op.type == PendingOperationType::REQUEST_STATUS ||
                                       op.type == PendingOperationType::REQUEST_NAME;
                              }),
               queue_.end());
  queue_.push_front({PendingOperationType::DISCOVER_AND_PAIR, {}, 0});
  return true;
}

std::optional<PendingOperation> OperationQueue::pop() {
  if (queue_.empty())
    return std::nullopt;
  PendingOperation op = std::move(queue_.front());
  queue_.pop_front();
  return op;
}

bool OperationQueue::empty() const { return queue_.empty(); }

std::size_t OperationQueue::size() const { return queue_.size(); }

const PendingOperation &OperationQueue::front() const { return queue_.front(); }

const PendingOperation &OperationQueue::back() const { return queue_.back(); }

const PendingOperation &OperationQueue::operator[](std::size_t index) const { return queue_[index]; }

std::deque<PendingOperation>::const_iterator OperationQueue::begin() const { return queue_.cbegin(); }

std::deque<PendingOperation>::const_iterator OperationQueue::end() const { return queue_.cend(); }

}  // namespace home_io_control
}  // namespace esphome
