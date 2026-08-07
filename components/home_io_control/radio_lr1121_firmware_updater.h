#pragma once

/// @file radio_lr1121_firmware_updater.h
/// @brief LR1121 bootloader-mode-*and*-loader-mode SPI transport, standalone from the running RadioDriver.
/// @ingroup hioc_radio
///
/// Entirely wrapped in IOHOME_LR1121_FIRMWARE_UPDATE so it compiles to nothing unless a user
/// opts in (components/home_io_control/__init__.py's `lr1121_firmware_update:` block sets the
/// define). Bootloader-mode code is deliberately kept out of RadioDriver and
/// RadioLR1121 — this class reimplements its own SPI transport against SpiAccess directly rather
/// than sharing RadioLR1121's, because RadioLR1121::write_command_() takes a `uint8_t` length and
/// WriteFlashEncrypted needs 4 + 256 = 260 parameter bytes in a single NSS cycle; that alone rules
/// out sharing the transport, not just the class boundary.
///
/// The transport itself is mode-agnostic and always was: `update_bootloader()`/
/// `verify_bootloader()`/`updater_reboot()` (gated behind IOHOME_LR1121_BOOTLOADER_UPDATE, the
/// bootloader-*rewrite* feature — see ADR 0021) send the `0x81xx` opcode family
/// while the chip is running the special *loader* transceiver firmware in NORMAL mode, not while
/// it is in the bootloader — the `0x8xxx` prefix misleads. Only this class's previous users
/// (bootloader-mode only) made it look bootloader-specific.
///
/// Takes the firmware image as a `(const uint32_t *, size_t)` parameter and never `#include`s the
/// generated image header, so it stays host-testable with a small synthetic image and the ~64 KB
/// blob stays out of every translation unit except the one that generated it.

// IOHOME_LR1121_FIRMWARE_UPDATE is only visible after something pulls in esphome/core/defines.h
// (ESPHome codegen's cg.add_define() lands there, not as a compiler -D flag) — the #include below
// must run before the #ifdef check, not after, or this file silently compiles to an empty
// translation unit in a real ESPHome build even though it works fine in host unit tests (whose
// Makefile passes -DIOHOME_LR1121_FIRMWARE_UPDATE directly, masking the ordering bug).
#include "radio_interface.h"
#include "esphome/core/hal.h"

#ifdef IOHOME_LR1121_FIRMWARE_UPDATE

#include <cstddef>
#include <cstdint>
#include <functional>

