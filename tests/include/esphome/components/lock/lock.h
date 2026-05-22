#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>

namespace esphome {
namespace lock {

#define LOG_LOCK(prefix, type, obj) ((void) 0)

enum LockState : uint8_t {
  LOCK_STATE_NONE = 0,
  LOCK_STATE_LOCKED = 1,
  LOCK_STATE_UNLOCKED = 2,
  LOCK_STATE_JAMMED = 3,
  LOCK_STATE_LOCKING = 4,
  LOCK_STATE_UNLOCKING = 5,
};

class Lock;

class LockTraits {
 public:
  bool get_supports_open() const { return this->supports_open_; }
  void set_supports_open(bool supports_open) { this->supports_open_ = supports_open; }
  bool get_requires_code() const { return this->requires_code_; }
  void set_requires_code(bool requires_code) { this->requires_code_ = requires_code; }
  bool get_assumed_state() const { return this->assumed_state_; }
  void set_assumed_state(bool assumed_state) { this->assumed_state_ = assumed_state; }
  void set_supported_states(std::initializer_list<LockState> states) {
    this->supported_states_mask_ = 0;
    for (auto state : states) {
      this->supported_states_mask_ |= static_cast<uint8_t>(1U << static_cast<uint8_t>(state));
    }
  }
  bool supports_state(LockState state) const {
    return (this->supported_states_mask_ & static_cast<uint8_t>(1U << static_cast<uint8_t>(state))) != 0;
  }

 protected:
  bool supports_open_{false};
  bool requires_code_{false};
  bool assumed_state_{false};
  uint8_t supported_states_mask_{
      static_cast<uint8_t>((1U << LOCK_STATE_NONE) | (1U << LOCK_STATE_LOCKED) | (1U << LOCK_STATE_UNLOCKED))};
};

class LockCall {
 public:
  explicit LockCall(Lock *parent) : parent_(parent) {}

  LockCall &set_state(LockState state) {
    this->state_ = state;
    return *this;
  }
  const std::optional<LockState> &get_state() const { return this->state_; }
  void perform();

 protected:
  Lock *const parent_;
  std::optional<LockState> state_;
};

class Lock {
 public:
  virtual ~Lock() = default;

  LockCall make_call() { return LockCall(this); }
  void publish_state(LockState state) { this->state = state; }
  void lock() { this->set_state_(LOCK_STATE_LOCKED); }
  void unlock() { this->set_state_(LOCK_STATE_UNLOCKED); }

  LockState state{LOCK_STATE_NONE};
  LockTraits traits;

 protected:
  friend class LockCall;

  void set_state_(LockState state) {
    LockCall call(this);
    call.set_state(state);
    this->control(call);
  }
  virtual void control(const LockCall &call) = 0;
};

inline void LockCall::perform() {
  if (this->parent_ != nullptr) {
    this->parent_->control(*this);
  }
}

}  // namespace lock
}  // namespace esphome