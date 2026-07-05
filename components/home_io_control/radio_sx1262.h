#pragma once

/// @file radio_sx1262.h
/// @brief SX1262 radio driver for IO-Homecontrol.
/// @ingroup hioc_radio
///
/// Implements the RadioDriver interface for the Semtech SX1262 transceiver.
/// Unlike the SX1276, the SX1262 uses opcode-based SPI commands and requires a
/// BUSY pin check before every SPI transaction. The capture path preserves the
/// radio-reported RX metadata so offline analysis can work from trustworthy data.
/// @todo Validate Heltec V4-family boards on real hardware, especially the assumed
///       front-end module enable pins and the required TCXO voltage selection.

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
static constexpr uint8_t SX1262_GET_RSSI_INST = 0x15;
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
static constexpr uint16_t SX1262_IRQ_PREAMBLE_DETECTED = 0x0004;
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

/// @brief SX1262 implementation of RadioDriver.
/// @ingroup hioc_radio
///
/// Manages the SX1262 via opcode‑based SPI using the SpiAccess interface.
/// Configures the chip in FSK mode with software CRC‑CCITT to match the
/// IO‑Homecontrol protocol (the SX1262 lacks the SX1276's IoHomeOn mode).
/// @todo Confirm whether additional SX1262 board variants need board-specific FEM
///       defaults beyond the currently documented Heltec V3/V4 assumptions.
class RadioSX1262 : public RadioDriver {
 public:
  RadioSX1262(SpiAccess *spi, InternalGPIOPin *rst_pin, InternalGPIOPin *dio1_pin, InternalGPIOPin *busy_pin,
              uint8_t tx_power, uint8_t tcxo_voltage, InternalGPIOPin *fem_en_pin = nullptr,
              InternalGPIOPin *vfem_pin = nullptr, InternalGPIOPin *fem_pa_pin = nullptr)
      : RadioDriver(rst_pin),
        spi_(spi),
        dio1_pin_(dio1_pin),
        busy_pin_(busy_pin),
        tx_power_(tx_power),
        tcxo_voltage_(tcxo_voltage),
        fem_en_pin_(fem_en_pin),
        vfem_pin_(vfem_pin),
        fem_pa_pin_(fem_pa_pin) {}

