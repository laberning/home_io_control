#pragma once

/// @file lr1121_firmware_update_controller.h
/// @brief LR1121 transceiver-firmware-update feature — orchestration collaborator.
/// @ingroup hioc_hub
///
/// Owns the impure side of the feature: the boot-time bootloader-version excursion, the cached
/// flash verdict, the two-press confirmation window, and the button-triggered flash sequence
/// itself. The pure decision logic lives in lr1121_firmware_decisions.h; the bootloader-mode SPI
/// transport lives in radio_lr1121_firmware_updater.h/.cpp. ADR 0020 and ADR 0021 record the
/// design; the single most important rule they state is repeated in the .cpp because it is easy to
/// violate by accident.
///
/// Header-weight note: `FlashDecision` / `BootloaderUpgradePath` are scoped enums with a fixed
/// underlying type and `Lr1121FirmwareUpdater` is held only as a pointer, so all three are
/// forward-declared here and the heavy headers are pulled in by the .cpp alone. Including them
/// here would drag them back into hub_core.h transitively and lose the payoff.

// IOHOME_LR1121_FIRMWARE_UPDATE is only visible after something pulls in esphome/core/defines.h
// (ESPHome codegen's cg.add_define() lands there, not as a compiler -D flag) — the #include below
// must run before the #ifdef check, not after, mirroring radio_lr1121_firmware_updater.h. hub_core.h
// already includes esphome/core/hal.h before this header, so this is belt-and-braces.
#include "esphome/core/hal.h"

#ifdef IOHOME_LR1121_FIRMWARE_UPDATE

#include "hub_hooks.h"

#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace home_io_control {

// Forward declarations — the full definitions are included only in the .cpp.
// InternalGPIOPin is NOT forward-declared here: it lives in namespace esphome (not
// esphome::home_io_control), and declaring it in this namespace would shadow the real type and
// break RadioDriver's constructor. esphome/core/hal.h (included above) already provides it.
class Lr1121FirmwareUpdater;
enum class FlashDecision : uint8_t;
enum class BootloaderUpgradePath : uint8_t;
class RadioDriver;
class SpiAccess;
class IOHomeControlComponent;

/// @brief Orchestrates the LR1121 transceiver-firmware-update feature.
///
/// Constructed once by IOHomeControlComponent (guarded by IOHOME_LR1121_FIRMWARE_UPDATE);
/// non-copyable because it holds injected callbacks and pointers into hub member addresses.
/// @ingroup hioc_hub
class Lr1121FirmwareUpdateController {
 public:
  /// @param radio    Double pointer to the hub's active radio driver (may be null after a failed
  ///                 init(); reflashing is exactly that recovery case).
  /// @param spi      SPI bus access — the hub itself (it implements SpiAccess).
  /// @param rst_pin  Double pointer to the hub's radio reset pin (set after construction).
  /// @param busy_pin Double pointer to the hub's radio BUSY pin (set after construction).
  /// @param busy     Pointer to the hub's `busy_` flag (protected; guards every radio action).
  /// @param begin_blocking_excursion  Raises the blocking-warn threshold (see BeginBlockingExcursionFn).
  /// @param hub      Hub pointer — used ONLY as the self key for App.scheduler.set_timeout(), never
  ///                 to reach a protected hub member.
  Lr1121FirmwareUpdateController(RadioDriver **radio, SpiAccess *spi, InternalGPIOPin **rst_pin,
                                 InternalGPIOPin **busy_pin, bool *busy,
                                 BeginBlockingExcursionFn begin_blocking_excursion, IOHomeControlComponent *hub);

  /// Non-copyable — holds injected callbacks and pointers into hub member addresses.
  Lr1121FirmwareUpdateController(const Lr1121FirmwareUpdateController &) = delete;
  Lr1121FirmwareUpdateController &operator=(const Lr1121FirmwareUpdateController &) = delete;

