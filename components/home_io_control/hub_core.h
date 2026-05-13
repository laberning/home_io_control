#pragma once

/// @file hub_core.h
/// @brief IO-Homecontrol ESPHome component — protocol controller.
///
/// This component manages the IO-Homecontrol 2W protocol: sending commands,
/// receiving responses with automatic authentication, device discovery/pairing,
/// and device state tracking. Radio hardware is delegated to a RadioDriver
/// implementation (SX1276, SX1262, etc.).
///
/// SPI configuration: MSB first, CPOL=0, CPHA=0 (Mode 0), 8 MHz clock.
/// The component inherits SPIDevice and implements SpiAccess to bridge
/// the ESPHome SPI framework to the radio driver.

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"
#include "esphome/components/spi/spi.h"
#include "esphome/components/button/button.h"
#include "proto_frame.h"
#include "radio_interface.h"
#include "hub_exchange.h"
#include "hub_decisions.h"
#include "hub_pairing.h"
#include <deque>
#include <map>
#include <vector>
#include <functional>

namespace esphome {
namespace home_io_control {

/// Callback type for notifying covers of device state changes.
using DeviceUpdateCallback = std::function<void(const std::string &device_id, const IoDevice &device)>;

// ============================================================================
// Main Component
// ============================================================================

/// The main IO-Homecontrol component. Manages the protocol layer and delegates
/// radio operations to a RadioDriver instance.
///
/// Inherits SPIDevice so that ESPHome's Python codegen can configure SPI pins.
/// Implements SpiAccess to provide the radio driver with SPI bus access.
class IOHomeControlComponent : public Component,
                               public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                                     spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_8MHZ>,
                               public SpiAccess {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // --- SpiAccess implementation (delegates to SPIDevice) ---
  void spi_enable() override { this->enable(); }
  void spi_disable() override { this->disable(); }
  uint8_t spi_transfer(uint8_t data) override { return this->transfer_byte(data); }
  void spi_write(uint8_t data) override { this->write_byte(data); }
  uint8_t spi_read() override { return this->read_byte(); }

  // --- YAML configuration setters (called by generated code) ---
  void set_rst_pin(InternalGPIOPin *pin) { this->rst_pin_ = pin; }
  void set_dio0_pin(InternalGPIOPin *pin) { this->dio0_pin_ = pin; }
  void set_dio4_pin(InternalGPIOPin *pin) { this->dio4_pin_ = pin; }
  void set_dio1_pin(InternalGPIOPin *pin) { this->dio1_pin_ = pin; }
  void set_busy_pin(InternalGPIOPin *pin) { this->busy_pin_ = pin; }
  void set_fem_en_pin(InternalGPIOPin *pin) { this->fem_en_pin_ = pin; }
  void set_vfem_pin(InternalGPIOPin *pin) { this->vfem_pin_ = pin; }
  void set_fem_pa_pin(InternalGPIOPin *pin) { this->fem_pa_pin_ = pin; }
  void set_node_id(const std::string &id) { this->node_id_str_ = id; }
  void set_system_key(const std::string &key) { this->system_key_str_ = key; }
  void set_tx_power(uint8_t power) { this->tx_power_ = power; }
  void set_pa_pin(uint8_t pa_pin) { this->pa_pin_ = pa_pin; }
  void set_radio_type(const std::string &type) { this->radio_type_ = type; }
  void set_tcxo_voltage(uint8_t voltage) { this->tcxo_voltage_ = voltage; }

  /// Declare that a remote (identified by its node ID) controls a registered device.
  /// When activity from this remote is overheard, a status poll is scheduled for the device.
  /// This is needed for 1W remotes whose destination address differs from the device's 2W ID.
  void add_linked_remote(const std::string &remote_id, const std::string &device_id) {
    this->linked_remotes_[remote_id].push_back(device_id);
  }

  // --- Device management (called by cover platform) ---
  /// Add a device to the registry (called by platform entities during setup).
  virtual void add_device(const std::string &device_id);
  /// Retrieve a device by ID; returns nullptr if not found.
  virtual IoDevice *get_device(const std::string &device_id);
  /// Register a callback invoked when any device updates.
  virtual void register_device_callback(DeviceUpdateCallback cb) { this->callbacks_.push_back(std::move(cb)); }

  // --- High-level operations ---
  /// Send a position command to a device. Returns true if device acknowledged.
  virtual bool set_device_position(const std::string &device_id, uint8_t position);
  /// Send a tilt command to a tilt-capable cover. Returns true if device acknowledged.
  virtual bool set_device_tilt(const std::string &device_id, uint8_t tilt_percent);
  /// Request current status from a device. Returns true if status received.
  virtual bool request_device_status(const std::string &device_id);
  /// Discover and pair a device that is in pairing mode. Returns true on success.
  virtual bool discover_and_pair();
  /// Semantic binary helper for light entities. Internally mapped to the shared execute path.
  virtual bool set_light_state(const std::string &device_id, bool on);
  /// Semantic binary helper for switch entities. Internally mapped to the shared execute path.
  virtual bool set_switch_state(const std::string &device_id, bool on);
  /// Queue an async position update; returns immediately, executed in loop().
  virtual void queue_set_device_position(const std::string &device_id, uint8_t position);
  /// Queue an async tilt update; returns immediately, executed in loop().
  virtual void queue_set_device_tilt(const std::string &device_id, uint8_t tilt_percent);
  /// Queue an async status request; returns immediately, executed in loop().
  virtual void queue_request_device_status(const std::string &device_id);
  /// Queue a pairing operation; executed in loop() when radio idle.
  virtual void queue_discover_and_pair();
  /// Async form of set_light_state() that keeps radio work serialized on the main loop.
  virtual void queue_set_light_state(const std::string &device_id, bool on);
  /// Async form of set_switch_state() that keeps radio work serialized on the main loop.
  virtual void queue_set_switch_state(const std::string &device_id, bool on);

 protected:
  // --- Protocol-level operations ---
  /// Transmit a raw IoFrame on the current frequency with given preamble length.
  bool transmit_frame_(const IoFrame &frame, uint32_t freq, uint16_t preamble);
  /// Main request/response exchange with retry and automatic authentication.
  bool send_and_receive_(const IoFrame &request, IoFrame &response, uint32_t freq);
  /// Handle an inbound authenticated command from a device (status updates, etc.).
  bool authenticate_request_(const IoFrame &request, uint32_t freq);
  /// Parse received packet, update device state if it's a status frame, and notify covers.
  void process_received_packet_(const RadioRxPacket &packet);
  /// Extract position/status info from a status or status-update frame and merge into device record.
  void update_device_status_(const IoFrame &frame);
  /// Fire all registered device update callbacks for the given device ID.
  void notify_device_update_(const std::string &id);
  /// Pop next pending operation from the queue and execute it (set position, request status, discover).
  void process_pending_operation_();

  // --- Outbound exchange helpers ---
  /// Wrap transmit_frame_ and mark context failed on error.
  bool transmit_request_(const IoFrame &request, uint32_t freq, uint16_t preamble,
                         exchange::OutboundExchangeContext &ctx);
  /// Wait loop for the first response packet; classifies via decisions::classify_exchange_first_response.
  decisions::ExchangeFirstResponseDisposition wait_for_first_response_(const IoFrame &request,
                                                                       exchange::OutboundExchangeContext &ctx);
  /// Perform challenge-response (TX auth response) after a 0x3C is received.
  bool handle_authentication_(const IoFrame &request, uint32_t freq, exchange::OutboundExchangeContext &ctx);
  /// Wait loop for the final authenticated response; uses is_valid_final_response().
  decisions::ExchangeFinalResponseDisposition wait_for_final_response_(const IoFrame &request,
                                                                       exchange::OutboundExchangeContext &ctx);

  // --- Pairing helpers ---
  /// Wait for a discovery response (0x29) during pairing.
  decisions::PairingDiscoveryDisposition wait_for_discovery_response_(uint32_t timeout_ms, RadioRxPacket &packet,
                                                                      IoFrame &response_frame);
  /// Wait for a key-challenge (0x3C) from target device during pairing key exchange.
  bool wait_for_key_challenge_(uint32_t timeout_ms, RadioRxPacket &packet, IoFrame &challenge_frame,
                               const uint8_t device_node_id[NODE_ID_SIZE]);

  // Parse a discovery response frame into device metadata and ID.
  static void parse_device_from_discovery(const IoFrame &frame, IoDevice &device, std::string &device_id);

  // --- Pairing phase helpers ---
  /// Phase 1: broadcast discovery (0x28) and wait for a device response (0x29).
  decisions::PairingDiscoveryDisposition run_discovery_phase_(pairing::PairingContext &context);
  /// Phase 2: authenticated key exchange (0x31 → 0x3C → 0x32 → 0x33).
  bool run_key_exchange_phase_(pairing::PairingContext &context);
  /// Phase 3: send SetConfig1 (0x71) to finalize device configuration.
  bool finalize_pairing_configuration_(pairing::PairingContext &context);

  enum class PendingOperationType : uint8_t {
    SET_POSITION,
    SET_TILT,
    SET_LIGHT_STATE,
    SET_SWITCH_STATE,
    REQUEST_STATUS,
    DISCOVER_AND_PAIR,
  };

  struct PendingOperation {
    PendingOperationType type;
    std::string device_id;
    uint8_t position{0};
  };

  struct ExchangeDebugInfo {
    const char *stage{"idle"};
    uint8_t tries{0};
    uint8_t request_cmd{0};
    bool saw_challenge{false};
    bool capture_valid{false};
    bool capture_rx_done{false};
    bool capture_crc_error{false};
    uint32_t capture_freq_hz{0};
    uint16_t capture_irq_status{0};
    uint8_t capture_packet_status{0};
    uint8_t capture_reported_len{0};
    uint8_t capture_frame_len{0};
    int16_t capture_rssi_dbm{0};
  };

  void reset_exchange_debug_(uint8_t request_cmd);
  void record_exchange_debug_(const char *stage, uint8_t tries, bool saw_challenge);
  void log_exchange_debug_(const char *device_id) const;

  // --- Frequency hopping ---
  void hop_frequency_();

  // --- Flash persistence ---
  void save_devices_();
  void load_devices_();

  // --- Radio driver ---
  RadioDriver *radio_{nullptr};

  // --- Hardware pins (set by YAML codegen, passed to radio driver in setup) ---
  InternalGPIOPin *rst_pin_{nullptr};
  InternalGPIOPin *dio0_pin_{nullptr};    ///< SX1276 DIO0 interrupt
  InternalGPIOPin *dio4_pin_{nullptr};    ///< SX1276 DIO4 preamble detect (optional)
  InternalGPIOPin *dio1_pin_{nullptr};    ///< SX1262 DIO1 interrupt
  InternalGPIOPin *busy_pin_{nullptr};    ///< SX1262 BUSY pin
  InternalGPIOPin *fem_en_pin_{nullptr};  ///< Front-end module enable
  InternalGPIOPin *vfem_pin_{nullptr};    ///< Front-end module power
  InternalGPIOPin *fem_pa_pin_{nullptr};  ///< Front-end module PA switch

  // --- Configuration (from YAML) ---
  std::string node_id_str_;
  std::string system_key_str_;
  std::string radio_type_;  ///< "sx1276", "sx1262", or "" (auto-detect)
  uint8_t node_id_[NODE_ID_SIZE]{};
  uint8_t system_key_[AES_KEY_SIZE]{};
  uint8_t tx_power_{17};
  uint8_t pa_pin_{0x80};
  uint8_t tcxo_voltage_{0x03};  ///< SX1262 TCXO voltage (default 1.8V)

  // --- Runtime state ---
  bool initialized_{false};
  bool busy_{false};
  uint32_t last_hop_us_{0};
  ExchangeDebugInfo last_exchange_debug_{};
  std::map<std::string, IoDevice> devices_;
  std::vector<DeviceUpdateCallback> callbacks_;
  std::deque<PendingOperation> pending_operations_;
  /// Maps remote node IDs to lists of device IDs they control.
  /// Used to trigger status polls when 1W remote activity is overheard.
  std::map<std::string, std::vector<std::string>> linked_remotes_;
};

// ============================================================================
// Discover & Pair Button
// ============================================================================

/// Button entity that triggers device discovery and pairing when pressed in Home Assistant.
class DiscoverButton : public button::Button, public Component {
 public:
  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }
  void dump_config() override {}

 protected:
  void press_action() override { this->parent_->queue_discover_and_pair(); }
  IOHomeControlComponent *parent_{nullptr};
};

// ----------------------------------------------------------------------------
// Test-visible helpers (inline for host unit tests)
// These are also defined as static in hub_core.cpp; inline definitions here
// allow tests to call them directly without violating ODR.
// ----------------------------------------------------------------------------

/// Check if a persisted node ID is valid (not all-zero, not all-0xFF).
inline bool persisted_node_id_is_valid(const uint8_t id[NODE_ID_SIZE]) {
  bool all_zero = true;
  bool all_ff = true;
  for (uint8_t i = 0; i < NODE_ID_SIZE; i++) {
    all_zero = all_zero && id[i] == 0x00;
    all_ff = all_ff && id[i] == 0xFF;
  }
  return !all_zero && !all_ff;
}

/// Format a position float as a human-readable string (e.g. "50%", "unknown").
inline std::string format_position(float pos) {
  if (pos == UNKNOWN_POSITION) {
    return "unknown";
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "%.0f%%", pos);
  return buf;
}

}  // namespace home_io_control
}  // namespace esphome