  /// @copydoc RadioDriver::init
  bool init() override;
  /// @copydoc RadioDriver::send_packet
  bool send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) override;
  /// @copydoc RadioDriver::wait_for_packet
  bool wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) override;
  /// @copydoc RadioDriver::check_for_packet
  bool check_for_packet(RadioRxPacket &packet) override;
  /// @copydoc RadioDriver::change_frequency
  void change_frequency(uint32_t freq_hz) override;
  /// @copydoc RadioDriver::read_rssi
  int16_t read_rssi() override;
  /// @copydoc RadioDriver::is_sync_detected
  bool is_sync_detected() override;
  /// @copydoc RadioDriver::is_preamble_detected
  bool is_preamble_detected() override;
  /// @copydoc RadioDriver::response_preamble
  [[nodiscard]] uint16_t response_preamble() const override { return this->response_preamble_; }
  /// @copydoc RadioDriver::set_rx_bandwidth
  void set_rx_bandwidth(SX1262RxBandwidth bandwidth) override;
  /// @copydoc RadioDriver::set_response_preamble
  void set_response_preamble(uint16_t preamble) override { this->response_preamble_ = preamble; }
  /// @copydoc RadioDriver::set_post_tx_settle_us
  void set_post_tx_settle_us(uint16_t delay_us) override { this->post_tx_settle_us_ = delay_us; }
  /// @copydoc RadioDriver::set_mode_rx
  void set_mode_rx() override;
  /// @copydoc RadioDriver::set_mode_standby
  void set_mode_standby() override;
  [[nodiscard]] bool is_failed() const override { return this->failed_; }
  [[nodiscard]] const char *chip_name() const override { return "sx1262"; }
  /// @brief Dump SX1262‑specific debug info.
  void dump_debug() override;

 protected:
  // --- SPI communication (opcode‑based) ---
  /// Wait until BUSY pin is low before any SPI transaction.
  void wait_busy_();
  /// Write an opcode with optional parameter bytes.
  /// @param opcode SX1262 opcode.
  /// @param params Pointer to parameter buffer (may be nullptr).
  /// @param len Parameter length.
  void write_opcode_(uint8_t opcode, const uint8_t *params, uint8_t len);
  /// Read response from an opcode.
  /// @param opcode Opcode that was previously written.
  /// @param data Output buffer.
  /// @param len Expected number of bytes to read.
  void read_opcode_(uint8_t opcode, uint8_t *data, uint8_t len);
  /// Write to a register (SX1262 uses opcodes for register access).
  /// @param addr 16‑bit register address.
  /// @param data Pointer to data bytes.
  /// @param len Number of bytes.
  void write_register_(uint16_t addr, const uint8_t *data, uint8_t len);
  /// Read from a register.
  /// @param addr 16‑bit register address.
  /// @param data Output buffer.
  /// @param len Number of bytes to read.
  void read_register_(uint16_t addr, uint8_t *data, uint8_t len);
  /// Write into the SX1262 TX/RX buffer at a given offset.
  /// @param offset Buffer offset.
  /// @param data Payload bytes.
  /// @param len Payload length.
  void write_buffer_(uint8_t offset, const uint8_t *data, uint8_t len);
  /// Read from the SX1262 RX buffer.
  /// @param offset Buffer offset.
  /// @param data Output buffer.
  /// @param len Number of bytes to read.
  void read_buffer_(uint8_t offset, uint8_t *data, uint8_t len);

  // --- Radio configuration ---
  /// Full radio initialization (called from init()).
  void configure_radio_();
  /// Set RF frequency via the frequency register.
  /// @param freq_hz Frequency in Hz.
  void set_frequency_register_(uint32_t freq_hz);
  /// Configure packet parameters (preamble, payload length, CRC).
  /// @param preamble_len Preamble length in symbols.
  /// @param payload_len Expected payload length.
  /// @param packet_type Fixed for GFSK.
  /// @param crc_type CRC configuration (off or on).
  void set_packet_params_(uint16_t preamble_len, uint8_t payload_len, uint8_t packet_type, uint8_t crc_type);
  /// Apply the runtime bandwidth setting to the SX1262 modulation parameters.
  void write_modulation_params_();
  /// Apply RX‑specific packet parameters (calls set_packet_params_ for RX).
  void set_rx_packet_params_();
  /// Clear all IRQ status bits.
  /// @param irq_mask Bitmask of IRQs to clear.
  void clear_irq_status_(uint16_t irq_mask);
  /// @brief Read device error flags (and clear them).
  /// @return Error bitmask.
  uint16_t get_device_errors_();
  /// @brief Clear device error flags.
  void clear_device_errors_();
  /// Reset RX state machine and buffer. Optionally force standby first.
  /// @param force_standby If true, switch to standby before reset.
  void reset_rx_state_(bool force_standby = true);
  /// Populate the RadioCaptureInfo from SX1262‑specific telemetry.
  /// @param blocking_wait true if this was a blocking wait.
  /// @param irq_status Raw IRQ status.
  /// @param rx_offset Reported RX buffer offset.
  /// @param reported_len Length reported by the radio.
  /// @param raw Pointer to raw buffer bytes (may be nullptr).
  /// @param raw_len Length of raw buffer.
  /// @param frame Pointer to parsed frame bytes (may be nullptr).
  /// @param frame_len Length of parsed frame.
  void fill_capture_info_(bool blocking_wait, uint16_t irq_status, uint8_t rx_offset, uint8_t reported_len,
                          const uint8_t *raw, uint8_t raw_len, const uint8_t *frame, uint8_t frame_len);

  /// Read a received packet from the buffer and return the raw bytes reported by the chip.
  /// This is virtual to allow test doubles.
  /// @param packet Output RadioRxPacket.
  /// @param blocking_wait true if called from a blocking wait path.
  /// @param irq_status Raw IRQ status word.
  /// @return true if a valid packet was extracted; false otherwise.
  virtual bool read_rx_packet(RadioRxPacket &packet, bool blocking_wait, uint16_t irq_status);

  /// Software CRC helper kept for transmit framing parity with the current implementation.
  /// @return Number of encoded bytes, or 0 if buffer too small.
  static uint8_t uart_encode_packet(const uint8_t *data, uint8_t len, uint8_t *encoded, uint8_t encoded_max_len);

  /// DIO1 ISR — sets dio_fired flag. Runs in interrupt context.
  static void gpio_intr(RadioSX1262 *arg);

  /// @brief Read the raw IRQ status from the radio.
  /// @return 16‑bit IRQ status word.
  virtual uint16_t read_irq_status_raw();

 private:
  // === wait_for_packet helpers (private) ===
  /// Poll until any radio activity (DIO1) or timeout.
  /// @param start Starting timestamp.
  /// @param timeout_ms Maximum wait.
  /// @param saw_dio1 Output: true if DIO1 fired.
  /// @param irq Output: captured IRQ status.
  /// @return true if activity detected before timeout; false otherwise.
  bool poll_until_activity_(uint32_t start, uint32_t timeout_ms, bool &saw_dio1, uint16_t &irq);
  /// Resolve the race between SYNC word detection and payload‑ready by peeking
  /// the RX buffer status and potentially restarting RX.
  /// @param start Starting timestamp.
  /// @param timeout_ms Maximum wait.
  /// @param irq Output: final IRQ status when success is determined.
  /// @return true if a frame boundary is resolved; false on timeout or error.
  bool resolve_sync_race_(uint32_t start, uint32_t timeout_ms, uint16_t &irq);
  /// Finalize receive: populate packet from the RX buffer and telemetry.
  /// @param packet Output RadioRxPacket.
  /// @param irq IRQ status at completion.
  /// @return true if packet extraction succeeded; false otherwise.
  bool finalize_receive_(RadioRxPacket &packet, uint16_t irq);

  SpiAccess *spi_;
  InternalGPIOPin *dio1_pin_;
  InternalGPIOPin *busy_pin_;
  InternalGPIOPin *fem_en_pin_;
  InternalGPIOPin *vfem_pin_;
  InternalGPIOPin *fem_pa_pin_;
  uint8_t tx_power_;
  uint8_t tcxo_voltage_;
  bool failed_{false};
  SX1262RxBandwidth rx_bandwidth_{SX1262RxBandwidth::BW_117_3_KHZ};  ///< Runtime-tunable RX bandwidth.
  uint16_t response_preamble_{SX1262_RESPONSE_PREAMBLE};             ///< Runtime-tunable response preamble.
  uint16_t post_tx_settle_us_{SX1262_POST_TX_SETTLE_US};             ///< Runtime-tunable post-TX settling delay.
};

// ============================================================================
// UART probe helpers (non-member, declared here for unit-test access)
// ============================================================================

/// @brief Result of the UART probe: best candidate frame within a raw capture.
struct UartProbeResult {
  bool valid{false};                            ///< A plausible frame was found.
  uint8_t bit_offset{0};                        ///< Bit offset where the best decode started.
  uint8_t decoded_len{0};                       ///< Total number of bytes decoded at that offset.
  uint8_t frame_start{0};                       ///< Index into decoded buffer where the frame begins.
  uint8_t frame_len{0};                         ///< Length of the candidate IoFrame (decoded bytes).
  uint8_t decoded[RADIO_PACKET_BUFFER_SIZE]{};  ///< Full decoded UART stream at the chosen offset.
};

/// @brief Decode a UART-encoded bitstream from the given bit offset.
uint8_t decode_uart_probe(const uint8_t *raw, uint8_t raw_len, uint8_t bit_offset, uint8_t *decoded,
                          uint8_t decoded_max_len);

/// @brief Search raw RX buffer for the best CRC-validated IO-Homecontrol frame.
UartProbeResult find_uart_probe(const uint8_t *raw, uint8_t raw_len);

}  // namespace home_io_control
}  // namespace esphome
