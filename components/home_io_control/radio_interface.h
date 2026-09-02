#pragma once

/// @file radio_interface.h
/// @brief Radio abstraction layer for IO-Homecontrol.
/// @ingroup hioc_radio
///
/// Defines the SpiAccess interface for SPI bus access and the RadioDriver abstract
/// class that encapsulates all chip-specific radio operations. This allows the
/// protocol layer to work with different radio chips (SX1276, SX1262, etc.)
/// without knowing the hardware details.

#include "proto_frame.h"
#include "proto_timing.h"
#include "tuning_config.h"
#include <atomic>
#include <cstdint>
#include "esphome/core/hal.h"

namespace esphome {
namespace home_io_control {

inline constexpr uint8_t RADIO_PACKET_BUFFER_SIZE =
    64;  ///< Scratch buffer size for raw radio packets and recovered frames.

/// @brief Longest a frame arriving on the current channel may hold off an idle-path channel hop,
/// in microseconds.
///
/// Sized to outlast the slowest thing a hop could destroy. On the software-PHY chips that is the
/// fixed-length RX_DONE, which lands 48 raw bytes = 10.0 ms after the sync word
/// (SOFT_PHY_RX_PROBE_PACKET_LEN at 38400 bps; a static_assert in radio_soft_phy_driver_base.h
/// ties this constant to that arithmetic, since neither header can see the other's constants).
/// On the SX1276 it is the frame's own air time, at most ~9.4 ms for the longest possible frame.
/// 12 ms covers both with margin for poll granularity.
///
/// It is a *bound*, not a target: every mechanism that sets the holdoff is expected to clear it
/// early, and the bound exists only so that a sync detection with no frame behind it — noise, a
/// truncated burst, a peer that gave up — cannot wedge channel hopping permanently.
inline constexpr uint32_t RX_HOP_HOLDOFF_US = 12000;

/// Interface for SPI bus access.
/// The ESPHome component implements this by delegating to its SPIDevice methods,
/// allowing radio drivers to perform SPI transactions without depending on the
/// ESPHome SPI framework directly.
/// @ingroup hioc_radio
class SpiAccess {
 public:
  virtual ~SpiAccess() = default;
  /// Enable the SPI bus (assert CS low).
  virtual void spi_enable() = 0;
  /// Disable the SPI bus (deassert CS).
  virtual void spi_disable() = 0;
  /// Transfer one byte full‑duplex (MOSI→MISO).
  /// @param data Byte to send.
  /// @return Byte received from MISO.
  virtual uint8_t spi_transfer(uint8_t data) = 0;
  /// Write one byte (MOSI only, MISO ignored).
  /// @param data Byte to send.
  virtual void spi_write(uint8_t data) = 0;
  /// Read one byte (MISO only, MOSI driven with 0).
  /// @return Byte received.
  virtual uint8_t spi_read() = 0;
};

/// Configuration for transmitting a packet: carrier frequency and preamble length.
struct RadioTxConfig {
  uint32_t freq_hz{FREQ_CH2};             ///< Carrier frequency in Hz.
  uint16_t preamble_len{SHORT_PREAMBLE};  ///< Preamble length in symbol periods (bytes).
};

/// Raw packet received from the radio.
struct RadioRxPacket {
  uint32_t freq_hz{0};                       ///< Frequency the packet was received on (Hz).
  uint8_t len{0};                            ///< Length of packet in bytes.
  uint8_t data[RADIO_PACKET_BUFFER_SIZE]{};  ///< Raw packet data buffer.
};

/// Diagnostic capture from a radio operation.
///
/// Populated after every wait_for_packet / check_for_packet. Contains both the
/// raw bytes reported by the chip (before any protocol-specific recovery) and
/// the parsed frame handed to the protocol layer.
struct RadioCaptureInfo {
  bool valid{false};          ///< True if capture is valid.
  bool blocking_wait{false};  ///< True if captured during a blocking wait.
  bool rx_done{false};        ///< True if RxDone IRQ fired.
  bool crc_error{false};      ///< True if a CRC error was detected. Chip-dependent: some drivers cannot report
                              ///< CRC failures — see the concrete driver's capture documentation.
  uint32_t timestamp_ms{0};   ///< Timestamp of capture (millis).
  uint32_t freq_hz{0};        ///< RF frequency of capture (Hz).
  int16_t rssi_dbm{0};        ///< Received signal strength (dBm).
  uint16_t irq_status{0};     ///< Raw IRQ status register value.
  uint8_t irq_flags1{0};      ///< IRQ flags group 1 (chip-specific).
  uint8_t irq_flags2{0};      ///< IRQ flags group 2 (chip-specific, includes CRC flag).
  uint8_t packet_status{0};   ///< Packet status byte (chip-specific).
  uint8_t rx_offset{0};       ///< RX buffer offset where the frame starts (0 for chips without offset reporting).
  uint8_t reported_len{0};    ///< Length reported by the radio chip.
                              // raw[] preserves the chip-reported bytes before any driver-specific recovery, while
                              // frame[] stores the bytes handed to parse(). Keeping both makes it possible to compare
                              // one driver's recovery output against reference captures from another.
  uint8_t raw_len{0};         ///< Number of valid bytes in raw[].
  uint8_t frame_len{0};       ///< Number of valid bytes in frame[].
  uint8_t raw[RADIO_PACKET_BUFFER_SIZE]{};    ///< Raw radio buffer bytes.
  uint8_t frame[RADIO_PACKET_BUFFER_SIZE]{};  ///< Parsed protocol frame bytes.
};

/// Abstract radio driver for IO-Homecontrol.
///
/// Encapsulates all chip-specific operations: initialization, packet TX/RX,
/// frequency control, and mode switching. Concrete implementations (RadioSX1276,
/// RadioSX1262, RadioLR1121) handle the register-level details for each chip.
/// @ingroup hioc_radio
class RadioDriver {
 public:
  explicit RadioDriver(InternalGPIOPin *rst_pin = nullptr) : rst_pin_(rst_pin) {}
  virtual ~RadioDriver() = default;

