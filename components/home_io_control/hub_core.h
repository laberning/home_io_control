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
///   - loop() drains the OperationQueue collaborator (serializes all radio work).
///   - All outbound commands go through send_and_receive_, a thin wrapper around
///     ExchangeEngine which owns retry, timing, and challenge-response auth.
///   - Inbound frames are processed in process_received_packet_ and may trigger
///     inbound authentication (ExchangeEngine::authenticate_request) if the device proves itself.
///   - DeviceRegistry and its callbacks provide fan-out to platform entities
///     (covers/lights/switches/locks); StatusPollPolicy schedules follow-up polls;
///     PairingEngine owns pairing; ManagementActions owns the rename/identify/force-open
///     hub-level Home Assistant actions.

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/api/custom_api_device.h"
#include "esphome/components/spi/spi.h"
#include "esphome/components/button/button.h"
#include "proto_codecs.h"
#include "proto_frame.h"
#include "radio_interface.h"
#include "tuning_config.h"
#include "hub_exchange.h"
#include "hub_decisions.h"
#include "hub_pairing.h"
#include "device_registry.h"
#include "status_poll_policy.h"
#include "operation_queue.h"
#include "exchange_engine.h"
#include "pairing_engine.h"
#include "management_actions.h"
#include "pairing_responder.h"
#include <map>
#include <vector>
#include <functional>

namespace esphome {
namespace home_io_control {

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
 public:
  /// Initialize ExchangeEngine, PairingEngine, and ManagementActions with double-pointer/
  /// reference indirection so that test assignments (`comp.radio_ = &mock`) propagate
  /// through all collaborators without calling setup().
  IOHomeControlComponent()
      : exchange_engine_(&radio_, node_id_, system_key_, &tuning_),
        pairing_engine_(&radio_, node_id_, system_key_, &tuning_, exchange_engine_, registry_, pairing_telemetry_),
        management_actions_(node_id_, exchange_engine_, registry_, &initialized_, this) {}

  /// @brief Result payload used by hub-level management actions such as rename.
  /// Alias of the standalone esphome::home_io_control::ManagementActionResult struct so that
  /// callers using the nested name IOHomeControlComponent::ManagementActionResult continue to work.
  using ManagementActionResult = esphome::home_io_control::ManagementActionResult;

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
  /// Set the DIO1 interrupt pin (SX1262; also carries the LR1121's DIO9 IRQ line).
  void set_dio1_pin(InternalGPIOPin *pin) { this->dio1_pin_ = pin; }
  /// Set the BUSY pin (SX1262/LR1121).
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
  /// Set radio type ("sx1276", "sx1262", or "lr1121"); empty string means auto‑detect
  /// (auto-detect only ever selects sx1276 or sx1262 — lr1121 is explicit-only).
  void set_radio_type(const std::string &type) { this->radio_type_ = type; }
  /// Set TCXO voltage for SX1262/LR1121 (1.8V / 3.3V).
  void set_tcxo_voltage(uint8_t voltage) { this->tcxo_voltage_ = voltage; }

  /// Apply the tuning configuration generated from YAML / UI entities.
  void set_tuning_config(const TuningConfig &config) { this->tuning_ = config; }
  /// Receive a numeric tuning update from a HA `number` entity.
  void update_tuning_number(const std::string &name, float value);
  /// Receive a select tuning update from a HA `select` entity.
  void update_tuning_select(const std::string &name, const std::string &value);
  /// Current value of a numeric tuning parameter, used to seed a HA `number` entity on boot.
  /// @param name YAML key of the parameter.
  /// @return Current value, or 0 for an unknown key.
  [[nodiscard]] float get_tuning_number_value(const std::string &name) const;
  /// Current option string of a select tuning parameter, used to seed a HA `select` entity on boot.
  /// @param name YAML key of the parameter.
  /// @return Current value formatted as its YAML option string, or empty for an unknown key.
  [[nodiscard]] std::string get_tuning_select_value(const std::string &name) const;