namespace esphome {
namespace home_io_control {

/// @brief Bootloader-mode SPI opcodes, distinct from RadioLR1121's normal-mode
/// opcode table — this class never touches that table or RadioLR1121 at all.
inline constexpr uint16_t LR1121_UPDATER_CMD_GET_VERSION = 0x0101;  ///< Same opcode in both normal and bootloader mode.
inline constexpr uint16_t LR1121_UPDATER_CMD_ERASE_FLASH = 0x8000;
inline constexpr uint16_t LR1121_UPDATER_CMD_WRITE_FLASH_ENCRYPTED = 0x8003;
inline constexpr uint16_t LR1121_UPDATER_CMD_GET_HASH = 0x8004;
inline constexpr uint16_t LR1121_UPDATER_CMD_REBOOT = 0x8005;

/// @brief `type` byte GetVersion reports while running the bootloader (LR11XX_TYPE_PRODUCTION_MODE).
inline constexpr uint8_t LR1121_UPDATER_BOOTLOADER_TYPE = 0xDF;

/// @brief Bytes in a GetHash (0x8004) response (Semtech's `LR11XX_BL_HASH_LENGTH` /
/// `lr11xx_bootloader_hash_t`). See read_hash()'s doc comment and the .cpp for what is (and is
/// not) established about what this hash covers and what it should be compared against.
inline constexpr size_t LR1121_UPDATER_HASH_LENGTH = 16;

/// @brief Words per WriteFlashEncrypted chunk (LR11XX_FLASH_DATA_MAX_LENGTH_UINT32) — 256 bytes of
/// payload, 262 bytes total with the 2-byte opcode and 4-byte offset in one NSS cycle.
inline constexpr size_t LR1121_UPDATER_FLASH_CHUNK_WORDS = 64;

/// @brief BUSY-wait timeout for ordinary bootloader commands (GetVersion, WriteFlashEncrypted
/// chunks, Reboot) — same order of magnitude as RadioLR1121's normal-mode LR1121_BUSY_TIMEOUT_MS.
inline constexpr uint32_t LR1121_UPDATER_BUSY_TIMEOUT_MS = 3000;
/// @brief BUSY-wait timeout for EraseFlash, which holds BUSY high for the whole erase
/// (seconds-scale) — the ordinary 3 s timeout is marginal for that.
inline constexpr uint32_t LR1121_UPDATER_ERASE_BUSY_TIMEOUT_MS = 30000;

#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE
/// @brief NORMAL-mode opcodes of the *loader* firmware (ADR 0021) — not a
/// bootloader-mode command set, despite the `0x8xxx` prefix shared with LR1121_UPDATER_CMD_ERASE_FLASH
/// etc. above. Gated separately from the rest of this file's opcodes because they are only ever
/// meaningful once the bootloader-*rewrite* feature (a superset of the base transceiver-update
/// feature) is compiled in.
/// @brief `type` byte the *loader* firmware reports from a normal-mode GetVersion once it is
/// running (i.e. after Stage 1b's reboot).
///
/// Undocumented by Semtech, whose own tool prints this byte and never checks it. Value observed on
/// real hardware 2026-08-07 (LilyGO T3-S3, `lr1121_loader_2100.bin`): 0xDE. Note how close that is
/// to LR1121_UPDATER_BOOTLOADER_TYPE (0xDF) — one bit — which is exactly why the Stage 1b
/// checkpoint checks this byte positively rather than merely asserting "not 0xDF".
///
/// Safe to pin: the only configuration that can ever reach Stage 1b is loader 0x2100 on a chip
/// whose bootloader is 0x2100 (lr1121_bootloader_upgrade_path()'s equality rule plus the
/// requires-newer rule), so the loader image involved is always this exact one. If a future loader
/// version ever becomes reachable, this check failing is the *safe* direction — it aborts before
/// the irreversible write, leaving the bootloader untouched.
inline constexpr uint8_t LR1121_UPDATER_LOADER_DEVICE_TYPE = 0xDE;

inline constexpr uint16_t LR1121_UPDATER_CMD_UPDATE_BOOTLOADER = 0x8100;
inline constexpr uint16_t LR1121_UPDATER_CMD_VERIFY_BOOTLOADER = 0x8101;
inline constexpr uint16_t LR1121_UPDATER_CMD_UPDATER_REBOOT = 0x8102;

/// @brief BUSY-wait timeout for 0x8100 UpdateBootloader, which — like EraseFlash — holds BUSY
/// high for the duration of an internal flash operation. Real-hardware timing is not yet known
/// — it is the one value in this feature never measured on real hardware — so this borrows
/// LR1121_UPDATER_ERASE_BUSY_TIMEOUT_MS's budget until a real measurement narrows it.
inline constexpr uint32_t LR1121_UPDATER_BOOTLOADER_UPDATE_BUSY_TIMEOUT_MS = LR1121_UPDATER_ERASE_BUSY_TIMEOUT_MS;

/// @brief `command_status` field of Stat1, as reported by the loader firmware's status read.
/// Values from Semtech's lr11xx_bootloader_updater_command_status_t.
enum class Lr1121UpdaterCommandStatus : uint8_t {
  FAIL = 0x00,  ///< The last command was not executed.
  PERR = 0x01,  ///< The last command had a parameter error.
  OK = 0x02,    ///< The last command was executed.
  DATA = 0x03,  ///< The last command was executed and data is available.
};

/// @brief Decoded status read (Stat1/Stat2/IrqStatus) taken between UpdateBootloader and
/// VerifyBootloader. Diagnostic: it says whether the chip *accepted* 0x8100 at all, which nothing
/// else in the sequence reports directly -- a rejected command otherwise only surfaces as a failed
/// verify, with no way to tell "rejected" from "written but bad".
struct Lr1121UpdaterStatus {
  Lr1121UpdaterCommandStatus command_status = Lr1121UpdaterCommandStatus::FAIL;
  bool interrupt_active = false;
  bool running_from_flash = false;
  uint8_t chip_mode = 0;
  uint8_t reset_status = 0;
  uint32_t irq_status = 0;
};

/// @brief Decoded 0x8101 VerifyBootloader response (bootloader_updater_driver's
/// lr11xx_bootloader_updater_verification_report_t — the only public specification for this
/// layout; see verify_bootloader()'s .cpp comment for the exact bit mapping).
struct Lr1121BootloaderVerification {
  bool signature_verified = false;
  bool version_verified = false;
  bool use_case_verified = false;
  bool version_major_verified = false;
  bool version_minor_verified = false;
  bool anti_rollback_verified = false;  ///< Semantics undocumented by Semtech (ADR 0021); never a gate.
  uint8_t use_case = 0;
  uint8_t version_major = 0;
  uint8_t version_minor = 0;

