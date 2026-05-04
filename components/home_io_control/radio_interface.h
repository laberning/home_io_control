#pragma once

/// @file radio_interface.h
/// @brief Radio abstraction layer for IO-Homecontrol.
///
/// Defines the SpiAccess interface for SPI bus access and the RadioDriver abstract
/// class that encapsulates all chip-specific radio operations. This allows the
/// protocol layer to work with different radio chips (SX1276, SX1262, etc.)
/// without knowing the hardware details.

#include "proto_frame.h"
#include <atomic>
#include <cstdint>
#include "esphome/core/hal.h"

namespace esphome {
namespace home_io_control {

/// Interface for SPI bus access.
/// The ESPHome component implements this by delegating to its SPIDevice methods,
/// allowing radio drivers to perform SPI transactions without depending on the
/// ESPHome SPI framework directly.
class SpiAccess {
 public:
  virtual ~SpiAccess() = default;
  virtual void spi_enable() = 0;
  virtual void spi_disable() = 0;
  virtual uint8_t spi_transfer(uint8_t data) = 0;
  virtual void spi_write(uint8_t data) = 0;
  virtual uint8_t spi_read() = 0;
};

struct RadioTxConfig {
  uint32_t freq_hz{FREQ_CH2};
  uint16_t preamble_len{SHORT_PREAMBLE};
};

struct RadioRxPacket {
  uint32_t freq_hz{0};
  uint8_t len{0};
  uint8_t data[64]{};
};

struct RadioCaptureInfo {
  bool valid{false};
  bool blocking_wait{false};
  bool rx_done{false};
  bool crc_error{false};
  uint32_t timestamp_ms{0};
  uint32_t freq_hz{0};
  int16_t rssi_dbm{0};
  uint16_t irq_status{0};
  uint8_t irq_flags1{0};
  uint8_t irq_flags2{0};
  uint8_t packet_status{0};
  uint8_t rx_offset{0};
  uint8_t reported_len{0};
  // raw[] preserves the chip-reported bytes before any protocol-specific recovery, while frame[]
  // stores the bytes handed to parse(). Keeping both made it possible to compare SX1262 recovery
  // output against the SX1276 reference path during bring-up.
  uint8_t raw_len{0};
  uint8_t frame_len{0};
  uint8_t raw[64]{};
  uint8_t frame[64]{};
};

/// Abstract radio driver for IO-Homecontrol.
///
/// Encapsulates all chip-specific operations: initialization, packet TX/RX,
/// frequency control, and mode switching. Concrete implementations (RadioSX1276,
/// RadioSX1262) handle the register-level details for each chip.
class RadioDriver {
 public:
  explicit RadioDriver(InternalGPIOPin *rst_pin = nullptr) : rst_pin_(rst_pin) {}
  virtual ~RadioDriver() = default;

  /// Initialize the radio hardware. Returns true on success.
  virtual bool init() = 0;

  /// Send a packet using the specified carrier frequency and preamble settings.
  /// The radio handles CRC automatically (IoHomeOn mode for SX1276).
  virtual bool send_packet(const uint8_t *data, uint8_t len, const RadioTxConfig &tx_config) = 0;

  /// Wait (blocking) for a packet with timeout. Returns true if a packet was received.
  /// Contract:
  /// - Clears last_capture_ and output packet before waiting.
  /// - On success: populates packet and last_capture_, returns true.
  /// - On timeout/failure: may populate last_capture_ for diagnostics, returns false.
  /// - Radio remains in RX mode on return (regardless of outcome).
  virtual bool wait_for_packet(RadioRxPacket &packet, uint32_t timeout_ms) = 0;

  /// Non-blocking check for a received packet. Called from loop().
  /// Returns true if a packet was read into packet.
  /// Contract:
  /// - Returns false immediately if no DIO interrupt has fired.
  /// - On success: populates packet and last_capture_, returns true.
  /// - On failure: may populate last_capture_ for diagnostics, returns false.
  virtual bool check_for_packet(RadioRxPacket &packet) = 0;

  /// Read instantaneous RSSI (in dBm) while in RX mode.
  /// Used for listen-before-talk (LBT) carrier sense before transmitting.
  virtual int16_t read_rssi() = 0;

