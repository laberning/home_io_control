#include "lr1121_firmware_decisions.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;

namespace {
constexpr uint8_t LR1121 = LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS;
constexpr uint8_t BL_OK = LR1121_BOOTLOADER_TYPE_FOR_FIRMWARE_DECISIONS;  ///< bootloader `type` == 0xDF
}  // namespace

TEST(Lr1121FlashDecision, KnownCompatiblePairNewerThanInstalledProceeds) {
  // 0x0103 on bootloader 0x2100, upgrading from an installed 0x0101.
  EXPECT_EQ(lr1121_flash_decision(LR1121, BL_OK, LR1121_BOOTLOADER_2100, 0x0101, 0x0103, false),
            FlashDecision::PROCEED);
}

// Regression guard: cache_lr1121_flash_verdict_() used to pass the *bootloader-mode* type
// byte (0xDF) where lr1121_flash_decision() expected the *normal-mode* device_type (0x03), so
// every real LR1121 hit REJECT_WRONG_CHIP and the feature was dead on hardware. This is the exact
// byte pattern a real LR1121 produces end to end: bootloader type 0xDF, bootloader 0x2100,
// normal-mode device_type 0x03, installed 0x0101, target (from the test image header) 0x0103.
TEST(Lr1121FlashDecision, RealLr1121BytePatternProceedsWithBootloaderTypeKeptSeparate) {
  EXPECT_EQ(lr1121_flash_decision(/*device_type=*/0x03, /*bootloader_chip_type=*/0xDF,
                                  /*bootloader_version=*/0x2100, /*installed_fw=*/0x0101, /*target_fw=*/0x0103,
                                  /*already_confirmed=*/false),
            FlashDecision::PROCEED);
}

TEST(Lr1121FlashDecision, KnownIncompatiblePairIsRejected) {
  // The case that will actually fire on Lars's board: 0x0104 requires bootloader 0x2101, but the
  // chip only has 0x2100.
  EXPECT_EQ(lr1121_flash_decision(LR1121, BL_OK, LR1121_BOOTLOADER_2100, 0x0101, 0x0104, false),
            FlashDecision::REJECT_BOOTLOADER_TOO_OLD);
}

TEST(Lr1121FlashDecision, KnownCompatiblePairOnNewerBootloaderProceeds) {
  EXPECT_EQ(lr1121_flash_decision(LR1121, BL_OK, LR1121_BOOTLOADER_2101, 0x0101, 0x0104, false),
            FlashDecision::PROCEED);
}

TEST(Lr1121FlashDecision, FutureFirmwareUnknownToTableNeedsConfirmationNotRejection) {
  // Regression guard for the "don't rot when new firmware ships" rule: a
  // hypothetical 0x0105 that this build has never heard of must NOT be treated as incompatible.
  EXPECT_EQ(lr1121_flash_decision(LR1121, BL_OK, LR1121_BOOTLOADER_2100, 0x0101, 0x0105, false),
            FlashDecision::NEEDS_CONFIRMATION);
  // A second press within the confirmation window proceeds anyway.
  EXPECT_EQ(lr1121_flash_decision(LR1121, BL_OK, LR1121_BOOTLOADER_2100, 0x0101, 0x0105, true), FlashDecision::PROCEED);
}

TEST(Lr1121FlashDecision, UnknownTargetVersionNeedsConfirmationAndSkipsNumericCompare) {
  // target_fw == 0 means "no version could be determined" (Step 1 could not parse a filename and
  // none was configured) — must route through confirmation, never be compared against
  // installed_fw as if 0 were a real (and always-older) version number.
  EXPECT_EQ(lr1121_flash_decision(LR1121, BL_OK, LR1121_BOOTLOADER_2100, 0x0103, 0, false),
            FlashDecision::NEEDS_CONFIRMATION);
  // Even when installed_fw is also unknown (0 == 0), it must still ask rather than silently
  // treating an unknown target as "already installed".
  EXPECT_EQ(lr1121_flash_decision(LR1121, BL_OK, LR1121_BOOTLOADER_2100, 0, 0, false),
            FlashDecision::NEEDS_CONFIRMATION);
}

// Regression guard: installed_fw == 0 (radio failed init(), so there is no installed
// version to compare against) used to fall into `target_fw > installed_fw` and PROCEED on a
// single unconfirmed press, treating "unknown" as "older than everything".
TEST(Lr1121FlashDecision, UnknownInstalledVersionNeedsConfirmationNotSilentProceed) {
  EXPECT_EQ(lr1121_flash_decision(LR1121, BL_OK, LR1121_BOOTLOADER_2100, /*installed_fw=*/0, 0x0103, false),
            FlashDecision::NEEDS_CONFIRMATION);
  // A confirmed second press still proceeds -- recovery must remain possible.
  EXPECT_EQ(lr1121_flash_decision(LR1121, BL_OK, LR1121_BOOTLOADER_2100, /*installed_fw=*/0, 0x0103, true),
            FlashDecision::PROCEED);
}