  /// @return true only when all six checks in the report are true. Semtech's own gate
  ///         (lr11xx_bootloader_update.c:242-247) requires every one before declaring success;
  ///         this project does the same rather than second-guessing which bits matter.
  [[nodiscard]] bool all_checks_passed() const {
    return this->signature_verified && this->version_verified && this->use_case_verified &&
           this->version_major_verified && this->version_minor_verified && this->anti_rollback_verified;
  }
};
#endif  // IOHOME_LR1121_BOOTLOADER_UPDATE

/// @brief Bootloader-mode SPI transport and update sequence for the LR1121, used only while
/// `IOHOME_LR1121_FIRMWARE_UPDATE` is compiled in.
/// @ingroup hioc_radio
class Lr1121FirmwareUpdater {
 public:
  /// @param spi SPI access, normally the hub itself (same wiring RadioLR1121 uses).
  /// @param rst_pin Radio reset pin (same physical wire RadioLR1121 uses).
  /// @param busy_pin Radio BUSY pin (same physical wire RadioLR1121 uses); temporarily driven as
  ///        a GPIO output during bootloader entry — see enter_bootloader().
  Lr1121FirmwareUpdater(SpiAccess *spi, InternalGPIOPin *rst_pin, InternalGPIOPin *busy_pin);

  /// @brief Read the transceiver firmware version while the chip is running normal (non-bootloader)
  /// firmware — normal-mode GetVersion (0x0101).
  /// @param device_type Output: chip identity byte (LR1121_DEVICE_TYPE == 0x03 when correct).
  /// @param fw_major Output: firmware major version byte.
  /// @param fw_minor Output: firmware minor version byte.
  /// @return true on a completed SPI exchange; false on a BUSY timeout (chip unresponsive).
  bool read_normal_version(uint8_t &device_type, uint8_t &fw_major, uint8_t &fw_minor);

  /// @brief Reset the chip into bootloader mode and read its identity there.
  ///
  /// GPIO/reset sequence (no SPI opcode exists for this): drive BUSY as an
  /// output LOW, pulse RST, wait 500 ms, return BUSY to input, wait a further 100 ms — then issue
  /// a bootloader-mode GetVersion. This is a hardware reset: whatever normal-mode configuration
  /// existed before this call is gone afterward, so the caller must then either run
  /// radio_->init() (boot-time only) or App.safe_reboot() — never simply return (ADR 0020).
  /// @param type Output: `type` byte from bootloader GetVersion (LR1121_UPDATER_BOOTLOADER_TYPE
  ///        == 0xDF when the chip is genuinely in its bootloader).
  /// @param bootloader_version Output: bootloader version (major<<8 | minor).
  /// @return true on a completed SPI exchange; false on a BUSY timeout.
  bool enter_bootloader(uint8_t &type, uint16_t &bootloader_version);

