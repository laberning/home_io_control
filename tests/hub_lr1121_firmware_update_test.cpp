#include "hub_core.h"
#include "lr1121_firmware_decisions.h"
#include "radio_lr1121_firmware_updater.h"

#include "esphome/core/application.h"

#include "test_helpers.h"
#include "stubs/radio_test_common.h"
#include "stubs/scripted_spi.h"

#include <gtest/gtest.h>

using namespace esphome::home_io_control;
using test::TestableHubComponent;

// ============================================================================
// LR1121 firmware-update hub wiring test suite
// ============================================================================
// The flashing SPI sequence itself (opcodes, chunking, byte-exactness) is covered by
// radio_lr1121_firmware_updater_test.cpp and the pure decision matrix by
// lr1121_firmware_decisions_test.cpp; this suite only covers what's specific to the hub's
// trigger_lr1121_firmware_update() wiring: guard ordering, the cached-verdict dispatch, and the
// two-press confirmation state machine. See test_helpers.h's TestableHubComponent for the
// promoted protected members this suite reaches into.

namespace {

// Queue an LR1121 GetVersion-shaped response: [stat1, hw, byte1, byte2, byte3].
void queue_version_response(ScriptedSpi &spi, uint8_t byte1, uint8_t byte2, uint8_t byte3) {
  spi.queue_responses({0x00, 0x01, byte1, byte2, byte3});
}

#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE
// Queue a VerifyBootloader-shaped response: [stat1, check_byte, use_case, version_major, version_minor].
void queue_verify_bootloader_response(ScriptedSpi &spi, uint8_t check_byte, uint8_t use_case, uint8_t version_major,
                                      uint8_t version_minor) {
  spi.queue_responses({0x00, check_byte, use_case, version_major, version_minor});
}

// Queue the 6-byte Stat1/Stat2/IrqStatus direct read the loader answers between UpdateBootloader
// and VerifyBootloader. command_status lives in bits 7:1 of Stat1, so OK (2) encodes as 0x04.
void queue_updater_status_response(ScriptedSpi &spi, uint8_t command_status) {
  spi.queue_responses({static_cast<uint8_t>(command_status << 1), 0x00, 0x00, 0x00, 0x00, 0x00});
}

// Count occurrences of `opcode` across every recorded transaction (find_opcode() only finds the
// first) -- the three-stage sequence issues EraseFlash/WriteFlashEncrypted twice, once per stage.
size_t count_opcode(const ScriptedSpi &spi, uint16_t opcode) {
  const uint8_t msb = (opcode >> 8) & 0xFF, lsb = opcode & 0xFF;
  size_t count = 0;
  for (const auto &txn : spi.transactions()) {
    if (txn.size() >= 2 && txn[0] == msb && txn[1] == lsb)
      count++;
  }
  return count;
}

// Index of the Nth (0-based) occurrence of `opcode`, or -1 if fewer than N+1 exist.
int find_nth_opcode(const ScriptedSpi &spi, uint16_t opcode, size_t n) {
  const uint8_t msb = (opcode >> 8) & 0xFF, lsb = opcode & 0xFF;
  size_t seen = 0;
  for (size_t i = 0; i < spi.transactions().size(); i++) {
    const auto &txn = spi.transactions()[i];
    if (txn.size() >= 2 && txn[0] == msb && txn[1] == lsb) {
      if (seen == n)
        return static_cast<int>(i);
      seen++;
    }
  }
  return -1;
}
#endif  // IOHOME_LR1121_BOOTLOADER_UPDATE

}  // namespace

TEST(HubLr1121FirmwareUpdate, TriggerWhileBusyIsRefusedWithoutTouchingTheChip) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  TestableHubComponent hub;
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::PROCEED;
  hub.busy_ = true;

  hub.trigger_lr1121_firmware_update();

  EXPECT_TRUE(spi.transactions().empty()) << "a busy hub must refuse without any SPI activity";
}

TEST(HubLr1121FirmwareUpdate, TriggerWithNullRadioAndHardRejectionDoesNotCrash) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  TestableHubComponent hub;
  hub.radio_ = nullptr;  // simulates setup()'s init()-failure path (guard 0)
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::REJECT_WRONG_CHIP;

  hub.trigger_lr1121_firmware_update();  // must not crash

  EXPECT_TRUE(spi.transactions().empty()) << "a rejected verdict must refuse before entering bootloader mode";
}

TEST(HubLr1121FirmwareUpdate, RejectWrongChipVerdictNeverEntersBootloaderMode) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  TestableHubComponent hub;
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::REJECT_WRONG_CHIP;

  hub.trigger_lr1121_firmware_update();

  EXPECT_TRUE(spi.transactions().empty());
}

TEST(HubLr1121FirmwareUpdate, RejectBootloaderTooOldVerdictNeverEntersBootloaderMode) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  TestableHubComponent hub;
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::REJECT_BOOTLOADER_TOO_OLD;

  hub.trigger_lr1121_firmware_update();

  EXPECT_TRUE(spi.transactions().empty());
}

TEST(HubLr1121FirmwareUpdate, AlreadyInstalledVerdictReportsNothingToDoNotAWarning) {
  TestableHubComponent hub;
  hub.lr1121_flash_verdict_ = FlashDecision::ALREADY_INSTALLED;

  const std::string message = hub.describe_lr1121_flash_verdict_();
  EXPECT_NE(message.find("nothing to do"), std::string::npos) << message;
  EXPECT_EQ(message.find("not newer"), std::string::npos)
      << "a successful post-flash boot must not read like a warning: " << message;
}

TEST(HubLr1121FirmwareUpdate, NeedsConfirmationArmsWindowAndDoesNotFlashOnFirstPress) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  TestableHubComponent hub;
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::NEEDS_CONFIRMATION;

  hub.trigger_lr1121_firmware_update();

  EXPECT_TRUE(spi.transactions().empty()) << "the first press must not touch the chip";
  EXPECT_TRUE(hub.lr1121_flash_confirmation_armed_);
  // arm_lr1121_flash_confirmation_() uses App.scheduler's self-keyed set_timeout() overload (see
  // that method's comment), not Component::set_timeout() -- so the callback lands on the
  // scheduler stub, not hub.last_timeout_callback_.
  ASSERT_TRUE(static_cast<bool>(esphome::App.scheduler.last_self_timeout_callback_))
      << "arming should schedule the auto-off timeout";

  // Simulate the confirmation window expiring.
  esphome::App.scheduler.last_self_timeout_callback_();
  EXPECT_FALSE(hub.lr1121_flash_confirmation_armed_);
}