  /// Initialize the radio hardware. Returns true on success.
  virtual bool init() = 0;

  /// Send a packet using the specified carrier frequency and preamble settings.
  /// The driver is responsible for appending the protocol CRC on the air
  /// (in hardware or software, depending on the chip).
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
  /// @return RSSI in dBm (negative value).
  virtual int16_t read_rssi() = 0;

  /// @brief Check if sync word has been detected (while in RX).
  /// Used to gate frequency hopping — prevents hopping away mid-frame.
  virtual bool is_sync_detected() = 0;

  /// @brief Check if preamble has been detected (while in RX).
  /// Used together with sync detection to gate frequency hopping.
  virtual bool is_preamble_detected() = 0;

  /// @brief Return the preamble length for response/continuation frames.
  ///
  /// Callers use this instead of hardcoding SHORT_PREAMBLE for any frame sent as
  /// an immediate reply within an exchange (challenge responses, key transfers,
  /// and any future non-START continuation frames — i.e. tight RX→TX turnaround).
  ///
  /// The default is the protocol's standard SHORT_PREAMBLE. Drivers whose TX
  /// waveform gives the peer device less synchronization margin override this
  /// with a longer preamble (see the concrete drivers for the chip-specific
  /// rationale).
  ///
  /// @return Preamble length in bytes.
  [[nodiscard]] virtual uint16_t response_preamble() const { return SHORT_PREAMBLE; }

  /// @brief Apply runtime tuning parameters to the driver.
  ///
  /// Each driver consumes only the fields it understands; the default is a no-op for
  /// chips with no runtime-tunable radio parameters. This keeps the hub free of
  /// chip-specific tuning knowledge — it hands over the whole config and lets the
  /// driver pick what it needs.
  /// @param tuning Current tuning configuration.
  virtual void apply_tuning(const TuningConfig &tuning) {}

  /// @brief Per-channel dwell for a rotating listen that does not name its own dwell.
  ///
  /// Every @ref ListenPolicy::ROTATE_ALL_CHANNELS or @ref ListenPolicy::ROTATE_SKIPPING_REQUEST
  /// listen falls back to this when @ref ListenSpec::dwell_ms is left at 0 — which is every call
  /// site today: pairing discovery and the broadcast roll-call both rotate, and neither has a
  /// measured reason to dwell differently from the other. The right dwell is inherently
  /// chip-specific — it depends on how fast the chip can retune (fast hop vs. a
  /// standby→retune→RX cycle) — so there is no generic default: each driver must return its
  /// value, normally from its user-facing tuning field. This answers a chip question ("how long
  /// must this radio sit on a channel before it can hear anything at all"), never a protocol one
  /// — a loop with a measured reason to dwell differently sets @ref ListenSpec::dwell_ms instead
  /// of asking for a second driver virtual.
  /// @param tuning Current tuning configuration.
  /// @return Dwell length in milliseconds.
  [[nodiscard]] virtual uint16_t hop_dwell_ms(const TuningConfig &tuning) const = 0;

  /// @brief Whether the chip re-enters RX fast enough after a TX to catch an
  /// immediate reply through the standard exchange wait.
  ///
  /// Some chips need a standby/settle cycle between TX and RX, so a device's
  /// immediate response (e.g. the pairing key-confirm 0x33) can arrive while the
  /// receiver is still settling and be lost. Callers choose between the standard
  /// exchange wait and a dedicated wait-and-retrigger strategy based on this.
  /// There is no safe generic default — each driver must declare it.
  /// @return true if an immediate reply after TX is reliably received.
  [[nodiscard]] virtual bool has_fast_tx_rx_turnaround() const = 0;

  /// Change the carrier frequency using fast hop (no standby transition needed).
  virtual void change_frequency(uint32_t freq_hz) = 0;

  /// Switch to continuous receive mode.
  virtual void set_mode_rx() = 0;

