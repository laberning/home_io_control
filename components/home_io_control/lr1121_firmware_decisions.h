#pragma once

/// @file lr1121_firmware_decisions.h
/// @brief Pure decision logic for the LR1121 transceiver-firmware-update feature.
/// @ingroup hioc_radio
///
/// Header-only pure functions, no I/O — same style as hub_decisions.h and
/// radio_lr1121.h::lr1121_firmware_is_outdated(), fully host-testable without radio hardware.
/// Deliberately does not include radio_lr1121.h: LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS below
/// is a local copy of that file's LR1121_DEVICE_TYPE (0x03), kept independent so this header pulls
/// in nothing but cstdint — the same reasoning that keeps hub_decisions.h free of driver headers.

#include <cstdint>

namespace esphome {
namespace home_io_control {

/// @brief LR1121 GetVersion `type` byte in normal mode — must match radio_lr1121.h's
/// LR1121_DEVICE_TYPE (0x03). Kept as a separate constant, not a shared header include, so this
/// header stays dependency-free; see the file header for why. A static_assert in
/// tests/radio_lr1121_firmware_updater_test.cpp (a translation unit that already includes both
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
/// Order of checks, per the design record's five-layer scheme (§6):
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
///   - `bootloader_version == 0`: the boot-time excursion (design record §0.3) never successfully
///     read one. 0x0000 is not a value any real LR11xx bootloader reports.
///   - `device_type == 0`: the normal-mode read never happened at all (failed init(), or the read
///     itself failed) -- see design record's Step 6b(c) recovery-path allowance.
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
/// @param bootloader_version Bootloader version read at boot (design record §0.3); one of
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
