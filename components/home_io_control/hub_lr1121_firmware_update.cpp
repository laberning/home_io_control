// IOHOME_LR1121_FIRMWARE_UPDATE is only visible after something pulls in esphome/core/defines.h
// (via hub_internal.h -> hub_core.h -> esphome/core/hal.h) — these #includes must run before the
// #ifdef check below, not after (see radio_lr1121_firmware_updater.h for the fuller explanation).
#include "hub_internal.h"
#include "lr1121_firmware_decisions.h"
#include "radio_lr1121_firmware_updater.h"

#ifdef IOHOME_LR1121_FIRMWARE_UPDATE

// Only ever generated when this define is set (components/home_io_control/__init__.py), so this
// #include must stay inside the guard above.
#include "lr1121_firmware_update_image.h"

#include "esphome/core/application.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <string>

/// @file hub_lr1121_firmware_update.cpp
/// @brief LR1121 transceiver-firmware-update feature — hub wiring.
/// @ingroup hioc_hub
///
/// Owns the impure side of the feature: the boot-time bootloader-version excursion, the cached
/// flash verdict, the two-press confirmation window, and the button-triggered flash sequence
/// itself. The pure decision logic lives in lr1121_firmware_decisions.h; the bootloader-mode SPI
/// transport lives in radio_lr1121_firmware_updater.h/.cpp. See the implementation plan for the
/// full design — the single most important rule it states is repeated here because it is easy to
/// violate by accident:
///
/// After any bootloader excursion, the chip is unconfigured. Exactly one of two things must
/// happen next: radio_->init() runs (the boot-time excursion below), or the ESP32 reboots
/// (run_lr1121_flash_sequence_(), every exit after calling enter_bootloader()). There is no third
/// option — an early `return` on an error path would leave the radio silently dead: it would
/// answer SPI, look initialized to the driver, and never work again.
///
/// This applies even when enter_bootloader() itself returns false. Its RST-pulse/BUSY-strap entry
/// sequence (radio_lr1121_firmware_updater.cpp) runs unconditionally, before the GetVersion read
/// that determines its return value — so a false return (e.g. that confirmatory read timing out)
/// does not mean the chip is untouched. It means entry was attempted and cannot be confirmed,
/// which is reason to reboot, not reason to skip rebooting.

