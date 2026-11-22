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
  Call &set_transition_length(uint32_t length_ms) {
    transition_length_ = length_ms;
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
  std::optional<uint32_t> transition_length_;
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
  void set_current_on(bool on) {
    current_values.set(on);
    remote_values.set(on);
  }
  void set_current_brightness(float b) {
    current_values.set_brightness(b);
    remote_values.set_brightness(b);
  }

  // Mirrors the real LightState::is_transformer_active(): true while a transition or flash writes its intermediate
  // values. test_transformer_active_ lets a test stand in for a transition in progress.
  bool is_transformer_active() const { return this->test_transformer_active_; }
  bool test_transformer_active_{false};

  Call make_call() { return Call(this); }

  // Mirrors the real set_default_transition_length(): a call that names no transition length takes this one.
  // test_last_transition_ms_ records the length the last performed call ran with (0 is an instant change).
  void set_default_transition_length(uint32_t length_ms) { this->default_transition_length_ = length_ms; }
  uint32_t default_transition_length_{0};
  uint32_t test_last_transition_ms_{0};

  // Mirrors the real LightState::loop(): a performed call leaves one pending write, and the light's next loop
  // hands the LightState to its output (write_if_pending_()), reading the values it holds at that moment. A
  // test binds the output with set_test_output(); the real LightState::setup() does that with its own.
  void set_test_output(LightOutput *output) { this->test_output_ = output; }
  void loop() {
    if (!this->next_write_)
      return;
    this->next_write_ = false;
    if (this->test_output_ != nullptr)
      this->test_output_->write_state(this);
  }
  bool next_write_{false};
  LightOutput *test_output_{nullptr};
};

inline void Call::perform() {
  if (state_) {
    // Like the real LightState: the call's target becomes the remote values at once (and, with no
    // transition, the current values too).
    state_->remote_values.set(target_);
    if (has_brightness_)
      state_->remote_values.set_brightness(brightness_);
    // An instant call sets the current values too and leaves one pending write (set_immediately_()). A call with a
    // transition leaves both to the transformer (start_transition_()): it moves the current values in later loops and
    // writes each step with is_transformer_active() true, which a test stands in for with test_transformer_active_.
    const uint32_t length = transition_length_.value_or(state_->default_transition_length_);
    if (length == 0) {
      state_->set_current_on(target_);
      if (has_brightness_)
        state_->set_current_brightness(brightness_);
      state_->next_write_ = true;
    }
    state_->test_last_transition_ms_ = length;
  }
}

}  // namespace light
}  // namespace esphome
