#include "radio_lr1121_firmware_updater.h"
#include "radio_lr1121.h"               // LR1121_DEVICE_TYPE
#include "lr1121_firmware_decisions.h"  // LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS, LR1121_BOOTLOADER_TYPE_FOR_FIRMWARE_DECISIONS

#include "esphome/core/application.h"

#include "stubs/radio_test_common.h"
#include "stubs/scripted_spi.h"

#include <gtest/gtest.h>
#include <vector>

using namespace esphome::home_io_control;

// lr1121_firmware_decisions.h deliberately keeps its own dependency-free copies of these two
// constants rather than including radio_lr1121.h / radio_lr1121_firmware_updater.h (see that
// file's header comment for why). Nothing else enforces they stay equal to the values the real
// driver and updater use -- a review found exactly this class of bug: a byte meant for one of these
// constants silently compared against the other. This file already includes radio_lr1121.h and is
// the natural place for radio_lr1121_firmware_updater.h's constant to be cross-checked too.
static_assert(LR1121_DEVICE_TYPE_FOR_FIRMWARE_DECISIONS == LR1121_DEVICE_TYPE,
              "lr1121_firmware_decisions.h's copy of the normal-mode device_type has drifted from radio_lr1121.h");
static_assert(LR1121_BOOTLOADER_TYPE_FOR_FIRMWARE_DECISIONS == LR1121_UPDATER_BOOTLOADER_TYPE,
              "lr1121_firmware_decisions.h's copy of the bootloader-mode type byte has drifted from "
              "radio_lr1121_firmware_updater.h");

namespace {

// Queue a bootloader-mode GetVersion response: [stat1, hw, type, bootloader_hi, bootloader_lo].
void queue_bootloader_version_response(ScriptedSpi &spi, uint8_t type, uint16_t bootloader_version) {
  spi.queue_responses(
      {0x00, 0x01, type, static_cast<uint8_t>(bootloader_version >> 8), static_cast<uint8_t>(bootloader_version)});
}

// Queue a normal-mode GetVersion response: [stat1, hw, device_type, fw_major, fw_minor].
void queue_normal_version_response(ScriptedSpi &spi, uint8_t device_type, uint8_t fw_major, uint8_t fw_minor) {
  spi.queue_responses({0x00, 0x01, device_type, fw_major, fw_minor});
}

}  // namespace

TEST(Lr1121FirmwareUpdater, ReadNormalVersionSendsGetVersionAndParsesResponse) {
  ScriptedSpi spi;
  MockPin rst;
  MockPin busy;  // digital_read() defaults to false (not busy) — every wait_busy_() returns immediately.
  queue_normal_version_response(spi, LR1121_DEVICE_TYPE, 0x01, 0x03);

  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
  uint8_t device_type = 0, fw_major = 0, fw_minor = 0;
  ASSERT_TRUE(updater.read_normal_version(device_type, fw_major, fw_minor));
  EXPECT_EQ(device_type, LR1121_DEVICE_TYPE);
  EXPECT_EQ(fw_major, 0x01);
  EXPECT_EQ(fw_minor, 0x03);

  int const idx = spi.find_opcode(LR1121_UPDATER_CMD_GET_VERSION);
  ASSERT_GE(idx, 0);
  EXPECT_EQ(spi.transactions()[idx].size(), 2u) << "GetVersion has no request params";
}

TEST(Lr1121FirmwareUpdater, EnterBootloaderSendsGetVersionAndParsesBootloaderResponse) {
  ScriptedSpi spi;
  MockPin rst;
  MockPin busy;
  queue_bootloader_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x2100);

  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
  uint8_t type = 0;
  uint16_t bootloader_version = 0;
  ASSERT_TRUE(updater.enter_bootloader(type, bootloader_version));
  EXPECT_EQ(type, LR1121_UPDATER_BOOTLOADER_TYPE);
  EXPECT_EQ(bootloader_version, 0x2100);

  EXPECT_GE(spi.find_opcode(LR1121_UPDATER_CMD_GET_VERSION), 0);
}

