#pragma once

/// @file hub_core.h
/// @brief IO-Homecontrol ESPHome component — protocol controller.
/// @ingroup hioc_hub
///
/// This component manages the IO-Homecontrol 2W protocol: sending commands,
/// receiving responses with automatic authentication, device discovery/pairing,
/// and device state tracking. Radio hardware is delegated to a RadioDriver
/// implementation (SX1276, SX1262, etc.).
///
/// SPI configuration: MSB first, CPOL=0, CPHA=0 (Mode 0), 8 MHz clock.
/// The component inherits SPIDevice and implements SpiAccess to bridge
/// the ESPHome SPI framework to the radio driver.
///
/// Architecture notes:
///   - setup() initializes radio, waits for YAML-driven device registration, and enters RX mode.
///   - loop() processes the pending_operations_ queue (serializes all radio work).
///   - All outbound commands go through send_and_receive_ which handles retry & auth.
///   - Inbound frames are processed in process_received_packet_ and may trigger
///     inbound authentication (hub_exchange.h) if the device proves itself.
///   - Device registry and callbacks provide fan‑out to platform entities (covers/lights/switches).

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/api/custom_api_device.h"
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

namespace detail {
class RenameDeviceServiceDescriptor;
}

/// Callback type for notifying covers of device state changes.
using DeviceUpdateCallback = std::function<void(const std::string &device_id, const IoDevice &device)>;

inline constexpr uint8_t DEFAULT_TX_POWER_DBM = 17;       ///< Default TX power used unless YAML overrides it.
inline constexpr uint8_t DEFAULT_PA_PIN_PA_BOOST = 0x80;  ///< SX1276 PA_CONFIG selector for the PA_BOOST output path.
inline constexpr uint8_t DEFAULT_TCXO_VOLTAGE_SETTING_1P8V = 0x03;  ///< SX1262 DIO3 setting value for a 1.8 V TCXO.
inline constexpr size_t POSITION_TEXT_BUFFER_SIZE = 16;  ///< Buffer for formatted position strings such as "100%".

// ============================================================================
// Main Component
// ============================================================================