  /// @brief Boot-time bootloader-version excursion.
  ///
  /// Called from setup() after select_and_construct_radio_() and before radio_->init() — at that
  /// point nothing has configured the radio yet, so a bootloader excursion costs one extra chip
  /// reset and needs no reboot afterward (unlike every other bootloader excursion in this
  /// feature). Constructs the Lr1121FirmwareUpdater, runs the excursion, and caches the bootloader
  /// version/type. Never fails setup(): a failed read just leaves the bootloader version "unknown"
  /// and lets radio_->init() proceed normally.
  void run_boot_time_bootloader_read();

  /// @brief Compute and cache the flash verdict once radio_->init() has produced (or failed to
  /// produce) an installed-firmware-version read.
  ///
  /// Must run after init(), not during the boot-time excursion above: the installed version comes
  /// from configure_radio_(), which runs inside init(). Called from setup() regardless of whether
  /// init() succeeded — see the null-radio recovery-path reasoning in trigger()'s guard 0.
  void cache_flash_verdict();

  /// @brief Emit the bootloader version and cached flash verdict to the config dump.
  /// Called from dump_config(), next to the existing radio_->dump_debug() call.
  void dump_debug() const;

  /// @brief Pure content behind dump_debug(), factored out so it is testable without a
  /// log-capturing harness (ESP_LOGCONFIG is a no-op in host tests). Always includes the verdict
  /// line when the verdict is known, independent of whether the bootloader version is — see the
  /// implementation for why that independence matters.
  std::vector<std::string> debug_lines() const;

  /// @brief Human-readable explanation of the cached verdict, shared by dump_debug() and trigger()
  /// so the boot-time config dump and a button-press log always say the same thing.
  /// @return A complete log message (no trailing newline).
  std::string describe_flash_verdict() const;

  /// @brief Entry point for the "Flash LR1121 Radio Firmware" button.
  ///
  /// See the .cpp for the full contract, including the safety invariant that every bootloader
  /// excursion this triggers must end in either radio_->init() or App.safe_reboot() — there is no
  /// third option.
  void trigger();

#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE
  /// @brief User-facing text for a bootloader-rewrite refusal at button-press time.
  ///
  /// Separate from describe_flash_verdict() rather than a suffix on it: that function opens with
  /// "CANNOT PROCEED", which reads as final and would then contradict an explanation that the
  /// rewrite is available. Leads with the outcome (nothing happened), then the reason, then the
  /// next action. Returns a string rather than logging directly so it stays testable — host builds
  /// compile the ESP_LOG* macros to no-ops.
  /// @param path The cached upgrade path that produced the refusal.
  /// @return The message to log.
  std::string describe_bootloader_refusal(BootloaderUpgradePath path) const;

  /// @brief Set by the "Allow LR1121 Bootloader Rewrite (Irreversible)" switch's write_state().
  ///
  /// A permission, not an override: this can only convert a cached
  /// BootloaderUpgradePath::AVAILABLE verdict into "run the three-stage sequence" (bootloader
  /// ADR 0021) — it never affects REJECT_WRONG_CHIP, the post-entry sanity check, the busy_ guard,
  /// or any other verdict. Read once, at button-press time (trigger()); the ESPHome loop is
  /// blocked for the whole three-stage sequence once it starts, so the switch cannot change
  /// mid-flash. Deliberately not named anything with "armed" — lr1121_flash_confirmation_armed_
  /// already means the two-press window this switch *replaces* for its own path, and a reader must
  /// never have to guess which is meant.
  void set_bootloader_rewrite_allowed(bool allowed) { this->bootloader_rewrite_allowed_ = allowed; }
#endif

  // --- State (public so the host tests that script individual stages can preset and inspect it;
  // names kept verbatim from the pre-extraction IOHomeControlComponent members). ---

