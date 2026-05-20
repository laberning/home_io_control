#pragma once

/// @file radio_sx1276.h
/// @brief SX1276 radio driver for IO-Homecontrol.
/// @ingroup hioc_radio
///
/// Implements the RadioDriver interface for the Semtech SX1276 transceiver.
/// Configures the chip in FSK mode with the IoHomeOn hardware feature that
/// handles CRC and packet framing specific to the IO-Homecontrol protocol.

#include "radio_interface.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace home_io_control {

// ============================================================================
// SX1276 Register Addresses (subset needed for IO-Homecontrol)
// Full register map: see Semtech SX1276 datasheet or sx1276Regs-Fsk.h
// ============================================================================

static constexpr uint8_t REG_FIFO = 0x00;         ///< FIFO read/write access
static constexpr uint8_t REG_OP_MODE = 0x01;      ///< Operating mode (sleep/standby/tx/rx)
static constexpr uint8_t REG_BITRATE_MSB = 0x02;  ///< Bit rate MSB = FXOSC / bitrate
static constexpr uint8_t REG_BITRATE_LSB = 0x03;
static constexpr uint8_t REG_FDEV_MSB = 0x04;  ///< Frequency deviation MSB
static constexpr uint8_t REG_FDEV_LSB = 0x05;
static constexpr uint8_t REG_FRF_MSB = 0x06;  ///< Carrier frequency MSB (freq = FRF * FXOSC / 2^19)
static constexpr uint8_t REG_FRF_MID = 0x07;
static constexpr uint8_t REG_FRF_LSB = 0x08;
static constexpr uint8_t REG_PA_CONFIG = 0x09;        ///< Power amplifier config (pin select + power level)
static constexpr uint8_t REG_PA_RAMP = 0x0A;          ///< PA ramp time and modulation shaping
static constexpr uint8_t REG_LNA = 0x0C;              ///< Low noise amplifier gain and boost
static constexpr uint8_t REG_RX_CONFIG = 0x0D;        ///< Receiver configuration (AFC, AGC, trigger)
static constexpr uint8_t REG_RSSI_CONFIG = 0x0E;      ///< RSSI smoothing
static constexpr uint8_t REG_RX_BW = 0x12;            ///< Receiver bandwidth
static constexpr uint8_t REG_AFC_FEI = 0x1A;          ///< AFC auto clear
static constexpr uint8_t REG_PREAMBLE_DETECT = 0x1F;  ///< Preamble detector config
static constexpr uint8_t REG_OSC = 0x24;              ///< Oscillator / clock output
static constexpr uint8_t REG_RSSI_VALUE = 0x11;       ///< Instant RSSI value in FSK mode
static constexpr uint8_t REG_PREAMBLE_MSB = 0x25;     ///< TX preamble length MSB
static constexpr uint8_t REG_PREAMBLE_LSB = 0x26;
static constexpr uint8_t REG_SYNC_CONFIG = 0x27;     ///< Sync word config (size, polarity, enable)
static constexpr uint8_t REG_SYNC_VALUE1 = 0x28;     ///< Sync word byte 1 (registers 0x28-0x2F for bytes 1-8)
static constexpr uint8_t REG_PACKET_CONFIG1 = 0x30;  ///< Packet format, CRC, encoding
static constexpr uint8_t REG_PACKET_CONFIG2 = 0x31;  ///< Packet mode, IoHomeOn, PowerFrame
static constexpr uint8_t REG_PAYLOAD_LENGTH = 0x32;  ///< Max payload length
static constexpr uint8_t REG_FIFO_THRESH = 0x35;     ///< FIFO threshold for TX start condition
static constexpr uint8_t REG_IRQ_FLAGS1 = 0x3E;      ///< IRQ flags: mode ready, preamble detect, etc.
static constexpr uint8_t REG_IRQ_FLAGS2 = 0x3F;      ///< IRQ flags: FIFO full/empty, payload ready, CRC ok
static constexpr uint8_t REG_DIO_MAPPING1 = 0x40;    ///< DIO0-DIO3 pin mapping
static constexpr uint8_t REG_DIO_MAPPING2 = 0x41;    ///< DIO4-DIO5 pin mapping
static constexpr uint8_t REG_VERSION = 0x42;         ///< Chip version (should read 0x12 for SX1276)
static constexpr uint8_t REG_PLLHOP = 0x44;          ///< PLL hop: fast frequency change without standby
static constexpr uint8_t REG_IMAGE_CAL = 0x3B;       ///< Image calibration

// SX1276 operating modes (bits [2:0] of REG_OP_MODE)
static constexpr uint8_t MODE_SLEEP = 0x00;
static constexpr uint8_t MODE_STDBY = 0x01;
static constexpr uint8_t MODE_TX = 0x03;
static constexpr uint8_t MODE_RX = 0x05;
static constexpr uint8_t MODE_MASK = 0x07;

/// SX1276 crystal oscillator frequency (32 MHz). Used to calculate register values
/// for bitrate, frequency deviation, and carrier frequency.
static constexpr uint32_t FXOSC = 32000000U;

// ============================================================================
// SX1276 Radio Driver
// ============================================================================