TEST(Lr1121FirmwareUpdater, EraseFlashSendsEraseFlashOpcodeWithNoParams) {
  ScriptedSpi spi;
  MockPin rst;
  MockPin busy;

  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
  ASSERT_TRUE(updater.erase_flash());

  int const idx = spi.find_opcode(LR1121_UPDATER_CMD_ERASE_FLASH);
  ASSERT_GE(idx, 0);
  EXPECT_EQ(spi.transactions()[idx].size(), 2u) << "EraseFlash has no params";
}

TEST(Lr1121FirmwareUpdater, WriteImageChunksExactlyAtTwoFullChunks) {
  ScriptedSpi spi;
  MockPin rst;
  MockPin busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  std::vector<uint32_t> image(2 * LR1121_UPDATER_FLASH_CHUNK_WORDS);
  for (size_t i = 0; i < image.size(); i++)
    image[i] = static_cast<uint32_t>(0x10000000 + i);

  std::vector<std::pair<size_t, size_t>> progress_calls;
  ASSERT_TRUE(updater.write_image(image.data(), image.size(),
                                  [&](size_t done, size_t total) { progress_calls.emplace_back(done, total); }));

  // Find the two WriteFlashEncrypted transactions in order.
  std::vector<size_t> write_indices;
  for (size_t i = 0; i < spi.transactions().size(); i++) {
    const auto &txn = spi.transactions()[i];
    if (txn.size() >= 2 && txn[0] == 0x80 && txn[1] == 0x03)
      write_indices.push_back(i);
  }
  ASSERT_EQ(write_indices.size(), 2u);

  const auto &chunk0 = spi.transactions()[write_indices[0]];
  // opcode(2) + offset(4) + 64 words * 4 bytes = 262 bytes.
  ASSERT_EQ(chunk0.size(), 2u + 4u + LR1121_UPDATER_FLASH_CHUNK_WORDS * 4u);
  EXPECT_EQ(chunk0[2], 0x00);
  EXPECT_EQ(chunk0[3], 0x00);
  EXPECT_EQ(chunk0[4], 0x00);
  EXPECT_EQ(chunk0[5], 0x00);  // offset 0x00000000
  // First word (0x10000000) big-endian at bytes [6..9].
  EXPECT_EQ(chunk0[6], 0x10);
  EXPECT_EQ(chunk0[7], 0x00);
  EXPECT_EQ(chunk0[8], 0x00);
  EXPECT_EQ(chunk0[9], 0x00);

  const auto &chunk1 = spi.transactions()[write_indices[1]];
  ASSERT_EQ(chunk1.size(), 2u + 4u + LR1121_UPDATER_FLASH_CHUNK_WORDS * 4u);
  EXPECT_EQ(chunk1[2], 0x00);
  EXPECT_EQ(chunk1[3], 0x00);
  EXPECT_EQ(chunk1[4], 0x01);
  EXPECT_EQ(chunk1[5], 0x00);  // offset 0x00000100 (64 words * 4 bytes)
  // First word of the second chunk: image[64] = 0x10000040.
  EXPECT_EQ(chunk1[6], 0x10);
  EXPECT_EQ(chunk1[7], 0x00);
  EXPECT_EQ(chunk1[8], 0x00);
  EXPECT_EQ(chunk1[9], 0x40);

  ASSERT_EQ(progress_calls.size(), 2u);
  EXPECT_EQ(progress_calls[0].first, LR1121_UPDATER_FLASH_CHUNK_WORDS);
  EXPECT_EQ(progress_calls[1].first, image.size());
  EXPECT_EQ(progress_calls[1].second, image.size());
}