TEST(Lr1121FlashDecision, UnreadBootloaderVersionNeedsConfirmationNotWrongChip) {
  // bootloader_version == 0 means the boot-time excursion never successfully read one (design
  // record §0.3) -- a read that never happened is not evidence of the wrong chip, so this must
  // NOT fall into REJECT_WRONG_CHIP alongside a genuinely-mismatched LR1120/LR1110 bootloader.
  EXPECT_EQ(lr1121_flash_decision(0, 0, 0, 0x0101, 0x0103, false), FlashDecision::NEEDS_CONFIRMATION);
  EXPECT_EQ(lr1121_flash_decision(0, 0, 0, 0x0101, 0x0103, true), FlashDecision::PROCEED);
}

// Regression guard for the layer-3/4 split: device_type == 0 means the *normal-mode* read never
// happened (failed init(), or the read itself failed) even though the boot-time bootloader
// excursion succeeded fine -- must route through confirmation, not REJECT_WRONG_CHIP.
TEST(Lr1121FlashDecision, UnreadDeviceTypeNeedsConfirmationNotWrongChip) {
  EXPECT_EQ(lr1121_flash_decision(/*device_type=*/0, BL_OK, LR1121_BOOTLOADER_2100, 0x0101, 0x0103, false),
            FlashDecision::NEEDS_CONFIRMATION);
  EXPECT_EQ(lr1121_flash_decision(/*device_type=*/0, BL_OK, LR1121_BOOTLOADER_2100, 0x0101, 0x0103, true),
            FlashDecision::PROCEED);
}

TEST(Lr1121FlashDecision, Lr1120BootloaderIsRejectedAsWrongChip) {
  EXPECT_EQ(lr1121_flash_decision(LR1121, BL_OK, 0x2000, 0x0101, 0x0103, false), FlashDecision::REJECT_WRONG_CHIP);
}

TEST(Lr1121FlashDecision, Lr1110BootloaderIsRejectedAsWrongChip) {
  EXPECT_EQ(lr1121_flash_decision(LR1121, BL_OK, 0x6500, 0x0101, 0x0103, false), FlashDecision::REJECT_WRONG_CHIP);
}

// Regression guard for the layer-4 check: the bootloader answered (a nonzero version was read),
// but its `type` byte was not the production-silicon value -- must be rejected even though the
// bootloader *version* alone looks like a genuine LR1121's.
TEST(Lr1121FlashDecision, WrongBootloaderTypeByteIsRejectedAsWrongChip) {
  EXPECT_EQ(lr1121_flash_decision(LR1121, /*bootloader_chip_type=*/0x00, LR1121_BOOTLOADER_2100, 0x0101, 0x0103, false),
            FlashDecision::REJECT_WRONG_CHIP);
}

TEST(Lr1121FlashDecision, WrongDeviceTypeIsRejectedAsWrongChip) {
  EXPECT_EQ(lr1121_flash_decision(0x01 /* LR1110 device type */, BL_OK, LR1121_BOOTLOADER_2100, 0x0101, 0x0103, false),
            FlashDecision::REJECT_WRONG_CHIP);
}

TEST(Lr1121FlashDecision, TargetEqualsInstalledReportsAlreadyInstalledNotAWarning) {
  // The state a user lands in after a *successful* flash: target now equals installed on every
  // subsequent boot. Must not read as a "not newer" warning.
  EXPECT_EQ(lr1121_flash_decision(LR1121, BL_OK, LR1121_BOOTLOADER_2100, 0x0103, 0x0103, false),
            FlashDecision::ALREADY_INSTALLED);
  // Still a legitimate recovery action on a confirmed second press.
  EXPECT_EQ(lr1121_flash_decision(LR1121, BL_OK, LR1121_BOOTLOADER_2100, 0x0103, 0x0103, true), FlashDecision::PROCEED);
}

TEST(Lr1121FlashDecision, TargetOlderThanInstalledNeedsConfirmation) {
  EXPECT_EQ(lr1121_flash_decision(LR1121, BL_OK, LR1121_BOOTLOADER_2100, 0x0103, 0x0102, false),
            FlashDecision::NEEDS_CONFIRMATION);
  EXPECT_EQ(lr1121_flash_decision(LR1121, BL_OK, LR1121_BOOTLOADER_2100, 0x0103, 0x0102, true), FlashDecision::PROCEED);
}