  /// Heap-allocated in run_boot_time_bootloader_read(), like radio_ — constructed once, used by
  /// both the boot-time excursion and any later button press. Never deleted/reconstructed at runtime.
  Lr1121FirmwareUpdater *lr1121_firmware_updater_{nullptr};
  bool lr1121_bootloader_version_known_{false};  ///< False until the boot-time excursion succeeds.
  uint8_t lr1121_bootloader_chip_type_{0};       ///< `type` byte from the boot-time bootloader GetVersion.
  uint16_t lr1121_bootloader_version_{0};        ///< Bootloader version from the boot-time excursion.
  bool lr1121_flash_verdict_known_{false};       ///< False until cache_flash_verdict() has run.
  /// Cached verdict (see decisions header). No NSDMI here because FlashDecision is only
  /// forward-declared in this header; it is initialized in the constructor's init-list, and any
  /// later constructor added to this class MUST do the same.
  FlashDecision lr1121_flash_verdict_;
  uint16_t lr1121_installed_fw_{0};  ///< Installed firmware version at the time the verdict was cached (0=unknown).
  /// `device_type` byte from the same normal-mode GetVersion that produced lr1121_installed_fw_
  /// (0=unknown, e.g. after a failed init()) — kept alongside it so describe_flash_verdict() can
  /// name which chip a REJECT_WRONG_CHIP verdict actually saw. See lr1121_flash_decision()'s
  /// `device_type` parameter (layer 3) for why this is a distinct value from
  /// lr1121_bootloader_chip_type_ above (layer 4, bootloader-mode).
  uint8_t lr1121_installed_device_type_{0};
  bool lr1121_flash_confirmation_armed_{false};  ///< True during the two-press confirmation window.
#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE
  /// Set by the arming switch (IOHomeLr1121BootloaderRewriteSwitch); see
  /// set_bootloader_rewrite_allowed()'s comment above for what this may and may not affect.
  bool bootloader_rewrite_allowed_{false};
#endif

 private:
  /// @brief Arm the two-press confirmation window and schedule its auto-disarm.
  /// Mirrors key_extraction_responder.cpp's KEY_EXTRACTION_AUTO_OFF_MS idiom (named set_timeout,
  /// guard against a stale callback after a fresh press already disarmed).
  void arm_flash_confirmation_();

  /// @brief The bootloader-entry-through-post-flash-verify sequence, run only once trigger() has
  /// decided to actually flash. Split out from that method to keep its own cognitive complexity
  /// within clang-tidy's threshold.
  ///
  /// Per the safety invariant: once this method's bootloader entry succeeds, every exit —
  /// including every failure path — ends in `App.safe_reboot()`. There is no `return` in here that
  /// leaves the chip unconfigured without also rebooting the ESP32.
  void run_flash_sequence_();

#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE
  /// @brief The three-stage bootloader-rewrite sequence (ADR 0021): write the loader image, reboot
  /// into it, rewrite the bootloader via 0x81xx, then write the transceiver image. Run only once
  /// trigger() has decided the switch permits it and the cached path is
  /// BootloaderUpgradePath::AVAILABLE.
  ///
  /// Same safety invariant as run_flash_sequence_(): every exit past the first enter_bootloader()
  /// call is App.safe_reboot(), including every stage's failure path — a stage-2 failure must
  /// reboot, not fall through to stage 3. Stage 2 (0x8100 in flight) is the one step with no
  /// recovery path in this project; every log line in that stage must say so plainly and must
  /// never reuse this component's ordinary "press again" phrasing.
  void run_bootloader_upgrade_sequence_();
#endif

  RadioDriver **radio_;
  SpiAccess *spi_;
  InternalGPIOPin **rst_pin_;
  InternalGPIOPin **busy_pin_;
  bool *busy_;
  BeginBlockingExcursionFn begin_blocking_excursion_;
  IOHomeControlComponent *hub_;
};

}  // namespace home_io_control
}  // namespace esphome

#endif  // IOHOME_LR1121_FIRMWARE_UPDATE