  /// Change the carrier frequency using fast hop (no standby transition needed).
  virtual void change_frequency(uint32_t freq_hz) = 0;

  /// Switch to continuous receive mode.
  virtual void set_mode_rx() = 0;

  /// Switch to standby mode.
  virtual void set_mode_standby() = 0;

  /// Returns true if the radio failed to initialize or encountered a fatal error.
  [[nodiscard]] virtual bool is_failed() const = 0;

  [[nodiscard]] virtual const char *chip_name() const = 0;

  /// Optional chip-specific diagnostics emitted from dump_config.
  virtual void dump_debug() {}

  [[nodiscard]] uint32_t get_current_freq() const { return this->current_freq_; }
  [[nodiscard]] const RadioCaptureInfo &get_last_capture() const { return this->last_capture_; }

  /// Set by the ISR when DIO fires. Using access helpers instead of touching the flag directly
  /// keeps the ISR/main-loop handoff explicit and lets ESP32 builds use atomic storage.
  [[nodiscard]] bool is_dio_fired() const {
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
    return this->dio_fired_.load(std::memory_order_acquire);
#else
    return this->dio_fired_;
#endif
  }

  void clear_dio_fired() {
    // The wait/check loops clear the latch only after they have observed it. That avoids losing
    // an edge when TX completion and the next RX event happen close together.
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
    this->dio_fired_.store(false, std::memory_order_release);
#else
    this->dio_fired_ = false;
#endif
  }

  void mark_dio_fired_from_isr() {
    // Keep the ISR work to a single flag store so the interrupt path remains deterministic.
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
    this->dio_fired_.store(true, std::memory_order_release);
#else
    this->dio_fired_ = true;
#endif
  }

 protected:
  void clear_last_capture_() { this->last_capture_ = RadioCaptureInfo{}; }

  /// Common preamble for blocking receive: clear diagnostics and output packet.
  void prepare_blocking_receive_(RadioRxPacket &packet) {
    this->clear_last_capture_();
    packet = RadioRxPacket{};
  }

  /// Common preamble for non-blocking receive: clear diagnostics, output packet, and DIO latch.
  void prepare_nonblocking_receive_(RadioRxPacket &packet) {
    this->clear_last_capture_();
    packet = RadioRxPacket{};
    this->clear_dio_fired();
  }

  /// Hardware reset sequence common to all SX chips.
  /// Drives RST pin low → 10ms → high → 10ms.
  void reset_hardware_();

  /// Populate the common fields of RadioCaptureInfo from raw telemetry.
  /// Chip-specific fields (rx_done, crc_error, irq_flags*, irq_status, packet_status, etc.)
  /// must be set by the derived driver after calling this helper.
  void populate_capture_base_(bool blocking_wait, uint32_t freq_hz, int16_t rssi_dbm, const uint8_t *raw,
                              uint8_t raw_len, const uint8_t *frame, uint8_t frame_len) {
    this->last_capture_ = RadioCaptureInfo{};
    this->last_capture_.valid = true;
    this->last_capture_.blocking_wait = blocking_wait;
    this->last_capture_.timestamp_ms = millis();
    this->last_capture_.freq_hz = freq_hz;
    this->last_capture_.rssi_dbm = rssi_dbm;
    if (raw != nullptr && raw_len > 0) {
      this->last_capture_.raw_len = std::min(raw_len, (uint8_t) sizeof(this->last_capture_.raw));
      memcpy(this->last_capture_.raw, raw, this->last_capture_.raw_len);
    }
    if (frame != nullptr && frame_len > 0) {
      this->last_capture_.frame_len = std::min(frame_len, (uint8_t) sizeof(this->last_capture_.frame));
      memcpy(this->last_capture_.frame, frame, this->last_capture_.frame_len);
    }
  }

  uint32_t current_freq_{FREQ_CH2};
  RadioCaptureInfo last_capture_{};
  InternalGPIOPin *rst_pin_{nullptr};

#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
  std::atomic<bool> dio_fired_{false};
#else
  volatile bool dio_fired_{false};
#endif
};

}  // namespace home_io_control
}  // namespace esphome
