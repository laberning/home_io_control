#include "platform_cover.h"
#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.cover";

void IOHomeCover::setup() {
  // Register this device with the controller so it's included in the device map
  this->parent_->add_device(this->device_id_);

  // Subscribe to status updates from the controller
  this->parent_->register_device_callback(
      [this](const std::string &id, const IoDevice &dev) { this->on_device_update_(id, dev); });

  // Request initial status after a short delay (give radio time to start hopping)
  this->set_timeout("init_status", 5000, [this]() { this->parent_->queue_request_device_status(this->device_id_); });
}

cover::CoverTraits IOHomeCover::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_supports_position(true);  // Slider in HA UI
  traits.set_supports_stop(true);      // Stop button in HA UI
  traits.set_is_assumed_state(false);  // We hopefully get real feedback from the device
  return traits;
}

void IOHomeCover::control(const cover::CoverCall &call) {
  if (call.get_stop()) {
    this->parent_->queue_set_device_position(this->device_id_, POS_STOP);
    return;
  }

  const auto &position_opt = call.get_position();
  if (position_opt.has_value()) {
    float ha_pos = *position_opt;  // HA: 1.0 = fully open, 0.0 = fully closed

    // Convert HA position (0.0-1.0) to IO position (0-100)
    // Standard: HA 1.0 (open) → IO 0 (open), HA 0.0 (closed) → IO 100 (closed)
    // Inverted: HA 1.0 (open) → IO 100, HA 0.0 (closed) → IO 0
    // (used for devices like horizontal awnings where the IO convention is reversed)
    uint8_t io_pos;
    if (this->invert_) {
      io_pos = (uint8_t) (ha_pos * 100.0F);
    } else {
      io_pos = (uint8_t) ((1.0F - ha_pos) * 100.0F);
    }

    this->parent_->queue_set_device_position(this->device_id_, io_pos);
  }
}

void IOHomeCover::on_device_update_(const std::string &id, const IoDevice &dev) {
  if (id != this->device_id_)
    return;

  if (dev.position == UNKNOWN_POSITION || !dev.is_stopped)
    return;

  // Convert IO position (0-100) back to HA position (0.0-1.0)
  float ha_pos;
  if (this->invert_) {
    ha_pos = dev.position / 100.0F;
  } else {
    ha_pos = 1.0F - (dev.position / 100.0F);
  }

  this->position = ha_pos;

  // Determine movement direction for the HA UI animation
  if (dev.is_stopped) {
    this->current_operation = cover::COVER_OPERATION_IDLE;
  } else if (dev.target != UNKNOWN_POSITION) {
    // Figure out if we're opening or closing based on target vs current position
    if (this->invert_) {
      this->current_operation =
          (dev.target > dev.position) ? cover::COVER_OPERATION_OPENING : cover::COVER_OPERATION_CLOSING;
    } else {
      // Standard: lower IO position = more open, so target < current = opening
      this->current_operation =
          (dev.target < dev.position) ? cover::COVER_OPERATION_OPENING : cover::COVER_OPERATION_CLOSING;
    }
  }

  this->publish_state();
}

void IOHomeCover::dump_config() {
  LOG_COVER("", "IO-Homecontrol Cover", this);
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Invert Position: %s", YESNO(this->invert_));
}

}  // namespace home_io_control
}  // namespace esphome