TEST(Lr1121FirmwareUpdater, WriteImageShortFinalChunk) {
  ScriptedSpi spi;
  MockPin rst;
  MockPin busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  // 65 words -> one full 64-word chunk, then a 1-word final chunk.
  std::vector<uint32_t> image(LR1121_UPDATER_FLASH_CHUNK_WORDS + 1, 0xAABBCCDD);
  ASSERT_TRUE(updater.write_image(image.data(), image.size(), nullptr));

  std::vector<size_t> write_indices;
  for (size_t i = 0; i < spi.transactions().size(); i++) {
    const auto &txn = spi.transactions()[i];
    if (txn.size() >= 2 && txn[0] == 0x80 && txn[1] == 0x03)
      write_indices.push_back(i);
  }
  ASSERT_EQ(write_indices.size(), 2u);
  EXPECT_EQ(spi.transactions()[write_indices[0]].size(), 2u + 4u + LR1121_UPDATER_FLASH_CHUNK_WORDS * 4u);
  // Final chunk: opcode(2) + offset(4) + 1 word * 4 bytes = 10 bytes.
  EXPECT_EQ(spi.transactions()[write_indices[1]].size(), 2u + 4u + 1u * 4u);
}

TEST(Lr1121FirmwareUpdater, ReadHashSendsGetHashWithNoParamsAndReturnsSixteenBytes) {
  ScriptedSpi spi;
  MockPin rst;
  MockPin busy;
  const uint8_t expected[LR1121_UPDATER_HASH_LENGTH] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
                                                        0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF};
  spi.queue_response(0x00);  // Stat1
  for (uint8_t b : expected)
    spi.queue_response(b);

  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
  uint8_t out[LR1121_UPDATER_HASH_LENGTH] = {0};
  ASSERT_TRUE(updater.read_hash(out, sizeof(out)));
  for (size_t i = 0; i < LR1121_UPDATER_HASH_LENGTH; i++)
    EXPECT_EQ(out[i], expected[i]) << "byte " << i;

  int const idx = spi.find_opcode(LR1121_UPDATER_CMD_GET_HASH);
  ASSERT_GE(idx, 0);
  EXPECT_EQ(spi.transactions()[idx].size(), 2u) << "GetHash has no request params";
}

TEST(Lr1121FirmwareUpdater, ReadHashRejectsUndersizedBufferWithoutTouchingTheChip) {
  ScriptedSpi spi;
  MockPin rst;
  MockPin busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  uint8_t out[LR1121_UPDATER_HASH_LENGTH - 1];
  EXPECT_FALSE(updater.read_hash(out, sizeof(out)));
  EXPECT_TRUE(spi.transactions().empty()) << "an undersized buffer must not issue any SPI transaction";
}

// The BUSY-as-reset-strap guard must restore the pin's actual configured mode (pullup/pulldown
// included), not a hardcoded plain FLAG_INPUT that would silently drop it -- see
// BusyPinAsResetStrapGuard's comment (radio_lr1121_firmware_updater.cpp). busy_pin is validated
// from pins.internal_gpio_input_pin_schema, which allows pullup:/pulldown:, so a real config can
// legitimately set this.
TEST(Lr1121FirmwareUpdater, EnterBootloaderRestoresBusyPinsConfiguredPullNotAHardcodedPlainInput) {
  ScriptedSpi spi;
  MockPin rst;
  MockPin busy;
  busy.set_flags(static_cast<esphome::gpio::Flags>(esphome::gpio::FLAG_INPUT | esphome::gpio::FLAG_PULLUP));
  queue_bootloader_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x2100);

  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
  uint8_t type = 0;
  uint16_t bootloader_version = 0;
  ASSERT_TRUE(updater.enter_bootloader(type, bootloader_version));

  ASSERT_FALSE(busy.pin_mode_calls().empty());
  const esphome::gpio::Flags restored = busy.pin_mode_calls().back();
  EXPECT_EQ(static_cast<uint8_t>(restored),
            static_cast<uint8_t>(esphome::gpio::FLAG_INPUT | esphome::gpio::FLAG_PULLUP))
      << "the pin must come back with its configured pull, not a bare FLAG_INPUT";
}

