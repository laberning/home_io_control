/// @file platform_cover.cpp
/// @brief ESPHome cover entity for IO-Homecontrol devices.
/// @ingroup hioc_platforms

#include "platform_cover.h"
#include "hub_internal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.cover";

namespace {

/// Movement direction implied by travelling from one IO position toward another.
///
/// One rule for every direction question this entity asks, so the inversion handling and the
/// "no information" case cannot drift between call sites. Equal endpoints, or either end unknown,
/// mean the direction is not known — never a direction picked by whichever way a comparison
/// happens to fall. Pure position arithmetic, so it lives here rather than on the entity.
/// @param invert Whether this device's open/close mapping is inverted.
/// @param from_io_position Starting IO position, or UNKNOWN_POSITION.
/// @param to_io_position Destination IO position, or UNKNOWN_POSITION.
cover::CoverOperation operation_toward(bool invert, float from_io_position, float to_io_position) {
  if (from_io_position == UNKNOWN_POSITION || to_io_position == UNKNOWN_POSITION ||
      from_io_position == to_io_position) {
    return cover::COVER_OPERATION_IDLE;
  }
  const bool opening = invert ? (to_io_position > from_io_position) : (to_io_position < from_io_position);
  return opening ? cover::COVER_OPERATION_OPENING : cover::COVER_OPERATION_CLOSING;
}

}  // namespace

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
  return operation_toward(invert, this->last_io_position_, current_io_position);
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
    this->parent_->apply_optimistic_tilt(this->device_id_, tilt);
    this->parent_->queue_set_device_position_and_tilt(this->device_id_, io_pos, tilt);
    return;
  }

  if (tilt_opt.has_value()) {
    auto const tilt = detail::round_percent(*tilt_opt);
    // Position commands get their optimistic feedback from apply_optimistic_target() above; tilt
    // needs its own because a tilt command's reply carries no usable slat angle, so without this
    // the slider snaps back to the pre-command angle until the next status poll.
    this->parent_->apply_optimistic_tilt(this->device_id_, tilt);
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
  } else if (dev.target != UNKNOWN_POSITION && dev.position != UNKNOWN_POSITION && dev.target != dev.position) {
    // Travelling toward a target we can see, from a position we can see.
    this->current_operation = operation_toward(invert, dev.position, dev.target);
  } else if (dev.target != UNKNOWN_POSITION && dev.position == UNKNOWN_POSITION) {
    // Moving, target known, live position withheld. Not every actuator publishes intermediate
    // positions: some report current = POS_UNKNOWN (0xD4) on every poll mid-travel (flagging
    // themselves "moving" vs. "at rest" instead), and only report a real value once they settle.
    // The last position we *did* see is enough to say which way the device is going, and
    // `this->position` keeps displaying that value meanwhile rather than blanking, because the
    // assignment above is skipped for an unknown reading.
    this->current_operation = operation_toward(invert, previous_io_position, dev.target);
  } else if (dev.position != UNKNOWN_POSITION) {
    // Either no target at all, or a target equal to the current position while the device says it
    // is moving. The second case is not a standstill: a device flags itself moving while still
    // reporting its *pre-command* target for roughly the first second, so equal endpoints here mean
    // "no information yet", not "closing". Fall through to the delta inference, which reports IDLE
    // until an actual position change reveals the direction.
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