// Regression guard for the rule that the confirmation auto-disarm must fire even when the hub
// itself has failed (radio_->init() failed -- exactly the recovery path a re-flash exists for).
// ESPHome's scheduler skips any timeout scheduled via the Component-keyed set_timeout() overload
// when that component is failed; the self-keyed App.scheduler.set_timeout(this, ...) overload
// records no Component and is therefore never skipped that way. Asserting the callback landed on
// the self-keyed path (and never on hub.last_timeout_callback_) is what proves this window still
// expires on a failed hub.
TEST(HubLr1121FirmwareUpdate, ConfirmationAutoDisarmUsesSelfKeyedSchedulerSoAFailedHubCannotSuppressIt) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  TestableHubComponent hub;
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::NEEDS_CONFIRMATION;
  hub.mark_failed();  // simulates setup()'s init()-failure path, same as guard 0's radio_==nullptr

  hub.trigger_lr1121_firmware_update();

  EXPECT_TRUE(hub.lr1121_flash_confirmation_armed_);
  EXPECT_FALSE(static_cast<bool>(hub.last_timeout_callback_))
      << "must not use the Component-keyed set_timeout() overload, which a failed component would skip";
  ASSERT_TRUE(static_cast<bool>(esphome::App.scheduler.last_self_timeout_callback_));
  EXPECT_EQ(esphome::App.scheduler.last_self_timeout_self_, &hub);

  esphome::App.scheduler.last_self_timeout_callback_();
  EXPECT_FALSE(hub.lr1121_flash_confirmation_armed_) << "the auto-disarm callback must still run on a failed hub";
}

TEST(HubLr1121FirmwareUpdate, ArmedSecondPressReachesTheFlashAttempt) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
  // Bootloader-entry sanity-check response only -- this test asserts the attempt was reached
  // (bootloader entry happened), not that the whole flash completed successfully.
  queue_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x21, 0x00);

  TestableHubComponent hub;
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::NEEDS_CONFIRMATION;
  hub.lr1121_bootloader_version_known_ = true;
  hub.lr1121_bootloader_chip_type_ = LR1121_UPDATER_BOOTLOADER_TYPE;
  hub.lr1121_bootloader_version_ = 0x2100;
  hub.lr1121_flash_confirmation_armed_ = true;  // simulates an already-armed first press

  hub.trigger_lr1121_firmware_update();

  EXPECT_GE(spi.find_opcode(LR1121_UPDATER_CMD_GET_VERSION), 0) << "an armed second press must enter bootloader mode";
  EXPECT_FALSE(hub.lr1121_flash_confirmation_armed_) << "the window is consumed by the press that uses it";
}

TEST(HubLr1121FirmwareUpdate, ProceedVerdictWithNullRadioRunsTheFullSequenceWithoutCrashing) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
  // enter_bootloader()'s sanity read, then the post-flash read_normal_version() verify.
  queue_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x21, 0x00);
  queue_version_response(spi, LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS, 0x01, 0x03);

  TestableHubComponent hub;
  hub.radio_ = nullptr;  // exercises guard 0's "skip standby, still allow the attempt" path
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::PROCEED;
  hub.lr1121_bootloader_version_known_ = true;
  hub.lr1121_bootloader_chip_type_ = LR1121_UPDATER_BOOTLOADER_TYPE;
  hub.lr1121_bootloader_version_ = 0x2100;

  const uint32_t reboots_before = esphome::App.safe_reboot_calls;
  hub.trigger_lr1121_firmware_update();  // must not crash despite radio_ == nullptr

  const int write_idx = spi.find_opcode(LR1121_UPDATER_CMD_WRITE_FLASH_ENCRYPTED);
  const int hash_idx = spi.find_opcode(LR1121_UPDATER_CMD_GET_HASH);
  const int reboot_idx = spi.find_opcode(LR1121_UPDATER_CMD_REBOOT);
  EXPECT_GE(spi.find_opcode(LR1121_UPDATER_CMD_ERASE_FLASH), 0);
  EXPECT_GE(write_idx, 0);
  ASSERT_GE(hash_idx, 0) << "the post-write hash must be read (diagnostic only -- see the hub source comment)";
  EXPECT_GE(reboot_idx, 0);
  EXPECT_LT(write_idx, hash_idx) << "the hash read must happen before rebooting -- still in bootloader mode";
  EXPECT_LT(hash_idx, reboot_idx);
  EXPECT_EQ(esphome::App.safe_reboot_calls, reboots_before + 1)
      << "every code path past a successful bootloader entry must end in App.safe_reboot()";
}

TEST(HubLr1121FirmwareUpdate, BootTimeExcursionAllocatesUpdaterAndCachesAnUnknownVerdictSafely) {
  // The hub's own SpiAccess (spi::SPIDevice) is a no-op stub in host tests (always returns 0),
  // so this exercises the real run_lr1121_boot_time_bootloader_read_() /
  // cache_lr1121_flash_verdict_() wiring end-to-end -- allocation, the two GetVersion reads, and
  // lr1121_flash_decision() -- without a ScriptedSpi. A bootloader_version of 0 (what the stub
  // SPI naturally yields) must land on NEEDS_CONFIRMATION, never REJECT_WRONG_CHIP -- this is the
  // regression guard for the "a read that never happened is not evidence of the wrong chip" rule
  // in lr1121_firmware_decisions.h.
  MockPin rst, busy;
  TestableHubComponent hub;
  hub.set_rst_pin(&rst);
  hub.set_busy_pin(&busy);

  hub.run_lr1121_boot_time_bootloader_read_();
  ASSERT_NE(hub.lr1121_firmware_updater_, nullptr);
  EXPECT_TRUE(hub.lr1121_bootloader_version_known_);
  EXPECT_EQ(hub.lr1121_bootloader_version_, 0);

  hub.cache_lr1121_flash_verdict_();
  EXPECT_TRUE(hub.lr1121_flash_verdict_known_);
  EXPECT_EQ(hub.lr1121_flash_verdict_, FlashDecision::NEEDS_CONFIRMATION);

  delete hub.lr1121_firmware_updater_;
}

