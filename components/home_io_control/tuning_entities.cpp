/// @file tuning_entities.cpp
/// @brief Home Assistant `number` and `select` tuning entity implementations.
/// @ingroup hioc_tuning

#include "tuning_entities.h"
#include "hub_core.h"

namespace esphome {
namespace home_io_control {

void IOHomeTuningNumber::setup() {
  if (this->parent_ != nullptr)
    this->publish_state(this->parent_->get_tuning_number_value(this->param_name_));
}

void IOHomeTuningSelect::setup() {
  if (this->parent_ == nullptr)
    return;
  const std::string value = this->parent_->get_tuning_select_value(this->param_name_);
  if (!value.empty())
    this->publish_state(value);
}

void IOHomeTuningNumber::control(float value) {
  // A value the radio cannot carry is refused: the entity shows the value that stays in effect.
  const bool accepted = this->parent_ == nullptr || this->parent_->update_tuning_number(this->param_name_, value);
  this->publish_state(accepted ? value : this->parent_->get_tuning_number_value(this->param_name_));
}

void IOHomeTuningSelect::control(const std::string &value) {
  this->publish_state(value);
  if (this->parent_ != nullptr)
    this->parent_->update_tuning_select(this->param_name_, value);
}

}  // namespace home_io_control
}  // namespace esphome