TEST(Lr1121BootloaderSupportsTarget, KnownPairsResolveCorrectly) {
  EXPECT_EQ(lr1121_bootloader_supports_target(0x0103, LR1121_BOOTLOADER_2100), BootloaderSupport::SUPPORTED);
  EXPECT_EQ(lr1121_bootloader_supports_target(0x0104, LR1121_BOOTLOADER_2100), BootloaderSupport::UNSUPPORTED);
  EXPECT_EQ(lr1121_bootloader_supports_target(0x0104, LR1121_BOOTLOADER_2101), BootloaderSupport::SUPPORTED);
  EXPECT_EQ(lr1121_bootloader_supports_target(0x0999, LR1121_BOOTLOADER_2100), BootloaderSupport::UNKNOWN_TARGET);
}

TEST(Lr1121BootloaderIsLr1121, AcceptsOnlyTheTwoKnownLr1121BootloaderVersions) {
  EXPECT_TRUE(lr1121_bootloader_is_lr1121(LR1121_BOOTLOADER_2100));
  EXPECT_TRUE(lr1121_bootloader_is_lr1121(LR1121_BOOTLOADER_2101));
  EXPECT_FALSE(lr1121_bootloader_is_lr1121(0x2000));
  EXPECT_FALSE(lr1121_bootloader_is_lr1121(0x6500));
}

// DRY: the LR1120/LR1110 bootloader-ID-to-family mapping used to be hardcoded literals in
// hub wiring; it now lives here as a named, host-testable pure function next to the LR1121
// bootloader constants it is compared against.
TEST(Lr1121ChipFamilyForBootloader, MapsKnownBootloaderVersionsToTheirChipFamily) {
  EXPECT_STREQ(lr1121_chip_family_for_bootloader(LR1120_BOOTLOADER_2000), "an LR1120");
  EXPECT_STREQ(lr1121_chip_family_for_bootloader(LR1120_BOOTLOADER_2001), "an LR1120");
  EXPECT_STREQ(lr1121_chip_family_for_bootloader(LR1110_BOOTLOADER_6500), "an LR1110");
  EXPECT_STREQ(lr1121_chip_family_for_bootloader(LR1110_BOOTLOADER_1001), "an LR1110");
  EXPECT_STREQ(lr1121_chip_family_for_bootloader(0x9999), "an unrecognized chip");
}

TEST(Lr1121ChipFamilyForDeviceType, MapsKnownDeviceTypesToTheirChipFamily) {
  EXPECT_STREQ(lr1121_chip_family_for_device_type(LR1110_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS), "an LR1110");
  EXPECT_STREQ(lr1121_chip_family_for_device_type(LR1120_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS), "an LR1120");
  EXPECT_STREQ(lr1121_chip_family_for_device_type(0x99), "an unrecognized chip");
}

// ============================================================================
// Bootloader-update post-filter (ADR 0021)
// ============================================================================

TEST(Lr1121RequiredBootloaderFor, KnownTargetsResolveTheirRequirementUnknownTargetsResolveZero) {
  EXPECT_EQ(lr1121_required_bootloader_for(0x0101), LR1121_BOOTLOADER_2100);
  EXPECT_EQ(lr1121_required_bootloader_for(0x0103), LR1121_BOOTLOADER_2100);
  EXPECT_EQ(lr1121_required_bootloader_for(0x0104), LR1121_BOOTLOADER_2101);
  EXPECT_EQ(lr1121_required_bootloader_for(0x0999), 0);
}

TEST(Lr1121BootloaderMismatchKind, UnknownTargetIsNone) {
  EXPECT_EQ(lr1121_bootloader_mismatch_kind(0x0999, LR1121_BOOTLOADER_2100), BootloaderMismatch::NONE);
}

TEST(Lr1121BootloaderMismatchKind, MatchingPairIsNone) {
  EXPECT_EQ(lr1121_bootloader_mismatch_kind(0x0103, LR1121_BOOTLOADER_2100), BootloaderMismatch::NONE);
  EXPECT_EQ(lr1121_bootloader_mismatch_kind(0x0104, LR1121_BOOTLOADER_2101), BootloaderMismatch::NONE);
}

TEST(Lr1121BootloaderMismatchKind, TargetNeedsNewerBootloaderIsTheTooOldDirection) {
  // 0x0104 requires 0x2101; the chip only has 0x2100 -- the direction that has always been
  // reachable, and what REJECT_BOOTLOADER_TOO_OLD's message has always described.
  EXPECT_EQ(lr1121_bootloader_mismatch_kind(0x0104, LR1121_BOOTLOADER_2100), BootloaderMismatch::TARGET_NEEDS_NEWER);
}