  /// Declare that a remote (identified by its node ID) controls a registered device.
  /// When activity from this remote is overheard, a status poll is scheduled for the device.
  /// This is needed for 1W remotes whose destination address differs from the device's 2W ID.
  /// @param remote_id Node ID of the remote control.
  /// @param device_id Node ID of the device it controls.
  void add_linked_remote(const std::string &remote_id, const std::string &device_id) {
    this->registry_.add_linked_remote(remote_id, device_id);
  }

  /// Declare that a device class's typed 1W broadcasts (e.g. "all awnings") also apply to
  /// @p device_id, matching how 1W remotes address a device class rather than a single node.
  /// @param type      Device class the broadcast targets.
  /// @param device_id Node ID of the device to add to that class.
  void add_linked_remote_class(DeviceType type, const std::string &device_id) {
    this->registry_.add_linked_remote_class(type, device_id);
  }

  /// Set an optimistic target position ahead of a confirming poll/response, and notify.
  /// No-op when the device is unknown or has `optimistic_state == false`. See
  /// DeviceRegistry::apply_optimistic_target() for the full contract.
  /// Virtual (like add_device/get_device) so platform unit tests can substitute a mock registry.
  /// @param device_id Target device ID.
  /// @param target_io_position Target position in IO units (0=open, 100=closed).
  /// @return true if the optimistic state was applied.
  virtual bool apply_optimistic_target(const std::string &device_id, float target_io_position) {
    return this->registry_.apply_optimistic_target(device_id, target_io_position);
  }

  /// Clear a device's optimistic target (e.g. on STOP), and notify.
  /// No-op when the device is unknown or has `optimistic_state == false`.
  /// Virtual (like add_device/get_device) so platform unit tests can substitute a mock registry.
  /// @param device_id Target device ID.
  /// @return true if the optimistic target was cleared.
  virtual bool clear_optimistic_target(const std::string &device_id) {
    return this->registry_.clear_optimistic_target(device_id);
  }

  /// Allow a 1W sender (identified by its node ID) to fire the `esphome.home_io_control_sender_event`
  /// event to Home Assistant. "Sender" is deliberately broader than "remote": the same 1W broadcast
  /// mechanism carries handheld/wall remotes and wind/rain sensors alike (they differ only in the
  /// `originator` byte inside the payload, not in how they address the radio) — see `decode_1w_frame()`.
  /// Overheard 1W traffic is always DEBUG-logged regardless of this list; this only controls which
  /// senders are allowed to reach Home Assistant as an event, independent of whether the sender is
  /// also linked to a device via `add_linked_remote`. Empty by default — a sender must be explicitly
  /// opted in.
  /// @param sender_id Node ID of the 1W sender (remote or sensor).
  void add_exposed_sender(const std::string &sender_id) { this->exposed_senders_.push_back(sender_id); }

  /// @return The telemetry recorded for the most recent (or in-progress) pairing attempt.
  [[nodiscard]] const PairingTelemetry &pairing_telemetry() const { return this->pairing_telemetry_; }

  /// Register a callback invoked once, right after every `discover_and_pair()` attempt
  /// completes — used by the "Last Pairing Result" text sensor to publish a fresh value.
  /// Single-slot: only one platform instance is expected per hub.
  /// @param cb Callable with no arguments.
  void set_pairing_result_callback(std::function<void()> cb) { this->pairing_result_callback_ = std::move(cb); }

  /// @brief Arm or disarm the "Accept Foreign Pairing (Key Extraction)" responder.
  ///
  /// Arming picks a fresh throwaway node ID, resets the pairing_responder state machine to
  /// ARMED_IDLE, and schedules a 10-minute auto-off. While armed, new 0x28/0x31/0x32 branches in
  /// process_received_packet_() emulate an unpaired device so a user's existing hub can pair to
  /// it and hand over its node_id/system_key (see pairing_responder.h). Disarming — manual, via
  /// the HA switch, on successful extraction, or on auto-off — immediately stops those branches
  /// from responding; it never touches the real device registry or the hub's own node_id_/
  /// system_key_. Virtual so platform unit tests can substitute a mock hub, matching every other
  /// queue_*/set_* entry point on this component.
  /// @param armed Desired state.
  virtual void set_key_extraction_armed(bool armed);

