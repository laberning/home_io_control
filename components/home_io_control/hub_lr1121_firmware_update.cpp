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

#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE
// Only ever generated when the nested bootloader: sub-block is configured (__init__.py's
// _create_lr1121_bootloader_update()), so this #include must stay inside this guard too.
#include "lr1121_bootloader_loader_image.h"
#endif

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
/// transport lives in radio_lr1121_firmware_updater.h/.cpp. ADR 0020 and ADR 0021 record the
/// design; the single most important rule they state is repeated here because it is easy to
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
/// excursion never successfully read a bootloader version.
std::string format_lr1121_bootloader_version(uint16_t version) {
  return version == 0 ? "unknown" : format_hex16(version);
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
/// INFORMATIONAL ONLY -- never a pass/fail gate, and it cannot become one.
///
/// GetHash (0x8004) is undocumented: it is absent from the LR1121 User Manual's bootloader command
/// table (which lists 0x8000/0x8003/0x8005/0x800B/0x800C/0x800D), and Semtech's own reference
/// updater defines the opcode but never calls it. No algorithm, no hashed range, no published
/// expected value.
///
/// The obvious hypothesis -- 16 bytes is MD5-sized and every published image ships a `.bin.md5`
/// sidecar, so perhaps this is the image's MD5 -- was DISPROVEN on real hardware 2026-08-07:
/// flashing lr1121_transceiver_0103.bin produced 321388054ac482d5ae703d0ab5e7af09, while that
/// image's sidecar reads 7e44170c815485559880592e7713407f.
///
/// That result has a likely structural explanation: WriteFlashEncrypted decrypts on the fly, so
/// flash holds *plaintext* firmware while the `.bin` is ciphertext. A hash over flash contents can
/// therefore never equal the file's MD5 -- reproducing it host-side would need the decrypted image,
/// and the key is Semtech's. So this value is not merely unverified, it is unverifiable by this
/// project, and no future code change should try to gate on it.
///
/// It is worth logging where it works: a stable fingerprint of what is actually in flash makes "do
/// these two boards hold the same image?" answerable. But it does not work everywhere -- bootloader
/// 0x2101 returns a fixed non-value (0x14 then fifteen zero bytes), observed twice on hardware
/// 2026-08-07 across two code paths and two images, where 0x2100 returned a plausible digest. That
/// case is detected and reported as "unavailable" rather than printed as though it identified
/// anything. The real correctness check is the post-flash version read-back that follows, which is
/// also what Semtech's reference relies on.
///
/// A failed read (BUSY timeout) is logged and otherwise ignored: this diagnostic must never block
/// or fail an otherwise-successful write, and the established recovery messaging elsewhere in this
/// sequence already covers what to do about a genuinely bad flash.
void lr1121_log_post_write_hash(Lr1121FirmwareUpdater &updater) {
  uint8_t hash[LR1121_UPDATER_HASH_LENGTH] = {0};
  if (!updater.read_hash(hash, sizeof(hash))) {
    ESP_LOGW(detail::TAG,
             "LR1121 firmware update: could not read the flash fingerprint (BUSY timeout). Harmless -- it is "
             "only an identifier, not a correctness check; the version check below is what confirms the flash.");
    return;
  }
  // Bootloader 0x2101 answers GetHash with a fixed non-value (0x14 then fifteen zero bytes),
  // observed twice on hardware across two different code paths and two different images, where
  // 0x2100 returned a plausible digest. Printing that as an "identifier for the image" would be a
  // lie: it is the same bytes whatever is flashed. Detect it generically rather than matching the
  // exact constant -- a genuine 16-byte digest ending in fifteen zero bytes is not a case worth
  // designing around.
  const bool degenerate = std::all_of(hash + 1, hash + LR1121_UPDATER_HASH_LENGTH, [](uint8_t b) { return b == 0; });
  if (degenerate) {
    ESP_LOGI(detail::TAG,
             "LR1121 firmware update: no flash fingerprint available on this bootloader (GetHash returned a "
             "fixed non-value). Harmless -- it was only ever an identifier, never a correctness check; the "
             "firmware version reported below is what confirms the flash worked.");
    return;
  }
  char hex[LR1121_UPDATER_HASH_LENGTH * 2 + 1];
  for (size_t i = 0; i < LR1121_UPDATER_HASH_LENGTH; i++)
    snprintf(hex + i * 2, 3, "%02x", hash[i]);
  ESP_LOGI(detail::TAG,
           "LR1121 firmware update: flash fingerprint %s -- an identifier for the image now on the chip, useful "
           "for comparing two boards. It is not the image's MD5 and cannot be checked against anything; the "
           "firmware version reported below is what confirms the flash worked.",
           hex);
}

/// @brief Erase, then chunk-write, one image -- with percentage progress logging prefixed by
/// `stage_label`. Shared by run_lr1121_flash_sequence_() (the single-image transceiver flash) and,
/// under IOHOME_LR1121_BOOTLOADER_UPDATE, run_lr1121_bootloader_upgrade_sequence_()'s two writes
/// so the erase/write/progress shape exists in exactly one place rather than being duplicated
/// per stage.
/// @return true if both erase and write succeeded; false leaves the region partially written, same
///         as before this was factored out -- the caller decides what that means for recovery.
bool lr1121_erase_and_write_image_(Lr1121FirmwareUpdater &updater, const char *stage_label, const uint32_t *image,
                                   size_t word_count, uint32_t &erase_elapsed_ms, uint32_t &write_elapsed_ms) {
  ESP_LOGI(detail::TAG, "%s: erasing radio flash, this takes a few seconds...", stage_label);
  const uint32_t erase_start_ms = millis();
  if (!updater.erase_flash()) {
    ESP_LOGE(detail::TAG, "%s: erase failed (BUSY timeout)", stage_label);
    return false;
  }
  erase_elapsed_ms = millis() - erase_start_ms;

  size_t last_logged_words = 0;
  const size_t log_step = std::max<size_t>(word_count / 10, 1);
  const uint32_t write_start_ms = millis();
  const bool write_ok = updater.write_image(image, word_count, [&](size_t done, size_t total) {
    // Percentage-based, not time-based: gives a consistent ~10 lines regardless of how long the
    // write actually takes, since image sizes differ nearly 4x between published versions and the
    // total duration is unknown until measured on real hardware.
    if (done - last_logged_words < log_step && done != total)
      return;
    last_logged_words = done;
    ESP_LOGI(detail::TAG, "%s: flashing %zu/%zu words (%u%%)", stage_label, done, total,
             static_cast<unsigned>((done * 100) / total));
  });
  write_elapsed_ms = millis() - write_start_ms;
  if (!write_ok) {
    ESP_LOGE(detail::TAG, "%s: write failed (BUSY timeout) after %" PRIu32 " ms", stage_label, write_elapsed_ms);
    return false;
  }
  ESP_LOGI(detail::TAG, "%s: erase took %" PRIu32 " ms, write took %" PRIu32 " ms", stage_label, erase_elapsed_ms,
           write_elapsed_ms);
  return true;
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
    case FlashDecision::REJECT_BOOTLOADER_TOO_OLD: {
      // REJECT_BOOTLOADER_TOO_OLD covers a bootloader/target mismatch in EITHER direction (see
      // lr1121_bootloader_mismatch_kind()'s doc comment) -- the "too new" direction only became
      // reachable once a chip could actually be running 0x2101, and its message would be exactly
      // backwards if it reused the "too old" wording below.
      const uint16_t required = lr1121_required_bootloader_for(target);
      if (lr1121_bootloader_mismatch_kind(target, this->lr1121_bootloader_version_) ==
          BootloaderMismatch::TARGET_NEEDS_OLDER) {
        return prefix + "CANNOT PROCEED: this chip's bootloader " + format_hex16(this->lr1121_bootloader_version_) +
               " is newer than this firmware supports (needs " + format_hex16(required) +
               ") -- there is no downgrade path";
      }
      std::string message = prefix + "CANNOT PROCEED: needs bootloader " + format_hex16(required) + ", this chip has " +
                            format_hex16(this->lr1121_bootloader_version_);
#ifndef IOHOME_LR1121_BOOTLOADER_UPDATE
      // Only meaningful advice when the feature isn't compiled in at all: a build with the
      // bootloader: sub-block already configured knows exactly which upgrade path applies (or
      // doesn't), and trigger_lr1121_firmware_update()/lr1121_firmware_update_debug_lines_() already
      // append their own path-specific suffix -- appending this one unconditionally used to produce
      // messages telling the user to add a block they had already added, or contradicting the
      // path-specific suffix outright.
      message += " -- add a bootloader: sub-block to lr1121_firmware_update: to enable the (irreversible) upgrade "
                 "path";
#endif
      return message;
    }
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
#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE
std::string IOHomeControlComponent::describe_lr1121_bootloader_refusal_(BootloaderUpgradePath path) const {
  // Deliberately NOT built by appending to describe_lr1121_flash_verdict_(): that function opens
  // with "CANNOT PROCEED", which reads as final and then contradicts a suffix explaining that the
  // rewrite is in fact available. Someone who has just pressed a button wants, in this order: what
  // happened, why, and what to do next. A returned string (rather than a direct ESP_LOGE) is what
  // makes these testable at all -- host builds compile the logging macros to no-ops.
  const std::string target_text = format_lr1121_fw_version(LR1121_FIRMWARE_UPDATE_TARGET_VERSION);
  const std::string chip_text = format_hex16(this->lr1121_bootloader_version_);
  const std::string required_text = format_hex16(lr1121_required_bootloader_for(LR1121_FIRMWARE_UPDATE_TARGET_VERSION));
  const std::string prefix = "LR1121 firmware update: nothing was done, the radio was not touched. ";

  switch (path) {
    case BootloaderUpgradePath::AVAILABLE:
      return prefix + "Firmware " + target_text + " needs bootloader " + required_text + " and this chip has " +
             chip_text +
             ", so the bootloader has to be rewritten first. To do that, turn on the \"Allow LR1121 Bootloader "
             "Rewrite (Irreversible)\" switch and press this button again. A bootloader rewrite cannot be undone.";
    case BootloaderUpgradePath::BLOCKED_UNKNOWN_TARGET:
      return prefix + "This build does not recognise firmware " + target_text +
             ", so it cannot tell which bootloader that image needs. The bootloader rewrite stays disabled rather "
             "than risk an irreversible write on a guess.";
    case BootloaderUpgradePath::BLOCKED_BOOTLOADER_NEWER:
      return prefix + "This chip's bootloader " + chip_text + " is already newer than firmware " + target_text +
             " supports (that image needs " + required_text + "), and there is no way back to an older bootloader.";
    case BootloaderUpgradePath::NOT_APPLICABLE:
    default:
      // Not reached from trigger_lr1121_firmware_update(), which falls through to the ordinary
      // transceiver-only refusal for NOT_APPLICABLE; present so the switch is total.
      return this->describe_lr1121_flash_verdict_();
  }
}
#endif  // IOHOME_LR1121_BOOTLOADER_UPDATE

std::vector<std::string> IOHomeControlComponent::lr1121_firmware_update_debug_lines_() const {
  std::vector<std::string> lines;
  if (this->lr1121_bootloader_version_known_) {
    lines.push_back("LR1121 bootloader version: " + format_hex16(this->lr1121_bootloader_version_));
  } else {
    lines.push_back("LR1121 firmware update: bootloader version could not be read at boot");
  }
  if (this->lr1121_flash_verdict_known_)
    lines.push_back(this->describe_lr1121_flash_verdict_());
#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE
  // Computed independently of the cached verdict/press logic --
  // lr1121_bootloader_upgrade_path() already folds in every precondition (known bootloader,
  // right chip family, loader match), so this is correct however it's called.
  const BootloaderUpgradePath upgrade_path = lr1121_bootloader_upgrade_path(
      /*block_present=*/true, this->lr1121_bootloader_version_known_, this->lr1121_bootloader_version_,
      LR1121_BOOTLOADER_LOADER_FW, LR1121_FIRMWARE_UPDATE_TARGET_VERSION);
  if (upgrade_path == BootloaderUpgradePath::AVAILABLE) {
    // Deliberately short: this prints on every boot. The switch is discoverable in Home Assistant
    // and the full reasoning (why the rewrite exists, why there is no reason to rush it) lives in
    // docs/home_io_control.md and ADR 0021 -- a config dump is the wrong place to repeat it. What
    // must survive the trim is the pair of versions and the fact that it cannot be undone.
    lines.push_back("LR1121 bootloader rewrite: AVAILABLE -- needs bootloader " +
                    format_hex16(lr1121_required_bootloader_for(LR1121_FIRMWARE_UPDATE_TARGET_VERSION)) +
                    ", this chip has " + format_hex16(this->lr1121_bootloader_version_) +
                    ". A bootloader rewrite cannot be undone.");
  } else if (upgrade_path == BootloaderUpgradePath::BLOCKED_UNKNOWN_TARGET) {
    lines.push_back("LR1121 bootloader rewrite: configured, but inert -- this build does not know what bootloader the "
                    "configured target requires, so it will not gamble an irreversible write on it.");
  } else if (upgrade_path == BootloaderUpgradePath::BLOCKED_BOOTLOADER_NEWER) {
    lines.push_back("LR1121 bootloader rewrite: configured, but inert -- this chip's bootloader is already newer than "
                    "the configured target needs; there is no downgrade path.");
  }
#endif
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

  // REJECT_WRONG_CHIP never proceeds no matter what, including the bootloader-rewrite switch
  // (hard rule 6) -- the verdict was already computed and logged at boot, so refusing here is a
  // cached-verdict read, not a fresh bootloader entry.
  if (verdict == FlashDecision::REJECT_WRONG_CHIP) {
    ESP_LOGE(detail::TAG, "%s", this->describe_lr1121_flash_verdict_().c_str());
    return;
  }

  if (verdict == FlashDecision::REJECT_BOOTLOADER_TOO_OLD) {
#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE
    // The one place BootloaderUpgradePath::AVAILABLE can convert a hard rejection into the
    // three-stage sequence -- see lr1121_bootloader_upgrade_path()'s doc comment for the full
    // evaluation order. Every other outcome here still refuses without touching the chip.
    const BootloaderUpgradePath upgrade_path = lr1121_bootloader_upgrade_path(
        /*block_present=*/true, this->lr1121_bootloader_version_known_, this->lr1121_bootloader_version_,
        LR1121_BOOTLOADER_LOADER_FW, LR1121_FIRMWARE_UPDATE_TARGET_VERSION);
    if (upgrade_path == BootloaderUpgradePath::AVAILABLE) {
      if (!this->bootloader_rewrite_allowed_) {
        ESP_LOGE(detail::TAG, "%s", this->describe_lr1121_bootloader_refusal_(upgrade_path).c_str());
        return;
      }
      // The switch is read once, here, and replaces the two-press confirmation for this path --
      // it is a permission, not something that stacks with the two-press window (hard rule 6's
      // "never an override" applies the other way too: it only ever *adds* this one path).
      ESP_LOGW(detail::TAG, "LR1121 bootloader rewrite: arming switch is on -- running the three-stage sequence now.");
      this->run_lr1121_bootloader_upgrade_sequence_();
      return;
    }
    if (upgrade_path == BootloaderUpgradePath::BLOCKED_UNKNOWN_TARGET ||
        upgrade_path == BootloaderUpgradePath::BLOCKED_BOOTLOADER_NEWER) {
      ESP_LOGE(detail::TAG, "%s", this->describe_lr1121_bootloader_refusal_(upgrade_path).c_str());
      return;
    }
    // upgrade_path == NOT_APPLICABLE: no upgrade possible/needed from this state (e.g. the
    // boot-time bootloader read failed, or the configured loader doesn't match this bootloader) --
    // fall through to the same refusal the transceiver-only build always gave.
#endif
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
    // Boot never got a reading, and a radio that failed to initialize is exactly the case
    // reflashing is meant to recover, so this path stays open; the type check just
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

  uint32_t erase_elapsed_ms = 0, write_elapsed_ms = 0;
  if (!lr1121_erase_and_write_image_(*this->lr1121_firmware_updater_, "LR1121 firmware update",
                                     LR1121_FIRMWARE_UPDATE_IMAGE, LR1121_FIRMWARE_UPDATE_IMAGE_WORDS, erase_elapsed_ms,
                                     write_elapsed_ms)) {
    ESP_LOGE(detail::TAG,
             "LR1121 firmware update: the radio firmware is now incomplete. This is recoverable: after this "
             "reboot, press the button again to re-flash.");
    App.safe_reboot();
    return;
  }

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

#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE

void IOHomeControlComponent::run_lr1121_bootloader_upgrade_sequence_() {
  this->busy_ = true;
  this->warn_if_blocking_over_ = WARN_IF_BLOCKING_OVER_MAX_CS;
  if (this->radio_ != nullptr)
    this->radio_->set_mode_standby();

  ESP_LOGW(detail::TAG,
           "LR1121 bootloader rewrite: starting the three-stage sequence, ~10s total. Mains power, not "
           "battery -- do not interrupt power. Stage 2 has no recovery path in this project if power is lost.");

  // --- Stage 1a: bootloader mode -- erase + write the loader image. Every exit from here on is
  // App.safe_reboot() (see this method's doc comment in hub_core.h). ---
  uint8_t sanity_type = 0;
  uint16_t sanity_bootloader_version = 0;
  if (!this->lr1121_firmware_updater_->enter_bootloader(sanity_type, sanity_bootloader_version)) {
    ESP_LOGE(detail::TAG,
             "LR1121 bootloader rewrite: Stage 1a bootloader entry could not be confirmed (BUSY timeout) -- "
             "the bootloader itself is untouched, this is recoverable: press the button again to retry.");
    App.safe_reboot();
    return;
  }

  const Lr1121SanityResult sanity = lr1121_check_bootloader_sanity(
      this->lr1121_bootloader_version_known_, this->lr1121_bootloader_version_, sanity_type, sanity_bootloader_version);
  if (sanity != Lr1121SanityResult::OK) {
    const std::string sanity_reason = lr1121_sanity_failure_reason(sanity, sanity_bootloader_version);
    ESP_LOGE(detail::TAG,
             "LR1121 bootloader rewrite: Stage 1a sanity check failed (%s) -- aborting before erasing anything; "
             "the bootloader is untouched, this is recoverable: press the button again to retry.",
             sanity_reason.c_str());
    this->lr1121_firmware_updater_->reboot(false);
    App.safe_reboot();
    return;
  }
  // No "adopt an unknown boot-time reading" branch here, unlike run_lr1121_flash_sequence_()'s
  // equivalent point: this function only ever runs when lr1121_bootloader_upgrade_path() returned
  // AVAILABLE (trigger_lr1121_firmware_update(), the only caller), and that function's rule 2
  // returns NOT_APPLICABLE whenever !lr1121_bootloader_version_known_ -- so an unknown bootloader
  // can never reach this far. Not adding that branch here is deliberate: it would silently imply a
  // reachable state that doesn't exist, right next to the irreversible write.

  uint32_t erase_elapsed_ms = 0, write_elapsed_ms = 0;
  if (!lr1121_erase_and_write_image_(
          *this->lr1121_firmware_updater_, "LR1121 bootloader rewrite: Stage 1a (loader write)",
          LR1121_BOOTLOADER_LOADER_IMAGE, LR1121_BOOTLOADER_LOADER_IMAGE_WORDS, erase_elapsed_ms, write_elapsed_ms)) {
    ESP_LOGE(detail::TAG,
             "LR1121 bootloader rewrite: Stage 1a failed -- the bootloader is untouched, this is recoverable: "
             "press the button again to retry.");
    App.safe_reboot();
    return;
  }

  // --- Stage 1b: reboot into the loader; require it reports fw == 0x2100. Last checkpoint before
  // the irreversible write -- a loader that did not land is caught here, not in Stage 2. ---
  if (!this->lr1121_firmware_updater_->reboot(false)) {
    ESP_LOGE(detail::TAG,
             "LR1121 bootloader rewrite: Stage 1b reboot-into-loader command failed to send (BUSY timeout) -- "
             "the bootloader is untouched, this is recoverable: press the button again to retry.");
    App.safe_reboot();
    return;
  }
  uint8_t loader_device_type = 0, loader_fw_major = 0, loader_fw_minor = 0;
  // Read failure and version mismatch are deliberately not folded into one "fw == 0" check: this
  // is the last checkpoint before the irreversible write, so a BUSY timeout (the chip reported
  // nothing) must not be logged as though the chip positively reported firmware 0x0000.
  if (!this->lr1121_firmware_updater_->read_normal_version(loader_device_type, loader_fw_major, loader_fw_minor)) {
    ESP_LOGE(detail::TAG,
             "LR1121 bootloader rewrite: Stage 1b checkpoint failed -- could not read the chip's firmware version "
             "after the reboot (BUSY timeout). Aborting before the irreversible write; the bootloader is "
             "untouched, this is recoverable: press the button again to retry.");
    App.safe_reboot();
    return;
  }
  // The version alone does NOT prove the loader is running: the loader image reports 0x2100, and
  // so does the *bootloader* (LR1121_LOADER_2100 and LR1121_BOOTLOADER_2100 are the same number by
  // design). reboot() only confirms the command was sent, never that the chip acted on it, so a
  // chip that stayed in the bootloader would answer this read with exactly the bytes a successful
  // loader boot produces -- and 0x8100 would then be sent to the bootloader, which does not
  // implement it. `type` is the discriminator, and it is checked *positively* against the value the
  // loader is known to report (LR1121_UPDATER_LOADER_DEVICE_TYPE, 0xDE, observed on hardware):
  // 0xDE and the bootloader's 0xDF differ by one bit, so "anything but 0xDF" would accept a
  // single-bit corruption of exactly the byte this check exists to trust.
  if (loader_device_type != LR1121_UPDATER_LOADER_DEVICE_TYPE) {
    const bool still_in_bootloader = loader_device_type == LR1121_UPDATER_BOOTLOADER_TYPE;
    ESP_LOGE(detail::TAG,
             "LR1121 bootloader rewrite: Stage 1b checkpoint failed -- chip reports type=0x%02X, expected the "
             "loader's 0x%02X%s. The loader is not confirmed to be running, so 0x8100 must not be sent. "
             "Aborting before the irreversible write; the bootloader is untouched, this is recoverable: press "
             "the button again to retry.",
             loader_device_type, LR1121_UPDATER_LOADER_DEVICE_TYPE,
             still_in_bootloader ? " (0xDF means the chip never left bootloader mode)" : "");
    App.safe_reboot();
    return;
  }
  const uint16_t loader_running_fw = (static_cast<uint16_t>(loader_fw_major) << 8) | loader_fw_minor;
  if (loader_running_fw != LR1121_LOADER_2100) {
    ESP_LOGE(detail::TAG,
             "LR1121 bootloader rewrite: Stage 1b checkpoint failed -- chip reports firmware %s after the "
             "reboot, expected the loader's %s. Aborting before the irreversible write; the bootloader is "
             "untouched, this is recoverable: press the button again to retry.",
             format_lr1121_fw_version(loader_running_fw).c_str(), format_lr1121_fw_version(LR1121_LOADER_2100).c_str());
    App.safe_reboot();
    return;
  }
  ESP_LOGI(detail::TAG,
           "LR1121 bootloader rewrite: Stage 1b checkpoint passed -- chip in transceiver mode: type=0x%02X fw=%s",
           loader_device_type, format_lr1121_fw_version(loader_running_fw).c_str());

  // --- Stage 2: normal mode, the loader is the running firmware. The one irreversible write. ---
  ESP_LOGW(detail::TAG,
           "LR1121 bootloader rewrite: Stage 2 -- rewriting the bootloader now. This step cannot be undone. Do "
           "not interrupt power.");
  if (!this->lr1121_firmware_updater_->update_bootloader()) {
    ESP_LOGE(detail::TAG,
             "LR1121 bootloader rewrite: Stage 2 UpdateBootloader timed out waiting for BUSY -- outcome "
             "unknown, the bootloader may be mid-write. There is no recovery path in this project for this "
             "failure. Rebooting.");
    App.safe_reboot();
    return;
  }

  // Semtech's reference tool issues exactly this read between UpdateBootloader and
  // VerifyBootloader. Kept so the wire traffic through the one untestable stage stays identical to
  // the vendor's known-working sequence, and because command_status is the only direct report of
  // whether 0x8100 was accepted -- without it, a rejected command is indistinguishable from a
  // completed-but-bad write. Diagnostic only (Semtech ignores the result too); the gate is the six
  // check bits below.
  Lr1121UpdaterStatus updater_status;
  if (!this->lr1121_firmware_updater_->read_updater_status(updater_status)) {
    ESP_LOGW(detail::TAG,
             "LR1121 bootloader rewrite: Stage 2 status read timed out (BUSY) -- continuing to the verification "
             "read, which is what actually decides the outcome");
  } else if (updater_status.command_status != Lr1121UpdaterCommandStatus::OK &&
             updater_status.command_status != Lr1121UpdaterCommandStatus::DATA) {
    ESP_LOGE(detail::TAG,
             "LR1121 bootloader rewrite: Stage 2 chip reports command_status=%u after UpdateBootloader (0=FAIL, "
             "1=PERR) -- the chip did not accept 0x8100, which most likely means the bootloader was NOT "
             "rewritten. The verification below decides; report this line if it appears.",
             static_cast<unsigned>(updater_status.command_status));
  } else {
    ESP_LOGI(detail::TAG, "LR1121 bootloader rewrite: Stage 2 chip accepted UpdateBootloader (command_status=%u)",
             static_cast<unsigned>(updater_status.command_status));
  }

  Lr1121BootloaderVerification verification;
  if (!this->lr1121_firmware_updater_->verify_bootloader(verification)) {
    ESP_LOGE(detail::TAG,
             "LR1121 bootloader rewrite: Stage 2 VerifyBootloader read timed out (BUSY) after the write already "
             "ran -- outcome unknown. There is no recovery path in this project for this failure. Rebooting.");
    App.safe_reboot();
    return;
  }
  if (!verification.all_checks_passed()) {
    ESP_LOGE(detail::TAG,
             "LR1121 bootloader rewrite: Stage 2 verification failed after the write already ran (signature=%d "
             "version=%d use_case=%d version_major=%d version_minor=%d anti_rollback=%d) -- the write already "
             "happened; do NOT retry Stage 2. There is no recovery path in this project for this failure. "
             "Rebooting.",
             verification.signature_verified, verification.version_verified, verification.use_case_verified,
             verification.version_major_verified, verification.version_minor_verified,
             verification.anti_rollback_verified);
    App.safe_reboot();
    return;
  }

  if (!this->lr1121_firmware_updater_->updater_reboot(false)) {
    ESP_LOGE(detail::TAG,
             "LR1121 bootloader rewrite: Stage 2 post-verify reboot command failed to send (BUSY timeout) -- "
             "the write and verification both succeeded, but the chip's resulting state cannot be confirmed. "
             "Rebooting the ESP32.");
    App.safe_reboot();
    return;
  }
  // Success is INVERTED here: the new bootloader is expected to refuse the loader image (built for
  // the OLD bootloader) and stay in the bootloader rather than boot it (ADR 0021). A boot back
  // into the loader here would mean the new bootloader is not actually running.
  uint8_t post_update_type = 0;
  uint16_t post_update_bootloader_version = 0;
  const bool post_update_read_ok =
      this->lr1121_firmware_updater_->read_bootloader_version(post_update_type, post_update_bootloader_version);
  if (!post_update_read_ok || post_update_type != LR1121_UPDATER_BOOTLOADER_TYPE ||
      post_update_bootloader_version != LR1121_BOOTLOADER_2101) {
    ESP_LOGE(detail::TAG,
             "LR1121 bootloader rewrite: Stage 2 succeeded but the chip is not behaving as expected afterward "
             "(read_ok=%d type=0x%02X bootloader=%s; expected to stay in the bootloader reporting 0x2101) -- "
             "the write already happened; this is NOT the recoverable kind of failure. If the chip still "
             "answers a bootloader-mode GetVersion with a sane version, the strap works and a transceiver "
             "image can be written for whichever bootloader it reports -- but do not auto-retry Stage 2. "
             "Rebooting.",
             post_update_read_ok, post_update_type, format_hex16(post_update_bootloader_version).c_str());
    App.safe_reboot();
    return;
  }
  this->lr1121_bootloader_chip_type_ = post_update_type;
  this->lr1121_bootloader_version_ = post_update_bootloader_version;
  ESP_LOGI(detail::TAG, "LR1121 bootloader rewrite: Stage 2 complete -- bootloader is now 0x2101.");

  // --- Stage 3: re-enter the bootloader explicitly (do not rely on Stage 2's implicit state) and
  // write the transceiver image. Full recovery from here: the bootloader is already 0x2101 and the
  // transceiver image is already resident in ESP32 flash (the compile-time recovery-image rule). ---
  uint8_t stage3_type = 0;
  uint16_t stage3_bootloader_version = 0;
  if (!this->lr1121_firmware_updater_->enter_bootloader(stage3_type, stage3_bootloader_version)) {
    ESP_LOGE(detail::TAG,
             "LR1121 bootloader rewrite: Stage 3 bootloader entry could not be confirmed (BUSY timeout) -- the "
             "bootloader was already rewritten successfully in Stage 2, so this is recoverable: press the "
             "ordinary flash button again (no switch needed) once power is stable.");
    App.safe_reboot();
    return;
  }
  if (stage3_type != LR1121_UPDATER_BOOTLOADER_TYPE || stage3_bootloader_version != LR1121_BOOTLOADER_2101) {
    ESP_LOGE(detail::TAG,
             "LR1121 bootloader rewrite: Stage 3 sanity check failed (type=0x%02X bootloader=%s, expected "
             "0x2101) -- aborting before erasing the transceiver region. The bootloader was already rewritten "
             "successfully in Stage 2; this is recoverable: press the ordinary flash button again.",
             stage3_type, format_hex16(stage3_bootloader_version).c_str());
    App.safe_reboot();
    return;
  }
  this->lr1121_bootloader_chip_type_ = stage3_type;
  this->lr1121_bootloader_version_ = stage3_bootloader_version;

  if (!lr1121_erase_and_write_image_(
          *this->lr1121_firmware_updater_, "LR1121 bootloader rewrite: Stage 3 (transceiver write)",
          LR1121_FIRMWARE_UPDATE_IMAGE, LR1121_FIRMWARE_UPDATE_IMAGE_WORDS, erase_elapsed_ms, write_elapsed_ms)) {
    ESP_LOGE(detail::TAG,
             "LR1121 bootloader rewrite: Stage 3 failed -- the bootloader is already on 0x2101 (that part is "
             "done and does not need to be repeated); this is recoverable: after this reboot, the ordinary "
             "flash button (no switch needed) can retry the transceiver write.");
    App.safe_reboot();
    return;
  }

  lr1121_log_post_write_hash(*this->lr1121_firmware_updater_);

  if (!this->lr1121_firmware_updater_->reboot(false)) {
    ESP_LOGW(detail::TAG, "LR1121 bootloader rewrite: Stage 3 reboot-to-image command failed to send (BUSY timeout)");
  } else {
    uint8_t device_type = 0, fw_major = 0, fw_minor = 0;
    if (this->lr1121_firmware_updater_->read_normal_version(device_type, fw_major, fw_minor)) {
      const uint16_t new_fw = (static_cast<uint16_t>(fw_major) << 8) | fw_minor;
      lr1121_log_post_flash_verify_result(new_fw, LR1121_FIRMWARE_UPDATE_TARGET_VERSION);
    } else {
      ESP_LOGW(detail::TAG, "LR1121 bootloader rewrite: could not read back the post-flash version (BUSY timeout)");
    }
  }

  App.safe_reboot();
}

#endif  // IOHOME_LR1121_BOOTLOADER_UPDATE

}  // namespace home_io_control
}  // namespace esphome

#endif  // IOHOME_LR1121_FIRMWARE_UPDATE
