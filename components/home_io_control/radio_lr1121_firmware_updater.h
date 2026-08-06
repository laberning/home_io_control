#pragma once

/// @file radio_lr1121_firmware_updater.h
/// @brief LR1121 bootloader-mode SPI transport, standalone from the running RadioDriver.
/// @ingroup hioc_radio
///
/// Entirely wrapped in IOHOME_LR1121_FIRMWARE_UPDATE so it compiles to nothing unless a user
/// opts in (components/home_io_control/__init__.py's `lr1121_firmware_update:` block sets the
/// define). Per the design record: bootloader-mode code must not be threaded into RadioDriver or
/// RadioLR1121 — this class reimplements its own SPI transport against SpiAccess directly rather
/// than sharing RadioLR1121's, because RadioLR1121::write_command_() takes a `uint8_t` length and
/// WriteFlashEncrypted needs 4 + 256 = 260 parameter bytes in a single NSS cycle; that alone rules
/// out sharing the transport, not just the class boundary.
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

/// @brief Bootloader-mode SPI opcodes (design plan §3.2), distinct from RadioLR1121's normal-mode
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
  /// GPIO/reset sequence (design plan §3.1, no SPI opcode exists for this): drive BUSY as an
  /// output LOW, pulse RST, wait 500 ms, return BUSY to input, wait a further 100 ms — then issue
  /// a bootloader-mode GetVersion. This is a hardware reset: whatever normal-mode configuration
  /// existed before this call is gone afterward. See the ground rules in the implementation plan
  /// for what must happen next.
  /// @param type Output: `type` byte from bootloader GetVersion (LR1121_UPDATER_BOOTLOADER_TYPE
  ///        == 0xDF when the chip is genuinely in its bootloader).
  /// @param bootloader_version Output: bootloader version (major<<8 | minor).
  /// @return true on a completed SPI exchange; false on a BUSY timeout.
  bool enter_bootloader(uint8_t &type, uint16_t &bootloader_version);

  /// @brief Erase the transceiver-firmware flash region. Bootloader-mode only. Never touches the
  /// bootloader region itself — see the design record's §7 for why that keeps a failed write
  /// recoverable.
  /// @return true if the command was sent and the chip reported BUSY-low again within
  ///         LR1121_UPDATER_ERASE_BUSY_TIMEOUT_MS; false otherwise.
  bool erase_flash();

  /// @brief Write a firmware image via chunked WriteFlashEncrypted, starting at flash offset 0.
  /// @param image Big-endian 32-bit words, exactly as they appear in the published `.bin` file.
  /// @param word_count Number of words in `image`.
  /// @param on_progress Called after each chunk with (words_written_so_far, word_count); may be
  ///        an empty std::function, in which case it is not called.
  /// @return true if every chunk was written; false on the first BUSY timeout, at which point the
  ///         image is left partially written (see the implementation plan's recovery notes).
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