/// @brief SX1276 implementation of RadioDriver.
/// @ingroup hioc_radio
///
/// Manages the SX1276 via SPI using the SpiAccess interface. Configures the chip
/// in FSK mode with IoHomeOn for hardware CRC and IO‑Homecontrol packet framing.
class RadioSX1276 : public RadioDriver {
 public:
  RadioSX1276(SpiAccess *spi, InternalGPIOPin *rst_pin, InternalGPIOPin *dio0_pin, InternalGPIOPin *dio4_pin,
              uint8_t tx_power, uint8_t pa_pin)
      : RadioDriver(rst_pin),
        spi_(spi),
        dio0_pin_(dio0_pin),
        dio4_pin_(dio4_pin),
        tx_power_(tx_power),
        pa_pin_(pa_pin) {}

  /// @brief Initialize the SX1276 radio (reset, calibrate, configure registers).
  /// @return true on success; false on failure (e.g., version check).
  bool init() override;
  /// @brief Transmit a frame with specified frequency and preamble.
  /// @param data Pointer to payload bytes.
  /// @param len Payload length.
  /// @param tx_config Transmission config (frequency, preamble).
  /// @return true if transmit succeeded.
  bool send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) override;
  /// @brief Blocking wait for a packet with timeout.
  /// @param packet Output: received packet (freq, len, data).
  /// @param timeout_ms Maximum time to wait.
  /// @return true if a packet was received; false on timeout.
  bool wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) override;
  /// @brief Non‑blocking check for a received packet (called from loop).
  /// @param packet Output: received packet if any.
  /// @return true if a packet was read; false if no interrupt fired.
  bool check_for_packet(RadioRxPacket &packet) override;
  /// @brief Change RF frequency using fast hop (no standby needed).
  /// @param freq_hz New frequency in Hz.
  void change_frequency(uint32_t freq_hz) override;
  /// @brief Read instantaneous RSSI (dBm) while in RX mode (used for LBT).
  /// @return RSSI in dBm (negative).
  int16_t read_rssi() override;
  /// @brief Switch radio into continuous receive mode.
  void set_mode_rx() override;
  /// @brief Switch radio into standby mode.
  void set_mode_standby() override;
  /// @copydoc RadioDriver::is_failed
  [[nodiscard]] bool is_failed() const override { return this->failed_; }
  /// @copydoc RadioDriver::chip_name
  [[nodiscard]] const char *chip_name() const override { return "sx1276"; }
  /// @brief Dump radio‑specific debug info to log.
  void dump_debug() override;

 protected:
  // --- SPI register access ---
  /// Read an SX1276 register over SPI.
  /// @param reg Register address (7 bits, MSB clear for read).
  /// @return Register value.
  uint8_t read_register_(uint8_t reg);
  /// Write an SX1276 register over SPI.
  /// @param reg Register address (7 bits, MSB set for write).
  /// @param value Byte to write.
  void write_register_(uint8_t reg, uint8_t value);

  // --- Radio hardware control ---
  /// Set the operating mode (sleep/standby/tx/rx) and wait for mode completion.
  /// @param mode One of MODE_SLEEP, MODE_STDBY, MODE_TX, or MODE_RX.
  void set_mode_(uint8_t mode);
  /// Perform full radio configuration (called during init).
  void configure_radio_();
  /// Run image calibration routine (required after reset).
  void run_image_cal_();
  /// Wait until FIFO payload is ready (polling for TX/RX readiness).
  /// @param timeout_ms How long to wait.
  /// @param saw_dio0 Output: true if DIO0 fired.
  /// @param irq1 Output: IRQ flags 1.
  /// @param irq2 Output: IRQ flags 2.
  /// @return true if payload ready before timeout; false otherwise.
  bool poll_until_payload_ready_(uint32_t timeout_ms, bool &saw_dio0, uint8_t &irq1, uint8_t &irq2);
  /// Read a packet from the RX FIFO into a buffer.
  /// @param buf Output buffer.
  /// @param buf_size Size of buf.
  /// @return Number of bytes read.
  uint8_t read_fifo_packet_(uint8_t *buf, uint8_t buf_size);
  /// Populate last_capture_ from raw telemetry.
  /// @param blocking_wait if this was a blocking receive.
  /// @param irq1 IRQ flags 1.
  /// @param irq2 IRQ flags 2.
  /// @param rssi RSSI value.
  /// @param raw Pointer to raw bytes (may be nullptr).
  /// @param raw_len Length of raw buffer.
  /// @param frame Pointer to parsed frame bytes (may be nullptr).
  /// @param frame_len Length of parsed frame.
  void fill_capture_info_(bool blocking_wait, uint8_t irq1, uint8_t irq2, uint8_t rssi, const uint8_t *raw,
                          uint8_t raw_len, const uint8_t *frame, uint8_t frame_len);

  /// DIO0 ISR — sets dio_fired flag. Runs in interrupt context.
  static void gpio_intr(RadioSX1276 *arg);

  SpiAccess *spi_;
  InternalGPIOPin *dio0_pin_;
  InternalGPIOPin *dio4_pin_;
  uint8_t tx_power_;
  uint8_t pa_pin_;
  bool failed_{false};
};

}  // namespace home_io_control
}  // namespace esphome