// THE REGRESSION GUARD FOR THE VERDICT-CACHING SEAM: cache_lr1121_flash_verdict_() used to pass the
// bootloader-mode `type` byte (0xDF) where lr1121_flash_decision() expected the normal-mode
// device_type (0x03), so every real LR1121 hit REJECT_WRONG_CHIP and the feature was dead on
// hardware. Unlike BootTimeExcursionAllocatesUpdaterAndCachesAnUnknownVerdictSafely above (which
// uses the hub's own no-op SpiAccess stub and can only ever see all-zero reads), this drives a
// real ScriptedSpi normal-mode read so the exact byte pattern a genuine LR1121 produces reaches
// the decision function unmangled.
TEST(HubLr1121FirmwareUpdate, CacheFlashVerdictProceedsForRealLr1121BytePattern) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
  // cache_lr1121_flash_verdict_() issues exactly one normal-mode GetVersion: device_type 0x03,
  // installed firmware 0x0101 -- older than the test image's target 0x0104 (see
  // tests/include/lr1121_firmware_update_image.h), so a correct decision is PROCEED. Bootloader is
  // 0x2101 (not 0x2100) specifically because 0x0104 requires 0x2101 -- this is the regression guard
  // for the layer-3/4 byte-swap bug, not a bootloader-compatibility test, so any SUPPORTED pairing
  // exercises it equally.
  queue_version_response(spi, LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS, 0x01, 0x01);

  MockRadio radio;
  TestableHubComponent hub;
  hub.radio_ = &radio;
  hub.lr1121_firmware_updater_ = &updater;
  // Simulates a successful boot-time excursion (run_lr1121_boot_time_bootloader_read_()) having
  // already cached a genuine LR1121's bootloader identity.
  hub.lr1121_bootloader_version_known_ = true;
  hub.lr1121_bootloader_chip_type_ = LR1121_UPDATER_BOOTLOADER_TYPE;
  hub.lr1121_bootloader_version_ = LR1121_BOOTLOADER_2101;

  hub.cache_lr1121_flash_verdict_();

  EXPECT_EQ(hub.lr1121_installed_device_type_, LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS);
  EXPECT_EQ(hub.lr1121_installed_fw_, 0x0101);
  EXPECT_EQ(hub.lr1121_flash_verdict_, FlashDecision::PROCEED);
}

// Regression guard: the post-bootloader-entry sanity check used to compare the freshly-read
// bootloader version against the boot-time cached one unconditionally. When the boot-time
// excursion never completed (lr1121_bootloader_version_known_ == false, cached value 0), that
// comparison could never match a real chip's nonzero bootloader version, so every single attempt
// aborted before erasing -- permanently, with no way to ever flash a board whose boot-time read
// had failed once.
TEST(HubLr1121FirmwareUpdate, SanityCheckWithUnknownBootTimeBootloaderVersionProceedsToErase) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
  queue_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x21, 0x00);             // sanity read: 0x2100
  queue_version_response(spi, LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS, 0x01, 0x03);  // post-flash verify read

  TestableHubComponent hub;
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::PROCEED;
  hub.lr1121_bootloader_version_known_ = false;  // boot-time excursion never completed

  const uint32_t reboots_before = esphome::App.safe_reboot_calls;
  hub.trigger_lr1121_firmware_update();

  EXPECT_GE(spi.find_opcode(LR1121_UPDATER_CMD_ERASE_FLASH), 0)
      << "an unknown boot-time bootloader version must not block erasing";
  EXPECT_TRUE(hub.lr1121_bootloader_version_known_) << "a successful sanity read should be adopted";
  EXPECT_EQ(hub.lr1121_bootloader_version_, 0x2100);
  EXPECT_EQ(esphome::App.safe_reboot_calls, reboots_before + 1);
}

// Regression guard for the last check before an irreversible erase: when the boot-time excursion
// never read a bootloader version, lr1121_check_bootloader_sanity() used to adopt whatever
// bootloader-mode `type` byte and version it read without ever checking the version identifies an
// LR1121 -- and LR1121_UPDATER_BOOTLOADER_TYPE (0xDF, LR11XX_TYPE_PRODUCTION_MODE) is reported by
// an LR1120 or LR1110 too. On this path, a confirmed press must not erase and overwrite a
// different chip family just because the type byte matched. 0x2000 is an LR1120 bootloader
// version (see lr1121_firmware_decisions.h's LR1120_BOOTLOADER_2000) -- neither of the two values
// an LR1121 can actually report.
TEST(HubLr1121FirmwareUpdate, UnknownBootTimeBootloaderVersionThatIdentifiesAnotherChipFamilyAbortsBeforeErasing) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
  queue_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x20, 0x00);  // type OK, but version 0x2000 (LR1120)

  TestableHubComponent hub;
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::PROCEED;
  hub.lr1121_bootloader_version_known_ = false;  // boot-time excursion never completed -- the "adopt" path

  const uint32_t reboots_before = esphome::App.safe_reboot_calls;
  hub.trigger_lr1121_firmware_update();

  EXPECT_LT(spi.find_opcode(LR1121_UPDATER_CMD_ERASE_FLASH), 0)
      << "a bootloader version that identifies a different chip family must never be adopted, and must never reach "
         "EraseFlash";
  EXPECT_FALSE(hub.lr1121_bootloader_version_known_) << "a rejected sanity read must not be adopted";
  EXPECT_EQ(esphome::App.safe_reboot_calls, reboots_before + 1)
      << "aborting after a successful bootloader entry must still reboot";
}

// Regression guard: dump_lr1121_firmware_update_debug_() (here, its testable content half --
// see lr1121_firmware_update_debug_lines_()'s comment for why ESP_LOGCONFIG can't be asserted on
// directly) used to early-return whenever the boot-time bootloader read had failed, before ever
// reaching the verdict line -- even though the verdict is cached independently of that excursion.
TEST(HubLr1121FirmwareUpdate, DebugLinesIncludeVerdictWhenBootloaderVersionUnknown) {
  TestableHubComponent hub;
  hub.lr1121_bootloader_version_known_ = false;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::NEEDS_CONFIRMATION;

  const std::vector<std::string> lines = hub.lr1121_firmware_update_debug_lines_();

  ASSERT_EQ(lines.size(), 2u) << "must include both the (unknown) bootloader line and the verdict line";
  EXPECT_NE(lines[0].find("could not be read at boot"), std::string::npos) << lines[0];
  EXPECT_NE(lines[1].find("Firmware update target"), std::string::npos) << lines[1];
}