  /// @brief Read the bootloader-mode GetVersion response without running enter_bootloader()'s
  /// RST-pulse/BUSY-strap entry sequence again.
  ///
  /// Factored out of enter_bootloader()'s tail so a caller already sitting in the chip's bootloader
  /// (or, for the bootloader-rewrite feature, in the special *loader* firmware answering the same
  /// opcode in normal mode) can re-read without a fresh hardware-reset excursion. Not gated behind
  /// IOHOME_LR1121_BOOTLOADER_UPDATE: enter_bootloader() itself calls this unconditionally, so it
  /// must exist in every build of this class.
  /// @param type Output: `type` byte (LR1121_UPDATER_BOOTLOADER_TYPE == 0xDF while genuinely in
  ///        the bootloader).
  /// @param bootloader_version Output: bootloader version (major<<8 | minor).
  /// @return true on a completed SPI exchange; false on a BUSY timeout.
  bool read_bootloader_version(uint8_t &type, uint16_t &bootloader_version);

#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE
  /// @brief Request the bootloader rewrite (0x8100 UpdateBootloader).
  ///
  /// NORMAL-mode command of the *loader* firmware, not a bootloader-mode command — see this file's
  /// header comment for why the `0x8xxx` prefix misleads here.
  /// No parameters. Blocks on its own explicit BUSY wait before returning, unlike Semtech's
  /// reference tool, which calls GetStatus exactly once and calls that "waiting for bootloader
  /// update termination" — see the .cpp for why that gap is not repeated here.
  /// @return true if the command was sent and BUSY cleared within
  ///         LR1121_UPDATER_BOOTLOADER_UPDATE_BUSY_TIMEOUT_MS; false otherwise.
  bool update_bootloader();

  /// @brief Read Stat1/Stat2/IrqStatus with a bare 6-byte SPI read and no opcode (Semtech's
  /// `lr11xx_hal_direct_read` shape).
  ///
  /// Issued between UpdateBootloader and VerifyBootloader, where Semtech's reference tool issues
  /// exactly the same transaction. Kept for two reasons: it keeps this project's wire traffic
  /// byte-for-byte identical to the vendor's known-working sequence through the one stage that
  /// cannot be tested without destroying a chip, and its `command_status` is the only direct
  /// report of whether 0x8100 was accepted. Diagnostic only, like Semtech's own use of it — the
  /// gate is verify_bootloader()'s six check bits.
  /// @param status Output: decoded Stat1/Stat2/IrqStatus.
  /// @return true on a completed SPI exchange; false on a BUSY timeout.
  bool read_updater_status(Lr1121UpdaterStatus &status);

  /// @brief Read the bootloader-update verification report (0x8101 VerifyBootloader). Same
  /// normal-mode-of-the-loader caveat as update_bootloader().
  /// @param report Output: the six check bits plus use-case/version bytes.
  /// @return true on a completed SPI exchange; false on a BUSY timeout.
  bool verify_bootloader(Lr1121BootloaderVerification &report);

  /// @brief Reboot the loader firmware (0x8102 Reboot). Same normal-mode-of-the-loader caveat, and
  /// the same wire encoding as reboot() (param 0x03/0x00) — kept as a separate method rather than
  /// reused only because the opcode differs. "Success" after this call is the chip *staying* in
  /// the bootloader and reporting the new version rather than booting — the freshly written
  /// bootloader is expected to refuse the loader image, which was built for the old one —
  /// callers must re-read with read_bootloader_version(), not assume a boot happened.
  /// @param stay_in_bootloader Same meaning as reboot()'s parameter.
  /// @return true if the command was sent; false on a BUSY timeout.
  bool updater_reboot(bool stay_in_bootloader);
#endif  // IOHOME_LR1121_BOOTLOADER_UPDATE

  /// @brief Erase the transceiver-firmware flash region. Bootloader-mode only. Never touches the
  /// bootloader region itself, which is what keeps a failed write recoverable: bootloader-mode
  /// entry is a GPIO strap, so it works regardless of what the transceiver region holds.
  /// @return true if the command was sent and the chip reported BUSY-low again within
  ///         LR1121_UPDATER_ERASE_BUSY_TIMEOUT_MS; false otherwise.
  bool erase_flash();

