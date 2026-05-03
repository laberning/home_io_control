#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>

namespace esphome {
namespace light {

enum class ColorMode { ON_OFF };

struct LightTraits {
  void set_supported_color_modes(const std::initializer_list<ColorMode> &modes) { (void) modes; }
};

class LightState;  // forward declaration

class LightOutput {
 public:
  virtual ~LightOutput() = default;
  virtual void setup_state(LightState *state) = 0;
  virtual void write_state(LightState *state) = 0;
  virtual LightTraits get_traits() = 0;
  virtual void publish_state(bool state) { (void) state; }
};

class Call {
 public:
  Call(LightState *st) : state_(st), target_(false), save_(true) {}
  Call &set_state(bool on) {
    target_ = on;
    return *this;
  }
  Call &set_save(bool save) {
    save_ = save;
    return *this;
  }
  void perform();
  LightState *state_;
  bool target_;
  bool save_;
};

class LightState {
 public:
  struct Values {
    bool is_on() const { return value_; }
    void set(bool v) { value_ = v; }

   private:
    bool value_{false};
  } current_values, remote_values;

  void current_values_as_binary(bool *out) const { *out = current_values.is_on(); }
  void set_current_on(bool on) { current_values.set(on); }

  Call make_call() { return Call(this); }
};

inline void Call::perform() {
  if (state_) {
    state_->set_current_on(target_);
  }
}

}  // namespace light
}  // namespace esphome