TEST(Lr1121BootloaderMismatchKind, TargetNeedsOlderBootloaderIsTheNewlyReachableDowngradeDirection) {
  // 0x0103 requires 0x2100; a chip that has been upgraded to 0x2101 hits the other direction,
  // unreachable before the bootloader-update feature existed.
  EXPECT_EQ(lr1121_bootloader_mismatch_kind(0x0103, LR1121_BOOTLOADER_2101), BootloaderMismatch::TARGET_NEEDS_OLDER);
}

namespace {
// Convenience alias matching lr1121_bootloader_upgrade_path()'s parameter order.
constexpr BootloaderUpgradePath upgrade_path(bool block_present, bool bl_known, uint16_t bl, uint16_t loader_fw,
                                             uint16_t target_fw) {
  return lr1121_bootloader_upgrade_path(block_present, bl_known, bl, loader_fw, target_fw);
}
}  // namespace

TEST(Lr1121BootloaderUpgradePath, Row1NoBlockIsNotApplicable) {
  EXPECT_EQ(upgrade_path(false, true, LR1121_BOOTLOADER_2100, LR1121_LOADER_2100, 0x0104),
            BootloaderUpgradePath::NOT_APPLICABLE);
}

TEST(Lr1121BootloaderUpgradePath, Row2UnknownBootloaderVersionIsNotApplicable) {
  // Regression guard: this path must NEVER adopt a fresh reading the way the ordinary transceiver
  // flash sequence does -- an unknown current bootloader can never justify an irreversible write.
  EXPECT_EQ(upgrade_path(true, false, 0, LR1121_LOADER_2100, 0x0104), BootloaderUpgradePath::NOT_APPLICABLE);
}

TEST(Lr1121BootloaderUpgradePath, Row3WrongChipBootloaderVersionIsNotApplicable) {
  // 0x2000 is an LR1120 bootloader version -- REJECT_WRONG_CHIP owns this, not this function.
  EXPECT_EQ(upgrade_path(true, true, 0x2000, 0x2000, 0x0104), BootloaderUpgradePath::NOT_APPLICABLE);
}

TEST(Lr1121BootloaderUpgradePath, Row4LoaderVersionMismatchIsNotApplicable) {
  // Semtech's equality rule: the loader image's version must equal the running bootloader.
  EXPECT_EQ(upgrade_path(true, true, LR1121_BOOTLOADER_2100, /*loader_fw=*/0x2101, 0x0104),
            BootloaderUpgradePath::NOT_APPLICABLE);
}

TEST(Lr1121BootloaderUpgradePath, Row4ChipAlreadyOnNewBootloaderIsNotApplicable) {
  // A chip already on 0x2101 can never equal the 0x2100 loader -- this is also how "already
  // upgraded, block is inert" (R5/R6) is reached.
  EXPECT_EQ(upgrade_path(true, true, LR1121_BOOTLOADER_2101, LR1121_LOADER_2100, 0x0104),
            BootloaderUpgradePath::NOT_APPLICABLE);
}

TEST(Lr1121BootloaderUpgradePath, Row5UnknownTargetIsBlocked) {
  EXPECT_EQ(upgrade_path(true, true, LR1121_BOOTLOADER_2100, LR1121_LOADER_2100, 0x0999),
            BootloaderUpgradePath::BLOCKED_UNKNOWN_TARGET);
}

TEST(Lr1121BootloaderUpgradePath, Row6NoUpgradeNeededIsNotApplicable) {
  EXPECT_EQ(upgrade_path(true, true, LR1121_BOOTLOADER_2100, LR1121_LOADER_2100, 0x0103),
            BootloaderUpgradePath::NOT_APPLICABLE);
}

TEST(Lr1121BootloaderUpgradePath, Row7TheOnePathThatProceedsIsAvailable) {
  EXPECT_EQ(upgrade_path(true, true, LR1121_BOOTLOADER_2100, LR1121_LOADER_2100, 0x0104),
            BootloaderUpgradePath::AVAILABLE);
}

TEST(Lr1121BootloaderUpgradePath, Row8DowngradeIsBlockedAsBootloaderNewer) {
  // Row 4's loader-equality gate means this can only be reached with a *different* loader image
  // than LR1121_LOADER_2100 (a hypothetical future one) whose version equals the chip's -- the
  // downgrade-detection branch itself (required < bootloader_version) must still fire correctly.
  EXPECT_EQ(upgrade_path(true, true, LR1121_BOOTLOADER_2101, /*loader_fw=*/LR1121_BOOTLOADER_2101, 0x0103),
            BootloaderUpgradePath::BLOCKED_BOOTLOADER_NEWER);
}
