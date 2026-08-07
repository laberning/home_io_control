/// @file radio_lr1121_firmware_updater.cpp
/// @brief LR1121 bootloader-mode SPI transport implementation.
/// @ingroup hioc_radio

// See radio_lr1121_firmware_updater.h's comment on include-before-ifdef ordering: this #include
// must run before the #ifdef check below so IOHOME_LR1121_FIRMWARE_UPDATE (defined via
// esphome/core/defines.h, pulled in transitively) is visible by the time it's tested.
#include "radio_lr1121_firmware_updater.h"

#ifdef IOHOME_LR1121_FIRMWARE_UPDATE

#include "esphome/core/application.h"

#include <algorithm>

namespace esphome {
namespace home_io_control {

namespace {

/// Reset low/high pulse width for bootloader entry — same value as RadioDriver::reset_hardware_()
/// uses for the ordinary transceiver-mode reset (radio_interface.cpp); this is a different
/// sequence (BUSY is also driven here) but there is no reason for the pulse itself to differ.
constexpr uint32_t LR1121_UPDATER_RESET_PULSE_MS = 10;
/// Wait after the reset pulse while BUSY is still held as an output. Semtech's value, from
/// lr11xx_reset_to_bootloader() (SWTL001 application/src/lr11xx_firmware_update.c).
constexpr uint32_t LR1121_UPDATER_BOOTLOADER_ENTRY_WAIT_MS = 500;
/// Further wait after BUSY is returned to input, before the chip is treated as ready. Also
/// Semtech's value, from the same function.
constexpr uint32_t LR1121_UPDATER_POST_ENTRY_SETTLE_MS = 100;
/// Settle time after Reboot is sent, before reboot() returns control to the caller — see
/// reboot()'s comment for the BUSY race this closes. Unlike the two entry-sequence waits above,
/// this one has no counterpart in Semtech's source: their reference tool
/// (lr11xx_update_post_flash_reboot_and_verification()) issues the post-reboot GetVersion
/// immediately with no wait of its own for the transceiver-firmware path. It is defensive against
/// this project's own HAL/BUSY timing, sized as half of LR1121_UPDATER_POST_ENTRY_SETTLE_MS since
/// Reboot drives the same internal chip reset without the external RST/BUSY strapping that makes
/// the entry sequence heavier.
constexpr uint32_t LR1121_UPDATER_POST_REBOOT_SETTLE_MS = 50;

/// RAII guard for the bootloader-entry BUSY-as-output-LOW trick: drives BUSY as a GPIO output LOW
/// on construction, restores it to its actual configured mode on destruction (including on every
/// early-return path through enter_bootloader(), since a stuck-as-output BUSY pin would break every
/// later normal-mode read that depends on it reporting real chip state).
///
/// Restoring the *configured* mode, not a hardcoded gpio::FLAG_INPUT, matters because busy_pin
/// comes from pins.internal_gpio_input_pin_schema, which allows `pullup:`/`pulldown:` — a
/// hardcoded FLAG_INPUT would silently drop whatever pull the user configured. get_flags() is
/// safe to read here specifically because pin_mode() never mutates it: on ESP32,
/// InternalGPIOPin::pin_mode() only programs the IDF GPIO driver (direction/pull registers) and
/// the pin's own flags_ member stays whatever setup() set it to from YAML. So capturing
/// get_flags() before flipping the pin to an output and restoring exactly that value afterward
/// reproduces the pin's real configuration, pulls included, not just its direction.
class BusyPinAsResetStrapGuard {
 public:
  explicit BusyPinAsResetStrapGuard(InternalGPIOPin *busy_pin)
      : busy_pin_(busy_pin), restore_flags_(busy_pin->get_flags()) {
    this->busy_pin_->pin_mode(gpio::FLAG_OUTPUT);
    this->busy_pin_->digital_write(false);
  }
  ~BusyPinAsResetStrapGuard() { this->busy_pin_->pin_mode(this->restore_flags_); }
  BusyPinAsResetStrapGuard(const BusyPinAsResetStrapGuard &) = delete;
  BusyPinAsResetStrapGuard &operator=(const BusyPinAsResetStrapGuard &) = delete;