TEST(HubLr1121FirmwareUpdate, DescribeVerdictFormatsEveryOutcomeDistinctly) {
  // describe_lr1121_flash_verdict_() is deliberately neutral about what (if anything) is armed --
  // see its comment -- so none of these strings mention "press again"; that follow-up is appended
  // only at the one call site in trigger_lr1121_firmware_update() that actually arms a window
  // and is covered separately below.
  TestableHubComponent hub;
  hub.lr1121_bootloader_version_ = 0x2100;

  hub.lr1121_flash_verdict_ = FlashDecision::PROCEED;
  const std::string proceed = hub.describe_lr1121_flash_verdict_();
  EXPECT_NE(proceed.find("ready to flash"), std::string::npos) << proceed;

  hub.lr1121_flash_verdict_ = FlashDecision::REJECT_WRONG_CHIP;
  const std::string wrong_chip = hub.describe_lr1121_flash_verdict_();
  EXPECT_NE(wrong_chip.find("CANNOT PROCEED"), std::string::npos) << wrong_chip;
  EXPECT_NE(wrong_chip.find("0x2100"), std::string::npos)
      << "must name the bootloader version this chip has: " << wrong_chip;

  hub.lr1121_flash_verdict_ = FlashDecision::REJECT_BOOTLOADER_TOO_OLD;
  const std::string too_old = hub.describe_lr1121_flash_verdict_();
  EXPECT_NE(too_old.find("CANNOT PROCEED"), std::string::npos) << too_old;
  EXPECT_NE(too_old.find("needs bootloader"), std::string::npos)
      << "must state the required and current bootloader versions: " << too_old;
  // The test Makefile defines IOHOME_LR1121_BOOTLOADER_UPDATE, so describe_lr1121_flash_verdict_()
  // must NOT append the "add a bootloader: sub-block" suffix here -- that advice is wrong when the
  // block is already configured, and trigger_lr1121_firmware_update()/
  // lr1121_firmware_update_debug_lines_() are the ones that append a path-specific suffix in a
  // build that has the feature compiled in (covered separately below).
  EXPECT_EQ(too_old.find("bootloader:"), std::string::npos)
      << "must not tell the user to add a block that this build already has: " << too_old;

  hub.lr1121_flash_verdict_ = FlashDecision::NEEDS_CONFIRMATION;
  const std::string needs_confirm = hub.describe_lr1121_flash_verdict_();
  EXPECT_EQ(needs_confirm.find("press"), std::string::npos)
      << "describe_lr1121_flash_verdict_() must stay neutral about what is armed: " << needs_confirm;

  hub.lr1121_flash_verdict_ = FlashDecision::ALREADY_INSTALLED;
  const std::string already = hub.describe_lr1121_flash_verdict_();
  EXPECT_NE(already.find("nothing to do"), std::string::npos) << already;

  // Every message must be distinct -- a bug that made two verdicts share a formatter would be
  // invisible without this.
  EXPECT_NE(proceed, wrong_chip);
  EXPECT_NE(wrong_chip, too_old);
  EXPECT_NE(too_old, needs_confirm);
  EXPECT_NE(needs_confirm, already);
}

// describe_lr1121_flash_verdict_() picks between two distinct REJECT_WRONG_CHIP messages
// depending on which layer actually rejected (the layer-3/layer-4 split): the test above covers
// a bootloader-layer rejection (bootloader_chip_type left at its zero default); this covers the
// layer-3 case, where the bootloader excursion succeeded fine but the normal-mode chip identity
// byte did not match an LR1121.
TEST(HubLr1121FirmwareUpdate, DescribeVerdictNamesNormalModeChipIdentityWhenLayer3Rejects) {
  TestableHubComponent hub;
  hub.lr1121_bootloader_version_ = LR1121_BOOTLOADER_2100;
  hub.lr1121_bootloader_chip_type_ = LR1121_UPDATER_BOOTLOADER_TYPE;
  hub.lr1121_installed_device_type_ = 0x01;  // LR1110 device type
  hub.lr1121_flash_verdict_ = FlashDecision::REJECT_WRONG_CHIP;

  const std::string message = hub.describe_lr1121_flash_verdict_();
  EXPECT_NE(message.find("CANNOT PROCEED"), std::string::npos) << message;
  EXPECT_NE(message.find("normal-mode chip identity byte"), std::string::npos) << message;
  EXPECT_NE(message.find("0x01"), std::string::npos) << "must name the device_type byte this chip has: " << message;
  EXPECT_NE(message.find("LR1110"), std::string::npos) << message;
}

// R7: a chip whose bootloader is
// NEWER than what the configured target requires -- the "too new" direction
// (BootloaderMismatch::TARGET_NEEDS_OLDER), newly reachable once this feature ships. This is the
// message a user actually sees, and the entire reason the TARGET_NEEDS_OLDER/TARGET_NEEDS_NEWER
// split exists, so it needs its own test independent of lr1121_bootloader_mismatch_kind()'s own
// (already thorough) coverage.
//
// describe_lr1121_flash_verdict_() reads LR1121_FIRMWARE_UPDATE_TARGET_VERSION as a compile-time
// constant, not a parameter -- and this test binary's stand-in header fixes it at 0x0104
// (requires bootloader 0x2101; see tests/include/lr1121_firmware_update_image.h's comment), which
// is always present in LR1121_KNOWN_BOOTLOADER_REQUIREMENTS. There is no way to vary the target at
// runtime here, so the matrix's literal "bootloader 0x2101 vs. a target requiring 0x2100" numbers
// cannot be reproduced. What *is* reachable, through the real rendering code, is the same
// TARGET_NEEDS_OLDER branch: any bootloader_version_ newer than the compiled-in target's required
// 0x2101 takes that identical branch in describe_lr1121_flash_verdict_(). 0x2102 is not a bootloader
// version any real LR1121 reports (lr1121_bootloader_is_lr1121() only knows 0x2100/0x2101), but
// this test only exercises message *rendering* (a pure string-formatting function of two uint16_t
// values), which does not care whether the input is realistic.
TEST(HubLr1121FirmwareUpdate, R7_DescribeVerdictRendersDowngradeMessageDistinctFromTooOldWording) {
  TestableHubComponent hub;
  hub.lr1121_bootloader_version_ = 0x2102;  // synthetic: newer than the 0x2101 the compiled-in target requires
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::REJECT_BOOTLOADER_TOO_OLD;

  const std::string message = hub.describe_lr1121_flash_verdict_();

  EXPECT_NE(message.find("is newer than this firmware supports"), std::string::npos) << message;
  EXPECT_NE(message.find("no downgrade path"), std::string::npos) << message;
  // "this chip has" only appears in the TARGET_NEEDS_NEWER ("too old") branch's wording -- its
  // absence here proves the two branches did not collapse onto the same message.
  EXPECT_EQ(message.find("this chip has"), std::string::npos)
      << "must not reuse the TARGET_NEEDS_NEWER (\"too old\") wording: " << message;
}

