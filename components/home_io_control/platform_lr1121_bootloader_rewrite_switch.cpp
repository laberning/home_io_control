/// @file platform_lr1121_bootloader_rewrite_switch.cpp
/// @brief Hub-level "Allow LR1121 Bootloader Rewrite (Irreversible)" switch entity.
/// @ingroup hioc_platforms

#include "platform_lr1121_bootloader_rewrite_switch.h"

#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE

#include "esphome/core/log.h"

namespace esphome {
namespace home_io_control {

static const char *const TAG = "home_io_control.lr1121_bootloader_switch";

void IOHomeLr1121BootloaderRewriteSwitch::setup() { this->publish_state(false); }

void IOHomeLr1121BootloaderRewriteSwitch::write_state(bool state) {
  if (this->parent_ != nullptr)
    this->parent_->set_bootloader_rewrite_allowed(state);
  this->publish_state(state);
}

void IOHomeLr1121BootloaderRewriteSwitch::dump_config() {
  // No irreversibility warning here on purpose: the config dump is not where it lands. The warning
  // is already on the boot-time AVAILABLE line, on the switch-off refusal, and in the sequence
  // itself -- i.e. every place a user is about to act on it. Repeating it in a startup dump only
  // adds noise, and the entity name already carries "(Irreversible)".
  LOG_SWITCH("", "LR1121 Bootloader Rewrite (Irreversible)", this);
}

}  // namespace home_io_control
}  // namespace esphome

#endif  // IOHOME_LR1121_BOOTLOADER_UPDATE