  /// Register a callback invoked whenever the key-extraction armed state changes — manual
  /// toggle, successful extraction, or auto-off timeout — so the switch entity can keep its
  /// displayed state in sync when the hub disarms itself rather than the user. Single-slot,
  /// mirrors set_pairing_result_callback().
  /// @param cb Callable receiving the new armed state.
  void set_key_extraction_armed_callback(std::function<void(bool)> cb) {
    this->key_extraction_armed_callback_ = std::move(cb);
  }

  // --- Device management (called by platform entities during setup) ---
  /// Add a device to the registry by device ID only (legacy/delegating overload).
  /// Type, subtype, and inverted default to UNKNOWN / 0 / false; use the 4-arg
  /// overload when type/subtype/inverted come from YAML declarations.
  /// @param device_id Hexadecimal node ID string.
  virtual void add_device(const std::string &device_id);
  /// Add a device to the registry with full metadata from YAML. Optimistic state defaults to
  /// enabled; use the 5-arg overload when the YAML declaration overrides it.
  /// @param device_id Hexadecimal node ID string.
  /// @param type Device type from YAML declaration (UNKNOWN if not specified).
  /// @param subtype Device subtype from YAML declaration.
  /// @param inverted Position inversion flag from YAML declaration.
  virtual void add_device(const std::string &device_id, DeviceType type, uint8_t subtype, bool inverted);
  /// Add a device to the registry with full metadata from YAML, including the optimistic-state flag.
  /// @param device_id Hexadecimal node ID string.
  /// @param type Device type from YAML declaration (UNKNOWN if not specified).
  /// @param subtype Device subtype from YAML declaration.
  /// @param inverted Position inversion flag from YAML declaration.
  /// @param optimistic_state Whether optimistic target updates are allowed for this device.
  virtual void add_device(const std::string &device_id, DeviceType type, uint8_t subtype, bool inverted,
                          bool optimistic_state);
  /// Retrieve a device by ID; returns nullptr if not found.
  /// @param device_id Hexadecimal node ID.
  /// @return Pointer to IoDevice, or nullptr.
  virtual IoDevice *get_device(const std::string &device_id);
  /// Register a callback invoked when any device updates.
  /// @param cb Callable with signature void(const std::string&, const IoDevice&).
  virtual void register_device_callback(DeviceUpdateCallback cb) { this->registry_.subscribe(std::move(cb)); }
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
  /// Trigger a device's physical identify (brief jog/flash) so a user can confirm which
  /// physical motor a device ID maps to.
  /// @param device_id Target device ID.
  /// @return Structured result describing success and any validation failure. `verified` is
  ///         always false — there is no readback for a physical identify jog.
  virtual ManagementActionResult identify_device(const std::string &device_id);
  /// @brief Move a cover device to fully open at elevated priority, intended to bypass
  /// wind/rain soft locks.
  ///
  /// Safety-sensitive: queues CoverCommand::FORCE_OPEN through the normal cover-command dispatch
  /// path. Only confirms the command was queued; the movement outcome arrives later via the
  /// device's normal cover-state/polling pipeline, so `verified` is always false. The lock-bypass
  /// behavior itself is experimental and unconfirmed against an active lock — see
  /// ManagementActions::force_open_device()'s doxygen for details.
  /// @param device_id Target device ID.
  /// @return Structured result describing whether the command was queued.
  virtual ManagementActionResult force_open_device(const std::string &device_id);
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
  /// @brief Queue an async named command (STOP, FAVORITE, VENT, FORCE_OPEN); returns immediately,
  /// executed in loop().
  ///
  /// Existing entity/button callers (cover, favorite button, vent button) intentionally ignore
  /// the return value — they always target a known, already-registered device. It exists so
  /// force_open_device() can report enqueue rejection distinctly from a queued-but-not-yet-run
  /// command.
  /// @param device_id Target device ID.
  /// @param cmd Named command to send.
  /// @return true if the hub is initialized, the device is registered, and the command matches
  ///         its capability class (so the command was enqueued); false otherwise.
  virtual bool queue_device_command(const std::string &device_id, CoverCommand cmd);
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

  // --- Key-extraction responder ("Accept Foreign Pairing") RX handlers ---
  // Small delegating wrappers called from process_received_packet_(); the actual decision logic
  // lives in pairing_responder.h so it stays pure and host-testable. See hub_key_extraction.cpp.