namespace esphome {
namespace home_io_control {

namespace {

constexpr uint32_t LR1121_FLASH_CONFIRM_WINDOW_MS = 60 * 1000;  ///< Q3: ~60s, a button two paces away.
/// Max value representable by Component::warn_if_blocking_over_ (a centisecond uint8_t) — see
/// run_lr1121_flash_sequence_() for why this only reduces log spam rather than eliminating it.
constexpr uint8_t WARN_IF_BLOCKING_OVER_MAX_CS = 255;

std::string format_lr1121_fw_version(uint16_t version) {
  if (version == 0)
    return "unknown";
  char buf[8];
  snprintf(buf, sizeof(buf), "%u.%u", static_cast<unsigned>(version >> 8), static_cast<unsigned>(version & 0xFF));
  return buf;
}

std::string format_hex16(uint16_t value) {
  char buf[8];
  snprintf(buf, sizeof(buf), "0x%04X", value);
  return buf;
}

std::string format_hex8(uint8_t value) {
  char buf[6];
  snprintf(buf, sizeof(buf), "0x%02X", value);
  return buf;
}

/// "unknown" for the same sentinel reason format_lr1121_fw_version() uses -- 0 means the boot-time
/// excursion (design record §0.3) never successfully read a bootloader version.
std::string format_lr1121_bootloader_version(uint16_t version) {
  return version == 0 ? "unknown" : format_hex16(version);
}

uint16_t lr1121_required_bootloader_for(uint16_t target_fw) {
  for (const auto &requirement : LR1121_KNOWN_BOOTLOADER_REQUIREMENTS) {
    if (requirement.target_fw == target_fw)
      return requirement.bootloader;
  }
  return 0;
}

/// @brief Outcome of the post-bootloader-entry sanity read, factored out of
/// run_lr1121_flash_sequence_() to keep its cognitive complexity within clang-tidy's threshold.
enum class Lr1121SanityResult {
  OK,                 ///< type matches, and either the version matches what boot recorded, or boot
                      ///< recorded nothing and the freshly-read version positively identifies an LR1121.
  WRONG_TYPE,         ///< type != LR1121_UPDATER_BOOTLOADER_TYPE -- not in bootloader mode at all.
  VERSION_MISMATCH,   ///< type is fine, but the version boot recorded no longer matches.
  WRONG_CHIP_FAMILY,  ///< Boot recorded nothing to compare against, and the freshly-read version
                      ///< does not identify an LR1121 -- see lr1121_check_bootloader_sanity()'s
                      ///< comment for why type alone cannot catch this.
};

/// @brief When the boot-time excursion never read a bootloader version (`known` is
/// false), there is nothing to compare `sanity_bootloader_version` against a prior reading, but it
/// must still be checked against something: `sanity_type` (LR11XX_TYPE_PRODUCTION_MODE, 0xDF) is
/// reported by an LR1120 or LR1110 too, so passing the type check alone does not prove this chip is
/// an LR1121 -- only the bootloader *version* does that (lr1121_bootloader_is_lr1121()). Without
/// this, the "boot-time read failed, adopt whatever bootloader-mode read we get now" recovery path
/// would erase and overwrite an LR1120/LR1110 with an LR1121 image on nothing more than a byte both
/// chips share. This is the last check before EraseFlash.
Lr1121SanityResult lr1121_check_bootloader_sanity(bool known, uint16_t known_bootloader_version, uint8_t sanity_type,
                                                  uint16_t sanity_bootloader_version) {
  if (sanity_type != LR1121_UPDATER_BOOTLOADER_TYPE)
    return Lr1121SanityResult::WRONG_TYPE;
  if (known) {
    if (sanity_bootloader_version != known_bootloader_version)
      return Lr1121SanityResult::VERSION_MISMATCH;
    return Lr1121SanityResult::OK;
  }
  if (!lr1121_bootloader_is_lr1121(sanity_bootloader_version))
    return Lr1121SanityResult::WRONG_CHIP_FAMILY;
  return Lr1121SanityResult::OK;
}

/// @brief Human-readable reason for a sanity-check failure, for the abort log line in
/// run_lr1121_flash_sequence_(). Factored out so that line stays one statement regardless of how
/// many distinct sanity failures exist.
std::string lr1121_sanity_failure_reason(Lr1121SanityResult sanity, uint16_t sanity_bootloader_version) {
  switch (sanity) {
    case Lr1121SanityResult::WRONG_TYPE:
      return "wrong type";
    case Lr1121SanityResult::WRONG_CHIP_FAMILY:
      return "bootloader version " + format_hex16(sanity_bootloader_version) + " identifies " +
             lr1121_chip_family_for_bootloader(sanity_bootloader_version) + ", not an LR1121";
    case Lr1121SanityResult::VERSION_MISMATCH:
    default:
      return "bootloader version changed since boot";
  }
}

/// @brief Log the post-flash version read-back. target_fw == 0 ("unknown", an explicitly
/// supported config per Step 1 when a renamed image's filename carries no version) must not be
/// compared numerically -- a successful flash of such an image used to log a spurious "post-flash
/// version is X, expected unknown" warning about a mismatch that was never a real one.
void lr1121_log_post_flash_verify_result(uint16_t new_fw, uint16_t target_fw) {
  if (target_fw == 0) {
    ESP_LOGI(detail::TAG,
             "LR1121 firmware update: now running %s; this build had no expected version to compare against",
             format_lr1121_fw_version(new_fw).c_str());
  } else if (new_fw == target_fw) {
    ESP_LOGI(detail::TAG, "LR1121 firmware update: success -- now running %s",
             format_lr1121_fw_version(new_fw).c_str());
  } else {
    ESP_LOGW(detail::TAG, "LR1121 firmware update: post-flash version is %s, expected %s",
             format_lr1121_fw_version(new_fw).c_str(), format_lr1121_fw_version(target_fw).c_str());
  }
}

/// @brief Read and log GetHash (0x8004) after a successful write, while still in bootloader mode.
///
/// INFORMATIONAL ONLY -- never a pass/fail gate. Semtech's own reference updater
/// (`lr11xx_update_firmware()`) never calls GetHash, and its published header documents only that
/// it "get[s] calculated hash of flash content" -- no stated algorithm, no stated hashed range
/// (whole flash vs. just the region just written), and therefore no documented host-side expected
/// value to compare against. Implementing an automated comparison here would mean guessing a check
/// this codebase cannot currently verify, which is worse than not checking at all: a wrong gate
/// would fail a genuinely good flash. Instead this logs the 16 bytes as hex so the user can compare
/// them by hand on a first real run. 16 bytes is MD5-sized, and every published image ships a
/// `.bin.md5` sidecar, which makes "GetHash returns the MD5 of the flashed image" a plausible
/// reading -- but that is an UNVERIFIED HYPOTHESIS, stated as such in the log line, not a fact this
/// code relies on.
///
/// A failed read (BUSY timeout) is logged and otherwise ignored: this diagnostic must never block
/// or fail an otherwise-successful write, and the established recovery messaging elsewhere in this
/// sequence already covers what to do about a genuinely bad flash.
void lr1121_log_post_write_hash(Lr1121FirmwareUpdater &updater) {
  uint8_t hash[LR1121_UPDATER_HASH_LENGTH] = {0};
  if (!updater.read_hash(hash, sizeof(hash))) {
    ESP_LOGW(detail::TAG,
             "LR1121 firmware update: could not read the post-write flash hash (BUSY timeout) -- diagnostic "
             "only, continuing");
    return;
  }
  char hex[LR1121_UPDATER_HASH_LENGTH * 2 + 1];
  for (size_t i = 0; i < LR1121_UPDATER_HASH_LENGTH; i++)
    snprintf(hex + i * 2, 3, "%02x", hash[i]);
  ESP_LOGI(detail::TAG,
           "LR1121 firmware update: post-write flash hash (GetHash, diagnostic only, not verified against any "
           "known-good value -- see code comment): %s",
           hex);
}

/// The specific reason behind a NEEDS_CONFIRMATION verdict. Re-derives the reason from the same
/// inputs lr1121_flash_decision() used, in the same priority order that function checks them, so
/// the two can never drift apart:
///   1. the boot-time bootloader read never completed;
///   2. the normal-mode read never completed (no installed-firmware read to compare against);
///   3. no target version could be determined at all;
///   4. the target is absent from this build's advisory compatibility table (unverified, not
///      refused);
///   5. the installed version specifically is unknown (device_type known-good, firmware bytes
///      were not);
///   6. target not newer than installed.
std::string lr1121_needs_confirmation_reason(uint8_t device_type, uint16_t bootloader_version, uint16_t installed_fw,
                                             uint16_t target_fw) {
  if (bootloader_version == 0)
    return "the bootloader version could not be read at boot, so chip identity and bootloader compatibility "
           "cannot be verified";
  if (device_type == 0)
    return "the installed firmware version could not be read (radio failed to initialize, or the read itself "
           "failed)";
  if (target_fw == 0)
    return "no target firmware version could be determined for the configured image";
  if (lr1121_bootloader_supports_target(target_fw, bootloader_version) == BootloaderSupport::UNKNOWN_TARGET) {
    return "target firmware " + format_lr1121_fw_version(target_fw) +
           " is not in this build's known bootloader-compatibility table (unverified, not refused)";
  }
  if (installed_fw == 0)
    return "the installed firmware version is unknown";
  return "target firmware " + format_lr1121_fw_version(target_fw) + " is not newer than the installed " +
         format_lr1121_fw_version(installed_fw);
}

}  // namespace

void IOHomeControlComponent::run_lr1121_boot_time_bootloader_read_() {
  this->lr1121_firmware_updater_ = new (std::nothrow) Lr1121FirmwareUpdater(this, this->rst_pin_, this->busy_pin_);
  if (this->lr1121_firmware_updater_ == nullptr) {
    ESP_LOGE(detail::TAG, "LR1121 firmware update: failed to allocate the updater; bootloader version unknown");
    return;
  }

  uint8_t type = 0;
  uint16_t bootloader_version = 0;
  if (!this->lr1121_firmware_updater_->enter_bootloader(type, bootloader_version)) {
    ESP_LOGW(detail::TAG, "LR1121 firmware update: could not read the bootloader version at boot (BUSY timeout) -- "
                          "bootloader version stays unknown until the next boot");
    return;
  }
  this->lr1121_bootloader_chip_type_ = type;
  this->lr1121_bootloader_version_ = bootloader_version;
  this->lr1121_bootloader_version_known_ = true;

  // Boot back into normal firmware so the radio_->init() called right after this finds the chip
  // in the mode it expects. If this specific command fails to send, init()'s own hardware-level
  // RST pulse forces the chip out of the bootloader anyway -- this is a courtesy, not a
  // dependency, so a failure here is a warning, not cause to skip caching the version above.
  if (!this->lr1121_firmware_updater_->reboot(false)) {
    ESP_LOGW(detail::TAG, "LR1121 firmware update: reboot-out-of-bootloader command failed to send (BUSY timeout); the "
                          "upcoming radio init's own hardware reset will recover it");
  }
}

void IOHomeControlComponent::cache_lr1121_flash_verdict_() {
  uint8_t device_type = 0;
  uint16_t installed_fw = 0;
  if (this->radio_ != nullptr && this->lr1121_firmware_updater_ != nullptr) {
    // Re-read via the updater's own transport rather than plumbing a getter through
    // RadioDriver/RadioLR1121 for this one caller — radio_lr1121.h gains zero new surface area
    // (Step 3/4's design). GetVersion is the same benign, side-effect-free read
    // RadioLR1121::dump_debug() already issues at arbitrary times without disrupting RX.
    uint8_t fw_major = 0, fw_minor = 0;
    // device_type/fw_major/fw_minor are left at their zero-initialized values above on a failed
    // read (BUSY timeout), which is exactly the "unknown" sentinel lr1121_flash_decision() expects.
    if (this->lr1121_firmware_updater_->read_normal_version(device_type, fw_major, fw_minor))
      installed_fw = (static_cast<uint16_t>(fw_major) << 8) | fw_minor;
  }
  this->lr1121_installed_device_type_ = device_type;
  this->lr1121_installed_fw_ = installed_fw;
  // device_type (normal-mode chip identity, layer 3) and lr1121_bootloader_chip_type_
  // (bootloader-mode `type` byte, layer 4) are distinct inputs -- passing the bootloader-mode byte
  // where device_type belongs made every real LR1121 fail its own chip-identity check.
  this->lr1121_flash_verdict_ =
      lr1121_flash_decision(device_type, this->lr1121_bootloader_chip_type_, this->lr1121_bootloader_version_,
                            installed_fw, LR1121_FIRMWARE_UPDATE_TARGET_VERSION, false);
  this->lr1121_flash_verdict_known_ = true;
}

// Returns a complete verdict sentence with no assumption about what (if anything) is armed --
// callers append their own context-appropriate follow-up (or none, at boot). Splitting the
// "press again" wording out of here is what keeps the boot-time config dump honest: nothing is
// armed at boot, so a sentence claiming otherwise would be false there.
std::string IOHomeControlComponent::describe_lr1121_flash_verdict_() const {
  const uint16_t target = LR1121_FIRMWARE_UPDATE_TARGET_VERSION;
  const std::string prefix = "Firmware update target: " + format_lr1121_fw_version(target) + " -- ";

  switch (this->lr1121_flash_verdict_) {
    case FlashDecision::REJECT_WRONG_CHIP: {
      // lr1121_flash_decision() checks layer 4 (bootloader-mode identity) before layer 3
      // (normal-mode identity), and never reaches either while bootloader_version is the
      // "unknown" sentinel -- so re-checking layer 4 here fully determines which one rejected.
      if (this->lr1121_bootloader_chip_type_ != LR1121_BOOTLOADER_TYPE_FOR_FIRMWARE_DECISIONS ||
          !lr1121_bootloader_is_lr1121(this->lr1121_bootloader_version_)) {
        return prefix + "CANNOT PROCEED: bootloader version " + format_hex16(this->lr1121_bootloader_version_) +
               " identifies " + lr1121_chip_family_for_bootloader(this->lr1121_bootloader_version_) + ", not an LR1121";
      }
      return prefix + "CANNOT PROCEED: normal-mode chip identity byte " +
             format_hex8(this->lr1121_installed_device_type_) + " identifies " +
             lr1121_chip_family_for_device_type(this->lr1121_installed_device_type_) + ", not an LR1121";
    }
    case FlashDecision::REJECT_BOOTLOADER_TOO_OLD:
      return prefix + "CANNOT PROCEED: needs bootloader " + format_hex16(lr1121_required_bootloader_for(target)) +
             ", this chip has " + format_hex16(this->lr1121_bootloader_version_) +
             " (bootloader updates are currently not supported)";
    case FlashDecision::ALREADY_INSTALLED:
      return prefix + "already running the configured firmware, nothing to do";
    case FlashDecision::NEEDS_CONFIRMATION:
      return prefix +
             lr1121_needs_confirmation_reason(this->lr1121_installed_device_type_, this->lr1121_bootloader_version_,
                                              this->lr1121_installed_fw_, target) +
             " (bootloader version " + format_lr1121_bootloader_version(this->lr1121_bootloader_version_) + ")";
    case FlashDecision::PROCEED:
    default:
      return prefix + "ready to flash (press \"Flash LR1121 Radio Firmware\")";
  }
}

// The verdict line must still be included when the bootloader version is unknown -- this
// used to early-return before it, so a failed boot-time excursion silently dropped the verdict
// line too, even though the verdict is cached independently (cache_lr1121_flash_verdict_() runs
// in setup() regardless of whether the boot-time excursion succeeded).
std::vector<std::string> IOHomeControlComponent::lr1121_firmware_update_debug_lines_() const {
  std::vector<std::string> lines;
  if (this->lr1121_bootloader_version_known_) {
    lines.push_back("LR1121 bootloader version: " + format_hex16(this->lr1121_bootloader_version_));
  } else {
    lines.push_back("LR1121 firmware update: bootloader version could not be read at boot");
  }
  if (this->lr1121_flash_verdict_known_)
    lines.push_back(this->describe_lr1121_flash_verdict_());
  return lines;
}

void IOHomeControlComponent::dump_lr1121_firmware_update_debug_() const {
  for (const auto &line : this->lr1121_firmware_update_debug_lines_())
    ESP_LOGCONFIG(detail::TAG, "  %s", line.c_str());
}

void IOHomeControlComponent::arm_lr1121_flash_confirmation_() {
  this->lr1121_flash_confirmation_armed_ = true;
  // Deliberately App.scheduler's self-keyed overload, not Component::set_timeout() (the
  // hub_key_extraction.cpp idiom this would otherwise mirror). Component::set_timeout() records
  // this component, and ESPHome's scheduler skips any scheduled item belonging to a *failed*
  // component (Scheduler::should_skip_item_() -> is_item_failed_()). This method exists precisely
  // for the recovery path where radio_->init() has failed and mark_failed() has already run -- if
  // the callback were skipped there too, the confirmation window would never auto-disarm on
  // exactly the board most likely to need a second press, degrading the two-press protection to
  // "two presses ever". The self-keyed overload stores no Component, so it always fires. Do not
  // "fix" this back to the named Component::set_timeout() idiom.
  App.scheduler.set_timeout(this, LR1121_FLASH_CONFIRM_WINDOW_MS, [this]() {
    // Guards against a stale timeout firing after a fresh press already consumed/re-armed the
    // window — mirrors hub_key_extraction.cpp's KEY_EXTRACTION_AUTO_OFF_MS idiom.
    if (!this->lr1121_flash_confirmation_armed_)
      return;
    this->lr1121_flash_confirmation_armed_ = false;
    ESP_LOGI(detail::TAG, "LR1121 firmware update: confirmation window expired without a second press");
  });
}

void IOHomeControlComponent::trigger_lr1121_firmware_update() {
  // Guard 0: setup() deletes the driver and nulls radio_ when init() fails, but this button is a
  // separate component whose press_action() still reaches the hub even after mark_failed().
  // Deliberately still allow the attempt -- skipping only the standby call below -- since a radio
  // that failed to initialize is exactly the case reflashing is meant to recover.
  if (this->radio_ == nullptr)
    ESP_LOGW(detail::TAG, "LR1121 firmware update: radio_ is null (failed init); proceeding without standby");

  // loop() guards every radio action behind `if (!this->busy_)`, so this is the same mechanism a
  // blocking exchange already uses -- no new coordination. The safety here comes from ESPHome's
  // cooperative single-threaded loop: an API-dispatched button press cannot land in the middle of
  // a blocking exchange to begin with.
  if (this->busy_) {
    ESP_LOGW(detail::TAG, "LR1121 firmware update: radio busy with another operation, ignoring press");
    return;
  }

  if (this->lr1121_firmware_updater_ == nullptr || !this->lr1121_flash_verdict_known_) {
    ESP_LOGE(detail::TAG, "LR1121 firmware update: no cached verdict available (setup() may have failed early)");
    return;
  }

  const FlashDecision verdict = this->lr1121_flash_verdict_;

  // Hard rejections never proceed, armed or not, and never touch the chip -- the verdict was
  // already computed and logged at boot, so refusing here is a cached-verdict read, not a fresh
  // bootloader entry.
  if (verdict == FlashDecision::REJECT_WRONG_CHIP || verdict == FlashDecision::REJECT_BOOTLOADER_TOO_OLD) {
    ESP_LOGE(detail::TAG, "%s", this->describe_lr1121_flash_verdict_().c_str());
    return;
  }

  const bool proceeding = (verdict == FlashDecision::PROCEED) || this->lr1121_flash_confirmation_armed_;
  if (!proceeding) {
    // ALREADY_INSTALLED is the state a *successful* user spends the rest of the build's life
    // in -- it must not read as a warning, and its "press again" follow-up talks about re-flashing
    // rather than proceeding. Both facts are decided here, at the one call site where anything is
    // actually about to be armed; describe_lr1121_flash_verdict_() itself stays neutral about it
    // (see that function's comment) so the boot-time config dump never claims a window is armed.
    const bool already_installed = (verdict == FlashDecision::ALREADY_INSTALLED);
    const std::string confirm_suffix = " -- press \"Flash LR1121 Radio Firmware\" again within " +
                                       std::to_string(LR1121_FLASH_CONFIRM_WINDOW_MS / 1000) + "s to " +
                                       (already_installed ? "re-flash anyway" : "proceed anyway");
    const std::string message = this->describe_lr1121_flash_verdict_() + confirm_suffix;
    if (already_installed) {
      ESP_LOGI(detail::TAG, "%s", message.c_str());
    } else {
      ESP_LOGW(detail::TAG, "%s", message.c_str());
    }
    this->arm_lr1121_flash_confirmation_();
    return;
  }

  this->lr1121_flash_confirmation_armed_ = false;
  this->run_lr1121_flash_sequence_();
}

void IOHomeControlComponent::run_lr1121_flash_sequence_() {
  this->busy_ = true;
  // Raised for the duration of the flash so the log fills with the progress output below rather
  // than component-blocking warnings. Component::warn_if_blocking_over_ is a centisecond uint8_t
  // (max 2550ms) -- a flash can run far longer than that regardless, so this reduces warning
  // spam, it cannot eliminate every warning for a longer block. Never restored -- every exit from
  // this point on is App.safe_reboot(), which makes the saved value moot.
  this->warn_if_blocking_over_ = WARN_IF_BLOCKING_OVER_MAX_CS;
  if (this->radio_ != nullptr)
    this->radio_->set_mode_standby();  // Never enter bootloader mode with RX armed.

  // Every exit from here on is App.safe_reboot() -- see the file header's invariant. That includes
  // the enter_bootloader() failure branch immediately below: its entry sequence runs unconditionally
  // before the read that can time out, so a false return here does not mean the chip is untouched.
  uint8_t sanity_type = 0;
  uint16_t sanity_bootloader_version = 0;
  if (!this->lr1121_firmware_updater_->enter_bootloader(sanity_type, sanity_bootloader_version)) {
    ESP_LOGE(detail::TAG,
             "LR1121 firmware update: bootloader entry could not be confirmed (BUSY timeout on the verification "
             "read) -- the entry sequence itself already ran, so the chip may be unconfigured; rebooting to "
             "recover it rather than risking a silently dead radio");
    App.safe_reboot();
    return;
  }

  const Lr1121SanityResult sanity = lr1121_check_bootloader_sanity(
      this->lr1121_bootloader_version_known_, this->lr1121_bootloader_version_, sanity_type, sanity_bootloader_version);
  if (sanity != Lr1121SanityResult::OK) {
    const std::string sanity_reason = lr1121_sanity_failure_reason(sanity, sanity_bootloader_version);
    ESP_LOGE(
        detail::TAG,
        "LR1121 firmware update: bootloader-entry sanity check failed (%s; read type=0x%02X bootloader=%s, "
        "boot-time bootloader was %s) -- aborting before erasing anything",
        sanity_reason.c_str(), sanity_type, format_hex16(sanity_bootloader_version).c_str(),
        this->lr1121_bootloader_version_known_ ? format_hex16(this->lr1121_bootloader_version_).c_str() : "unknown");
    this->lr1121_firmware_updater_->reboot(false);
    App.safe_reboot();
    return;
  }
  if (!this->lr1121_bootloader_version_known_) {
    // Boot never got a reading (design record's recovery-path allowance); the type check just
    // above is all we could verify, so adopt this read for the rest of the attempt and future log
    // lines rather than leaving lr1121_bootloader_version_ stuck at the "unknown" sentinel.
    ESP_LOGI(detail::TAG,
             "LR1121 firmware update: boot-time bootloader version was unknown; type check passed and bootloader "
             "%s is now adopted",
             format_hex16(sanity_bootloader_version).c_str());
    this->lr1121_bootloader_chip_type_ = sanity_type;
    this->lr1121_bootloader_version_ = sanity_bootloader_version;
    this->lr1121_bootloader_version_known_ = true;
  }

  ESP_LOGI(detail::TAG, "LR1121 firmware update: erasing radio flash, this takes a few seconds...");
  const uint32_t erase_start_ms = millis();
  if (!this->lr1121_firmware_updater_->erase_flash()) {
    ESP_LOGE(detail::TAG,
             "LR1121 firmware update: erase failed (BUSY timeout) -- the radio firmware is now incomplete. This "
             "is recoverable: after this reboot, press the button again to re-flash.");
    App.safe_reboot();
    return;
  }
  const uint32_t erase_elapsed_ms = millis() - erase_start_ms;

  size_t last_logged_words = 0;
  const size_t log_step = std::max<size_t>(LR1121_FIRMWARE_UPDATE_IMAGE_WORDS / 10, 1);
  const uint32_t write_start_ms = millis();
  const bool write_ok = this->lr1121_firmware_updater_->write_image(
      LR1121_FIRMWARE_UPDATE_IMAGE, LR1121_FIRMWARE_UPDATE_IMAGE_WORDS, [&](size_t done, size_t total) {
        // Percentage-based, not time-based: gives a consistent ~10 lines regardless of how long
        // the write actually takes, since image sizes differ nearly 4x between published
        // versions and the total duration is unknown until measured on real hardware.
        if (done - last_logged_words < log_step && done != total)
          return;
        last_logged_words = done;
        ESP_LOGI(detail::TAG, "Flashing LR1121: %zu/%zu words (%u%%)", done, total,
                 static_cast<unsigned>((done * 100) / total));
      });
  const uint32_t write_elapsed_ms = millis() - write_start_ms;

  if (!write_ok) {
    ESP_LOGE(detail::TAG,
             "LR1121 firmware update: write failed (BUSY timeout) after %" PRIu32
             " ms -- the radio firmware is now incomplete. This is recoverable: after this reboot, press the "
             "button again to re-flash.",
             write_elapsed_ms);
    App.safe_reboot();
    return;
  }

  ESP_LOGI(detail::TAG, "LR1121 firmware update: erase took %" PRIu32 " ms, write took %" PRIu32 " ms",
           erase_elapsed_ms, write_elapsed_ms);

  // Read while still in bootloader mode, before rebooting into the newly written image -- see
  // lr1121_log_post_write_hash()'s comment for why this is diagnostic-only.
  lr1121_log_post_write_hash(*this->lr1121_firmware_updater_);

  if (!this->lr1121_firmware_updater_->reboot(false)) {
    ESP_LOGW(detail::TAG, "LR1121 firmware update: reboot-to-image command failed to send (BUSY timeout)");
  } else {
    uint8_t device_type = 0, fw_major = 0, fw_minor = 0;
    if (this->lr1121_firmware_updater_->read_normal_version(device_type, fw_major, fw_minor)) {
      const uint16_t new_fw = (static_cast<uint16_t>(fw_major) << 8) | fw_minor;
      lr1121_log_post_flash_verify_result(new_fw, LR1121_FIRMWARE_UPDATE_TARGET_VERSION);
    } else {
      ESP_LOGW(detail::TAG, "LR1121 firmware update: could not read back the post-flash version (BUSY timeout)");
    }
  }

  // A clean ESP32 restart is the post-flash path rather than re-running radio_->init() in place:
  // by now init() has long since run and attached a DIO9 interrupt, so re-attachment, stale
  // driver state and partial reconfiguration are all avoided at once.
  App.safe_reboot();
}

}  // namespace home_io_control
}  // namespace esphome

#endif  // IOHOME_LR1121_FIRMWARE_UPDATE
