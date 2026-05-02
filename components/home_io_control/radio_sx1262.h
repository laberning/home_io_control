#pragma once

/// @file radio_sx1262.h
/// @brief SX1262 radio driver for IO-Homecontrol.
///
/// Implements the RadioDriver interface for the Semtech SX1262 transceiver.
/// Unlike the SX1276, the SX1262 uses opcode-based SPI commands and requires a
/// BUSY pin check before every SPI transaction. The capture path preserves the
/// radio-reported RX metadata so offline analysis can work from trustworthy data.

#include "radio_interface.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace home_io_control {

// ============================================================================
// SX1262 Opcode Constants
// ============================================================================

static constexpr uint8_t SX1262_SET_STANDBY = 0x80;
static constexpr uint8_t SX1262_SET_RX = 0x82;
static constexpr uint8_t SX1262_SET_TX = 0x83;
static constexpr uint8_t SX1262_SET_RF_FREQUENCY = 0x86;
static constexpr uint8_t SX1262_SET_RX_TX_FALLBACK_MODE = 0x93;
static constexpr uint8_t SX1262_WRITE_BUFFER = 0x0E;
static constexpr uint8_t SX1262_READ_BUFFER = 0x1E;
static constexpr uint8_t SX1262_SET_DIO_IRQ_PARAMS = 0x08;
static constexpr uint8_t SX1262_GET_IRQ_STATUS = 0x12;
static constexpr uint8_t SX1262_GET_PACKET_STATUS = 0x14;
static constexpr uint8_t SX1262_GET_DEVICE_ERRORS = 0x17;
static constexpr uint8_t SX1262_CLEAR_IRQ_STATUS = 0x02;
static constexpr uint8_t SX1262_CLEAR_DEVICE_ERRORS = 0x07;
static constexpr uint8_t SX1262_SET_PACKET_TYPE = 0x8A;
static constexpr uint8_t SX1262_SET_MODULATION_PARAMS = 0x8B;
static constexpr uint8_t SX1262_SET_PACKET_PARAMS = 0x8C;
static constexpr uint8_t SX1262_SET_BUFFER_BASE_ADDRESS = 0x8F;
static constexpr uint8_t SX1262_SET_PA_CONFIG = 0x95;
static constexpr uint8_t SX1262_SET_TX_PARAMS = 0x8E;
static constexpr uint8_t SX1262_SET_DIO2_AS_RF_SWITCH_CTRL = 0x9D;
static constexpr uint8_t SX1262_SET_DIO3_AS_TCXO_CTRL = 0x97;
static constexpr uint8_t SX1262_CALIBRATE = 0x89;
static constexpr uint8_t SX1262_CALIBRATE_IMAGE = 0x98;
static constexpr uint8_t SX1262_GET_RX_BUFFER_STATUS = 0x13;
static constexpr uint8_t SX1262_SET_REGULATOR_MODE = 0x96;
static constexpr uint8_t SX1262_GET_STATUS = 0xC0;

// SPI register access opcodes
static constexpr uint8_t SX1262_WRITE_REGISTER = 0x0D;
static constexpr uint8_t SX1262_READ_REGISTER = 0x1D;

// ============================================================================
// SX1262 IRQ Bit Masks
// ============================================================================

static constexpr uint16_t SX1262_IRQ_TX_DONE = 0x0001;
static constexpr uint16_t SX1262_IRQ_RX_DONE = 0x0002;
static constexpr uint16_t SX1262_IRQ_SYNC_WORD_VALID = 0x0008;
static constexpr uint16_t SX1262_IRQ_CRC_ERR = 0x0040;
// Sync word register base address
static constexpr uint16_t SX1262_REG_SYNC_WORD = 0x06C0;
static constexpr uint16_t SX1262_REG_RX_GAIN = 0x08AC;
static constexpr uint16_t SX1262_REG_TX_CLAMP_CONFIG = 0x08D8;

static constexpr uint8_t SX1262_GFSK_PACKET_TYPE_KNOWN_LENGTH = 0x00;
static constexpr uint8_t SX1262_GFSK_CRC_OFF = 0x01;
static constexpr uint8_t SX1262_FALLBACK_STDBY_XOSC = 0x30;