TEST(Lr1121FirmwareUpdater, RebootSendsCorrectParamForEachDirection) {
  {
    ScriptedSpi spi;
    MockPin rst;
    MockPin busy;
    Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
    ASSERT_TRUE(updater.reboot(true));
    int const idx = spi.find_opcode(LR1121_UPDATER_CMD_REBOOT);
    ASSERT_GE(idx, 0);
    ASSERT_EQ(spi.transactions()[idx].size(), 3u);
    EXPECT_EQ(spi.transactions()[idx][2], 0x03) << "stay_in_bootloader=true must send param 0x03";
  }
  {
    ScriptedSpi spi;
    MockPin rst;
    MockPin busy;
    Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
    ASSERT_TRUE(updater.reboot(false));
    int const idx = spi.find_opcode(LR1121_UPDATER_CMD_REBOOT);
    ASSERT_GE(idx, 0);
    ASSERT_EQ(spi.transactions()[idx].size(), 3u);
    EXPECT_EQ(spi.transactions()[idx][2], 0x00) << "stay_in_bootloader=false must send param 0x00 (boot the image)";
  }
}

// ============================================================================
// Bootloader-rewrite primitives (0x81xx family) -- normal-mode commands of the *loader* firmware,
// not bootloader-mode commands; see radio_lr1121_firmware_updater.h's file comment. Byte-exact
// against SWTL001's bootloader_updater_driver/src/lr11xx_bootloader_updater.c, the only public
// specification for this wire protocol.
// ============================================================================

TEST(Lr1121FirmwareUpdater, ReadBootloaderVersionSendsGetVersionWithoutRunningTheEntrySequence) {
  ScriptedSpi spi;
  MockPin rst;
  MockPin busy;
  queue_bootloader_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x2101);

  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
  uint8_t type = 0;
  uint16_t bootloader_version = 0;
  ASSERT_TRUE(updater.read_bootloader_version(type, bootloader_version));
  EXPECT_EQ(type, LR1121_UPDATER_BOOTLOADER_TYPE);
  EXPECT_EQ(bootloader_version, 0x2101);

  // Exactly the two transactions read_command_() always uses (write phase, read phase) -- no
  // RST pulse/BUSY-strap GPIO dance, unlike enter_bootloader().
  ASSERT_EQ(spi.transactions().size(), 2u);
  EXPECT_GE(spi.find_opcode(LR1121_UPDATER_CMD_GET_VERSION), 0);
}

TEST(Lr1121FirmwareUpdater, UpdateBootloaderSendsOpcodeWithNoParamsAndWaitsBusy) {
  ScriptedSpi spi;
  MockPin rst;
  MockPin busy;  // not busy -> wait_busy_() returns immediately, both before and after the command.
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  ASSERT_TRUE(updater.update_bootloader());

  int const idx = spi.find_opcode(LR1121_UPDATER_CMD_UPDATE_BOOTLOADER);
  ASSERT_GE(idx, 0);
  EXPECT_EQ(spi.transactions()[idx].size(), 2u) << "UpdateBootloader has no request params";
}

TEST(Lr1121FirmwareUpdater, UpdateBootloaderFailsOnBusyTimeoutWithoutAnyTransaction) {
  ScriptedSpi spi;
  MockPin rst;
  MockPin busy(true);  // BUSY held high -> the pre-command wait_busy_() in write_command_() times out.
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  EXPECT_FALSE(updater.update_bootloader());
  EXPECT_TRUE(spi.transactions().empty());
}

// Queue a VerifyBootloader response: [stat1, check_byte, use_case, version_major, version_minor].
namespace {
void queue_verify_bootloader_response(ScriptedSpi &spi, uint8_t check_byte, uint8_t use_case, uint8_t version_major,
                                      uint8_t version_minor) {
  spi.queue_responses({0x00, check_byte, use_case, version_major, version_minor});
}
}  // namespace

