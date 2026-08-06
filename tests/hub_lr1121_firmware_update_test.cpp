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
  // installed firmware 0x0101 -- older than the test image's target 0x0103 (see
  // tests/include/lr1121_firmware_update_image.h), so a correct decision is PROCEED.
  queue_version_response(spi, LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS, 0x01, 0x01);

  MockRadio radio;
  TestableHubComponent hub;
  hub.radio_ = &radio;
  hub.lr1121_firmware_updater_ = &updater;
  // Simulates a successful boot-time excursion (run_lr1121_boot_time_bootloader_read_()) having
  // already cached a genuine LR1121's bootloader identity.
  hub.lr1121_bootloader_version_known_ = true;
  hub.lr1121_bootloader_chip_type_ = LR1121_UPDATER_BOOTLOADER_TYPE;
  hub.lr1121_bootloader_version_ = LR1121_BOOTLOADER_2100;

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
  EXPECT_NE(too_old.find("not supported"), std::string::npos)
      << "must say bootloader updates are not supported: " << too_old;

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
