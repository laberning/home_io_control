/// @file platform_cover.cpp
/// @brief ESPHome cover entity for IO-Homecontrol devices.
/// @ingroup hioc_platforms

#include "platform_cover.h"
#include "hub_internal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.cover";

void IOHomeCover::setup() {
  // Covers compute their initial inversion from the explicit YAML override or the device-type
  // default; the rest of the registration ritual is shared with the other entity types.
  const bool initial_invert = this->invert_explicit_ ? this->invert_ : default_inverted_for_type(this->device_type_);
  this->register_device_binding_(
      this, initial_invert, [this](const std::string &id, const IoDevice &dev) { this->on_device_update_(id, dev); });
}

cover::CoverTraits IOHomeCover::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_supports_position(true);  // Slider in HA UI
  traits.set_supports_stop(true);      // Stop button in HA UI
  traits.set_supports_tilt(this->supports_tilt());
  traits.set_is_assumed_state(false);  // We hopefully get real feedback from the device
  return traits;
}

bool IOHomeCover::supports_tilt() const {
  return this->device_type_ != DeviceType::UNKNOWN && device_supports_tilt(this->device_type_);
}

bool IOHomeCover::effective_invert_() const {
  if (this->invert_explicit_)
    return this->invert_;
  if (this->parent_ == nullptr)
    return false;
  const auto *dev = this->parent_->get_device(this->device_id_);
  return dev != nullptr && dev->inverted;
}

cover::CoverOperation IOHomeCover::infer_operation_from_position_delta_(bool invert, float current_io_position) const {
  if (this->last_io_position_ == UNKNOWN_POSITION || current_io_position == UNKNOWN_POSITION ||
      current_io_position == this->last_io_position_) {
    return cover::COVER_OPERATION_IDLE;
  }

  if (invert) {
    return (current_io_position > this->last_io_position_) ? cover::COVER_OPERATION_OPENING
                                                           : cover::COVER_OPERATION_CLOSING;
  }

  return (current_io_position < this->last_io_position_) ? cover::COVER_OPERATION_OPENING
                                                         : cover::COVER_OPERATION_CLOSING;
}

void IOHomeCover::control(const cover::CoverCall &call) {
  // Optimistic state gives immediate HA UI feedback for the queue-dispatch + TX/response gap;
  // the queued command's own response (update_device_status_()) remains the source of truth
  // and will overwrite this. No-op when this device has optimistic_state=false (see
  // DeviceRegistry::apply_optimistic_target()/clear_optimistic_target()).
  if (call.get_stop()) {
    this->parent_->clear_optimistic_target(this->device_id_);
    this->parent_->queue_device_command(this->device_id_, CoverCommand::STOP);
    return;
  }

  const auto &tilt_opt = call.get_tilt();
  const auto &position_opt = call.get_position();

  // Combined position+tilt in one atomic command when both are present.
  /// @todo Monitor https://github.com/home-assistant/core/issues/174533 — if HA adds a combined
  ///       cover.set_cover_position_and_tilt action, this branch would be exercised directly from
  ///       a single CoverCall. The queue coalescing in queue_set_device_position/tilt remains a
  ///       useful optimization for the two-separate-calls path regardless.
  // Position/tilt fraction -> IO percent uses detail::round_percent() (hub_internal.h) — rounds
  // rather than truncates, since HA quantizes call values to 0-255 before they reach us (its
  // "50%" is 128/255=0.502, not exactly 0.5) and a truncating cast would compound that into a
  // consistent ~1% bias (caught on hardware; see PlatformCover.ControlRoundsQuantizedPosition-
  // InsteadOfTruncating).
  if (position_opt.has_value() && tilt_opt.has_value() && this->supports_tilt()) {
    float const ha_pos = *position_opt;
    const bool invert = this->effective_invert_();
    uint8_t const io_pos = invert ? detail::round_percent(ha_pos) : detail::round_percent(1.0F - ha_pos);
    auto const tilt = detail::round_percent(*tilt_opt);
    this->parent_->apply_optimistic_target(this->device_id_, io_pos);
    this->parent_->queue_set_device_position_and_tilt(this->device_id_, io_pos, tilt);
    return;
  }

  if (tilt_opt.has_value()) {
    auto const tilt = detail::round_percent(*tilt_opt);
    this->parent_->queue_set_device_tilt(this->device_id_, tilt);
    return;
  }

  if (position_opt.has_value()) {
    float const ha_pos = *position_opt;  // HA: 1.0 = fully open, 0.0 = fully closed
    const bool invert = this->effective_invert_();

    // Convert HA position (0.0-1.0) to IO position (0-100)
    // Standard: HA 1.0 (open) → IO 0 (open), HA 0.0 (closed) → IO 100 (closed)
    // Inverted: HA 1.0 (open) → IO 100, HA 0.0 (closed) → IO 0
    // (used for devices like horizontal awnings where the IO convention is reversed)
    const uint8_t io_pos = invert ? detail::round_percent(ha_pos) : detail::round_percent(1.0F - ha_pos);

    this->parent_->apply_optimistic_target(this->device_id_, io_pos);
    this->parent_->queue_set_device_position(this->device_id_, io_pos);
  }
}

void IOHomeCover::on_device_update_(const std::string &id, const IoDevice &dev) {
  if (id != this->device_id_)
    return;

  const bool invert = this->effective_invert_();
  const float previous_io_position = this->last_io_position_;

  if (dev.position != UNKNOWN_POSITION) {
    // Convert IO position (0-100) back to HA position (0.0-1.0)
    float ha_pos;
    if (invert) {
      ha_pos = dev.position / 100.0F;
    } else {
      ha_pos = 1.0F - (dev.position / 100.0F);
    }

    this->position = ha_pos;
  }

  if (this->supports_tilt() && dev.tilt != UNKNOWN_POSITION) {
    this->tilt = dev.tilt / 100.0F;
  }

  // Determine movement direction for the HA UI animation
  if (dev.is_stopped) {
    this->current_operation = cover::COVER_OPERATION_IDLE;
  } else if (dev.target != UNKNOWN_POSITION && dev.position != UNKNOWN_POSITION) {
    // Figure out if we're opening or closing based on target vs current position
    if (invert) {
      this->current_operation =
          (dev.target > dev.position) ? cover::COVER_OPERATION_OPENING : cover::COVER_OPERATION_CLOSING;
    } else {
      // Standard: lower IO position = more open, so target < current = opening
      this->current_operation =
          (dev.target < dev.position) ? cover::COVER_OPERATION_OPENING : cover::COVER_OPERATION_CLOSING;
    }
  } else if (dev.position != UNKNOWN_POSITION) {
    this->current_operation = this->infer_operation_from_position_delta_(invert, dev.position);
  }

  if (dev.position != UNKNOWN_POSITION) {
    this->last_io_position_ = dev.position;
  } else {
    this->last_io_position_ = previous_io_position;
  }

  this->publish_state();
}

void IOHomeCover::dump_config() {
  LOG_COVER("", "IO-Homecontrol Cover", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Invert Position Override: %s", this->invert_explicit_ ? YESNO(this->invert_) : "AUTO");
  this->log_poll_interval_config_(TAG);
  ESP_LOGCONFIG(TAG, "  Supports Tilt: %s", YESNO(this->supports_tilt()));
}

}  // namespace home_io_control
}  // namespace esphome