/// The main IO-Homecontrol component. Manages the protocol layer and delegates
/// radio operations to a RadioDriver instance.
///
/// Inherits SPIDevice so that ESPHome's Python codegen can configure SPI pins.
/// Implements SpiAccess to provide the radio driver with SPI bus access.
/// @ingroup hioc_hub
class IOHomeControlComponent : public Component,
                               public api::CustomAPIDevice,
                               public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                                     spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_8MHZ>,
                               public SpiAccess {
  friend class detail::RenameDeviceServiceDescriptor;

 public:
  /// @brief Result payload used by hub-level management actions such as rename.
  struct ManagementActionResult {
    bool success{false};          ///< Whether the requested management action succeeded.
    bool verified{false};         ///< Whether a follow-up readback verified the applied state.
    bool has_result_code{false};  ///< True when result_code contains a decoded CMD_ERROR_RESP byte.
    uint8_t result_code{0};       ///< Optional CMD_ERROR_RESP result byte from the device.
    std::string action;           ///< Action name, for example "rename_device".
    std::string device_id;        ///< Target IO-homecontrol device ID.
    std::string message;          ///< Human-readable outcome summary.
    std::string requested_name;   ///< Requested normalized UTF-8 name for rename actions.
    std::string applied_name;     ///< Verified cached UTF-8 name after a readback, when available.
  };

  /// @brief Initialize hardware (radio and device registry).
  void setup() override;
  /// @brief Main loop: process pending operations and drive radio state machine.
  void loop() override;
  /// @brief Dump configuration and radio debug info to the log.
  void dump_config() override;
  /// @brief Get setup priority (HARDWARE to initialize early).
  /// @return setup_priority::HARDWARE.
  [[nodiscard]] float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // --- SpiAccess implementation (delegates to SPIDevice) ---
  /// @brief Enable the SPI bus.
  void spi_enable() override { this->enable(); }
  /// @brief Disable the SPI bus.
  void spi_disable() override { this->disable(); }
  /// @brief Transfer one byte full‑duplex.
  /// @param data Byte to send.
  /// @return Received byte.
  uint8_t spi_transfer(uint8_t data) override { return this->transfer_byte(data); }
  /// @brief Write one byte (MOSI only).
  /// @param data Byte to send.
  void spi_write(uint8_t data) override { this->write_byte(data); }
  /// @brief Read one byte (MISO only).
  /// @return Received byte.
  uint8_t spi_read() override { return this->read_byte(); }

  /// @brief Suspend the hub's normal loop (packet processing, hopping, polling).
  /// Used by loopback test configs to take exclusive control of the radio.
  void set_radio_test_mode(bool active) { this->radio_test_mode_ = active; }

  /// @brief Get the underlying radio driver (for diagnostics and test tooling).
  [[nodiscard]] RadioDriver *get_radio() const { return this->radio_; }

  // --- YAML configuration setters (called by generated code) ---
  /// Set the radio reset pin.
  void set_rst_pin(InternalGPIOPin *pin) { this->rst_pin_ = pin; }
  /// Set the DIO0 interrupt pin (SX1276).
  void set_dio0_pin(InternalGPIOPin *pin) { this->dio0_pin_ = pin; }
  /// Set the DIO4 preamble‑detect pin (SX1276, optional).
  void set_dio4_pin(InternalGPIOPin *pin) { this->dio4_pin_ = pin; }
  /// Set the DIO1 interrupt pin (SX1262).
  void set_dio1_pin(InternalGPIOPin *pin) { this->dio1_pin_ = pin; }
  /// Set the BUSY pin (SX1262).
  void set_busy_pin(InternalGPIOPin *pin) { this->busy_pin_ = pin; }
  /// Set the front‑end module enable pin.
  void set_fem_en_pin(InternalGPIOPin *pin) { this->fem_en_pin_ = pin; }
  /// Set the VFEM power pin.
  void set_vfem_pin(InternalGPIOPin *pin) { this->vfem_pin_ = pin; }
  /// Set the FEM PA switch pin.
  void set_fem_pa_pin(InternalGPIOPin *pin) { this->fem_pa_pin_ = pin; }
  /// Set the controller's node ID (hex string).
  void set_node_id(const std::string &id) { this->node_id_str_ = id; }
  /// Set the system key (hex string).
  void set_system_key(const std::string &key) { this->system_key_str_ = key; }
  /// Set transmit power (dBm).
  void set_tx_power(uint8_t power) { this->tx_power_ = power; }
  /// Set PA boost pin configuration.
  void set_pa_pin(uint8_t pa_pin) { this->pa_pin_ = pa_pin; }
  /// Set radio type ("sx1276" or "sx1262"); empty string means auto‑detect.
  void set_radio_type(const std::string &type) { this->radio_type_ = type; }
  /// Set TCXO voltage for SX1262 (1.8V / 3.3V).
  void set_tcxo_voltage(uint8_t voltage) { this->tcxo_voltage_ = voltage; }

  /// Declare that a remote (identified by its node ID) controls a registered device.
  /// When activity from this remote is overheard, a status poll is scheduled for the device.
  /// This is needed for 1W remotes whose destination address differs from the device's 2W ID.
  /// @param remote_id Node ID of the remote control.
  /// @param device_id Node ID of the device it controls.
  void add_linked_remote(const std::string &remote_id, const std::string &device_id) {
    this->linked_remotes_[remote_id].push_back(device_id);
  }

  // --- Device management (called by platform entities during setup) ---
  /// Add a device to the registry by device ID only (legacy/delegating overload).
  /// Type, subtype, and inverted default to UNKNOWN / 0 / false; use the 4-arg
  /// overload when type/subtype/inverted come from YAML declarations.
  /// @param device_id Hexadecimal node ID string.
  virtual void add_device(const std::string &device_id);
  /// Add a device to the registry with full metadata from YAML.
  /// @param device_id Hexadecimal node ID string.
  /// @param type Device type from YAML declaration (UNKNOWN if not specified).
  /// @param subtype Device subtype from YAML declaration.
  /// @param inverted Position inversion flag from YAML declaration.
  virtual void add_device(const std::string &device_id, DeviceType type, uint8_t subtype, bool inverted);
  /// Retrieve a device by ID; returns nullptr if not found.
  /// @param device_id Hexadecimal node ID.
  /// @return Pointer to IoDevice, or nullptr.
  virtual IoDevice *get_device(const std::string &device_id);
  /// Register a callback invoked when any device updates.
  /// @param cb Callable with signature void(const std::string&, const IoDevice&).
  virtual void register_device_callback(DeviceUpdateCallback cb) { this->callbacks_.push_back(std::move(cb)); }
  /// Configure the optional follow-up polling interval for a registered device.
  /// @param device_id Target device ID.
  /// @param poll_interval_ms Poll interval in milliseconds; zero keeps the legacy one-shot settle poll only.
  virtual void set_device_status_poll_interval(const std::string &device_id, uint32_t poll_interval_ms);

  // --- High-level operations ---
  /// Send a position command to a device.
  /// @param device_id Target device ID.
  /// @param position Desired position (0–100, or POS_STOP/POS_FAVORITE).
  /// @return true if device acknowledged; false on timeout or radio error.
  virtual bool set_device_position(const std::string &device_id, uint8_t position);
  /// Send a tilt command to a tilt‑capable cover.
  /// @param device_id Target device ID.
  /// @param tilt_percent Desired tilt (0–100).
  /// @return true if device acknowledged; false otherwise.
  virtual bool set_device_tilt(const std::string &device_id, uint8_t tilt_percent);
  /// Set both position and tilt of a tilt-capable cover in one atomic command.
  /// @param device_id Target device ID.
  /// @param position Desired position (0–100, open→closed).
  /// @param tilt_percent Desired tilt (0–100).
  /// @return true if device acknowledged; false otherwise.
  virtual bool set_device_position_and_tilt(const std::string &device_id, uint8_t position, uint8_t tilt_percent);
  /// Request current status from a device.
  /// @param device_id Target device ID.
  /// @return true if status frame was received and processed.
  virtual bool request_device_status(const std::string &device_id);
  /// Request the stored device name from a device.
  /// @param device_id Target device ID.
  /// @return true if a name response frame was received and processed.
  virtual bool request_device_name(const std::string &device_id);
  /// Rename a device and verify the result by reading the name back.
  /// @param device_id Target device ID.
  /// @param new_name Requested UTF-8 device name.
  /// @return Structured result describing success, verification, and any validation failure.
  virtual ManagementActionResult rename_device(const std::string &device_id, const std::string &new_name);
  /// Discover and pair a device that is in pairing mode.
  /// @return true if pairing completed successfully; false otherwise.
  virtual bool discover_and_pair();
  /// Semantic binary helper for light entities. Internally mapped to the shared execute path.
  /// @param device_id Target device ID.
  /// @param on Desired on/off state.
  /// @return true if device acknowledged.
  virtual bool set_light_state(const std::string &device_id, bool on);
  /// Semantic binary helper for switch entities. Internally mapped to the shared execute path.
  /// @param device_id Target device ID.
  /// @param on Desired on/off state.
  /// @return true if device acknowledged.
  virtual bool set_switch_state(const std::string &device_id, bool on);
  /// Semantic lock helper for lock entities. Internally mapped to the shared execute path.
  /// @param device_id Target device ID.
  /// @param locked Desired locked/unlocked state.
  /// @return true if device acknowledged.
  virtual bool set_lock_state(const std::string &device_id, bool locked);
  /// @brief Queue an async position update; returns immediately, executed in loop().
  ///
  /// If a pending SET_TILT operation for the same device is already in the queue, the two are
  /// coalesced into a single SET_POSITION_AND_TILT command to avoid two radio exchanges.
  /// This transparently handles Home Assistant sending cover.set_cover_position and
  /// cover.set_cover_tilt_position as separate rapid calls.
  /// @param device_id Target device ID.
  /// @param position Desired position (0–100).
  virtual void queue_set_device_position(const std::string &device_id, uint8_t position);
  /// Queue an async named command (STOP, FAVORITE, VENT, FORCE_OPEN); returns immediately, executed in loop().
  /// @param device_id Target device ID.
  /// @param cmd Named command to send.
  virtual void queue_device_command(const std::string &device_id, CoverCommand cmd);
  /// @brief Queue an async tilt update; returns immediately, executed in loop().
  ///
  /// If a pending SET_POSITION operation for the same device is already in the queue, the two are
  /// coalesced into a single SET_POSITION_AND_TILT command to avoid two radio exchanges.
  /// This transparently handles Home Assistant sending cover.set_cover_position and
  /// cover.set_cover_tilt_position as separate rapid calls.
  /// @param device_id Target device ID.
  /// @param tilt_percent Desired tilt (0–100).
  virtual void queue_set_device_tilt(const std::string &device_id, uint8_t tilt_percent);
  /// Queue an async combined position+tilt update; returns immediately, executed in loop().
  /// @param device_id Target device ID.
  /// @param position Desired position (0–100).
  /// @param tilt_percent Desired tilt (0–100).
  virtual void queue_set_device_position_and_tilt(const std::string &device_id, uint8_t position, uint8_t tilt_percent);
  /// Queue an async status request; returns immediately, executed in loop().
  /// @param device_id Target device ID.
  virtual void queue_request_device_status(const std::string &device_id);
  /// Queue an async device-name request; returns immediately, executed in loop().
  /// @param device_id Target device ID.
  virtual void queue_request_device_name(const std::string &device_id);
  /// Queue a pairing operation; executed in loop() when radio idle.
  virtual void queue_discover_and_pair();
  /// Async form of set_light_state() that keeps radio work serialized on the main loop.
  /// @param device_id Target device ID.
  /// @param on Desired on/off state.
  virtual void queue_set_light_state(const std::string &device_id, bool on);
  /// Async form of set_switch_state() that keeps radio work serialized on the main loop.
  /// @param device_id Target device ID.
  /// @param on Desired on/off state.
  virtual void queue_set_switch_state(const std::string &device_id, bool on);
  /// Async form of set_lock_state() that keeps radio work serialized on the main loop.
  /// @param device_id Target device ID.
  /// @param locked Desired locked/unlocked state.
  virtual void queue_set_lock_state(const std::string &device_id, bool locked);

 protected:
  // --- Protocol-level operations ---
  /// Transmit a raw IoFrame on the current frequency with given preamble length.
  /// @param frame IoFrame to transmit.
  /// @param freq RF frequency in Hz.
  /// @param preamble Preamble length in bytes (LONG_PREAMBLE or SHORT_PREAMBLE).
  bool transmit_frame_(const IoFrame &frame, uint32_t freq, uint16_t preamble);
  /// Main request/response exchange with retry and automatic authentication.
  /// @param request Outbound request IoFrame.
  /// @param response Output: received response IoFrame.
  /// @param freq RF frequency in Hz.
  /// @return true if exchange succeeded; false otherwise.
  bool send_and_receive_(const IoFrame &request, IoFrame &response, uint32_t freq);
  /// Handle an inbound authenticated command from a device (status updates, etc.).
  /// @param request Inbound authenticated request (e.g., CMD_STATUS_UPDATE).
  /// @param freq RF frequency the packet arrived on.
  /// @return true if authentication succeeded; false otherwise.
  bool authenticate_request_(const IoFrame &request, uint32_t freq);
  /// Parse a received frame, merge supported device state or metadata, and notify callbacks.
  /// @param packet Raw radio packet containing a parsed IoFrame.
  void process_received_packet_(const RadioRxPacket &packet);
  /// Extract supported position or metadata info from a response frame and merge it into the device record.
  /// @param frame IoFrame containing a supported inbound command such as CMD_PRIVATE_RESP,
  /// CMD_STATUS_UPDATE, CMD_GET_NAME_RESP, or CMD_GET_INFO2_RESP.
  void update_device_status_(const IoFrame &frame);
  /// Schedule a delayed status poll for a registered device using the Component timeout API.
  /// @param device_id ID of the device to poll.
  /// @param delay_ms Delay in milliseconds before polling.
  /// @note Uses ESPHome's set_timeout() mechanism; the callback executes in loop().
  ///       A zero delay schedules immediately on the next loop iteration.
  void schedule_status_poll_(const std::string &device_id, uint32_t delay_ms);
  /// Begin bounded follow-up polling for a device after a command or overheard remote activity.
  /// @param device_id ID of the device to poll.
  /// @param initial_delay_ms Delay before the first follow-up poll.
  void begin_status_poll_tracking_(const std::string &device_id, uint32_t initial_delay_ms);
  /// Shared request/response helper for high-level operations.
  /// @param device_id Target device ID.
  /// @param request Outbound request frame.
  /// @param warn_on_no_response If true, logs a warning when no response is received.
  /// @param retry_after_fail_ms If non-zero, schedules next status poll after this delay on failure.
  /// @return true if device acknowledged; false otherwise.
  bool execute_request_and_update_(const std::string &device_id, const IoFrame &request, bool warn_on_no_response,
                                   uint32_t retry_after_fail_ms = 0);
  /// Execute a named device command (STOP, FAVORITE, VENT, FORCE_OPEN) via the authenticated exchange.
  /// @param device_id Target device ID.
  /// @param cmd Named command to execute.
  /// @return true if device acknowledged; false otherwise.
  bool execute_device_command_(const std::string &device_id, CoverCommand cmd);
  /// Register hub-level Home Assistant actions exposed through ESPHome's native API.
  void register_management_actions_();
  /// Publish the outcome of a management action as a Home Assistant event and structured logs.
  /// @param result Management action result to emit.
  void publish_management_result_(const ManagementActionResult &result);
  /// Native API action callback: rename a registered device.
  /// @param device_id Target device ID as provided by Home Assistant.
  /// @param new_name Requested UTF-8 device name.
  /// @note This callback is wired from the native API service descriptor and forwards
  ///       the decoded string arguments directly into rename_device().
  void api_rename_device_(const std::string &device_id, const std::string &new_name);
  /// Fire all registered device update callbacks for the given device ID.
  /// @param id Device ID that updated.
  void notify_device_update_(const std::string &id);
  /// Pop next pending operation from the queue and execute it (set position, request status, discover).
  void process_pending_operation_();

  // --- Outbound exchange helpers ---
  /// Wrap transmit_frame_ and mark context failed on error.
  /// @param request Outbound IoFrame to transmit.
  /// @param freq RF frequency in Hz.
  /// @param preamble Preamble length in bytes.
  /// @param ctx Exchange context (state updated on failure).
  /// @return true if transmit succeeded; false otherwise.
  bool transmit_request_(const IoFrame &request, uint32_t freq, uint16_t preamble,
                         exchange::OutboundExchangeContext &ctx);
  /// Wait loop for the first response packet; classifies via decisions::classify_exchange_first_response.
  /// @param request Original request frame (used for endpoint matching).
  /// @param ctx Exchange context (provides deadline and receives rx frame on accept).
  /// @return Disposition indicating next step.
  decisions::ExchangeFirstResponseDisposition wait_for_first_response_(const IoFrame &request,
                                                                       exchange::OutboundExchangeContext &ctx);
  /// Perform challenge-response (TX auth response) after a 0x3C is received.
  /// @param request Original request frame (needed for HMAC derivation).
  /// @param freq RF channel frequency (same channel used for the request).
  /// @param ctx Exchange context holding the challenge frame and state.
  /// @return true if challenge response was sent successfully; false otherwise.
  bool handle_authentication_(const IoFrame &request, uint32_t freq, exchange::OutboundExchangeContext &ctx);
  /// Wait loop for the final authenticated response; uses is_valid_final_response().
  /// @param request Original request frame (used for endpoint matching).
  /// @param ctx Exchange context (receives final rx frame on accept).
  /// @return ACCEPT if a matching final response arrives; IGNORE_UNRELATED on timeout.
  decisions::ExchangeFinalResponseDisposition wait_for_final_response_(const IoFrame &request,
                                                                       exchange::OutboundExchangeContext &ctx);

  // --- Pairing helpers ---
  /// Wait for a discovery response (0x29) during pairing.
  /// @param timeout_ms Maximum time to wait in milliseconds.
  /// @param packet Output: raw RadioRxPacket of the accepted frame.
  /// @param response_frame Output: parsed IoFrame of the accepted frame.
  /// @return PairingDiscoveryDisposition: ACCEPT on success; NO_RESPONSE or INVALID otherwise.
  decisions::PairingDiscoveryDisposition wait_for_discovery_response_(uint32_t timeout_ms, RadioRxPacket &packet,
                                                                      IoFrame &response_frame);
  /// Wait for a key-challenge (0x3C) from target device during pairing key exchange.
  /// @param timeout_ms Maximum time to wait in milliseconds.
  /// @param packet Output: raw RadioRxPacket of the challenge frame.
  /// @param challenge_frame Output: parsed IoFrame containing the challenge.
  /// @param device_node_id Node ID of the device we are pairing (expected sender).
  /// @return true if a valid challenge was received; false on timeout.
  bool wait_for_key_challenge_(uint32_t timeout_ms, RadioRxPacket &packet, IoFrame &challenge_frame,
                               const uint8_t device_node_id[NODE_ID_SIZE]);

  /// Transmit 0x32 key transfer with SHORT_PREAMBLE and wait for 0x33 key confirm.
  /// Uses a dedicated wait loop: no frequency hopping, longer timeout than generic exchanges.
  bool wait_for_key_confirm_(pairing::PairingContext &context);

  /// Parse a discovery response frame into device metadata and ID.
  /// @param frame       Parsed discovery response.
  /// @param device      Output: populated IoDevice (node_id, type, subtype, inverted, position/target/stopped).
  /// @param device_id   Output: hex string representation of node ID.
  static void parse_device_from_discovery(const IoFrame &frame, IoDevice &device, std::string &device_id);

  // --- Pairing phase helpers ---
  /// Phase 1: broadcast discovery (0x28) and wait for a device response (0x29).
  /// @param context Pairing context modified on success.
  /// @return PairingDiscoveryDisposition: ACCEPT, NO_RESPONSE, or INVALID.
  decisions::PairingDiscoveryDisposition run_discovery_phase_(pairing::PairingContext &context);
  /// Phase 2: authenticated key exchange (0x31 → 0x3C → 0x32 → 0x33).
  /// @param context Pairing context populated by run_discovery_phase_().
  /// @return true if key exchange completes successfully; false otherwise.
  bool run_key_exchange_phase_(pairing::PairingContext &context);
  /// Phase 3: send SetConfig1 (0x6F) to finalize device configuration.
  /// @param context Pairing context with device information.
  /// @return true (pairing proceeds regardless of set‑config outcome).
  bool finalize_pairing_configuration_(pairing::PairingContext &context);

  /// @brief Type of queued pending operation for the main loop.
  enum class PendingOperationType : uint8_t {
    SET_POSITION,           ///< Queue a set_device_position call (position 0–100 or special values).
    SET_TILT,               ///< Queue a set_device_tilt call (tilt percentage 0–100).
    SET_POSITION_AND_TILT,  ///< Queue a combined set_device_position_and_tilt call.
    DEVICE_COMMAND,         ///< Queue a named device command (STOP, FAVORITE, VENT).
    SET_LIGHT_STATE,        ///< Queue a set_light_state call (binary on/off).
    SET_LOCK_STATE,         ///< Queue a set_lock_state call (locked/unlocked).
    SET_SWITCH_STATE,       ///< Queue a set_switch_state call (binary on/off).
    REQUEST_STATUS,         ///< Queue a request_device_status call (poll for current position).
    REQUEST_NAME,           ///< Queue a request_device_name call (poll for stored device name).
    DISCOVER_AND_PAIR,      ///< Queue a discover_and_pair call (starts 3‑phase pairing flow).
  };

  /// @brief A single queued operation to be processed in loop().
  struct PendingOperation {
    PendingOperationType type;  ///< Operation type (determines which queue handler to invoke).
    std::string device_id;      ///< Target device ID (hex string, e.g., "123ABC").
    uint8_t position{0};        ///< Position/tilt value (0–100) or binary state (ON/UNLOCK=0, OFF/LOCK=100).
    uint8_t tilt{0};            ///< Tilt value for SET_POSITION_AND_TILT (0–100).
    CoverCommand command{CoverCommand::STOP};  ///< Named command for DEVICE_COMMAND operations.
  };

  /// @brief Debug snapshot of the last exchange attempt.
  struct ExchangeDebugInfo {
    const char *stage{"idle"};       ///< Current stage name (e.g., "TX_REQUEST", "WAIT_FIRST_RESPONSE", "FAILED").
    uint8_t tries{0};                ///< Try number (1‑based; increments on each retry within EXCHANGE_RETRY_COUNT).
    uint8_t request_cmd{0};          ///< Command ID of the original request (e.g., CMD_EXECUTE=0x00).
    bool saw_challenge{false};       ///< True if a challenge (0x3C) was seen during the exchange.
    bool capture_valid{false};       ///< True if radio capture data is valid for the last packet seen.
    bool capture_rx_done{false};     ///< True if RxDone interrupt fired (packet fully received).
    bool capture_crc_error{false};   ///< True if CRC error flagged (SX1262 only; SX1276 IoHomeOn filters in hardware).
    uint32_t capture_freq_hz{0};     ///< RF frequency of the captured packet (Hz).
    uint16_t capture_irq_status{0};  ///< Raw IRQ status register value from the radio chip.
    uint8_t capture_packet_status{0};  ///< Packet status byte (chip-specific; SX1262 includes CRC flag).
    uint8_t capture_reported_len{0};   ///< Length reported by the radio's packet engine.
    uint8_t capture_frame_len{0};      ///< Length of the parsed protocol frame after recovery/UART decoding.
    int16_t capture_rssi_dbm{0};       ///< Received signal strength of the captured packet (dBm, negative).
  };

  void reset_exchange_debug_(uint8_t request_cmd);
  void record_exchange_debug_(const char *stage, uint8_t tries, bool saw_challenge);
  void log_exchange_debug_(const char *device_id) const;

  // --- Frequency hopping ---
  void hop_frequency_();

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
  uint8_t tx_power_{DEFAULT_TX_POWER_DBM};
  uint8_t pa_pin_{DEFAULT_PA_PIN_PA_BOOST};
  uint8_t tcxo_voltage_{DEFAULT_TCXO_VOLTAGE_SETTING_1P8V};  ///< SX1262 TCXO voltage setting (default 1.8 V)

  // --- Runtime state ---
  bool initialized_{false};
  bool busy_{false};
  bool radio_test_mode_{false};  ///< When true, loop() is suspended for loopback testing.
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
/// @ingroup hioc_platforms
class IOHomeDiscoverButton : public button::Button, public Component {
 public:
  void set_parent(IOHomeControlComponent *parent) { this->parent_ = parent; }
  void dump_config() override {}

 protected:
  /// @brief When button is pressed, queue a discovery/pair operation.
  void press_action() override { this->parent_->queue_discover_and_pair(); }
  IOHomeControlComponent *parent_{nullptr};
};

// ----------------------------------------------------------------------------
// Test-visible helpers (inline for host unit tests)
// ----------------------------------------------------------------------------

/// Check if a stored node ID is valid (not all-zero, not all-0xFF).
/// @param id 3‑byte node ID buffer.
/// @return true if the ID is non-zero and non-0xFF.
inline bool stored_node_id_is_valid(const uint8_t id[NODE_ID_SIZE]) {
  bool all_zero = true;
  bool all_ff = true;
  for (uint8_t i = 0; i < NODE_ID_SIZE; i++) {
    all_zero = all_zero && id[i] == 0;
    all_ff = all_ff && id[i] == UINT8_MAX;
  }
  return !all_zero && !all_ff;
}

/// Format a position float as a human‑readable string (e.g. "50%", "unknown").
/// @param pos Position value (0–100 or UNKNOWN_POSITION).
/// @return String like "50%" or "unknown".
inline std::string format_position(float pos) {
  if (pos == UNKNOWN_POSITION) {
    return "unknown";
  }
  char buf[POSITION_TEXT_BUFFER_SIZE];
  snprintf(buf, sizeof(buf), "%.0f%%", pos);
  return buf;
}

}  // namespace home_io_control
}  // namespace esphome
