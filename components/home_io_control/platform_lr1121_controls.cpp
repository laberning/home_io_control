/// @file platform_lr1121_controls.cpp
/// @brief Hub-level LR1121 firmware-update entities: out-of-line bodies for the
/// bootloader-rewrite arming switch. The flash button is header-inline; this translation unit
/// pulls its class in (via the header), so clang-tidy analyzes the button too whenever it runs
/// against a config that enables `lr1121_firmware_update:`
/// (`scripts/run-clang-tidy.sh config/tests/test-esp32-lr1121-fwupdate.yaml`) — the default
/// clang-tidy target sets neither define, so both classes expand to nothing there.
/// @ingroup hioc_platforms

#include "platform_lr1121_controls.h"

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