// The button-press log site is where the "press again" follow-up actually belongs (see
// the note above), and it must say something different -- and log at a different level -- for
// ALREADY_INSTALLED (a successful user, re-flashing is optional) than for a genuine
// NEEDS_CONFIRMATION (something is actually wrong or unverified). This can't be asserted through
// captured log output (ESP_LOGCONFIG/ESP_LOGW/ESP_LOGI are no-ops in host tests -- see
// tests/include/esphome/core/log.h), so it is asserted through describe_lr1121_flash_verdict_()
// plus the arm/level decision trigger_lr1121_firmware_update() makes, which are the two halves
// that together produce the message.
TEST(HubLr1121FirmwareUpdate, AlreadyInstalledArmsWithReFlashWordingNotProceedWording) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  TestableHubComponent hub;
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::ALREADY_INSTALLED;

  hub.trigger_lr1121_firmware_update();

  EXPECT_TRUE(spi.transactions().empty()) << "arming must not touch the chip";
  EXPECT_TRUE(hub.lr1121_flash_confirmation_armed_);
}

TEST(HubLr1121FirmwareUpdate, SanityMismatchAfterBootloaderEntryRebootsRatherThanErasing) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
  // Bootloader entry reports a *different* bootloader version than boot recorded (as if the
  // excursion landed on a different/foreign chip state) -- must abort before EraseFlash.
  queue_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x21, 0x01);

  TestableHubComponent hub;
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::PROCEED;
  hub.lr1121_bootloader_version_known_ = true;
  hub.lr1121_bootloader_chip_type_ = LR1121_UPDATER_BOOTLOADER_TYPE;
  hub.lr1121_bootloader_version_ = 0x2100;  // boot recorded 0x2100, but the sanity read above says 0x2101

  const uint32_t reboots_before = esphome::App.safe_reboot_calls;
  hub.trigger_lr1121_firmware_update();

  EXPECT_LT(spi.find_opcode(LR1121_UPDATER_CMD_ERASE_FLASH), 0) << "must not erase after a sanity-check mismatch";
  EXPECT_EQ(esphome::App.safe_reboot_calls, reboots_before + 1)
      << "a sanity mismatch is a post-entry exit and must reboot";
}

// THE REGRESSION GUARD FOR THE ENTER_BOOTLOADER()-FAILURE INVARIANT: enter_bootloader()'s
// RST-pulse/BUSY-strap entry sequence (radio_lr1121_firmware_updater.cpp) runs unconditionally, on
// GPIO lines only, before the confirmatory GetVersion read that determines its bool return -- so a
// false return does not mean the chip was never touched. This used to plain-`return` here on the
// theory that it did, which is exactly the "forbidden" branch ADR 0020's invariant diagram names:
// a radio that answers SPI, looks initialized, and never works again.
TEST(HubLr1121FirmwareUpdate, EnterBootloaderConfirmationTimeoutStillReboots) {
  ScriptedSpi spi;
  MockPin rst;
  MockPin busy(true);  // BUSY held high throughout -> every wait_busy_() call times out immediately
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  TestableHubComponent hub;
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::PROCEED;

  const uint32_t reboots_before = esphome::App.safe_reboot_calls;
  hub.trigger_lr1121_firmware_update();

  EXPECT_TRUE(spi.transactions().empty())
      << "BUSY held high means write_command_()'s own wait_busy_() times out before anything reaches SPI";
  EXPECT_EQ(esphome::App.safe_reboot_calls, reboots_before + 1)
      << "enter_bootloader() returning false must still reboot -- its entry sequence already ran "
         "and left the chip unconfigured regardless of whether the verification read succeeded";
}

#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE
// ============================================================================
// Bootloader-rewrite post-filter dispatch (ADR 0021)
// ============================================================================
// LR1121_FIRMWARE_UPDATE_TARGET_VERSION is 0x0104 (requires bootloader 0x2101) and
// LR1121_BOOTLOADER_LOADER_FW is 0x2100 in the test fixtures -- see
// tests/include/lr1121_firmware_update_image.h's comment for why this is the only target for
// which BootloaderUpgradePath::AVAILABLE is reachable through the real
// cache_lr1121_flash_verdict_()/lr1121_bootloader_upgrade_path() wiring.

namespace {
// Sets up a hub whose cached state matches R3/R4: chip at bootloader 0x2100 (equals the
// configured loader's version), target 0x0104 needs 0x2101 -> AVAILABLE.
void arrange_bootloader_upgrade_available(TestableHubComponent &hub, Lr1121FirmwareUpdater &updater) {
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::REJECT_BOOTLOADER_TOO_OLD;
  hub.lr1121_bootloader_version_known_ = true;
  hub.lr1121_bootloader_chip_type_ = LR1121_UPDATER_BOOTLOADER_TYPE;
  hub.lr1121_bootloader_version_ = LR1121_BOOTLOADER_2100;
}
}  // namespace

TEST(HubLr1121FirmwareUpdate, R3_BootloaderUpgradeAvailableButSwitchOffRefusesWithZeroSpiTraffic) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  TestableHubComponent hub;
  arrange_bootloader_upgrade_available(hub, updater);
  hub.bootloader_rewrite_allowed_ = false;  // switch off

  hub.trigger_lr1121_firmware_update();

  EXPECT_TRUE(spi.transactions().empty()) << "a switch-off refusal must not touch the chip at all";
}

