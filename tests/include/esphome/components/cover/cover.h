#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace esphome {
namespace cover {

static constexpr float COVER_OPEN = 1.0f;
static constexpr float COVER_CLOSED = 0.0f;

struct CoverTraits {
  void set_supports_position(bool supports) { supports_position_ = supports; }
  void set_supports_stop(bool stop) { (void) stop; }
  void set_supports_tilt(bool tilt) { supports_tilt_ = tilt; }
  void set_is_assumed_state(bool assumed) { is_assumed_state_ = assumed; }
  bool get_supports_position() const { return supports_position_; }
  bool get_supports_tilt() const { return supports_tilt_; }
  bool get_is_assumed_state() const { return is_assumed_state_; }

 private:
  bool supports_position_{false};
  bool supports_tilt_{false};
  bool is_assumed_state_{false};
};

enum CoverOperation {
  COVER_OPERATION_IDLE,
  COVER_OPERATION_OPENING,
  COVER_OPERATION_CLOSING,
};

class Cover {
 public:
  virtual ~Cover() = default;
  virtual void control(const class CoverCall &call) = 0;
  virtual CoverTraits get_traits() = 0;

  float position{COVER_OPEN};  // the real class starts open
  float tilt{0.0f};
  CoverOperation current_operation{COVER_OPERATION_IDLE};
  void publish_state() {}  // no-op
};

class CoverCall {
 public:
  explicit CoverCall(Cover *parent) : parent_(parent) {}
  CoverCall &set_position(float pos) {
    position_ = pos;
    return *this;
  }
  CoverCall &set_tilt(float tilt) {
    tilt_ = tilt;
    return *this;
  }
  CoverCall &set_stop(bool stop) {
    stop_ = stop;
    return *this;
  }
  bool get_stop() const { return stop_; }
  std::optional<float> get_position() const { return position_; }
  const std::optional<float> &get_tilt() const { return tilt_; }

 private:
  [[maybe_unused]] Cover *parent_;
  std::optional<float> position_;
  std::optional<float> tilt_;
  bool stop_{false};
};

#define LOG_COVER(prefix, type, obj) ((void) 0)

}  // namespace cover
}  // namespace esphome