  /// Dispatch a frame to the key-extraction responder if it's one of its 0x28/0x31/0x32 frames
  /// and the responder is armed. Factored out of process_received_packet_() purely to keep that
  /// function's cognitive complexity under the clang-tidy threshold, mirroring
  /// PairingEngine::record_discovery_rx_telemetry_()'s reason for existing.
  /// @param frame Parsed inbound frame.
  /// @return true if the frame was handled (caller should stop further dispatch for it).
  bool try_handle_key_extraction_frame_(const IoFrame &frame);
  /// Handle an inbound CMD_DISCOVER_REQ (0x28) while the key-extraction responder is armed.
  /// @param frame Parsed inbound discovery broadcast.
  void handle_key_extraction_discover_(const IoFrame &frame);
  /// Handle an inbound CMD_KEY_INIT (0x31) addressed to our throwaway node ID while armed.
  /// @param frame Parsed inbound key-init frame.
  void handle_key_extraction_key_init_(const IoFrame &frame);
  /// Handle an inbound CMD_KEY_TRANSFER (0x32) addressed to our throwaway node ID while armed.
  /// @param frame Parsed inbound key-transfer frame.
  void handle_key_extraction_key_transfer_(const IoFrame &frame);
  /// Generate a random throwaway node ID for one key-extraction arm cycle, avoiding collisions
  /// with the broadcast addresses, this hub's own real node ID, and any registered device.
  /// @param out Output: 3-byte node ID.
  void generate_key_extraction_throwaway_id_(uint8_t out[NODE_ID_SIZE]);
  /// Transmit a key-extraction reply frame on all 3 IO-homecontrol channels, using the radio
  /// driver's response_preamble() rather than a fixed SHORT_PREAMBLE/LONG_PREAMBLE constant —
  /// long enough that a channel-hopping receiver reliably lands on it, short enough that 3
  /// sequential transmissions don't block the main loop for the better part of a second (see the
  /// implementation comment in hub_key_extraction.cpp for the hardware-confirmed reasoning).
  /// Shared by all three RX handlers so the preamble choice and channel list are defined once.
  /// @param frame Frame to broadcast (already built by the caller).
  void broadcast_key_extraction_reply_(const IoFrame &frame);
  /// Emit the security-sensitive "system key extracted" log block (see redaction.h — this is the
  /// one deliberate, explicit exception to that file's masking, not a loosening of it).
  void log_key_extraction_result_();