TEST(Lr1121FirmwareUpdater, VerifyBootloaderDecodesAllSixChecksWhenAllPass) {
  ScriptedSpi spi;
  MockPin rst;
  MockPin busy;
  queue_verify_bootloader_response(spi, 0x3F, 0x01, 0x21, 0x01);  // all six bits set

  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
  Lr1121BootloaderVerification report;
  ASSERT_TRUE(updater.verify_bootloader(report));
  EXPECT_TRUE(report.all_checks_passed());
  EXPECT_TRUE(report.signature_verified);
  EXPECT_TRUE(report.version_verified);
  EXPECT_TRUE(report.use_case_verified);
  EXPECT_TRUE(report.version_major_verified);
  EXPECT_TRUE(report.version_minor_verified);
  EXPECT_TRUE(report.anti_rollback_verified);
  EXPECT_EQ(report.use_case, 0x01);
  EXPECT_EQ(report.version_major, 0x21);
  EXPECT_EQ(report.version_minor, 0x01);

  EXPECT_GE(spi.find_opcode(LR1121_UPDATER_CMD_VERIFY_BOOTLOADER), 0);
}

// Each of the six check bits, individually false, with the other five set -- the highest-value
// decode test, one per check bit: a single-bit decode bug would only
// show up when every other bit is correct.
TEST(Lr1121FirmwareUpdater, VerifyBootloaderDetectsEachCheckBitIndividuallyFailing) {
  struct Case {
    uint8_t check_byte;
    const char *name;
  };
  const Case cases[] = {
      {0x3E, "signature"},      // bit 0 clear
      {0x3D, "version"},        // bit 1 clear
      {0x3B, "use_case"},       // bit 2 clear
      {0x37, "version_major"},  // bit 3 clear
      {0x2F, "version_minor"},  // bit 4 clear
      {0x1F, "anti_rollback"},  // bit 5 clear
  };
  for (const auto &c : cases) {
    ScriptedSpi spi;
    MockPin rst;
    MockPin busy;
    queue_verify_bootloader_response(spi, c.check_byte, 0, 0, 0);

    Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
    Lr1121BootloaderVerification report;
    ASSERT_TRUE(updater.verify_bootloader(report));
    EXPECT_FALSE(report.all_checks_passed()) << "check_byte=0x" << std::hex << static_cast<int>(c.check_byte) << " ("
                                             << c.name << " bit clear) must fail all_checks_passed()";
  }
}

TEST(Lr1121FirmwareUpdater, UpdaterRebootSendsCorrectParamForEachDirection) {
  {
    ScriptedSpi spi;
    MockPin rst;
    MockPin busy;
    Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
    ASSERT_TRUE(updater.updater_reboot(true));
    int const idx = spi.find_opcode(LR1121_UPDATER_CMD_UPDATER_REBOOT);
    ASSERT_GE(idx, 0);
    ASSERT_EQ(spi.transactions()[idx].size(), 3u);
    EXPECT_EQ(spi.transactions()[idx][2], 0x03) << "stay_in_bootloader=true must send param 0x03";
  }
  {
    ScriptedSpi spi;
    MockPin rst;
    MockPin busy;
    Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
    ASSERT_TRUE(updater.updater_reboot(false));
    int const idx = spi.find_opcode(LR1121_UPDATER_CMD_UPDATER_REBOOT);
    ASSERT_GE(idx, 0);
    ASSERT_EQ(spi.transactions()[idx].size(), 3u);
    EXPECT_EQ(spi.transactions()[idx][2], 0x00) << "stay_in_bootloader=false must send param 0x00";
  }
}

