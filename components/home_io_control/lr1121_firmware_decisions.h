#pragma once

/// @file lr1121_firmware_decisions.h
/// @brief Pure decision logic for the LR1121 transceiver-firmware-update feature.
/// @ingroup hioc_radio
///
/// Header-only pure functions, no I/O — same style as the decisions_*.h headers and
/// radio_lr1121.h::lr1121_firmware_is_outdated(), fully host-testable without radio hardware.
/// Deliberately does not include radio_lr1121.h: LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS below
/// is a local copy of that file's LR1121_DEVICE_TYPE (0x03), kept independent so this header pulls
/// in nothing but cstdint — the same reasoning that keeps the decisions_*.h headers free of driver headers.

#include <cstdint>

namespace esphome {
namespace home_io_control {

/// @brief LR1121 GetVersion `type` byte in normal mode — must match radio_lr1121.h's
/// LR1121_DEVICE_TYPE (0x03). Kept as a separate constant, not a shared header include, so this
/// header stays dependency-free; see the file header for why. A static_assert in
/// tests/radio/radio_lr1121_firmware_updater_test.cpp (a translation unit that already includes both
/// headers) guards the two from silently drifting apart.
inline constexpr uint8_t LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS = 0x03;

/// @brief Normal-mode GetVersion `type` bytes for the two chips an LR1121 is most easily confused
/// with — companions to LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS above, used only to name the
/// chip family in a REJECT_WRONG_CHIP message. Last checked 2026-08-05.
inline constexpr uint8_t LR1110_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS = 0x01;
inline constexpr uint8_t LR1120_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS = 0x02;

/// @brief LR1121 GetVersion `type` byte reported while running the *bootloader*
/// (LR11XX_TYPE_PRODUCTION_MODE) — must match radio_lr1121_firmware_updater.h's
/// LR1121_UPDATER_BOOTLOADER_TYPE (0xDF). Kept as a separate constant for the same
/// dependency-free reason as LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS above. Note this byte
/// identifies production silicon, not chip family — bootloader `type` is 0xDF on an LR1120 or
/// LR1110 too, which is why chip family is decided from the bootloader *version*, not this byte.
inline constexpr uint8_t LR1121_BOOTLOADER_TYPE_FOR_FIRMWARE_DECISIONS = 0xDF;

/// LR1121 bootloader versions, from Semtech's published compatibility matrix. An LR1121
/// reports one of these two; LR1120 reports 0x2000/0x2001 and LR1110 0x6500/0x1001, which is
/// what makes checking against these two a chip-family check as well as a capability check.
inline constexpr uint16_t LR1121_BOOTLOADER_2100 = 0x2100;
inline constexpr uint16_t LR1121_BOOTLOADER_2101 = 0x2101;

/// @brief Bootloader versions reported by the two chips an LR1121 is most easily confused with —
/// used only to name the chip family in a REJECT_WRONG_CHIP message. Same snapshot discipline as
/// LR1121_BOOTLOADER_2100/2101; last checked 2026-08-05.
inline constexpr uint16_t LR1120_BOOTLOADER_2000 = 0x2000;
inline constexpr uint16_t LR1120_BOOTLOADER_2001 = 0x2001;
inline constexpr uint16_t LR1110_BOOTLOADER_6500 = 0x6500;
inline constexpr uint16_t LR1110_BOOTLOADER_1001 = 0x1001;

/// @brief Version the `lr1121_loader_2100.bin` bootloader-*loader* image reports of itself.
/// Numerically identical to LR1121_BOOTLOADER_2100, but a distinct concept: this is the loader
/// firmware's own self-reported version, which Semtech's compatibility rule requires to *equal*
/// the bootloader version currently running before the loader may be used -- not a
/// "requires bootloader >= X" rule like LR1121_KNOWN_BOOTLOADER_REQUIREMENTS below.
inline constexpr uint16_t LR1121_LOADER_2100 = 0x2100;

/// @brief Normal-mode GetVersion `type` byte the bootloader *loader* image reports while running —
/// must match radio_lr1121_firmware_updater.h's LR1121_UPDATER_LOADER_DEVICE_TYPE (0xDE). Kept as a
/// separate constant for the same dependency-free reason as LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS
/// above. It differs from LR1121_BOOTLOADER_TYPE_FOR_FIRMWARE_DECISIONS by exactly one bit, which is
/// why lr1121_loader_type_is_loader() compares for equality instead of excluding the bootloader's value.
inline constexpr uint8_t LR1121_LOADER_TYPE_FOR_FIRMWARE_DECISIONS = 0xDE;

/// @brief `command_status` values of the loader's status read that mean the chip accepted the
/// command — must match radio_lr1121_firmware_updater.h's Lr1121UpdaterCommandStatus::OK / DATA.
inline constexpr uint8_t LR1121_COMMAND_STATUS_OK_FOR_FIRMWARE_DECISIONS = 0x02;
inline constexpr uint8_t LR1121_COMMAND_STATUS_DATA_FOR_FIRMWARE_DECISIONS = 0x03;

/// @return true if `bootloader_version` is one of the two bootloader versions an LR1121 (as
/// opposed to an LR1120 or LR1110) can report.
[[nodiscard]] constexpr bool lr1121_bootloader_is_lr1121(uint16_t bootloader_version) {
  return bootloader_version == LR1121_BOOTLOADER_2100 || bootloader_version == LR1121_BOOTLOADER_2101;
}

/// @brief Human-readable chip family for a bootloader version that is not one of the two LR1121
/// values above, for an actionable REJECT_WRONG_CHIP message. Moved here from hub wiring so the
/// LR1120/LR1110 bootloader IDs live in one place (next to the LR1121 ones they are compared
/// against) and the mapping is host-testable without a hub.
[[nodiscard]] constexpr const char *lr1121_chip_family_for_bootloader(uint16_t bootloader_version) {
  if (bootloader_version == LR1120_BOOTLOADER_2000 || bootloader_version == LR1120_BOOTLOADER_2001)
    return "an LR1120";
  if (bootloader_version == LR1110_BOOTLOADER_6500 || bootloader_version == LR1110_BOOTLOADER_1001)
    return "an LR1110";
  return "an unrecognized chip";
}

/// @brief Human-readable chip family for a normal-mode device_type that is not the LR1121 value
/// above, for an actionable REJECT_WRONG_CHIP message (the layer-3 counterpart of
/// lr1121_chip_family_for_bootloader() above).
[[nodiscard]] constexpr const char *lr1121_chip_family_for_device_type(uint8_t device_type) {
  if (device_type == LR1110_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS)
    return "an LR1110";
  if (device_type == LR1120_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS)
    return "an LR1120";
  return "an unrecognized chip";
}

/// @brief One (target firmware, required bootloader) pairing from Semtech's published
/// compatibility matrix.
struct Lr1121BootloaderRequirement {
  uint16_t target_fw;
  uint16_t bootloader;
};

/// Bootloader requirements we know about, copied from Semtech's published pairings
/// (SWTL001/application/src/lr11xx_update_utils.c :: compatibility_matrix[]): 0x0101/0x0102/0x0103
/// need bootloader 0x2100, 0x0104 needs 0x2101. This is an ADVISORY list of pairs we can warn
/// about, not a compatibility authority — it is a point-in-time snapshot and a target absent from
/// it is "unverified", never "incompatible". Extending this when Semtech publishes a new image is
/// a one-line edit; it belongs next to LR1121_KNOWN_LATEST_FW_* (radio_lr1121.h) as the same kind
/// of snapshot, with the same staleness discipline. Last checked 2026-08-05.
inline constexpr Lr1121BootloaderRequirement LR1121_KNOWN_BOOTLOADER_REQUIREMENTS[] = {
    {0x0101, LR1121_BOOTLOADER_2100},
    {0x0102, LR1121_BOOTLOADER_2100},
    {0x0103, LR1121_BOOTLOADER_2100},
    {0x0104, LR1121_BOOTLOADER_2101},
};

/// @brief Whether a target firmware version is known to work with a given bootloader version.
enum class BootloaderSupport : uint8_t {
  SUPPORTED,       ///< Both versions known, and the pairing is in LR1121_KNOWN_BOOTLOADER_REQUIREMENTS.
  UNSUPPORTED,     ///< target_fw is known, but not paired with this bootloader_version.
  UNKNOWN_TARGET,  ///< target_fw does not appear in LR1121_KNOWN_BOOTLOADER_REQUIREMENTS at all.
};

/// @brief Look up whether `target_fw` is known to require `bootloader_version`.
///
/// A target absent from LR1121_KNOWN_BOOTLOADER_REQUIREMENTS is UNKNOWN_TARGET, never
/// UNSUPPORTED — the table is a snapshot of what Semtech had published when this file was last
/// updated, not an allow-list; a future firmware version this build has never heard of must not
/// be rejected on that basis alone (see lr1121_flash_decision()'s three-way rule).
[[nodiscard]] constexpr BootloaderSupport lr1121_bootloader_supports_target(uint16_t target_fw,
                                                                            uint16_t bootloader_version) {
  for (const auto &requirement : LR1121_KNOWN_BOOTLOADER_REQUIREMENTS) {
    if (requirement.target_fw == target_fw) {
      return requirement.bootloader == bootloader_version ? BootloaderSupport::SUPPORTED
                                                          : BootloaderSupport::UNSUPPORTED;
    }
  }
  return BootloaderSupport::UNKNOWN_TARGET;
}

/// @brief Required bootloader for a known target firmware version.
/// @return The paired bootloader from LR1121_KNOWN_BOOTLOADER_REQUIREMENTS, or 0 when target_fw
///         is not in the table -- an "unverified", not "incompatible", target (see that table's
///         comment), and never a real requirement value since no table entry uses 0.
[[nodiscard]] constexpr uint16_t lr1121_required_bootloader_for(uint16_t target_fw) {
  for (const auto &requirement : LR1121_KNOWN_BOOTLOADER_REQUIREMENTS) {
    if (requirement.target_fw == target_fw)
      return requirement.bootloader;
  }
  return 0;
}

/// @brief Direction of a bootloader/target mismatch, for messaging.
///
/// lr1121_flash_decision()'s REJECT_BOOTLOADER_TOO_OLD verdict fires whenever
/// lr1121_bootloader_supports_target() returns UNSUPPORTED, which covers both directions: the
/// target needs a newer bootloader than this chip has (the only direction reachable before the
/// bootloader-update feature existed), and the target needs an OLDER bootloader than this chip
/// has -- a downgrade, unreachable until a chip can actually be running 0x2101. Used for
/// *messaging* only, in both the transceiver-only and bootloader-update builds -- it never
/// changes lr1121_flash_decision()'s own verdict.
enum class BootloaderMismatch : uint8_t {
  NONE,                ///< target_fw is unknown, or its required bootloader matches bootloader_version.
  TARGET_NEEDS_NEWER,  ///< The "too old" direction -- the direction REJECT_BOOTLOADER_TOO_OLD covers.
  TARGET_NEEDS_OLDER,  ///< The "too new" direction -- a downgrade.
};

/// @brief Classify a bootloader/target mismatch by direction; see BootloaderMismatch.
[[nodiscard]] constexpr BootloaderMismatch lr1121_bootloader_mismatch_kind(uint16_t target_fw,
                                                                           uint16_t bootloader_version) {
  const uint16_t required = lr1121_required_bootloader_for(target_fw);
  if (required == 0 || required == bootloader_version)
    return BootloaderMismatch::NONE;
  return required > bootloader_version ? BootloaderMismatch::TARGET_NEEDS_NEWER
                                       : BootloaderMismatch::TARGET_NEEDS_OLDER;
}

/// @brief Whether the three-stage bootloader-rewrite sequence (ADR 0021) is applicable, and if
/// not, why.
enum class BootloaderUpgradePath : uint8_t {
  NOT_APPLICABLE,            ///< No block, or no upgrade needed/possible to evaluate. Keep the original verdict.
  AVAILABLE,                 ///< Three-stage is possible. Still requires the arming switch to actually run.
  BLOCKED_UNKNOWN_TARGET,    ///< Block present, but this build cannot know what the target needs.
  BLOCKED_BOOTLOADER_NEWER,  ///< Target needs an OLDER bootloader. Not reachable today, likely never.
};

/// @brief Whether the three-stage bootloader upgrade is applicable for the current cached state.
///
/// A post-filter, consulted only when lr1121_flash_decision() has already returned
/// REJECT_BOOTLOADER_TOO_OLD -- it never runs earlier and never changes that function's verdict
/// (see this header's file comment and lr1121_flash_decision()'s doc comment). The evaluation
/// order below *is* the specification -- each rule exists to close a specific way an irreversible
/// write could be justified on insufficient evidence:
///   1. !block_present -> NOT_APPLICABLE -- feature not built in.
///   2. !bootloader_version_known -> NOT_APPLICABLE -- an unknown current bootloader cannot
///      justify an irreversible write. Note Lr1121FirmwareUpdateController::run_flash_sequence_()
///      *adopts* a fresh reading at flash time for the ordinary transceiver path; that allowance
///      must not extend to here.
///   3. bootloader_version doesn't identify an LR1121 -> NOT_APPLICABLE -- REJECT_WRONG_CHIP owns
///      wrong-chip messaging, this function does not duplicate it.
///   4. loader_fw != bootloader_version -> NOT_APPLICABLE -- Semtech's equality rule for the
///      loader image (see LR1121_LOADER_2100); also the "chip is already on 0x2101" case, since
///      no 0x2101-chip can equal the 0x2100 loader.
///   5. target not in the compatibility table (required == 0) -> BLOCKED_UNKNOWN_TARGET -- never
///      gamble an irreversible write on an unrecognised target.
///   6. required == bootloader_version -> NOT_APPLICABLE -- no upgrade needed.
///   7. required > bootloader_version -> AVAILABLE -- the one path that proceeds.
///   8. else (required < bootloader_version) -> BLOCKED_BOOTLOADER_NEWER -- a downgrade. The
///      0x8101 report includes an anti-rollback check, whose exact semantics Semtech does not
///      document, but which most likely makes this permanently impossible; ADR 0021 records how
///      far that is inference.
/// @param block_present Whether a `bootloader:` sub-block is configured (the build flag).
/// @param bootloader_version_known Whether the boot-time excursion successfully read a bootloader
///        version -- same "unknown is never evidence" sentinel rule as lr1121_flash_decision().
/// @param bootloader_version Bootloader version read at boot; meaningless if !bootloader_version_known.
/// @param loader_fw Version parsed from the bootloader: sub-block's loader source: image.
/// @param target_fw Configured target firmware version (0 if unknown).
[[nodiscard]] constexpr BootloaderUpgradePath lr1121_bootloader_upgrade_path(bool block_present,
                                                                             bool bootloader_version_known,
                                                                             uint16_t bootloader_version,
                                                                             uint16_t loader_fw, uint16_t target_fw) {
  if (!block_present)
    return BootloaderUpgradePath::NOT_APPLICABLE;
  if (!bootloader_version_known)
    return BootloaderUpgradePath::NOT_APPLICABLE;
  if (!lr1121_bootloader_is_lr1121(bootloader_version))
    return BootloaderUpgradePath::NOT_APPLICABLE;
  if (loader_fw != bootloader_version)
    return BootloaderUpgradePath::NOT_APPLICABLE;

  const uint16_t required = lr1121_required_bootloader_for(target_fw);
  if (required == 0)
    return BootloaderUpgradePath::BLOCKED_UNKNOWN_TARGET;
  if (required == bootloader_version)
    return BootloaderUpgradePath::NOT_APPLICABLE;
  if (required > bootloader_version)
    return BootloaderUpgradePath::AVAILABLE;
  return BootloaderUpgradePath::BLOCKED_BOOTLOADER_NEWER;
}

// === Acceptance checks of the bootloader-rewrite sequence ===

/// @brief Outcome of the sanity read taken right after entering the bootloader, before anything
/// is erased.
enum class Lr1121SanityResult : uint8_t {
  OK,                 ///< type matches, and either the version matches what boot recorded, or boot
                      ///< recorded nothing and the freshly-read version positively identifies an LR1121.
  WRONG_TYPE,         ///< type is not the bootloader's -- not in bootloader mode at all.
  VERSION_MISMATCH,   ///< type is fine, but the version boot recorded no longer matches.
  WRONG_CHIP_FAMILY,  ///< Boot recorded nothing to compare against, and the freshly-read version
                      ///< does not identify an LR1121 -- see lr1121_check_bootloader_sanity().
};

/// @brief Sanity check on the bootloader-mode GetVersion read taken before the first erase.
///
/// The `type` byte is the production-silicon marker an LR1120 or LR1110 reports too, so passing it
/// proves only that the chip is in bootloader mode. When boot recorded a bootloader version
/// (`known`), the fresh read must equal it. When boot recorded nothing, the fresh read has no
/// earlier value to match, so it must itself identify an LR1121 (lr1121_bootloader_is_lr1121());
/// otherwise the "adopt whatever bootloader-mode read we get now" recovery path would overwrite an
/// LR1120 or LR1110 with an LR1121 image on a byte both chips share. This is the last check before
/// the erase.
/// @param known Whether boot recorded a bootloader version.
/// @param known_bootloader_version The version boot recorded; ignored when !known.
/// @param sanity_type `type` byte of the fresh read.
/// @param sanity_bootloader_version Bootloader version of the fresh read.
[[nodiscard]] constexpr Lr1121SanityResult lr1121_check_bootloader_sanity(bool known, uint16_t known_bootloader_version,
                                                                          uint8_t sanity_type,
                                                                          uint16_t sanity_bootloader_version) {
  if (sanity_type != LR1121_BOOTLOADER_TYPE_FOR_FIRMWARE_DECISIONS)
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

/// @brief Whether the `type` byte read after rebooting into the loader image is the loader's.
///
/// The loader and the bootloader report the same version number, so the version alone cannot tell
/// them apart; a chip that stayed in the bootloader would pass a version-only check. The byte is
/// compared for equality with the loader's value, never as "not the bootloader's", so a single-bit
/// corruption of it is rejected too.
[[nodiscard]] constexpr bool lr1121_loader_type_is_loader(uint8_t device_type) {
  return device_type == LR1121_LOADER_TYPE_FOR_FIRMWARE_DECISIONS;
}

/// @brief Whether the firmware version read after rebooting into the loader image is the loader's
/// own version (LR1121_LOADER_2100).
[[nodiscard]] constexpr bool lr1121_loader_firmware_is_loader(uint16_t firmware) {
  return firmware == LR1121_LOADER_2100;
}

/// @brief Whether the loader's `command_status` says it accepted the last command (executed, with
/// or without data). FAIL and PERR, and any value outside the defined set, are not acceptance.
[[nodiscard]] constexpr bool lr1121_update_command_accepted(uint8_t command_status) {
  return command_status == LR1121_COMMAND_STATUS_OK_FOR_FIRMWARE_DECISIONS ||
         command_status == LR1121_COMMAND_STATUS_DATA_FOR_FIRMWARE_DECISIONS;
}

/// @brief Whether a bootloader-mode GetVersion identifies the rewritten bootloader
/// (LR1121_BOOTLOADER_2101): the bootloader's `type` byte and the new version.
[[nodiscard]] constexpr bool lr1121_is_rewritten_bootloader(uint8_t type, uint16_t bootloader_version) {
  return type == LR1121_BOOTLOADER_TYPE_FOR_FIRMWARE_DECISIONS && bootloader_version == LR1121_BOOTLOADER_2101;
}

/// @brief Whether the read taken after the bootloader write shows the write took effect.
///
/// The new bootloader is expected to refuse the loader image, which was built for the old
/// bootloader, and stay in bootloader mode, so success is a bootloader-mode read reporting the new
/// version. A failed read is never success.
/// @param read_ok Whether the read completed.
/// @param type `type` byte of the read.
/// @param bootloader_version Bootloader version of the read.
[[nodiscard]] constexpr bool lr1121_bootloader_rewrite_took(bool read_ok, uint8_t type, uint16_t bootloader_version) {
  return read_ok && lr1121_is_rewritten_bootloader(type, bootloader_version);
}

/// @brief Outcome of lr1121_flash_decision().
enum class FlashDecision : uint8_t {
  PROCEED,                    ///< Safe to erase and write.
  ALREADY_INSTALLED,          ///< target_fw == installed_fw (both known) — the post-success state.
  NEEDS_CONFIRMATION,         ///< Not unsafe, but not an unambiguous "yes" either — needs a second press.
  REJECT_WRONG_CHIP,          ///< device_type or bootloader_version doesn't identify an LR1121.
  REJECT_BOOTLOADER_TOO_OLD,  ///< target_fw is known and positively incompatible with this bootloader.
};

/// @brief The single decision point for whether/how to flash `target_fw`.
///
/// Order of checks. Layers 3 and 4 below are the two chip-identity checks (normal-mode
/// device_type and bootloader-mode version); everything else exists to make sure an absent read
/// is never mistaken for evidence:
///   1. Boot excursion never completed at all (bootloader_version unknown) -- nothing below can
///      be evaluated, so this is checked first and short-circuits straight to NEEDS_CONFIRMATION.
///   2. Layer 4 -- bootloader-mode identity: `type` must be the production-silicon byte AND the
///      bootloader version must be one an LR1121 actually reports (as opposed to an LR1120 or
///      LR1110, which also answer bootloader-mode GetVersion, just with different values).
///   3. Normal-mode read never happened (device_type unknown, e.g. after a failed init()) --
///      unknown is not evidence of the wrong chip, so this is checked *before* layer 3 can turn
///      it into a false REJECT_WRONG_CHIP.
///   4. Layer 3 -- normal-mode chip identity: distinguishes LR1121 from LR1110/LR1120 while
///      normal-mode firmware can still answer.
///   5. Bootloader/target compatibility (the three-way rule -- known-compatible proceeds,
///      known-incompatible refuses, unknown-target asks for confirmation rather than refusing so
///      a future firmware release keeps working without a code change here).
///   6. Only once the pairing is positively known-good: the not-newer-so-confirm version compare.
///
/// Every input below has its own "unknown" sentinel, and every one of them routes to
/// NEEDS_CONFIRMATION rather than a rejection or a false PROCEED -- an absent read is never
/// evidence of anything, safe or unsafe:
///   - `bootloader_version == 0`: the boot-time excursion never successfully read one. 0x0000 is
///     not a value any real LR11xx bootloader reports.
///   - `device_type == 0`: the normal-mode read never happened at all (failed init(), or the read
///     itself failed). Deliberately still allowed to reach a flash after confirmation -- a radio
///     that failed to initialize is exactly the case reflashing is meant to recover.
///   - `target_fw == 0`: the build could not derive a version from the filename and none was
///     configured; can never match a LR1121_KNOWN_BOOTLOADER_REQUIREMENTS entry (none uses 0), so
///     it always resolves to BootloaderSupport::UNKNOWN_TARGET.
///   - `installed_fw == 0`: the installed version is unknown (device_type known-good but the
///     firmware-version bytes could not be read) -- treated exactly like "not newer", never as
///     "older than everything" the way a naive `target_fw > installed_fw` would.
///
/// @param device_type Chip identity byte from a *normal-mode* GetVersion (layer 3); 0 if that
///        read never happened.
/// @param bootloader_chip_type `type` byte from the *bootloader-mode* GetVersion read at boot
///        (layer 4); 0xDF (LR1121_BOOTLOADER_TYPE_FOR_FIRMWARE_DECISIONS) for production silicon
///        of any LR11xx family, or 0 if the boot excursion never completed.
/// @param bootloader_version Bootloader version read at boot; one of
///        LR1121_BOOTLOADER_2100/2101 for a genuine LR1121, or 0 if never successfully read.
/// @param installed_fw Currently-installed transceiver firmware version (0 if unknown, e.g. after
///        a failed init()).
/// @param target_fw Configured target firmware version (0 if unknown — see above).
/// @param already_confirmed True on a second button press within the confirmation window; allows
///        every non-hard-reject outcome to proceed instead of asking again.
/// @return The decision; see FlashDecision.
[[nodiscard]] constexpr FlashDecision lr1121_flash_decision(uint8_t device_type, uint8_t bootloader_chip_type,
                                                            uint16_t bootloader_version, uint16_t installed_fw,
                                                            uint16_t target_fw, bool already_confirmed) {
  if (bootloader_version == 0)
    return already_confirmed ? FlashDecision::PROCEED : FlashDecision::NEEDS_CONFIRMATION;

  // Layer 4: bootloader-mode identity. `type` is LR11XX_TYPE_PRODUCTION_MODE regardless of chip
  // family, so family comes from the bootloader version itself, not this byte.
  if (bootloader_chip_type != LR1121_BOOTLOADER_TYPE_FOR_FIRMWARE_DECISIONS ||
      !lr1121_bootloader_is_lr1121(bootloader_version))
    return FlashDecision::REJECT_WRONG_CHIP;

  if (device_type == 0)
    return already_confirmed ? FlashDecision::PROCEED : FlashDecision::NEEDS_CONFIRMATION;

  // Layer 3: normal-mode chip identity.
  if (device_type != LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS)
    return FlashDecision::REJECT_WRONG_CHIP;

  const BootloaderSupport support = lr1121_bootloader_supports_target(target_fw, bootloader_version);
  if (support == BootloaderSupport::UNSUPPORTED)
    return FlashDecision::REJECT_BOOTLOADER_TOO_OLD;
  if (support == BootloaderSupport::UNKNOWN_TARGET)
    return already_confirmed ? FlashDecision::PROCEED : FlashDecision::NEEDS_CONFIRMATION;

  // support == SUPPORTED: both versions are known and the pairing is a positive match, so the
  // version numbers are meaningful to compare (unlike the UNKNOWN_TARGET case above) -- provided
  // installed_fw was actually read; if not, that is "unknown", not "older than everything".
  if (installed_fw == 0)
    return already_confirmed ? FlashDecision::PROCEED : FlashDecision::NEEDS_CONFIRMATION;
  if (target_fw == installed_fw)
    return already_confirmed ? FlashDecision::PROCEED : FlashDecision::ALREADY_INSTALLED;
  if (target_fw > installed_fw)
    return FlashDecision::PROCEED;
  return already_confirmed ? FlashDecision::PROCEED : FlashDecision::NEEDS_CONFIRMATION;
}

}  // namespace home_io_control
}  // namespace esphome