  /// Extract supported position or metadata info from a response frame and merge it into the device record.
  /// @param frame IoFrame containing a supported inbound command such as CMD_PRIVATE_RESP,
  /// CMD_STATUS_UPDATE, CMD_GET_NAME_RESP, or CMD_GET_INFO2_RESP.
  /// @param trust_position False to apply `is_stopped` but skip target/position decode for a
  /// CMD_PRIVATE_RESP — the immediate reply to our own just-sent CMD_EXECUTE echoes stale
  /// pre-command target/current values on at least some devices (see
  /// tests/corpus/captures/somfy_awning/execute_ack_reports_stale_target_*.yaml), so
  /// execute_request_and_update_() passes false there; every other caller trusts as before.
  void update_device_status_(const IoFrame &frame, bool trust_position = true);
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
  /// Schedule status polls for a fixed list of devices (shared by the id-linked and
  /// class-linked 1W paths, and by schedule_linked_remote_polls_()).
  /// @param device_ids Devices to poll.
  /// @param delay_ms Poll delay in milliseconds.
  void schedule_device_polls_(const std::vector<std::string> &device_ids, uint32_t delay_ms);
  /// Schedule status polls for all devices associated with a linked remote.
  /// @param remote_id Source node ID of the remote.
  /// @param delay_ms Poll delay; default REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS. A STOP intent
  ///        passes 0 (position settles immediately, no need to wait out the usual travel-time
  ///        assumption behind the default delay).
  void schedule_linked_remote_polls_(const std::string &remote_id,
                                     uint32_t delay_ms = REMOTE_ACTIVITY_STATUS_POLL_DELAY_MS);
  /// Resolve the set of devices a 1W frame should affect: devices linked to the sending remote
  /// by node ID, plus — when the frame targets a typed broadcast (e.g. "all awnings") — devices
  /// linked to that device class, deduplicated so a device linked both ways is touched once.
  /// @param info Already-decoded 1W frame info (see decode_1w_frame()).
  /// @param src_id Sender's node ID as a string (already computed by the caller).
  /// @return Deduplicated device IDs (may be empty).
  [[nodiscard]] std::vector<std::string> resolve_1w_target_devices_(const OneWayFrameInfo &info,
                                                                    const std::string &src_id) const;
  /// Apply optimistic target state to every device in @p device_ids, when the decoded frame
  /// carries a resolvable intent. Skips devices whose known type doesn't match the frame's
  /// typed-broadcast target (an "all awnings" press must not optimistically move a linked
  /// shutter — it is still polled by schedule_device_polls_()). No-op per device when that
  /// device has `optimistic_state == false` (see DeviceRegistry::apply_optimistic_target()).
  /// @param info Already-decoded 1W frame info (see decode_1w_frame()).
  /// @param device_ids Devices to apply optimistic state to (see resolve_1w_target_devices_()).
  /// @return true if the intent resolved to a STOP (caller should poll immediately).
  bool apply_optimistic_linked_state_(const OneWayFrameInfo &info, const std::vector<std::string> &device_ids);
  /// Fire the sender HA event for a decoded 1W frame, if the sender is exposed.
  /// DEBUG-logs the reason when it does not fire (API disconnected / sender not exposed) so a
  /// live log capture can distinguish "never reached this check" from "reached it and skipped".
  /// @param info Already-decoded 1W frame info (see decode_1w_frame()).
  /// @param linked True if the sender is linked to at least one registered device.
  /// @param src_id Sender's node ID as a string (already computed by the caller).
  void maybe_fire_sender_event_(const OneWayFrameInfo &info, bool linked, const std::string &src_id);
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
  /// Fire all registered device update callbacks for the given device ID.
  /// @param id Device ID that updated.
  void notify_device_update_(const std::string &id);
  /// Apply backoff after a failed background status poll and log the result.
  /// @param device_id Target device ID.
  /// @param auth_like True when the failed exchange saw a 0x3C challenge.
  void schedule_background_poll_backoff_(const std::string &device_id, bool auth_like);
  /// Pop next pending operation from the queue and execute it (set position, request status, discover).
  void process_pending_operation_();

  // --- Exchange helpers (thin wrappers delegating to ExchangeEngine) ---

  /// Log the last exchange debug snapshot (delegates to exchange_engine_).
  void log_exchange_debug_(const char *device_id) const { this->exchange_engine_.log_debug(device_id); }

  // --- Tuning ---
  /// Apply the current tuning configuration to the active radio driver.
  void apply_tuning_to_radio_();

  // --- Management actions (thin wrappers delegating to management_actions_) ---
  /// Register hub-level Home Assistant actions; called from setup().
  void register_management_actions_() { this->management_actions_.register_actions(); }
  /// Native API callback: rename a registered device.
  void api_rename_device_(const std::string &device_id, const std::string &new_name) {
    this->management_actions_.api_rename_device(device_id, new_name);
  }
  /// Native API callback: trigger a registered device's physical identify.
  void api_identify_device_(const std::string &device_id) { this->management_actions_.api_identify_device(device_id); }
  /// Native API callback: force-open a registered cover device.
  void api_force_open_device_(const std::string &device_id) {
    this->management_actions_.api_force_open_device(device_id);
  }

  // --- Frequency hopping ---
  void hop_frequency_();

  // --- Radio driver selection (called once from setup()) ---
  /// Select and construct the configured (or auto-detected) radio driver.
  ///
  /// Kept as its own method rather than inlined into setup(): the three-way chip branch
  /// (SX1276/SX1262/LR1121) plus auto-detection is enough logic that folding it into
  /// setup() pushes that function's cognitive complexity past clang-tidy's threshold.
  /// Validates the pins each driver needs and logs a clear error (without calling
  /// mark_failed() itself — the caller decides how to react) when a required pin is
  /// missing. On success, `*chip_name_out` is set to a static string naming the selected
  /// chip (used for logging), and the returned pointer is the heap-allocated (not yet
  /// initialized) driver instance.
  /// @param chip_name_out Output: human-readable chip name for logging (always set,
  ///                      even on failure, to the best-known name for error messages).
  /// @return Newly allocated RadioDriver, or nullptr if pin validation or allocation failed.
  RadioDriver *select_and_construct_radio_(const char **chip_name_out);