 private:
  InternalGPIOPin *busy_pin_;
  gpio::Flags restore_flags_;
};

}  // namespace

Lr1121FirmwareUpdater::Lr1121FirmwareUpdater(SpiAccess *spi, InternalGPIOPin *rst_pin, InternalGPIOPin *busy_pin)
    : spi_(spi), rst_pin_(rst_pin), busy_pin_(busy_pin) {
  // This class exists specifically to run before RadioLR1121::init() has ever executed (the
  // boot-time bootloader-version read), so the pins it
  // needs cannot rely on the driver having set them up already. GPIOPin::setup() is idempotent —
  // init() calling it again afterward on the same pins is harmless.
  this->rst_pin_->setup();
  this->busy_pin_->setup();
}

bool Lr1121FirmwareUpdater::wait_busy_(uint32_t timeout_ms) {
  uint32_t const start = millis();
  while (this->busy_pin_->digital_read()) {
    if (millis() - start > timeout_ms)
      return false;
    App.feed_wdt();
  }
  return true;
}

bool Lr1121FirmwareUpdater::write_command_(uint16_t opcode, const uint8_t *params, size_t len,
                                           uint32_t busy_timeout_ms) {
  if (!this->wait_busy_(busy_timeout_ms))
    return false;
  this->spi_->spi_enable();
  this->spi_->spi_write((opcode >> 8) & 0xFF);
  this->spi_->spi_write(opcode & 0xFF);
  for (size_t i = 0; i < len; i++)
    this->spi_->spi_write(params[i]);
  this->spi_->spi_disable();
  return true;
}

bool Lr1121FirmwareUpdater::read_command_(uint16_t opcode, const uint8_t *params, size_t params_len, uint8_t *out,
                                          size_t out_len, uint32_t busy_timeout_ms) {
  if (!this->write_command_(opcode, params, params_len, busy_timeout_ms))
    return false;
  if (!this->wait_busy_(busy_timeout_ms))
    return false;
  this->spi_->spi_enable();
  this->spi_->spi_read();  // Stat1 — this class has no diagnostic use for it, unlike RadioLR1121.
  for (size_t i = 0; i < out_len; i++)
    out[i] = this->spi_->spi_read();
  this->spi_->spi_disable();
  return true;
}

bool Lr1121FirmwareUpdater::read_normal_version(uint8_t &device_type, uint8_t &fw_major, uint8_t &fw_minor) {
  uint8_t resp[4] = {0};
  if (!this->read_command_(LR1121_UPDATER_CMD_GET_VERSION, nullptr, 0, resp, sizeof(resp),
                           LR1121_UPDATER_BUSY_TIMEOUT_MS))
    return false;
  // Response layout [hw, device_type, fw_major, fw_minor] — same as RadioLR1121::configure_radio_()
  // reads in normal mode; device type is byte 1, not byte 0.
  device_type = resp[1];
  fw_major = resp[2];
  fw_minor = resp[3];
  return true;
}

bool Lr1121FirmwareUpdater::enter_bootloader(uint8_t &type, uint16_t &bootloader_version) {
  {
    BusyPinAsResetStrapGuard const busy_guard(this->busy_pin_);
    this->rst_pin_->digital_write(false);
    delay(LR1121_UPDATER_RESET_PULSE_MS);
    this->rst_pin_->digital_write(true);
    delay(LR1121_UPDATER_BOOTLOADER_ENTRY_WAIT_MS);
  }  // Guard destructor returns BUSY to input here, before the post-entry settle wait.
  delay(LR1121_UPDATER_POST_ENTRY_SETTLE_MS);

  return this->read_bootloader_version(type, bootloader_version);
}

bool Lr1121FirmwareUpdater::read_bootloader_version(uint8_t &type, uint16_t &bootloader_version) {
  uint8_t resp[4] = {0};
  if (!this->read_command_(LR1121_UPDATER_CMD_GET_VERSION, nullptr, 0, resp, sizeof(resp),
                           LR1121_UPDATER_BUSY_TIMEOUT_MS))
    return false;
  // Same response layout as normal-mode GetVersion; in bootloader mode byte 1 is
  // LR1121_UPDATER_BOOTLOADER_TYPE (0xDF) and bytes 2-3 are the bootloader version, not a
  // transceiver firmware version.
  type = resp[1];
  bootloader_version = (static_cast<uint16_t>(resp[2]) << 8) | resp[3];
  return true;
}

#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE

bool Lr1121FirmwareUpdater::update_bootloader() {
  if (!this->write_command_(LR1121_UPDATER_CMD_UPDATE_BOOTLOADER, nullptr, 0, LR1121_UPDATER_BUSY_TIMEOUT_MS))
    return false;
  // Semtech's own reference tool calls GetStatus exactly once here and calls that "waiting for
  // bootloader update termination" (lr11xx_bootloader_update.c:138) -- a single poll, not a wait
  // loop. This project's erase_flash() already handles an operation that holds BUSY for an
  // internal flash write correctly; do the same here, so a hung write surfaces as a timeout rather
  // than as a verify_bootloader() read taken before the chip is actually ready.
  return this->wait_busy_(LR1121_UPDATER_BOOTLOADER_UPDATE_BUSY_TIMEOUT_MS);
}

bool Lr1121FirmwareUpdater::read_updater_status(Lr1121UpdaterStatus &status) {
  if (!this->wait_busy_(LR1121_UPDATER_BUSY_TIMEOUT_MS))
    return false;
  // No opcode: a bare NSS-low/clock/NSS-high read returns Stat1, Stat2 and the 4-byte IrqStatus,
  // matching Semtech's lr11xx_hal_direct_read() with LR11XX_BL_UPDATER_GET_STATUS_CMD_LENGTH (6).
  uint8_t data[6] = {0};
  this->spi_->spi_enable();
  for (unsigned char &byte : data)
    byte = this->spi_->spi_read();
  this->spi_->spi_disable();

  // Bit layout matches lr11xx_bootloader_updater_get_status() verbatim.
  status.interrupt_active = (data[0] & 0x01) != 0;
  status.command_status = static_cast<Lr1121UpdaterCommandStatus>(data[0] >> 1);
  status.running_from_flash = (data[1] & 0x01) != 0;
  status.chip_mode = static_cast<uint8_t>((data[1] & 0x0F) >> 1);
  status.reset_status = static_cast<uint8_t>((data[1] & 0xF0) >> 4);
  status.irq_status = (static_cast<uint32_t>(data[2]) << 24) | (static_cast<uint32_t>(data[3]) << 16) |
                      (static_cast<uint32_t>(data[4]) << 8) | static_cast<uint32_t>(data[5]);
  return true;
}

bool Lr1121FirmwareUpdater::verify_bootloader(Lr1121BootloaderVerification &report) {
  uint8_t resp[4] = {0};
  if (!this->read_command_(LR1121_UPDATER_CMD_VERIFY_BOOTLOADER, nullptr, 0, resp, sizeof(resp),
                           LR1121_UPDATER_BUSY_TIMEOUT_MS))
    return false;
  // Bit layout matches SWTL001's lr11xx_bootloader_updater_verify_bootloader() verbatim
  // (bootloader_updater_driver/src/lr11xx_bootloader_updater.c:170-179) -- the only public
  // specification for this response; the User Manual predates the bootloader updater entirely.
  const uint8_t check_byte = resp[0];
  report.signature_verified = (check_byte & 0x01) != 0;
  report.version_verified = (check_byte & 0x02) != 0;
  report.use_case_verified = (check_byte & 0x04) != 0;
  report.version_major_verified = (check_byte & 0x08) != 0;
  report.version_minor_verified = (check_byte & 0x10) != 0;
  report.anti_rollback_verified = (check_byte & 0x20) != 0;
  report.use_case = resp[1];
  report.version_major = resp[2];
  report.version_minor = resp[3];
  return true;
}

bool Lr1121FirmwareUpdater::updater_reboot(bool stay_in_bootloader) {
  uint8_t const param = stay_in_bootloader ? 0x03 : 0x00;
  if (!this->write_command_(LR1121_UPDATER_CMD_UPDATER_REBOOT, &param, 1, LR1121_UPDATER_BUSY_TIMEOUT_MS))
    return false;
  // Same BUSY race as reboot() -- see its comment. The caller's very next step is always a
  // read_bootloader_version() to check whether the chip stayed in the bootloader, which is exactly
  // the kind of immediately-following wait_busy_() reboot()'s comment warns about.
  delay(LR1121_UPDATER_POST_REBOOT_SETTLE_MS);
  return true;
}

#endif  // IOHOME_LR1121_BOOTLOADER_UPDATE

bool Lr1121FirmwareUpdater::erase_flash() {
  if (!this->write_command_(LR1121_UPDATER_CMD_ERASE_FLASH, nullptr, 0, LR1121_UPDATER_BUSY_TIMEOUT_MS))
    return false;
  // EraseFlash holds BUSY high for the whole erase (seconds-scale) rather than for the brief
  // processing window ordinary commands need, so this waits it out with its own generous budget
  // before handing control back — callers should never need to know this detail.
  return this->wait_busy_(LR1121_UPDATER_ERASE_BUSY_TIMEOUT_MS);
}

bool Lr1121FirmwareUpdater::write_image(const uint32_t *image, size_t word_count,
                                        const std::function<void(size_t, size_t)> &on_progress) {
  size_t written = 0;
  while (written < word_count) {
    size_t const chunk_words = std::min<size_t>(LR1121_UPDATER_FLASH_CHUNK_WORDS, word_count - written);
    uint32_t const offset_bytes = static_cast<uint32_t>(written * sizeof(uint32_t));

    uint8_t params[4 + LR1121_UPDATER_FLASH_CHUNK_WORDS * 4];
    params[0] = static_cast<uint8_t>(offset_bytes >> 24);
    params[1] = static_cast<uint8_t>(offset_bytes >> 16);
    params[2] = static_cast<uint8_t>(offset_bytes >> 8);
    params[3] = static_cast<uint8_t>(offset_bytes);
    for (size_t i = 0; i < chunk_words; i++) {
      uint32_t const word = image[written + i];
      params[4 + i * 4 + 0] = static_cast<uint8_t>(word >> 24);
      params[4 + i * 4 + 1] = static_cast<uint8_t>(word >> 16);
      params[4 + i * 4 + 2] = static_cast<uint8_t>(word >> 8);
      params[4 + i * 4 + 3] = static_cast<uint8_t>(word);
    }

    if (!this->write_command_(LR1121_UPDATER_CMD_WRITE_FLASH_ENCRYPTED, params, 4 + chunk_words * 4,
                              LR1121_UPDATER_BUSY_TIMEOUT_MS))
      return false;

    written += chunk_words;
    // 255 chunks of SPI without yielding is itself long enough to matter for the watchdog, on
    // top of whatever wait_busy_() already fed while waiting for each chunk's BUSY.
    App.feed_wdt();
    if (on_progress)
      on_progress(written, word_count);
  }
  return true;
}

bool Lr1121FirmwareUpdater::read_hash(uint8_t *out, size_t out_len) {
  if (out_len < LR1121_UPDATER_HASH_LENGTH)
    return false;
  return this->read_command_(LR1121_UPDATER_CMD_GET_HASH, nullptr, 0, out, LR1121_UPDATER_HASH_LENGTH,
                             LR1121_UPDATER_BUSY_TIMEOUT_MS);
}

bool Lr1121FirmwareUpdater::reboot(bool stay_in_bootloader) {
  uint8_t const param = stay_in_bootloader ? 0x03 : 0x00;
  if (!this->write_command_(LR1121_UPDATER_CMD_REBOOT, &param, 1, LR1121_UPDATER_BUSY_TIMEOUT_MS))
    return false;
  // wait_busy_() samples BUSY the instant it's called; nothing guarantees the chip has reacted to
  // Reboot and reasserted BUSY yet, so a caller's very next wait_busy_() (e.g. the post-flash
  // read_normal_version() verification) could sample BUSY before the chip drives it, return
  // immediately, and clock a command into a chip that is still mid-reset -- reading garbage. See
  // LR1121_UPDATER_POST_REBOOT_SETTLE_MS above for why this specific value and why it applies
  // here rather than only at the post-flash call site.
  delay(LR1121_UPDATER_POST_REBOOT_SETTLE_MS);
  return true;
}

}  // namespace home_io_control
}  // namespace esphome

#endif  // IOHOME_LR1121_FIRMWARE_UPDATE