  /// @brief Write a firmware image via chunked WriteFlashEncrypted, starting at flash offset 0.
  /// @param image Big-endian 32-bit words, exactly as they appear in the published `.bin` file.
  /// @param word_count Number of words in `image`.
  /// @param on_progress Called after each chunk with (words_written_so_far, word_count); may be
  ///        an empty std::function, in which case it is not called.
  /// @return true if every chunk was written; false on the first BUSY timeout, at which point the
  ///         image is left partially written — recoverable, since the bootloader region is
  ///         untouched and a retry can re-enter and rewrite.
  bool write_image(const uint32_t *image, size_t word_count, const std::function<void(size_t, size_t)> &on_progress);

  /// @brief Read the bootloader's hash of flash content (GetHash, 0x8004). Bootloader-mode only.
  ///
  /// INFORMATIONAL ONLY. Semtech's own reference updater (`lr11xx_update_firmware()`) never calls
  /// this command, and its published header documents only that it "get[s] calculated hash of
  /// flash content" — no algorithm, no stated hashed range (whole flash vs. just-written region),
  /// and therefore no documented host-side expected value to compare against. This class exposes
  /// the raw read so the caller can log it for the user to compare by hand; it deliberately does
  /// not attempt an automated pass/fail comparison against anything, because that would mean
  /// guessing a check we cannot currently verify — see the caller for how the value is used.
  /// @param out Buffer for the hash bytes; must be at least LR1121_UPDATER_HASH_LENGTH long.
  /// @param out_len Size of `out`. Caller-supplied (rather than a fixed-size array parameter) so
  ///        this header doesn't need to hardcode the same length twice.
  /// @return true on a completed SPI exchange; false on a BUSY timeout, or if `out_len` is too
  ///         small (in which case nothing is sent).
  bool read_hash(uint8_t *out, size_t out_len);

  /// @brief Reboot the chip out of the bootloader command loop.
  ///
  /// Includes a settle delay after the command is sent, before returning — see the .cpp for the
  /// BUSY race this closes and why the delay lives here rather than only at one call site.
  /// @param stay_in_bootloader true to remain in the bootloader (param 0x03); false to boot the
  ///        (newly written, or still the pre-existing) transceiver firmware (param 0x00).
  /// @return true if the command was sent; false on a BUSY timeout.
  bool reboot(bool stay_in_bootloader);

 private:
  /// @brief Block until BUSY reads low, feeding the watchdog while waiting.
  /// @param timeout_ms Per-call timeout — see LR1121_UPDATER_BUSY_TIMEOUT_MS /
  ///        LR1121_UPDATER_ERASE_BUSY_TIMEOUT_MS.
  /// @return true if BUSY went low in time; false on timeout.
  bool wait_busy_(uint32_t timeout_ms);
  /// @brief Wait for BUSY, then write opcode + params in a single NSS cycle (write-only).
  /// @param len Parameter length. `size_t`, not `uint8_t`: WriteFlashEncrypted's 4-byte offset
  ///        plus up to 256 bytes of payload is 260 bytes, which does not fit a byte length.
  /// @return true on success; false if the preceding BUSY wait timed out.
  bool write_command_(uint16_t opcode, const uint8_t *params, size_t len, uint32_t busy_timeout_ms);
  /// @brief write_command_(), then wait BUSY again and clock out a Stat1 byte (discarded — this
  /// class has no diagnostic use for it) followed by `out_len` response bytes.
  /// @return true on success; false if either BUSY wait timed out.
  bool read_command_(uint16_t opcode, const uint8_t *params, size_t params_len, uint8_t *out, size_t out_len,
                     uint32_t busy_timeout_ms);

  SpiAccess *spi_;
  InternalGPIOPin *rst_pin_;
  InternalGPIOPin *busy_pin_;
};

}  // namespace home_io_control
}  // namespace esphome

#endif  // IOHOME_LR1121_FIRMWARE_UPDATE