  // --- Radio driver ---
  RadioDriver *radio_{nullptr};

  // --- Hardware pins (set by YAML codegen, passed to radio driver in setup) ---
  InternalGPIOPin *rst_pin_{nullptr};
  InternalGPIOPin *dio0_pin_{nullptr};    ///< SX1276 DIO0 interrupt
  InternalGPIOPin *dio4_pin_{nullptr};    ///< SX1276 DIO4 preamble detect (optional)
  InternalGPIOPin *dio1_pin_{nullptr};    ///< SX1262 DIO1 interrupt; also carries the LR1121's DIO9 IRQ line
  InternalGPIOPin *busy_pin_{nullptr};    ///< SX1262/LR1121 BUSY pin
  InternalGPIOPin *fem_en_pin_{nullptr};  ///< Front-end module enable
  InternalGPIOPin *vfem_pin_{nullptr};    ///< Front-end module power
  InternalGPIOPin *fem_pa_pin_{nullptr};  ///< Front-end module PA switch

  // --- Configuration (from YAML) ---
  std::string node_id_str_;
  std::string system_key_str_;
  std::string radio_type_;  ///< "sx1276", "sx1262", "lr1121", or "" (auto-detect; never selects lr1121)
  uint8_t node_id_[NODE_ID_SIZE]{};
  uint8_t system_key_[AES_KEY_SIZE]{};
  uint8_t tx_power_{DEFAULT_TX_POWER_DBM};
  uint8_t pa_pin_{DEFAULT_PA_PIN_PA_BOOST};
  uint8_t tcxo_voltage_{DEFAULT_TCXO_VOLTAGE_SETTING_1P8V};  ///< SX1262/LR1121 TCXO voltage setting (default 1.8 V)

  // --- Runtime state ---
  bool initialized_{false};
  bool busy_{false};
  bool radio_test_mode_{false};  ///< When true, loop() is suspended for loopback testing.
  TuningConfig tuning_{};        ///< Runtime tuning overrides.
  DeviceRegistry registry_;
  /// 1W sender node IDs (remotes or sensors) allowed to fire the sender HA event
  /// (`add_exposed_sender`). Config-time list (populated once from YAML), not a per-frame allocation.
  std::vector<std::string> exposed_senders_;
  /// Invoked once after every pairing attempt completes; see set_pairing_result_callback().
  std::function<void()> pairing_result_callback_;
  /// State for the current "Accept Foreign Pairing" (key-extraction) arm cycle; DISARMED by
  /// default so a fresh boot never responds to foreign pairing traffic. See pairing_responder.h.
  pairing_responder::ResponderContext key_extraction_ctx_;
  /// Invoked whenever the key-extraction armed state changes; see set_key_extraction_armed_callback().
  std::function<void(bool)> key_extraction_armed_callback_;
  StatusPollPolicy poll_policy_;
  OperationQueue op_queue_;
  PairingTelemetry pairing_telemetry_;    ///< Per-attempt pairing telemetry, shared with ExchangeEngine/PairingEngine.
  ExchangeEngine exchange_engine_;        ///< Owns all authenticated exchange and LBT/hop logic.
  PairingEngine pairing_engine_;          ///< Owns the three-phase device pairing flow.
  ManagementActions management_actions_;  ///< Owns rename, identify, force-open, and other hub-level HA actions.

  /// @brief Tracks the last logged 1W frame per remote to suppress duplicates.
  ///
  /// 1W remotes repeat each command 4× at 40ms intervals (protocol requirement for
  /// reliability). Without dedup, a single button press generates 4+ identical log lines.
  struct OneWayDedup {
    std::string src_id;     ///< Last seen remote source ID.
    uint8_t cmd{0};         ///< Last seen command byte.
    uint32_t timestamp{0};  ///< millis() when last logged.
  };
  OneWayDedup last_1w_logged_{};
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