  /// Switch to standby mode.
  virtual void set_mode_standby() = 0;

  /// Returns true if the radio failed to initialize or encountered a fatal error.
  /// @return true on failure.
  [[nodiscard]] virtual bool is_failed() const = 0;

  /// @brief Get a human‑readable chip name.
  /// @return Short lowercase identifier (e.g. "sx1276").
  [[nodiscard]] virtual const char *chip_name() const = 0;

  /// Optional chip-specific diagnostics emitted from dump_config.
  virtual void dump_debug() {}

  /// @brief Get the current RF frequency.
  /// @return Frequency in Hz.
  [[nodiscard]] uint32_t get_current_freq() const { return this->current_freq_; }
  /// @brief Get the most recent radio capture info.
  /// @return const reference to RadioCaptureInfo.
  [[nodiscard]] const RadioCaptureInfo &get_last_capture() const { return this->last_capture_; }

  /// @brief Reset the diagnostic capture buffer.
  ///
  /// Public because ExchangeEngine blanks it at the start of every exchange: the radio only clears
  /// this buffer when it actually begins a listen, so without an explicit reset a fully-silent
  /// exchange's failure report would inherit the *previous* exchange's capture (frame length, RSSI,
  /// IRQ bits) and claim "we heard something" when nothing was on air.
  void clear_last_capture() { this->last_capture_ = RadioCaptureInfo{}; }

  /// @brief True while a frame is arriving on the current channel and retuning would destroy it.
  ///
  /// Consulted by ExchangeEngine::maybe_hop() — the idle-path hop — which is purely time-gated and
  /// otherwise fires on essentially every loop() pass (issue #81). Not consulted by
  /// hop_frequency() itself: the blocking listen() loop does its own, differently-shaped gating
  /// through preamble_or_sync_incoming(), and a caller that asked for a hop explicitly must get one.
  ///
  /// The default implementation is the recorded-state one: a driver reports a reception by calling
  /// note_reception_in_progress_() from wherever it can actually observe one, and the holdoff
  /// expires by itself after RX_HOP_HOLDOFF_US. That default is correct for any driver whose RX
  /// path passes through check_for_packet() while the frame is still arriving, and it is what both
  /// software-PHY drivers use. A driver that cannot observe a reception from check_for_packet()
  /// overrides this and reads the chip at hop time instead — see RadioSX1276.
  ///
  /// Non-const because it expires its own latch, and because an override may do SPI.
  [[nodiscard]] virtual bool reception_in_progress() {
    if (!this->rx_hold_armed_)
      return false;
    if (micros() - this->rx_hold_since_us_ >= RX_HOP_HOLDOFF_US) {
      this->rx_hold_armed_ = false;
      return false;
    }
    return true;
  }

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
  /// Common preamble for blocking receive: clear diagnostics and output packet.
  /// @param packet Output packet buffer to zero and prepare.
  void prepare_blocking_receive_(RadioRxPacket &packet) {
    this->clear_last_capture();
    packet = RadioRxPacket{};
  }

  /// Common preamble for non‑blocking receive: clear diagnostics, output packet, and DIO latch.
  /// @param packet Output packet buffer to zero and prepare.
  void prepare_nonblocking_receive_(RadioRxPacket &packet) {
    this->clear_last_capture();
    packet = RadioRxPacket{};
    this->clear_dio_fired();
  }

  /// Record that a frame is arriving right now. Re-arming refreshes the deadline, so a driver may
  /// call this on every poll that still sees the reception.
  void note_reception_in_progress_() {
    this->rx_hold_armed_ = true;
    this->rx_hold_since_us_ = micros();
  }
  /// Drop the holdoff: the reception ended, was delivered, or was torn down deliberately.
  void clear_reception_in_progress_() { this->rx_hold_armed_ = false; }

  /// Shared hardware reset sequence for chips with an active-low RST pin.
  /// Drives RST pin low → 10 ms → high → 10 ms. Called from derived driver init().
  void reset_hardware_();

  /// Populate the common fields of RadioCaptureInfo from raw telemetry.
  /// Chip‑specific fields (rx_done, crc_error, irq_flags*, irq_status, packet_status, etc.)
  /// must be set by the derived driver after calling this helper.
  /// @param blocking_wait if this was a blocking receive.
  /// @param freq_hz RF frequency of the capture.
  /// @param rssi_dbm Received signal strength.
  /// @param raw Pointer to raw bytes (may be nullptr).
  /// @param raw_len Length of raw buffer.
  /// @param frame Pointer to parsed frame bytes (may be nullptr).
  /// @param frame_len Length of parsed frame.
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

  bool rx_hold_armed_{false};     ///< Idle-hop holdoff latch — see reception_in_progress().
  uint32_t rx_hold_since_us_{0};  ///< micros() timestamp the holdoff was last (re-)armed at.

#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
  std::atomic<bool> dio_fired_{false};
#else
  volatile bool dio_fired_{false};
#endif
};

}  // namespace home_io_control
}  // namespace esphome