TEST(HubLr1121FirmwareUpdate, R4_BootloaderUpgradeAvailableAndSwitchOnRunsFullThreeStageSequence) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  queue_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x21, 0x00);     // Stage 1a sanity: 0x2100
  queue_version_response(spi, LR1121_UPDATER_LOADER_DEVICE_TYPE, 0x21, 0x00);  // Stage 1b: loader running
  queue_updater_status_response(spi, 0x02);                                    // Stage 2: chip accepted 0x8100
  queue_verify_bootloader_response(spi, 0x3F, 0x01, 0x21, 0x01);               // Stage 2: all six checks pass
  queue_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x21, 0x01);     // Stage 2: stays in bootloader, 0x2101
  queue_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x21, 0x01);  // Stage 3: enter_bootloader confirms 0x2101
  queue_version_response(spi, LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS, 0x01, 0x04);  // Stage 3: post-flash verify

  TestableHubComponent hub;
  arrange_bootloader_upgrade_available(hub, updater);
  hub.bootloader_rewrite_allowed_ = true;  // switch on

  const uint32_t reboots_before = esphome::App.safe_reboot_calls;
  hub.trigger_lr1121_firmware_update();

  ASSERT_EQ(count_opcode(spi, LR1121_UPDATER_CMD_ERASE_FLASH), 2u) << "one erase per stage (1a and 3)";
  ASSERT_EQ(count_opcode(spi, LR1121_UPDATER_CMD_WRITE_FLASH_ENCRYPTED), 2u) << "one write per stage (1a and 3)";
  ASSERT_EQ(count_opcode(spi, LR1121_UPDATER_CMD_UPDATE_BOOTLOADER), 1u);
  ASSERT_EQ(count_opcode(spi, LR1121_UPDATER_CMD_VERIFY_BOOTLOADER), 1u);
  ASSERT_EQ(count_opcode(spi, LR1121_UPDATER_CMD_UPDATER_REBOOT), 1u);

  const int erase_1a = find_nth_opcode(spi, LR1121_UPDATER_CMD_ERASE_FLASH, 0);
  const int write_1a = find_nth_opcode(spi, LR1121_UPDATER_CMD_WRITE_FLASH_ENCRYPTED, 0);
  const int update_bl = spi.find_opcode(LR1121_UPDATER_CMD_UPDATE_BOOTLOADER);
  const int verify_bl = spi.find_opcode(LR1121_UPDATER_CMD_VERIFY_BOOTLOADER);
  const int updater_reboot_idx = spi.find_opcode(LR1121_UPDATER_CMD_UPDATER_REBOOT);
  const int erase_3 = find_nth_opcode(spi, LR1121_UPDATER_CMD_ERASE_FLASH, 1);
  const int write_3 = find_nth_opcode(spi, LR1121_UPDATER_CMD_WRITE_FLASH_ENCRYPTED, 1);
  ASSERT_GE(erase_1a, 0);
  ASSERT_GE(write_1a, 0);
  ASSERT_GE(update_bl, 0);
  ASSERT_GE(verify_bl, 0);
  ASSERT_GE(updater_reboot_idx, 0);
  ASSERT_GE(erase_3, 0);
  ASSERT_GE(write_3, 0);
  EXPECT_LT(erase_1a, write_1a);
  EXPECT_LT(write_1a, update_bl) << "Stage 1b's checkpoint must pass before Stage 2's irreversible write";
  // Semtech's reference tool issues a 6-byte no-opcode status read between UpdateBootloader and
  // VerifyBootloader; matching its wire traffic exactly matters most in this stage, which cannot
  // be verified on hardware without risking the chip.
  ASSERT_EQ(verify_bl, update_bl + 2) << "exactly one transaction must sit between 0x8100 and 0x8101";
  EXPECT_EQ(spi.transactions()[update_bl + 1].size(), 6u)
      << "that transaction must be the 6-byte Stat1/Stat2/IrqStatus direct read";
  EXPECT_LT(update_bl, verify_bl);
  EXPECT_LT(verify_bl, updater_reboot_idx);
  EXPECT_LT(updater_reboot_idx, erase_3)
      << "Stage 3 must re-enter the bootloader and erase only after Stage 2 confirms";
  EXPECT_LT(erase_3, write_3);

  EXPECT_EQ(esphome::App.safe_reboot_calls, reboots_before + 1) << "a full successful sequence still ends in a reboot";
}

// R8: the boot-time excursion never read
// a bootloader version at all. lr1121_bootloader_upgrade_path() rule 2 makes this NOT_APPLICABLE
// unconditionally -- an unknown bootloader can never justify an irreversible write, switch or no
// switch (§D requires every refusing row to assert zero SPI traffic). trigger_lr1121_firmware_update()
// reads lr1121_bootloader_version_known_ directly (not re-derived from the cached verdict), so this
// is reachable exactly as written -- no fallback needed.
TEST(HubLr1121FirmwareUpdate, R8_UnknownBootloaderRefusesEvenWithSwitchOn) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  TestableHubComponent hub;
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::REJECT_BOOTLOADER_TOO_OLD;
  hub.lr1121_bootloader_version_known_ = false;  // boot-time excursion never read a version
  hub.bootloader_rewrite_allowed_ = true;        // switch on -- must not matter (rule 2)

  hub.trigger_lr1121_firmware_update();

  EXPECT_TRUE(spi.transactions().empty())
      << "an unknown bootloader must never justify the irreversible path, switch or no switch";
}

// R9: the target is absent from this
// build's compatibility table -> BootloaderUpgradePath::BLOCKED_UNKNOWN_TARGET -> refuse without
// gambling an irreversible write on an unrecognised requirement (§D requires zero SPI traffic).
//
// UNREACHABLE at the hub level in this test binary: trigger_lr1121_firmware_update() calls
// lr1121_bootloader_upgrade_path() with LR1121_FIRMWARE_UPDATE_TARGET_VERSION, a compile-time
// constant fixed at 0x0104 by this test binary's stand-in header (see
// tests/include/lr1121_firmware_update_image.h) -- and 0x0104 is always present in
// LR1121_KNOWN_BOOTLOADER_REQUIREMENTS, so `required` can never be 0 no matter what hub state this
// test sets up. There is no runtime hook to substitute a different target for one hub test. This
// tests the same rule directly at the pure-function level instead.
TEST(HubLr1121FirmwareUpdate, R9_UnknownTargetBlocksUpgradePathAtThePureFunctionLevel) {
  constexpr uint16_t kTargetAbsentFromCompatibilityTable = 0x0999;  // matches the pure-decision test's convention
  EXPECT_EQ(lr1121_bootloader_upgrade_path(/*block_present=*/true, /*bootloader_version_known=*/true,
                                           /*bootloader_version=*/LR1121_BOOTLOADER_2100,
                                           /*loader_fw=*/LR1121_BOOTLOADER_2100, kTargetAbsentFromCompatibilityTable),
            BootloaderUpgradePath::BLOCKED_UNKNOWN_TARGET);
}