// ============================================================================
// SX1262 Radio Driver
// ============================================================================

/// SX1262 implementation of RadioDriver.
///
/// Manages the SX1262 via opcode-based SPI using the SpiAccess interface.
/// Configures the chip in FSK mode with software CRC-CCITT to match the
/// IO-Homecontrol protocol (the SX1262 lacks the SX1276's IoHomeOn mode).
class RadioSX1262 : public RadioDriver {
 public:
  RadioSX1262(SpiAccess *spi, InternalGPIOPin *rst_pin, InternalGPIOPin *dio1_pin, InternalGPIOPin *busy_pin,
              uint8_t tx_power, uint8_t tcxo_voltage, InternalGPIOPin *fem_en_pin = nullptr,
              InternalGPIOPin *vfem_pin = nullptr, InternalGPIOPin *fem_pa_pin = nullptr)
      : spi_(spi),
        rst_pin_(rst_pin),
        dio1_pin_(dio1_pin),
        busy_pin_(busy_pin),
        tx_power_(tx_power),
        tcxo_voltage_(tcxo_voltage),
        fem_en_pin_(fem_en_pin),
        vfem_pin_(vfem_pin),
        fem_pa_pin_(fem_pa_pin) {}

  bool init() override;
  bool send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) override;
  bool wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) override;
  bool check_for_packet(RadioRxPacket &packet) override;
  void change_frequency(uint32_t freq_hz) override;
  void set_mode_rx() override;
  void set_mode_standby() override;
  bool is_failed() const override { return this->failed_; }
  const char *chip_name() const override { return "sx1262"; }
  void dump_debug() override;

  /// Diagnostic: log chip status, sync word, pin states. Called from dump_config.
 protected:
  // --- SPI communication (opcode-based) ---
  void wait_busy_();
  void write_opcode_(uint8_t opcode, const uint8_t *params, uint8_t len);
  void read_opcode_(uint8_t opcode, uint8_t *data, uint8_t len);
  void write_register_(uint16_t addr, const uint8_t *data, uint8_t len);
  void read_register_(uint16_t addr, uint8_t *data, uint8_t len);
  void write_buffer_(uint8_t offset, const uint8_t *data, uint8_t len);
  void read_buffer_(uint8_t offset, uint8_t *data, uint8_t len);

  // --- Radio configuration ---
  void configure_radio_();
  void set_packet_params_(uint16_t preamble_len, uint8_t payload_len, uint8_t packet_type, uint8_t crc_type);
  void set_rx_packet_params_();
  void clear_irq_status_(uint16_t irq_mask);
  uint16_t get_device_errors_();
  void clear_device_errors_();
  void reset_rx_state_(bool force_standby = true);
  void fill_capture_info_(bool blocking_wait, uint16_t irq_status, uint8_t rx_offset, uint8_t reported_len,
                          const uint8_t *raw, uint8_t raw_len, const uint8_t *frame, uint8_t frame_len);

  /// Read a received packet from the buffer and return the raw bytes reported by the chip.
  bool read_rx_packet_(RadioRxPacket &packet, bool blocking_wait, uint16_t irq_status);

  /// Software CRC helper kept for transmit framing parity with the current implementation.
  static uint8_t uart_encode_packet(const uint8_t *data, uint8_t len, uint8_t *encoded, uint8_t encoded_max_len);

  /// DIO1 ISR — sets dio_fired flag. Runs in interrupt context.
  static void gpio_intr(RadioSX1262 *arg);

  SpiAccess *spi_;
  InternalGPIOPin *rst_pin_;
  InternalGPIOPin *dio1_pin_;
  InternalGPIOPin *busy_pin_;
  InternalGPIOPin *fem_en_pin_;
  InternalGPIOPin *vfem_pin_;
  InternalGPIOPin *fem_pa_pin_;
  uint8_t tx_power_;
  uint8_t tcxo_voltage_;
  bool failed_{false};
};

}  // namespace home_io_control
}  // namespace esphome