TEST(Lr1121FirmwareUpdater, FullSequenceOrdering) {
  ScriptedSpi spi;
  MockPin rst;
  MockPin busy;
  queue_bootloader_version_response(spi, LR1121_UPDATER_BOOTLOADER_TYPE, 0x2100);

  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);
  uint8_t type = 0;
  uint16_t bootloader_version = 0;
  ASSERT_TRUE(updater.enter_bootloader(type, bootloader_version));
  ASSERT_TRUE(updater.erase_flash());
  uint32_t const image[4] = {1, 2, 3, 4};
  ASSERT_TRUE(updater.write_image(image, 4, nullptr));
  uint8_t hash[LR1121_UPDATER_HASH_LENGTH] = {0};
  ASSERT_TRUE(updater.read_hash(hash, sizeof(hash)));  // must still work while in bootloader mode, pre-reboot
  ASSERT_TRUE(updater.reboot(false));

  int const version_idx = spi.find_opcode(LR1121_UPDATER_CMD_GET_VERSION);
  int const erase_idx = spi.find_opcode(LR1121_UPDATER_CMD_ERASE_FLASH);
  int const write_idx = spi.find_opcode(LR1121_UPDATER_CMD_WRITE_FLASH_ENCRYPTED);
  int const hash_idx = spi.find_opcode(LR1121_UPDATER_CMD_GET_HASH);
  int const reboot_idx = spi.find_opcode(LR1121_UPDATER_CMD_REBOOT);
  ASSERT_GE(version_idx, 0);
  ASSERT_GE(erase_idx, 0);
  ASSERT_GE(write_idx, 0);
  ASSERT_GE(hash_idx, 0);
  ASSERT_GE(reboot_idx, 0);
  EXPECT_LT(version_idx, erase_idx);
  EXPECT_LT(erase_idx, write_idx);
  EXPECT_LT(write_idx, hash_idx) << "GetHash must be read before rebooting -- still in bootloader mode";
  EXPECT_LT(hash_idx, reboot_idx);
}

// The status read is a bare 6-byte transaction with NO opcode -- Semtech's lr11xx_hal_direct_read()
// shape. Sending an opcode here would be a different command entirely, so the byte count and the
// absence of a leading opcode are both part of the contract.
TEST(Lr1121FirmwareUpdater, ReadUpdaterStatusIsAnOpcodeLessSixByteReadAndDecodesCommandStatus) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  // Stat1 = 0x05: bit0 (interrupt active) set, command_status = 0x05 >> 1 = 2 (OK).
  // Stat2 = 0x23: running_from_flash set, chip_mode = (0x23 & 0x0F) >> 1 = 1, reset_status = 2.
  spi.queue_responses({0x05, 0x23, 0xDE, 0xAD, 0xBE, 0xEF});

  Lr1121UpdaterStatus status;
  ASSERT_TRUE(updater.read_updater_status(status));

  EXPECT_EQ(status.command_status, Lr1121UpdaterCommandStatus::OK);
  EXPECT_TRUE(status.interrupt_active);
  EXPECT_TRUE(status.running_from_flash);
  EXPECT_EQ(status.chip_mode, 1);
  EXPECT_EQ(status.reset_status, 2);
  EXPECT_EQ(status.irq_status, 0xDEADBEEFu);

  ASSERT_EQ(spi.transactions().size(), 1u);
  EXPECT_EQ(spi.transactions()[0].size(), 6u) << "must clock exactly 6 bytes, matching Semtech's status read";
}

// A rejected 0x8100 is the one failure this read exists to surface: nothing else in the sequence
// distinguishes "command refused" from "written but bad".
TEST(Lr1121FirmwareUpdater, ReadUpdaterStatusDecodesARejectedCommand) {
  ScriptedSpi spi;
  MockPin rst, busy;
  Lr1121FirmwareUpdater updater(&spi, &rst, &busy);

  spi.queue_responses({0x02, 0x00, 0x00, 0x00, 0x00, 0x00});  // command_status = 0x02 >> 1 = 1 (PERR)

  Lr1121UpdaterStatus status;
  ASSERT_TRUE(updater.read_updater_status(status));
  EXPECT_EQ(status.command_status, Lr1121UpdaterCommandStatus::PERR);
}