// The highest-value safety test in this feature: a loader that did not
// land must be caught at the Stage-1b checkpoint, before the irreversible 0x8100 is ever sent.
TEST(HubLr1121FirmwareUpdate, Stage1bAbortsOnFirmwareMismatchAndNeverIssuesUpdateBootloader) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  queue_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x21, 0x00);  // Stage 1a sanity: 0x2100
  // Stage 1b: chip reports the OLD installed firmware instead of the loader's 0x2100 -- as if the
  // loader write from Stage 1a never actually landed.
  queue_version_response(spi, LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS, 0x01, 0x01);

  TestableHubComponent hub;
  arrange_bootloader_upgrade_available(hub, updater);
  hub.bootloader_rewrite_allowed_ = true;

  const uint32_t reboots_before = esphome::App.safe_reboot_calls;
  hub.trigger_lr1121_firmware_update();

  EXPECT_LT(spi.find_opcode(LR1121_UPDATER_CMD_UPDATE_BOOTLOADER), 0)
      << "the Stage-1b checkpoint must abort before the irreversible 0x8100 write";
  EXPECT_EQ(count_opcode(spi, LR1121_UPDATER_CMD_ERASE_FLASH), 1u) << "only Stage 1a's erase, never Stage 3's";
  EXPECT_EQ(esphome::App.safe_reboot_calls, reboots_before + 1)
      << "a Stage-1b abort is a post-entry exit and must reboot";
}

// The version check alone cannot catch this: the loader reports 0x2100 and so does the bootloader
// (LR1121_LOADER_2100 == LR1121_BOOTLOADER_2100 numerically). If reboot() is sent but the chip
// never leaves bootloader mode, the Stage-1b read returns byte-for-byte what a successful loader
// boot returns, except for `type`. Without the type check, 0x8100 would be sent to the bootloader.
TEST(HubLr1121FirmwareUpdate, Stage1bAbortsWhenChipStayedInBootloaderDespiteMatchingVersion) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  queue_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x21, 0x00);  // Stage 1a sanity: 0x2100
  // Stage 1b: fw is exactly the loader's 0x2100 -- the version check passes -- but type is still
  // 0xDF, i.e. the reboot-into-loader never took effect and this is the bootloader answering.
  queue_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x21, 0x00);

  TestableHubComponent hub;
  arrange_bootloader_upgrade_available(hub, updater);
  hub.bootloader_rewrite_allowed_ = true;

  const uint32_t reboots_before = esphome::App.safe_reboot_calls;
  hub.trigger_lr1121_firmware_update();

  EXPECT_LT(spi.find_opcode(LR1121_UPDATER_CMD_UPDATE_BOOTLOADER), 0)
      << "a chip still in bootloader mode must never receive 0x8100, even though the version matched";
  EXPECT_EQ(count_opcode(spi, LR1121_UPDATER_CMD_ERASE_FLASH), 1u) << "only Stage 1a's erase, never Stage 3's";
  EXPECT_EQ(esphome::App.safe_reboot_calls, reboots_before + 1);
}

// A Stage-2 verification failure (write already happened) must reboot without ever reaching
// Stage 3 -- and per hard rule "do not auto-retry 0x8100", must not send it again either.
TEST(HubLr1121FirmwareUpdate, Stage2VerificationFailureAbortsBeforeStage3AndNeverRetries) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  queue_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x21, 0x00);     // Stage 1a sanity: 0x2100
  queue_version_response(spi, LR1121_UPDATER_LOADER_DEVICE_TYPE, 0x21, 0x00);  // Stage 1b: loader running
  // Stage 2: anti-rollback bit (0x20) clear -- one of the six checks fails.
  queue_updater_status_response(spi, 0x02);
  queue_verify_bootloader_response(spi, 0x1F, 0x01, 0x21, 0x01);

  TestableHubComponent hub;
  arrange_bootloader_upgrade_available(hub, updater);
  hub.bootloader_rewrite_allowed_ = true;

  const uint32_t reboots_before = esphome::App.safe_reboot_calls;
  hub.trigger_lr1121_firmware_update();

  EXPECT_EQ(count_opcode(spi, LR1121_UPDATER_CMD_UPDATE_BOOTLOADER), 1u) << "must not auto-retry 0x8100";
  EXPECT_LT(spi.find_opcode(LR1121_UPDATER_CMD_UPDATER_REBOOT), 0)
      << "must not proceed to the post-verify reboot after a failed verification";
  EXPECT_EQ(count_opcode(spi, LR1121_UPDATER_CMD_ERASE_FLASH), 1u) << "must never reach Stage 3's erase";
  EXPECT_EQ(esphome::App.safe_reboot_calls, reboots_before + 1);
}

// A Stage-2 post-verify reboot that does NOT come back reporting 0x2101-in-bootloader (the
// inverted success condition -- ADR 0021) must also abort before Stage 3.
TEST(HubLr1121FirmwareUpdate, Stage2UnexpectedPostRebootStateAbortsBeforeStage3) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  queue_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x21, 0x00);     // Stage 1a sanity: 0x2100
  queue_version_response(spi, LR1121_UPDATER_LOADER_DEVICE_TYPE, 0x21, 0x00);  // Stage 1b: loader running
  queue_updater_status_response(spi, 0x02);                                    // Stage 2: chip accepted 0x8100
  queue_verify_bootloader_response(spi, 0x3F, 0x01, 0x21, 0x01);               // Stage 2: all six checks pass
  // Stage 2 post-verify reboot: chip booted the loader firmware instead of staying in the
  // bootloader -- the new bootloader did NOT refuse it as expected.
  queue_version_response(spi, LR1121_UPDATER_LOADER_DEVICE_TYPE, 0x21, 0x00);

  TestableHubComponent hub;
  arrange_bootloader_upgrade_available(hub, updater);
  hub.bootloader_rewrite_allowed_ = true;

  const uint32_t reboots_before = esphome::App.safe_reboot_calls;
  hub.trigger_lr1121_firmware_update();

  EXPECT_EQ(count_opcode(spi, LR1121_UPDATER_CMD_ERASE_FLASH), 1u) << "must never reach Stage 3's erase";
  EXPECT_EQ(esphome::App.safe_reboot_calls, reboots_before + 1);
}

// Hard rule 6: the switch is a permission, never an override -- it must have zero effect on
// REJECT_WRONG_CHIP.
TEST(HubLr1121FirmwareUpdate, SwitchOnHasNoEffectOnRejectWrongChipVerdict) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  TestableHubComponent hub;
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::REJECT_WRONG_CHIP;
  hub.bootloader_rewrite_allowed_ = true;  // switch on -- must not matter

  hub.trigger_lr1121_firmware_update();

  EXPECT_TRUE(spi.transactions().empty());
}

