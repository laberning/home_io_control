#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>

namespace esphome {
namespace light {

enum class ColorMode { ON_OFF, BRIGHTNESS };

struct LightTraits {
  void set_supported_color_modes(const std::initializer_list<ColorMode> &modes) {
    if (modes.size() > 0)
      mode_ = *modes.begin();
  }
  ColorMode get_color_mode() const { return mode_; }

 private:
  ColorMode mode_{ColorMode::ON_OFF};
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
  Call &set_brightness(float brightness) {
    brightness_ = brightness;
    has_brightness_ = true;
    return *this;
  }
  Call &set_save(bool save) {
    save_ = save;
    return *this;
  }
  void perform();
  LightState *state_;
  bool target_;
  float brightness_{1.0f};
  bool has_brightness_{false};
  bool save_;
};

class LightState {
 public:
  struct Values {
    bool is_on() const { return value_; }
    void set(bool v) { value_ = v; }
    float get_brightness() const { return brightness_; }
    void set_brightness(float b) { brightness_ = b; }
    // Mirrors the real LightColorValues::as_brightness(): folds on/off into the scalar (no gamma
    // — that's applied separately by LightState::current_values_as_brightness(), not here).
    void as_brightness(float *out) const { *out = value_ ? brightness_ : 0.0f; }

   private:
    bool value_{false};
    float brightness_{1.0f};
  } current_values, remote_values;

  void current_values_as_binary(bool *out) const { *out = current_values.is_on(); }
  // Mirrors the real LightState::current_values_as_brightness(): gamma-corrects on top of
  // Values::as_brightness(). Kept for API-surface parity even though platform_light.cpp
  // deliberately avoids this one (see its write_state() comment) in favor of the ungamma'd
  // current_values.as_brightness() above. test_gamma_scale_ lets a test simulate gamma actually
  // distorting the value (the real bug: gamma_correct defaults to 2.8, not 1.0), so a test can
  // assert write_state() used the raw path and not this one.
  void current_values_as_brightness(float *out) const {
    current_values.as_brightness(out);
    *out *= this->test_gamma_scale_;
  }
  float test_gamma_scale_{1.0f};
  void set_current_on(bool on) { current_values.set(on); }
  void set_current_brightness(float b) {
    current_values.set_brightness(b);
    remote_values.set_brightness(b);
  }

  Call make_call() { return Call(this); }
};

inline void Call::perform() {
  if (state_) {
    state_->set_current_on(target_);
    if (has_brightness_)
      state_->set_current_brightness(brightness_);
  }
}

}  // namespace light
}  // namespace esphome