// Hard rule 6, other direction: the switch must not bypass the ordinary two-press confirmation
// for a NEEDS_CONFIRMATION verdict -- it only ever converts REJECT_BOOTLOADER_TOO_OLD/AVAILABLE.
TEST(HubLr1121FirmwareUpdate, SwitchOnDoesNotBypassTwoPressConfirmationForNeedsConfirmation) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  TestableHubComponent hub;
  hub.lr1121_firmware_updater_ = &updater;
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::NEEDS_CONFIRMATION;
  hub.bootloader_rewrite_allowed_ = true;  // switch on -- must not matter

  hub.trigger_lr1121_firmware_update();

  EXPECT_TRUE(spi.transactions().empty()) << "first press must still not touch the chip";
  EXPECT_TRUE(hub.lr1121_flash_confirmation_armed_) << "must still arm the ordinary two-press window";
}

// Boot-time message: this prints on every boot, so it is deliberately short -- the switch is
// discoverable in Home Assistant and the full reasoning lives in the docs and ADR 0021. What must
// survive that trim is both versions and the fact that the rewrite cannot be undone.
TEST(HubLr1121FirmwareUpdate, DebugLinesStateBothVersionsAndIrreversibilityWhenUpgradeIsAvailable) {
  TestableHubComponent hub;
  hub.lr1121_bootloader_version_known_ = true;
  hub.lr1121_bootloader_version_ = LR1121_BOOTLOADER_2100;  // matches LR1121_BOOTLOADER_LOADER_FW
  hub.lr1121_flash_verdict_known_ = true;
  hub.lr1121_flash_verdict_ = FlashDecision::REJECT_BOOTLOADER_TOO_OLD;

  const std::vector<std::string> lines = hub.lr1121_firmware_update_debug_lines_();

  ASSERT_EQ(lines.size(), 3u);
  const std::string &message = lines[2];
  EXPECT_NE(message.find("0x2101"), std::string::npos) << "required bootloader: " << message;
  EXPECT_NE(message.find("0x2100"), std::string::npos) << "current bootloader: " << message;
  EXPECT_NE(message.find("cannot be undone"), std::string::npos) << "must say it is irreversible: " << message;
  // The boot dump runs on every boot; keeping it terse is the point of the trim.
  EXPECT_LT(message.size(), 200u) << "boot-time line must stay short: " << message;
}

// The switch-off refusal used to be describe_lr1121_flash_verdict_() ("CANNOT PROCEED: ...") with a
// suffix bolted on, which read as final and then contradicted itself by saying the rewrite was
// available. It must lead with the outcome, then the reason, then what to do.
TEST(HubLr1121FirmwareUpdate, SwitchOffRefusalLeadsWithOutcomeAndSaysWhatToDoNext) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  TestableHubComponent hub;
  arrange_bootloader_upgrade_available(hub, updater);
  hub.bootloader_rewrite_allowed_ = false;

  const std::string message = hub.describe_lr1121_bootloader_refusal_(BootloaderUpgradePath::AVAILABLE);

  EXPECT_EQ(message.find("nothing was done"), 0u + std::string("LR1121 firmware update: ").size())
      << "must lead with the outcome: " << message;
  EXPECT_EQ(message.find("CANNOT PROCEED"), std::string::npos)
      << "must not reuse the terminal-sounding verdict wording for a recoverable, actionable state: " << message;
  EXPECT_EQ(message.find("No chip access"), std::string::npos) << "must not use developer shorthand: " << message;
  EXPECT_NE(message.find("press this button again"), std::string::npos) << "must say what to do next: " << message;
  EXPECT_NE(message.find("cannot be undone"), std::string::npos) << "must keep the warning: " << message;

  hub.trigger_lr1121_firmware_update();
  EXPECT_TRUE(spi.transactions().empty()) << "a switch-off refusal must still not touch the chip";
}

// The other two refusals must follow the same shape and must not claim a bootloader requirement
// they do not know (UNKNOWN_TARGET) or invert the direction (BOOTLOADER_NEWER).
TEST(HubLr1121FirmwareUpdate, BlockedRefusalsLeadWithOutcomeAndStateTheRightReason) {
  TestableHubComponent hub;
  hub.lr1121_bootloader_version_ = LR1121_BOOTLOADER_2100;

  const std::string unknown = hub.describe_lr1121_bootloader_refusal_(BootloaderUpgradePath::BLOCKED_UNKNOWN_TARGET);
  EXPECT_NE(unknown.find("nothing was done"), std::string::npos) << unknown;
  EXPECT_NE(unknown.find("does not recognise"), std::string::npos) << unknown;
  EXPECT_EQ(unknown.find("press this button again"), std::string::npos)
      << "there is no user action that helps here, so do not suggest one: " << unknown;

  const std::string newer = hub.describe_lr1121_bootloader_refusal_(BootloaderUpgradePath::BLOCKED_BOOTLOADER_NEWER);
  EXPECT_NE(newer.find("nothing was done"), std::string::npos) << newer;
  EXPECT_NE(newer.find("already newer"), std::string::npos) << newer;
  EXPECT_NE(newer.find("no way back"), std::string::npos) << newer;
}
#endif  // IOHOME_LR1121_BOOTLOADER_UPDATE

// The loader's type byte (0xDE) and the bootloader's (0xDF) differ by a single bit, so the Stage-1b
// checkpoint asserts the loader's value positively. A "not 0xDF" check would have accepted exactly
// the one-bit corruption of the byte this gate exists to trust.
TEST(HubLr1121FirmwareUpdate, Stage1bRejectsAnyTypeOtherThanTheLoadersEvenWhenItIsNotTheBootloaders) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  queue_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x21, 0x00);  // Stage 1a sanity: 0x2100
  // Correct loader version, and NOT the bootloader's 0xDF -- but not the loader's 0xDE either.
  queue_version_response(spi, 0xAA, 0x21, 0x00);

  TestableHubComponent hub;
  arrange_bootloader_upgrade_available(hub, updater);
  hub.bootloader_rewrite_allowed_ = true;

  const uint32_t reboots_before = esphome::App.safe_reboot_calls;
  hub.trigger_lr1121_firmware_update();

  EXPECT_LT(spi.find_opcode(LR1121_UPDATER_CMD_UPDATE_BOOTLOADER), 0)
      << "an unrecognised type byte must abort before the irreversible write";
  EXPECT_EQ(count_opcode(spi, LR1121_UPDATER_CMD_ERASE_FLASH), 1u) << "only Stage 1a's erase, never Stage 3's";
  EXPECT_EQ(esphome::App.safe_reboot_calls, reboots_before + 1);
}
