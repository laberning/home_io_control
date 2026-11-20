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
#include "esphome/core/helpers.h"
#include "esphome/components/api/custom_api_device.h"
#include "esphome/components/spi/spi.h"
#include "proto_codecs.h"
#include "proto_device_kinds.h"
#include "proto_device_readings.h"
#include "proto_frame.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"
#include "radio_interface.h"
#include "tuning_config.h"
#include "hub_exchange.h"
#include "decisions_oneway.h"
#include "decisions_attempts.h"
#include "hub_pairing.h"
#include "device_registry.h"
#include "status_poll_policy.h"
#include "periodic_poll_policy.h"
#include "operation_queue.h"
#include "exchange_engine.h"
#include "pairing_engine.h"
#include "management_actions.h"
#include "heater_controller.h"
#include "transfer_reader.h"
#include "hub_ports.h"
#include "key_extraction_responder.h"
#include "oneway_controller.h"
#include "oneway_transmitter.h"
#include "oneway_key_adoption.h"
#include "oneway_receiver.h"
#include "overheard_frame_observer.h"
#include "device_event_log.h"
// lr1121_firmware_update_controller.h forward-declares FlashDecision / BootloaderUpgradePath /
// Lr1121FirmwareUpdater so lr1121_firmware_decisions.h and radio_lr1121_firmware_updater.h are
// NOT pulled into hub_core.h — that header weight stays in the collaborator's .cpp alone.
#include "lr1121_firmware_update_controller.h"
#include <map>
#include <span>
#include <vector>
#include <functional>

namespace esphome {
namespace home_io_control {

inline constexpr uint8_t DEFAULT_TX_POWER_DBM = 17;       ///< Default TX power used unless YAML overrides it.
inline constexpr uint8_t DEFAULT_PA_PIN_PA_BOOST = 0x80;  ///< SX1276 PA_CONFIG selector for the PA_BOOST output path.
inline constexpr uint8_t DEFAULT_TCXO_VOLTAGE_SETTING_1P8V = 0x02;  ///< 0-based TCXO voltage code for 1.8 V,
                                                                    ///< passed verbatim to the SX1262/LR1121 chip.
inline constexpr size_t POSITION_TEXT_BUFFER_SIZE = 16;  ///< Buffer for formatted position strings such as "100%".

#ifdef IOHOME_LR1121_FIRMWARE_UPDATE
/// Max value representable by Component::warn_if_blocking_over_ (a centisecond uint8_t, ~2550 ms
/// ceiling). Raised for a flash excursion so the log fills with progress output rather than
/// component-blocking warnings — a flash still runs far longer than 2550 ms, so this reduces
/// warning spam, it cannot eliminate it. Lives here rather than in the collaborator because
/// warn_if_blocking_over_ is protected on ESPHome's Component and only the initializer-list lambda
/// below legitimately writes it.
inline constexpr uint8_t LR1121_FLASH_WARN_BLOCKING_MAX_CS = 255;
#endif

/// @brief How long after a pairing attempt ends a Discover & Pair press counts as made during it.
///
/// Pairing blocks loop() for up to about 20 s, so a press made meanwhile is handled a few milliseconds
/// after the attempt ends (issue #165: the queued second attempt found nothing, the radiator had left
/// its pairing mode). A press a person makes after reading the result comes seconds later.
static constexpr uint32_t PAIRING_STALE_PRESS_MS = 1000;

// ============================================================================
// Main Component
// ============================================================================

/// The main IO-Homecontrol component. Manages the protocol layer and delegates
/// radio operations to a RadioDriver instance.
///
/// Inherits SPIDevice so that ESPHome's Python codegen can configure SPI pins.
/// Implements SpiAccess to provide the radio driver with SPI bus access.
/// Implements the three entity ports of hub_ports.h (HubDevicePort, HubHeatingPort, HubControlPort): the
/// entity platforms reach the hub only through them.
/// @ingroup hioc_hub
class IOHomeControlComponent : public Component,
                               public api::CustomAPIDevice,
                               public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                                     spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_8MHZ>,
                               public SpiAccess,
                               public HubDevicePort,
                               public HubHeatingPort,
                               public HubControlPort,
                               public OverheardFrameObserver {
 public:
  /// Initialize ExchangeEngine, PairingEngine, and ManagementActions with double-pointer/
  /// reference indirection so that test assignments (`comp.radio_ = &mock`) propagate
  /// through all collaborators without calling setup().
  IOHomeControlComponent()
      : exchange_engine_(&radio_, node_id_, system_key_, &tuning_),
        pairing_engine_(&radio_, node_id_, system_key_, &tuning_, exchange_engine_, registry_, pairing_telemetry_,
                        oneway_receiver_.recent_pairing_sighting()),
        management_actions_(node_id_, system_key_, &tuning_, exchange_engine_, registry_, &initialized_, this),
        heater_controller_(node_id_, &initialized_, &radio_, registry_, poll_policy_, exchange_engine_,
                           this->make_heater_controller_hub_()),
        transfer_reader_(node_id_, TRANSFER_RECIPES, this->make_transfer_reader_hub_()),
        // Capturing `this` is safe here: the callback is only ever invoked from send_burst(),
        // long after construction. It injects the *ability* to transmit rather than a reference
        // to whichever collaborator currently owns the radio. `&tuning_` is read per burst
        // (never cached), same pattern as exchange_engine_ above, so a live change to
        // `start_preamble_ms` takes effect on the next 1W command without a reboot.
        oneway_transmitter_([this](const IoFrame &frame, uint32_t freq,
                                   uint16_t preamble) { return this->transmit_frame_(frame, freq, preamble); },
                            &tuning_, [this](float ms) { return this->exchange_engine_.preamble_bytes_for_ms(ms); }),
        // set_timeout() is protected on the real ESPHome Component (only public in the host stub),
        // so a lambda defined here — with protected access — is the one legitimate caller. See
        // oneway_key_adoption.h's NamedTimeoutFn.
        oneway_key_adoption_([this](const char *name, uint32_t delay_ms, std::function<void()> cb) {
          this->set_timeout(name, delay_ms, std::move(cb));
        }),
        key_extraction_(
            node_id_, &radio_, &tuning_, registry_,
            [this](const IoFrame &frame, uint32_t freq, uint16_t preamble) {
              return this->transmit_frame_(frame, freq, preamble);
            },
            [this](const char *name, uint32_t delay_ms, std::function<void()> cb) {
              this->set_timeout(name, delay_ms, std::move(cb));
            })
#ifdef IOHOME_LR1121_FIRMWARE_UPDATE
        // Guarded initializer for the guarded member. `busy_`/`warn_if_blocking_over_` are
        // protected: the busy pointer is taken here, and the lambda — with protected access — is
        // the one legitimate writer of warn_if_blocking_over_. `this` doubles as SpiAccess* and as
        // the self key for App.scheduler.set_timeout(), never to reach a protected member.
        ,
        lr1121_firmware_update_(
            &radio_, this, &rst_pin_, &busy_pin_, &busy_,
            [this]() { this->warn_if_blocking_over_ = LR1121_FLASH_WARN_BLOCKING_MAX_CS; }, this)
#endif
  {
    // The hub only supplies what it knows about a target; the engine draws its own decisions from
    // it (the wake belief and the re-send of an unconfirmed movement command). Installed here
    // rather than in setup() so a component that never runs setup() (the host tests) is wired
    // exactly like production.
    this->exchange_engine_.set_target_evidence_provider([this](const uint8_t *dst, decisions::TargetEvidence &out) {
      const IoDevice *dev = this->registry_.get(node_id_to_string(dst));
      if (dev == nullptr)
        return false;
      out = decisions::target_evidence(*dev);
      return true;
    });
    this->exchange_engine_.set_device_key_provider([this](const uint8_t *node) -> const uint8_t * {
      const IoDevice *dev = this->registry_.get(node_id_to_string(node));
      return dev != nullptr && dev->has_system_key ? dev->system_key : nullptr;
    });
    this->exchange_engine_.set_wake_evidence_spent_handler([this](const uint8_t *dst) {
      if (IoDevice *dev = this->registry_.get(node_id_to_string(dst)); dev != nullptr)
        clear_moving_evidence(*dev);
    });
    this->exchange_engine_.set_overheard_frame_observer(this);
    // Channel access draws its channels and back-offs from ESPHome's random source (a settable value
    // in the host tests, like the poll jitter).
    this->exchange_engine_.set_random_source([]() { return random_uint32(); });
  }

  /// @brief Result payload used by hub-level management actions such as rename.
  /// Alias of the standalone esphome::home_io_control::ManagementActionResult struct so that
  /// callers using the nested name IOHomeControlComponent::ManagementActionResult continue to work.
  using ManagementActionResult = esphome::home_io_control::ManagementActionResult;

  /// OverheardFrameObserver: keep a 1W frame an exchange wait ignored as unrelated, for
  /// drain_overheard_oneway_frames_() once the exchange has ended. A two-way frame is not kept.
  void on_overheard_frame(const IoFrame &frame, const RadioRxPacket &packet, int16_t rssi_dbm, uint32_t now) override;

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
  /// Set which FEM part fem_pa_pin is wired to (`fem:` in YAML) — selects whether the SX1262
  /// driver drives that pin per-transmission (GC1109/KCT8103L/XY16P35) or leaves it static-HIGH
  /// (NONE, the legacy raw-pin behaviour).
  void set_fem_profile(FemProfile profile) { this->fem_profile_ = profile; }
  /// Set the controller's node ID (hex string).
  void set_node_id(const std::string &id) { this->node_id_str_ = id; }
  /// Set the system key (hex string).
  void set_system_key(const std::string &key) { this->system_key_str_ = key; }
  /// Set transmit power (dBm).
  void set_tx_power(uint8_t power) { this->tx_power_ = power; }
  /// Set PA boost pin configuration.
  void set_pa_pin(uint8_t pa_pin) { this->pa_pin_ = pa_pin; }
  /// Set radio type ("sx1276", "sx1262", or "lr1121"); required by the YAML schema.
  void set_radio_type(const std::string &type) { this->radio_type_ = type; }
  /// Set the SX1262/LR1121 TCXO control-voltage code (0-based, `TCXO_VOLTAGE_OPTIONS` in
  /// `hub_validators.py`: `1_6V`=0x00 .. `3_3V`=0x07), or `TCXO_VOLTAGE_NONE` (0xFF) for a board with a
  /// bare crystal and no DIO3-controlled TCXO.
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

  /// @brief Render a device's "last commanded by" string, resolving this hub's own node ID.
  ///
  /// Thin wrapper over detail::describe_last_commander() (entity_helpers.h); exists because the
  /// hub's node ID is not reachable from a companion entity.
  /// @param dev Device record to read.
  /// @return See detail::describe_last_commander().
  [[nodiscard]] std::string describe_last_commander(const IoDevice &dev) const override;

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
  /// Part of HubDevicePort; platform tests drive the entity against a port double instead.
  /// @param device_id Target device ID.
  /// @param target_io_position Target position in IO units (0=open, 100=closed).
  /// @return true if the optimistic state was applied.
  bool apply_optimistic_target(const std::string &device_id, float target_io_position) override {
    return this->registry_.apply_optimistic_target(device_id, target_io_position);
  }

  /// Predict that a device has stopped (e.g. on STOP), and notify.
  /// No-op when the device is unknown or has `optimistic_state == false`. See
  /// DeviceRegistry::apply_optimistic_stop() for why this records a prediction rather than a clear.
  /// Restorable: if the STOP then fails, the movement prediction it replaced comes back.
  /// Part of HubDevicePort; platform tests drive the entity against a port double instead.
  /// @param device_id Target device ID.
  /// @return true if the optimistic stop was applied.
  bool apply_optimistic_stop(const std::string &device_id) override {
    // Restorable: this is the entity's own STOP, which the hub is about to send and which can fail.
    return this->registry_.apply_optimistic_stop(device_id, /*restorable=*/true);
  }

  /// Set an optimistic slat angle ahead of a confirming status poll, and notify.
  /// No-op when the device is unknown, has `optimistic_state == false`, or is not tilt-capable.
  /// See DeviceRegistry::apply_optimistic_tilt() for the full contract and for why a tilt
  /// command cannot rely on its own reply the way a position command can.
  /// Part of HubDevicePort; platform tests drive the entity against a port double instead.
  /// @param device_id    Target device ID.
  /// @param tilt_percent Slat angle in the same percent scale as `IoDevice::tilt` (0-100).
  /// @return true if the optimistic tilt was applied.
  bool apply_optimistic_tilt(const std::string &device_id, float tilt_percent) override {
    return this->registry_.apply_optimistic_tilt(device_id, tilt_percent);
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
  void add_exposed_sender(const std::string &sender_id) { this->oneway_receiver_.add_exposed_sender(sender_id); }

  /// Register a configured 1W controller identity (see oneway_controller.h). Called once per
  /// `oneway_controllers:` entry from generated code. Both the source address and the key are
  /// already resolved at schema time — a derived address is computed there so a collision with
  /// the hub's own address or another identity fails the build rather than silently desyncing a
  /// transmitter at runtime.
  /// @param identity Fully-resolved controller identity.
  void add_oneway_controller(const OneWayControllerIdentity &identity) {
    this->oneway_transmitter_.add_identity(identity);
  }

  /// @return The configured 1W controller identities.
  [[nodiscard]] const OneWayControllerRegistry &oneway_controllers() const {
    return this->oneway_transmitter_.identities();
  }

  /// @brief Queue a 1W named command, sent as the given controller identity.
  ///
  /// Goes through the operation queue like every other radio operation (ADR 0013), so a 1W burst
  /// can never interleave with a 2W exchange. Unlike a 2W command this reports nothing back: 1W
  /// has no reply, so a queued command that a device ignores is indistinguishable from one it
  /// obeyed. The "Last 1W Command" diagnostic reports what was *transmitted*, which is the only
  /// half of that the hub can know.
  /// @param controller_id Controller-identity handle from `oneway_controllers:`.
  /// @param cmd Named command (STOP, FAVORITE, VENT, FORCE_OPEN); PARTIAL and PEDESTRIAN have no 1W
  ///        encoding and are not transmitted.
  void send_oneway_command(const std::string &controller_id, CoverCommand cmd) {
    this->op_queue_.enqueue_oneway_command(controller_id, cmd);
  }

  /// @brief Queue a 1W numeric position, sent as the given controller identity.
  /// @param controller_id Controller-identity handle from `oneway_controllers:`.
  /// @param position Target position 0–100 (0 = fully open, 100 = fully closed), as the user means it;
  ///        the transmitter mirrors it on the wire for an inverted identity (effective_inverted()).
  void send_oneway_position(const std::string &controller_id, uint8_t position) {
    this->op_queue_.enqueue_oneway_position(controller_id, position);
  }

  /// @brief Queue whichever of position/command a generated button's action resolves to.
  ///
  /// The mapping is applied here, at enqueue time, so the queue only ever holds concrete
  /// operations — OPEN and CLOSE are positions on the wire, not commands, and nothing downstream
  /// should have to know that twice.
  /// @param controller_id Controller-identity handle from `oneway_controllers:`.
  /// @param action Button action to send.
  void send_oneway_action(const std::string &controller_id, OneWayButtonAction action) override {
    const OneWayActionEncoding encoding = encode_oneway_action(action);
    if (encoding.is_position) {
      this->send_oneway_position(controller_id, encoding.position);
    } else {
      this->send_oneway_command(controller_id, encoding.command);
    }
  }

  /// @brief Queue a 1W enrollment for the given controller identity — the enroll button's press
  /// handler.
  ///
  /// Sends `0x39` (self-directed) then `0x30`, back to back — see
  /// OneWayTransmitter::send_enrollment().
  /// @param controller_id Controller-identity handle from `oneway_controllers:`.
  void send_oneway_enroll(const std::string &controller_id) override {
    this->op_queue_.enqueue_oneway_enroll(controller_id);
  }

  /// @brief Queue a standalone 1W un-enrollment (remove-controller) for the given controller
  /// identity, reached only through the explicitly-named `oneway_remove_controller` native API
  /// action — the same `0x39` send_oneway_enroll() also fires as its own prelude, but here alone.
  ///
  /// @warning **Works only inside the receiver's programming window.** Confirmed on a Somfy Izymo
  /// dimmer only: put the receiver in its programming mode first, then fire this; with no window
  /// open it changes nothing. Other receivers are untested. See ADR 0026 § Consequences.
  /// @param controller_id Controller-identity handle from `oneway_controllers:`.
  void send_oneway_unenroll(const std::string &controller_id) {
    this->op_queue_.enqueue_oneway_unenroll(controller_id);
  }

  /// @brief Subscribe to the report emitted after every 1W command attempt.
  ///
  /// A list rather than a single slot: each identity gets its own "Last 1W Command" sensor, and
  /// each filters the reports down to its own handle.
  /// @param callback Invoked for every attempt, successful or not.
  void add_oneway_command_report_callback(OneWayCommandReportFn callback) override {
    this->oneway_report_callbacks_.push_back(std::move(callback));
  }

  /// @return The 1W transmit collaborator, for diagnostics and the sequence-resync path.
  [[nodiscard]] OneWayTransmitter &oneway_transmitter() { return this->oneway_transmitter_; }

  /// @return The telemetry recorded for the most recent (or in-progress) pairing attempt.
  [[nodiscard]] const PairingTelemetry &pairing_telemetry() const { return this->pairing_telemetry_; }

  /// @return The most recent pairing attempt as the structured line of PairingTelemetry::result_sensor_string().
  [[nodiscard]] std::string pairing_result_text() const override {
    return this->pairing_telemetry_.result_sensor_string();
  }

  /// Register a callback invoked once, right after every `discover_and_pair()` attempt
  /// completes — used by the "Last Pairing Result" text sensor to publish a fresh value.
  /// Single-slot: only one platform instance is expected per hub.
  /// @param cb Callable with no arguments.
  void set_pairing_result_callback(std::function<void()> cb) override {
    this->pairing_result_callback_ = std::move(cb);
  }

  /// @return How many device events the DeviceEventLog has recorded since boot.
  [[nodiscard]] uint32_t device_event_count() const { return this->device_event_log_.count(); }

  /// @return The most recent device event rendered as `#<n> t=<uptime s> <SRC> <cmd> <payload hex>`,
  /// or an empty string before the first one. The counter makes a repeated identical report a new
  /// value, which Home Assistant's history needs (it records state changes only).
  [[nodiscard]] const std::string &last_device_event() const override { return this->device_event_log_.last(); }

  /// Record a diagnostic probe's reply on the "Last Device Event" sensor when it is a device event
  /// report (decisions::is_device_event_report()), such as the event a `subscribe_event` probe draws
  /// at once. The reply is only recorded, never applied.
  /// @param reply The probe's reply frame.
  /// @return true when it was recorded.
  bool record_probe_reply_event(const IoFrame &reply);

  /// Run one authenticated request/answer exchange on the command channel with the shared exchange bookkeeping
  /// (exchange counters, link health of every ending, result code of an error reply, device event reports) and
  /// no poll backoff. The path of the `read_object` probe and of the transfer reader.
  /// @param device_id Target device ID.
  /// @param request Request frame to send.
  /// @param[out] response The device's reply.
  /// @return REPLY when a reply frame, an error reply included, is in @p response; otherwise whether the device at
  ///         least accepted the request (ACCEPTED_NO_REPLY) or stayed silent (SILENT).
  RecordedAnswer exchange_recorded(const std::string &device_id, const IoFrame &request, IoFrame &response);

  /// True while the transfer reader holds a transfer open to @p device_id (TransferReader::transfer_open_to()); the
  /// `read_object` probe refuses then.
  [[nodiscard]] bool transfer_open_to(const std::string &device_id) const {
    return this->transfer_reader_.transfer_open_to(device_id);
  }

  /// Register a callback invoked after every recorded device event, used by the "Last Device Event"
  /// text sensor. Single-slot, mirrors set_pairing_result_callback().
  /// @param cb Callable with no arguments.
  void set_device_event_callback(std::function<void()> cb) override {
    this->device_event_log_.set_callback(std::move(cb));
  }

  /// @brief Arm or disarm the "Recover System Key" (key extraction) responder.
  ///
  /// Thin forwarder to the KeyExtractionResponder collaborator (key_extraction_responder.h).
  /// Arming picks a fresh throwaway node ID, resets the pairing_responder state machine to
  /// ARMED_IDLE, and schedules a 10-minute auto-off. While armed, the responder's try_handle_frame()
  /// (called from process_received_packet_()) answers pairing frames as an unpaired device would so a user's existing
  /// hub can pair to it and hand over its node_id/system_key (see pairing_responder.h). Disarming — manual, via the HA
  /// switch, on successful extraction, or on auto-off — immediately stops those answers; it never touches the real
  /// device registry or the hub's own node_id_/ system_key_. Part of HubControlPort, through which the hub-level switch
  /// arms it.
  /// @param armed Desired state.
  void set_key_extraction_armed(bool armed) override { this->key_extraction_.set_armed(armed); }

  /// Register a callback invoked whenever the key-extraction armed state changes — manual
  /// toggle, successful extraction, or auto-off timeout — so the switch entity can keep its
  /// displayed state in sync when the hub disarms itself rather than the user. Single-slot,
  /// mirrors set_pairing_result_callback().
  /// @param cb Callable receiving the new armed state.
  void set_key_extraction_armed_callback(std::function<void(bool)> cb) override {
    this->key_extraction_.set_armed_callback(std::move(cb));
  }

  /// Arm or disarm the 1W controller-key adoption listener. Thin forwarder to the OnewayKeyAdoption
  /// collaborator (oneway_key_adoption.h) — while armed, an overheard CMD_ONEWAY_ADD_CONTROLLER
  /// broadcast is decrypted and reported once, after which the listener disarms itself (one
  /// adoption per arm). Receive-only: unlike 2W key extraction this never transmits, it only
  /// listens for a frame a 1W device broadcasts of its own accord. Part of HubControlPort, through
  /// which the hub-level switch arms it.
  /// @param armed Desired state.
  void set_oneway_key_adoption_armed(bool armed) override { this->oneway_key_adoption_.set_armed(armed); }

  /// Register a callback invoked whenever the 1W key-adoption armed state changes — manual
  /// toggle, successful adoption, or auto-off timeout — so the switch entity stays in sync when
  /// the hub disarms itself rather than the user. Single-slot, mirrors
  /// set_key_extraction_armed_callback().
  /// @param cb Callable receiving the new armed state.
  void set_oneway_key_adoption_armed_callback(std::function<void(bool)> cb) override {
    this->oneway_key_adoption_.set_armed_callback(std::move(cb));
  }

  /// Whether the 1W key-adoption listener is currently armed.
  /// @return true while armed.
  [[nodiscard]] bool oneway_key_adoption_armed() const { return this->oneway_key_adoption_.armed(); }

  /// @brief Set whether DiagnosticProbes::probe_device()/probe_sweep() are allowed to run.
  ///
  /// Set once from the `diagnostic_probes:` YAML boolean (`__init__.py`); off by default, so a
  /// build that doesn't opt in never sends an undecoded probe opcode. Not a runtime toggle: there
  /// is no entity and nothing else calls this after setup — the gate is "was this build
  /// configured with `diagnostic_probes: true`", not a state a user flips per session.
  /// @param enabled Desired state.
  void set_diagnostic_probes_enabled(bool enabled) { this->diagnostic_probes_enabled_ = enabled; }

  /// @brief Whether diagnostic probes are enabled for this build.
  [[nodiscard]] bool diagnostic_probes_enabled() const { return this->diagnostic_probes_enabled_; }

  // --- Device management (called by platform entities during setup) ---
  /// Add a device to the registry by device ID only (undeclared/legacy path).
  /// Type, subtype and optimistic_state default to UNKNOWN / 0 / true, with no overrides; use the
  /// `DeviceConfig` overload when metadata comes from a YAML declaration.
  /// @param device_id Hexadecimal node ID string.
  void add_device(const std::string &device_id) override;
  /// Add a device to the registry with full metadata from a YAML declaration.
  /// @param device_id Hexadecimal node ID string.
  /// @param cfg Device type/subtype/overrides/optimistic-state metadata.
  void add_device(const std::string &device_id, const DeviceConfig &cfg) override;
  /// Retrieve a device by ID; returns nullptr if not found.
  /// @param device_id Hexadecimal node ID.
  /// @return Pointer to IoDevice, or nullptr.
  IoDevice *get_device(const std::string &device_id) override;
  /// True once loop() has seen the radio latch a failure after setup (a runtime BUSY timeout, for
  /// example). Sticky: a failed driver never recovers without a reboot. Every device's Active Issue
  /// sensor reports it.
  [[nodiscard]] bool radio_stopped_responding() const override { return this->radio_failure_reported_; }
  /// Select a device's travel speed at runtime (see IOHomeCoverTravelSpeedSelect and
  /// IOHomeCoverSilentSwitch). Part of HubDevicePort.
  /// @param device_id Hexadecimal node ID string.
  /// @param speed     Speed profile of the device's position moves.
  void set_device_travel_speed(const std::string &device_id, TravelSpeed speed) override;
  /// Register a callback invoked when any device updates.
  /// @param cb Callable with signature void(const std::string&, const IoDevice&).
  void register_device_callback(DeviceUpdateCallback cb) override { this->registry_.subscribe(std::move(cb)); }
  /// @brief The periodic polls, each with its own opt-in schedule (PeriodicPollPolicy).
  ///
  /// STATUS_REFRESH reads nothing of its own and queues an ordinary status request; DEVICE_READING reads one
  /// reading of the reading table (a data object, an event or a limitation: see request_device_reading()), one
  /// schedule per device and reading.
  enum class PeriodicPoll : uint8_t {
    STATUS_REFRESH,  ///< A status read for a device that reported nothing for a whole interval.
    DEVICE_READING,  ///< A scheduled reading (DataObjectSource, EventSource, LimitationSource), keyed by device and
                     ///< reading.
  };

  /// Configure the optional follow-up polling interval for a registered device.
  /// @param device_id Target device ID.
  /// @param poll_interval_ms Poll interval in milliseconds; zero keeps the legacy one-shot settle poll only.
  void set_device_status_poll_interval(const std::string &device_id, uint32_t poll_interval_ms) override;
  /// Opt a registered device into the status refresh at rest: a status read whenever the device has
  /// not reported its status for a whole interval.
  /// @param device_id Target device ID.
  /// @param interval_ms Refresh interval in milliseconds; zero removes the device from the schedule.
  void set_device_status_refresh_interval(const std::string &device_id, uint32_t interval_ms) override;
  /// Opt a registered device into the scheduled read of one reading. A data-object reading is first read
  /// DEVICE_READING_FIRST_READ_DELAY_MS after the call, then every interval (raised to
  /// DEVICE_READING_MIN_INTERVAL_MS); an event or limitation reading starts at a random delay of
  /// periodic_poll_first_delay_ms() and repeats every interval as given (the YAML validates its floor).
  /// @param device_id Target device ID.
  /// @param reading A reading with a DataObjectSource, EventSource or LimitationSource; any other is ignored, and so
  ///        is a reading whose capability the device lacks.
  /// @param interval_ms Read interval in milliseconds; zero removes the reading from the schedule.
  void set_device_reading_poll_interval(const std::string &device_id, DeviceReading reading,
                                        uint32_t interval_ms) override;

  // --- High-level operations ---
  /// Send a position command to a device.
  /// @param device_id Target device ID.
  /// @param position Desired position, 0–100 (open→closed). Named commands (STOP, FAVORITE,
  ///        VENT) go through execute_device_command_()/create_execute_command() instead.
  /// @return true if device acknowledged; false on timeout or radio error, or when the device's
  ///         features do not accept the value (an intermediate position on a device without
  ///         device_feature::LEVELS): nothing is sent and the optimistic target is withdrawn.
  bool set_device_position(const std::string &device_id, uint8_t position);
  /// Send a tilt command to a tilt‑capable cover.
  /// @param device_id Target device ID.
  /// @param tilt_percent Desired tilt (0–100).
  /// @return true if device acknowledged; false otherwise.
  bool set_device_tilt(const std::string &device_id, uint8_t tilt_percent);
  /// Set both position and tilt of a tilt-capable cover in one atomic command.
  /// @param device_id Target device ID.
  /// @param position Desired position (0–100, open→closed).
  /// @param tilt_percent Desired tilt (0–100).
  /// @return true if device acknowledged; false otherwise.
  bool set_device_position_and_tilt(const std::string &device_id, uint8_t position, uint8_t tilt_percent);
  /// Request current status from a device.
  /// @param device_id Target device ID.
  /// @return true if status frame was received and processed.
  bool request_device_status(const std::string &device_id);
  /// Read one reading that is a scheduled read: defer while the device moves, then ask for it by its source.
  ///
  /// A data-object reading queues a transfer read of its recipe; the answer arrives later, from the loop, and is
  /// folded in by apply_device_reading_answer_(). An event or limitation reading is one exchange (the event
  /// subscription, the limitation read) whose answer is the reading, accepted only inside it (ADR 0022, ADR 0064), so
  /// it never reaches the position decode and cannot move a device's position or target; a moving device is not
  /// asked and the poll is deferred instead.
  /// @param device_id Target device ID.
  /// @param reading A reading with a DataObjectSource, EventSource or LimitationSource.
  /// A data-object reading whose last read is still queued or running is not queued again (ADR 0108).
  /// @return true if a data-object read was queued or an answer of the captured shape arrived and was applied.
  bool request_device_reading(const std::string &device_id, DeviceReading reading);
  /// Request the stored device name from a device.
  /// @param device_id Target device ID.
  /// @return true if a name response frame was received and processed.
  bool request_device_name(const std::string &device_id);
  /// Discover and pair a device that is in pairing mode.
  /// @return true if pairing completed successfully; false otherwise.
  bool discover_and_pair();
  /// Send an arbitrary IO position (0-100) to a light entity. Internally mapped to the shared
  /// execute path; a binary light sends the two extremes through queue_set_light_state(), a dimmable
  /// one anything in between through queue_set_light_position().
  /// @param device_id Target device ID.
  /// @param position Desired IO position (0-100); this device family's convention maps 0 to full
  ///        brightness and 100 to off, the same 0-100 scale platform_cover.cpp uses.
  /// @return true if device acknowledged.
  bool set_light_position(const std::string &device_id, uint8_t position);
  /// Send a heat level to an exterior heater (a fan entity) as an IO position. Mapped to the shared
  /// execute path, like set_light_position().
  /// @param device_id Target device ID.
  /// @param position Desired IO position (0-100): 0 = full heat, 100 = off (the KLF 200 exterior
  ///        heating mapping, MP = position x 0x200).
  /// @return true if device acknowledged.
  bool set_fan_position(const std::string &device_id, uint8_t position);
  /// Semantic binary helper for switch entities. Internally mapped to the shared execute path.
  /// @param device_id Target device ID.
  /// @param on Desired on/off state.
  /// @return true if device acknowledged.
  bool set_switch_state(const std::string &device_id, bool on);
  /// Semantic lock helper for lock entities. Internally mapped to the shared execute path.
  /// @param device_id Target device ID.
  /// @param locked Desired locked/unlocked state.
  /// @return true if device acknowledged.
  bool set_lock_state(const std::string &device_id, bool locked);
  // --- Heating and hot water: one-line forwarders to HeaterController (ADR 0083) ---
  // The contracts are documented on the HeaterController methods.

  /// @brief Run one heating function on the heater's private register protocol (a write for a settable
  /// register, a read for a read-only one). The `heating_control` action and the climate entity call this.
  /// @see HeaterController::send_heating_command()
  bool send_heating_command(const std::string &device_id, DeviceRegister fn, float value) override {
    return this->heater_controller_.send_heating_command(device_id, fn, value);
  }
  /// @brief Send a planned climate write and read the register back; the climate entity's one entry point.
  /// @see HeaterController::execute_heating_plan()
  bool execute_heating_plan(const std::string &device_id, const HeatingWritePlan &plan,
                            const std::function<void()> &on_accepted) override {
    return this->heater_controller_.execute_heating_plan(device_id, plan, on_accepted);
  }
  /// @brief Ask a heater with a programme what it runs. @see HeaterController::read_programme_state()
  bool read_programme_state(const std::string &device_id) override {
    return this->heater_controller_.read_programme_state(device_id);
  }
  /// @brief Set a water heater's target temperature. @see HeaterController::set_water_heater_target()
  bool set_water_heater_target(const std::string &device_id, float celsius) override {
    return this->heater_controller_.set_water_heater_target(device_id, celsius);
  }
  /// @brief The range a water heater's target is encoded over. @see HeaterController::water_heater_target_range()
  [[nodiscard]] HeatingCardRange water_heater_target_range(const std::string &device_id) const override {
    return this->heater_controller_.water_heater_target_range(device_id);
  }
  /// @brief The step a water heater's target is offered in. @see HeaterController::water_heater_target_step()
  [[nodiscard]] std::optional<float> water_heater_target_step(const std::string &device_id) const override {
    return this->heater_controller_.water_heater_target_step(device_id);
  }
  /// @brief Set a water heater's operating mode. @see HeaterController::set_water_heater_mode()
  bool set_water_heater_mode(const std::string &device_id, DhwMode mode) override {
    return this->heater_controller_.set_water_heater_mode(device_id, mode);
  }
  /// @brief Start or end a water heater's boost. @see HeaterController::set_water_heater_boost()
  bool set_water_heater_boost(const std::string &device_id, bool on) override {
    return this->heater_controller_.set_water_heater_boost(device_id, on);
  }
  /// @brief Start or end a water heater's away mode. @see HeaterController::set_water_heater_away()
  bool set_water_heater_away(const std::string &device_id, bool on) override {
    return this->heater_controller_.set_water_heater_away(device_id, on);
  }
  /// @brief Set the temperature encoding range of a heater (YAML `min_temperature:` / `max_temperature:`).
  /// @see HeaterController::set_device_temperature_range()
  void set_device_temperature_range(const std::string &device_id, float min_c, float max_c) override {
    this->heater_controller_.set_device_temperature_range(device_id, min_c, max_c);
  }
  /// @brief Have every poll of a heater read the registers of one more reading.
  /// @see HeaterController::enable_device_reading()
  void enable_device_reading(const std::string &device_id, DeviceReading reading) override {
    this->heater_controller_.enable_device_reading(device_id, reading);
  }
  /// @brief Read one register of a heater and keep the decoded value in the device's readings.
  /// @see HeaterController::read_heating_register()
  bool read_heating_register(const std::string &device_id, DeviceRegister fn, float *value_out,
                             uint8_t max_tries) override {
    return this->heater_controller_.read_heating_register(device_id, fn, value_out, max_tries);
  }
  /// @brief Queue an async position update; returns immediately, executed in loop().
  ///
  /// If a pending SET_TILT operation for the same device is already in the queue, the two are
  /// coalesced into a single SET_POSITION_AND_TILT command to avoid two radio exchanges.
  /// This transparently handles Home Assistant sending cover.set_cover_position and
  /// cover.set_cover_tilt_position as separate rapid calls.
  /// @param device_id Target device ID.
  /// @param position Desired position (0–100).
  void queue_set_device_position(const std::string &device_id, uint8_t position) override;
  /// @brief Queue an async named command (STOP, FAVORITE, VENT, PARTIAL, PEDESTRIAN, FORCE_OPEN);
  /// returns immediately, executed in loop(). PEDESTRIAN is experimental.
  ///
  /// Existing entity/button callers (cover, favorite, vent, partial and pedestrian buttons) intentionally ignore
  /// the return value — they always target a known, already-registered device. It exists so
  /// force_open_device() can report enqueue rejection distinctly from a queued-but-not-yet-run
  /// command.
  /// @param device_id Target device ID.
  /// @param cmd Named command to send.
  /// @return true if the hub is initialized, the device is registered, and the command matches
  ///         its capability class (so the command was enqueued); false otherwise.
  bool queue_device_command(const std::string &device_id, CoverCommand cmd) override;
  /// @brief Queue an async tilt update; returns immediately, executed in loop().
  ///
  /// If a pending SET_POSITION operation for the same device is already in the queue, the two are
  /// coalesced into a single SET_POSITION_AND_TILT command to avoid two radio exchanges.
  /// This transparently handles Home Assistant sending cover.set_cover_position and
  /// cover.set_cover_tilt_position as separate rapid calls.
  /// @param device_id Target device ID.
  /// @param tilt_percent Desired tilt (0–100).
  void queue_set_device_tilt(const std::string &device_id, uint8_t tilt_percent) override;
  /// Queue an async combined position+tilt update; returns immediately, executed in loop().
  /// @param device_id Target device ID.
  /// @param position Desired position (0–100).
  /// @param tilt_percent Desired tilt (0–100).
  void queue_set_device_position_and_tilt(const std::string &device_id, uint8_t position,
                                          uint8_t tilt_percent) override;
  /// Queue an async status request; returns immediately, executed in loop().
  /// @param device_id Target device ID.
  void queue_request_device_status(const std::string &device_id) override;
  /// Queue the scheduled read of one reading; executed in loop() as a background operation (yields to commands and
  /// 1W activity).
  /// @param key Key of the schedule entry (device and reading), as the DEVICE_READING policy holds it.
  void queue_request_device_reading(const std::string &key);
  /// Queue an async device-name request; returns immediately, executed in loop().
  /// @param device_id Target device ID.
  void queue_request_device_name(const std::string &device_id) override;
  /// Queue a pairing operation; executed in loop() when radio idle.
  ///
  /// A press handled within PAIRING_STALE_PRESS_MS of the end of an attempt is dropped with a log
  /// line: pairing blocks loop(), so a press made while it ran is only handled once it ends, when the
  /// device has usually left its pairing mode.
  void queue_discover_and_pair() override;
  /// @brief Entry point for the "Scan Paired Devices" button: run the roll-call and publish its
  /// report to the log and the Home Assistant result event, exactly as the native API action does.
  ///
  /// Deliberately not queued through OperationQueue, unlike queue_discover_and_pair(): the
  /// roll-call has no priority, coalescing or dedup semantics to preserve, and it already runs
  /// this way from the native API action. What it *is* guarded on is `busy_` — a button, unlike an
  /// API action, is also reachable from an ESPHome automation, which can fire from inside a
  /// blocking exchange (an entity callback -> on_value: -> button.press: chain) and would
  /// otherwise re-enter ExchangeEngine mid-exchange. See ADR 0013 for why one blocking radio
  /// operation at a time is the whole concurrency model. A press while `busy_` still fires the
  /// log/event pair (a failed result), matching every other rejected management action rather
  /// than going silent.
  ///
  /// Blocks the ESPHome loop for up to roughly 6 × `pairing_discovery_wait_ms` (both power-class
  /// passes, three channels each; fewer if `scan_power_classes` narrows the sweep) and will log the
  /// "operation took a long time" warning, same as the action — see docs/pairing.md.
  void trigger_scan_paired_devices() override;
  /// Async form of set_light_position() that keeps radio work serialized on the main loop.
  /// queue_set_light_state() is a thin binary-position wrapper around this.
  /// @param device_id Target device ID.
  /// @param position Desired IO position (0-100).
  void queue_set_light_position(const std::string &device_id, uint8_t position) override;
  /// Async form of set_fan_position() that keeps radio work serialized on the main loop.
  /// @param device_id Target device ID.
  /// @param position Desired IO position (0-100).
  void queue_set_fan_position(const std::string &device_id, uint8_t position) override;
  /// Async binary on/off form of the light command that keeps radio work serialized on the main loop.
  /// @param device_id Target device ID.
  /// @param on Desired on/off state.
  void queue_set_light_state(const std::string &device_id, bool on) override;
  /// Async form of set_switch_state() that keeps radio work serialized on the main loop.
  /// @param device_id Target device ID.
  /// @param on Desired on/off state.
  void queue_set_switch_state(const std::string &device_id, bool on) override;
  /// Async form of set_lock_state() that keeps radio work serialized on the main loop.
  /// @param device_id Target device ID.
  /// @param locked Desired locked/unlocked state.
  void queue_set_lock_state(const std::string &device_id, bool locked) override;

#ifdef IOHOME_LR1121_FIRMWARE_UPDATE
  /// @brief Entry point for the "Flash LR1121 Radio Firmware" button. Thin forwarder to the
  /// Lr1121FirmwareUpdateController collaborator (lr1121_firmware_update_controller.h).
  ///
  /// Only exists when a `lr1121_firmware_update:` block is configured. See
  /// lr1121_firmware_update_controller.cpp for the full contract, including the safety invariant
  /// that every bootloader excursion this triggers must end in either radio_->init() or
  /// App.safe_reboot() — there is no third option.
  void trigger_lr1121_firmware_update() override { this->lr1121_firmware_update_.trigger(); }

#ifdef IOHOME_LR1121_BOOTLOADER_UPDATE
  /// @brief Set by the "Allow LR1121 Bootloader Rewrite (Irreversible)" switch's write_state().
  /// Thin forwarder to the Lr1121FirmwareUpdateController collaborator.
  ///
  /// A permission, not an override: this can only convert a cached
  /// BootloaderUpgradePath::AVAILABLE verdict into "run the three-stage sequence" (bootloader
  /// ADR 0021) -- it never affects REJECT_WRONG_CHIP, the post-entry sanity
  /// check, the busy_ guard, or any other verdict. Read once, at button-press time
  /// (trigger_lr1121_firmware_update()); the ESPHome loop is blocked for the whole three-stage
  /// sequence once it starts, so the switch cannot change mid-flash. Deliberately not named
  /// anything with "armed" -- lr1121_flash_confirmation_armed_ already means the two-press window
  /// this switch *replaces* for its own path, and a reader must never have to guess which is meant.
  void set_bootloader_rewrite_allowed(bool allowed) override {
    this->lr1121_firmware_update_.set_bootloader_rewrite_allowed(allowed);
  }
#endif
#endif

 protected:
  // --- Protocol-level operations ---
  /// Transmit a raw IoFrame on the current frequency with given preamble length.
  /// @param frame IoFrame to transmit.
  /// @param freq RF frequency in Hz.
  /// @param preamble Preamble length in bytes (e.g. `LONG_PREAMBLE`, `SHORT_PREAMBLE`, or a
  ///        tuning-configured duration such as `start_preamble_ms`, converted to bytes).
  bool transmit_frame_(const IoFrame &frame, uint32_t freq, uint16_t preamble);
  /// Main request/response exchange with retry and automatic authentication. Every exchange ends
  /// with StatusPollPolicy::on_exchange_ended(), so it moves an open catch-up burst's read out
  /// (ADR 0080).
  /// @param request Outbound request IoFrame.
  /// @param response Output: received response IoFrame.
  /// @param freq RF frequency in Hz.
  /// @param attempts A transmit-attempt cap (forwarded to ExchangeEngine::send_and_receive()), or a
  ///        read kind whose plan the engine builds for @p request (the engine builds the plan itself, once).
  /// @return true if exchange succeeded; false otherwise.
  ExchangeOutcome send_and_receive_(const IoFrame &request, IoFrame &response, uint32_t freq,
                                    const decisions::AttemptRequest &attempts = EXCHANGE_RETRY_COUNT);
  /// Handle an inbound authenticated command from a device (status updates, etc.).
  /// @param request Inbound authenticated request (e.g., CMD_STATUS_UPDATE).
  /// @param freq RF frequency the packet arrived on.
  /// @return true if authentication succeeded; false otherwise.
  bool authenticate_request_(const IoFrame &request, uint32_t freq);
  /// Parse a received frame, merge supported device state or metadata, and notify callbacks.
  /// @param packet Raw radio packet containing a parsed IoFrame.
  void process_received_packet_(const RadioRxPacket &packet);

  /// The 1W branch of process_received_packet_(): note the frame in the OneWayReceiver, decode it, let key
  /// adoption observe it, drop a repeat of the previous press, then log it, fire the sender event and
  /// follow a linked remote's press. That order is load-bearing: activity and pairing gestures count
  /// repeats, and key adoption sees every frame before the duplicate filter.
  /// @param frame Parsed frame with the CTRL0 1W bit set.
  /// @param now millis() when the frame was heard: now for an idle frame, the wait's time for one an
  ///        exchange kept, so the duplicate filter and burst window see the real arrival.
  /// @param rssi_dbm RSSI of the capture, in dBm.
  void handle_oneway_frame_(const IoFrame &frame, uint32_t now, int16_t rssi_dbm);

  /// Run the 1W frames heard inside an exchange through handle_oneway_frame_(), oldest first, at
  /// their arrival times. Called at the top of loop(), so it precedes the idle receive, the
  /// burst-window check and the next queued operation.
  void drain_overheard_oneway_frames_();

  /// Handle an authenticated CMD_STATUS_UPDATE from a registered device: a device event report
  /// (decisions::is_device_event_report()) is recorded, any other 0x71 is applied as a position update.
  /// @param id The device's node ID string.
  /// @param dev The device record.
  /// @param frame The status update.
  void apply_status_update_(const std::string &id, IoDevice &dev, const IoFrame &frame);

  /// Record @p frame as a device event when decisions::should_record_device_event() says it
  /// qualifies. The receive path's single decision point; the exchange paths record a device event
  /// report they took off the air through record_device_event_() directly.
  /// @param frame The received frame.
  /// @param reached_unhandled_fallback True from the fallback after every other handler declined.
  /// @return true when the frame was recorded.
  bool record_if_device_event_(const IoFrame &frame, bool reached_unhandled_fallback);

  /// Record a device event report, or a frame from a registered device that nothing else here
  /// handles, in the DeviceEventLog; INFO with `diagnostic_probes: true`, DEBUG otherwise. The
  /// recording never answers the frame and never applies it to the device record, whatever it claims
  /// to carry (ADR 0022, ADR 0060). The one acknowledgement is the ordinary one: a CMD_STATUS_UPDATE
  /// addressed to this hub is authenticated and acknowledged with CMD_STATUS_UPDATE_RESP before it
  /// gets here, whatever event it carries.
  /// @param frame The received frame; its source is a registered device.
  void record_device_event_(const IoFrame &frame) {
    this->device_event_log_.record(frame, this->diagnostic_probes_enabled_);
  }

  /// True while the key-extraction responder is mid-attempt and still within its bounded listen
  /// window. Thin forwarder to KeyExtractionResponder::awaiting_reply() (key_extraction_responder.h)
  /// — kept on the hub because defer_background_poll_() and tests/hub/hub_core_test.cpp reach it here,
  /// mirroring the two set_key_extraction_armed* bindings.
  [[nodiscard]] bool key_extraction_awaiting_reply_() const { return this->key_extraction_.awaiting_reply(); }

  /// One idle pass of listening for the key-extraction responder's peer, in place of the generic idle
  /// hop while key_extraction_awaiting_reply_(). Inside the session window
  /// (KeyExtractionResponder::in_session_hold()) it keeps the radio on the session's channel, where
  /// the peer's in-session frame comes; after it, a rotating listen over all three channels for one
  /// KEY_EXTRACTION_SCAN_SLICE_MS slice, handing a received frame to process_received_packet_().
  /// While a discovery answer waits for its slot, the slice ends at that slot (and is skipped once
  /// it has come), so the scheduler sends the answer on time.
  void listen_for_key_extraction_peer_();

  /// Extract supported position or metadata info from a response frame and merge it into the device record.
  /// @param frame IoFrame containing a supported inbound command such as CMD_PRIVATE_RESP,
  /// CMD_STATUS_UPDATE, CMD_GET_NAME_RESP, or CMD_GET_INFO2_RESP.
  /// @param trust_position False to apply `is_stopped` but skip target/position decode for a
  /// CMD_PRIVATE_RESP — the immediate reply to our own just-sent CMD_EXECUTE echoes stale
  /// pre-command target/current values on at least some devices (see
  /// tests/corpus/captures/exchange/somfy_awning_exchange_ack_reports_stale_target_*.yaml), so
  /// execute_request_and_update_() passes false there; every other caller trusts as before.
  void update_device_status_(const IoFrame &frame, bool trust_position = true);
  /// The device just reported its status: move its status refresh (if it has one) a whole interval out.
  /// @param device_id Device that reported.
  void note_status_reported_(const std::string &device_id);
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
  /// Settle every device whose polling window ran out (StatusPollPolicy::pop_ended_device()).
  /// A device still settling — its last report said "stopped" off its target and the confirming
  /// poll never came back — is taken as at rest there: with no poll left, the device's own flag
  /// stands. Any other device is left as it is.
  void settle_ended_status_tracking_();

  /// Hold ESPHome's continuous-loop request while the radio is set up and has not failed, release it
  /// otherwise (decisions::wants_continuous_loop()). Idempotent; called every loop pass, the one place
  /// the request is decided.
  void update_idle_loop_request_();
  /// Arm the confirming poll that follows a command, because a CMD_EXECUTE reply is never trusted
  /// for position (see update_device_status_()'s trust_position parameter) and therefore leaves the
  /// hub with no idea where the device actually is. Re-arms the bounded tracking window rather than
  /// only setting a due time: the same untrusted reply clears that window whenever it claims the
  /// device is stopped, and pop_due_device() discards a due poll that has no active window. An
  /// already-scheduled earlier poll wins.
  /// @param device_id Device the command was sent to.
  /// @param for_stop True for STOP (and position POS_STOP), which settles under
  ///        STOP_SETTLE_POLL_CAP_MS instead of the normal settle cadence.
  void arm_execute_confirmation_poll_(const std::string &device_id, bool for_stop);
  /// Schedule status polls for a fixed list of devices (shared by the id-linked and
  /// class-linked 1W paths).
  /// @param device_ids Devices to poll.
  /// @param delay_ms Poll delay in milliseconds.
  void schedule_device_polls_(const std::vector<std::string> &device_ids, uint32_t delay_ms);
  /// Whether background radio work (a queued status poll or other background operation, a queued transfer read)
  /// may run now: the radio is not in an exchange, the hub is not in radio test mode, no key-extraction attempt
  /// is waiting for its peer's next frame and no linked 1W remote is transmitting. A background exchange is
  /// blocking and owns the radio for 1-3 s; dispatched while a remote is still transmitting it would blind the
  /// hub to the rest of the press, and while the key-extraction responder is listening for its peer's next frame
  /// (an expected CMD_KEY_TRANSFER (0x32), say) it would swallow it — see listen_for_key_extraction_peer_() for
  /// the other half of that listen. The one list of these conditions: loop() gates the operation queue on it
  /// (defer_background_poll_()) and the transfer reader's steps wait on it.
  [[nodiscard]] bool background_radio_allowed_() const {
    return !this->busy_ && !this->radio_test_mode_ && !this->key_extraction_awaiting_reply_() &&
           !this->oneway_receiver_.defers_background_poll(true, millis());
  }
  /// Whether loop() should skip dispatching the queue this iteration: the pending work is a background
  /// operation and background_radio_allowed_() says no. Only background operations yield, a user command must
  /// never wait on any kind of background activity.
  [[nodiscard]] bool defer_background_poll_() const {
    const bool next_op_is_background =
        !this->op_queue_.empty() && OperationQueue::is_background_op(this->op_queue_.front().type);
    return next_op_is_background && !this->background_radio_allowed_();
  }
  /// Decide, and act on, what a frame of another controller means for one registered device: a
  /// heard write, or a device challenge that follows no heard read, arms a tracked poll; any other
  /// frame arms one only if the device was not read recently (StatusPollPolicy::on_foreign_frame());
  /// while such a poll is pending the frame only pushes its timer out. A frame that arms nothing
  /// changes neither the tracking window nor the failure streaks.
  /// @param device_id Registered device the frame concerns.
  /// @param kind Request to the device, or the device's challenge to another controller.
  /// @param controller The other controller's node ID (request source, or challenge destination).
  /// @param cmd Command of the heard frame.
  void act_on_foreign_frame_(const std::string &device_id, ForeignFrameKind kind, const std::string &controller,
                             uint8_t cmd);
  /// Hand a registered device's challenge (0x3C) to another controller to act_on_foreign_frame_().
  /// A challenge addressed to this hub, from an unregistered node, or a 0x3D is left alone.
  /// @param frame An exchange-internal frame (is_exchange_internal_command()).
  void note_foreign_challenge_(const IoFrame &frame);
  /// Hand another controller's frame to act_on_foreign_frame_(): per the registered device it is
  /// addressed to, or per device linked to its source when that is a linked 2W controller.
  /// @param frame A parsed 2W frame that is not a status reply.
  /// @return true when the frame was another controller's request and has been handled.
  bool act_on_overheard_request_(const IoFrame &frame);
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
  /// React to a deduplicated 1W press from a remote: apply the optimistic state to the devices it
  /// controls (resolve_1w_target_devices_()), schedule their status polls (at once after a STOP,
  /// otherwise after `remote_activity_poll_delay_ms`) and, when switched on, open the catch-up burst
  /// (StatusPollPolicy::on_linked_remote_activity(), ADR 0080). No-op for a remote that controls none.
  /// @param info Already-decoded 1W frame info (see decode_1w_frame()).
  /// @param src_id Sender's node ID as a string (already computed by the caller).
  void follow_linked_remote_press_(const OneWayFrameInfo &info, const std::string &src_id);
  /// Fire the sender HA event for a decoded 1W frame, if the sender is exposed.
  /// DEBUG-logs the reason when it does not fire (API disconnected / sender not exposed) so a
  /// live log capture can distinguish "never reached this check" from "reached it and skipped".
  /// @param info Already-decoded 1W frame info (see decode_1w_frame()).
  /// @param linked True if the sender is linked to at least one registered device.
  /// @param src_id Sender's node ID as a string (already computed by the caller).
  void maybe_fire_sender_event_(const OneWayFrameInfo &info, bool linked, const std::string &src_id);
  /// Handle an explicit CMD_ERROR_RESP refusal from the device: record the result code, stamp link
  /// health, and schedule the poll backoff. Kept apart from exchange_and_record_() so that
  /// function's outcome dispatch stays readable — a refusal is a distinct concern from "what did the
  /// exchange achieve".
  /// @param device_id Target device ID.
  /// @param request Outbound request frame that drew the refusal.
  /// @param response The CMD_ERROR_RESP frame.
  /// @param retry_after_fail_ms If non-zero, schedules next status poll after this delay.
  /// @return Always false; a refusal is never a success.
  bool handle_error_response_(const std::string &device_id, const IoFrame &request, const IoFrame &response,
                              uint32_t retry_after_fail_ms);

  /// The schedule of one periodic poll.
  /// @param poll Which poll.
  /// @return The PeriodicPollPolicy instance that schedules it.
  PeriodicPollPolicy &periodic_poll_policy_(PeriodicPoll poll);

  /// Opt a registered device into one periodic poll; the public set_device_*_interval() setters
  /// forward here. Does nothing for an unknown device.
  /// @param poll Which poll.
  /// @param device_id Target device ID.
  /// @param interval_ms Poll interval in milliseconds; zero removes the device from the schedule.
  void set_periodic_poll_interval_(PeriodicPoll poll, const std::string &device_id, uint32_t interval_ms);

  /// The exchange of an event or limitation reading: send the source kind's request, decode the answer, fold the
  /// result into the device record. The device is registered, stopped and has the reading.
  /// @param device_id Target device ID.
  /// @param row The reading's row, whose source is an EventSource or a LimitationSource.
  /// @return true if an answer of the captured shape arrived and was applied.
  bool request_frame_reading_(const std::string &device_id, const DeviceReadingRow &row);

  /// Fold one scheduled read's outcome into the device record: a value replaces the stored one and clears the miss
  /// count; a miss keeps the last value until PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN, after which a reading with
  /// ReadingStale::DROP loses it. Logs a change at INFO and tells subscribers in every case.
  /// @param device_id Target device ID.
  /// @param reading Which reading.
  /// @param value The decoded value of an answered read, empty for a miss.
  /// @param detail_text Extra text for the state-change log line, empty for a miss.
  /// @param unanswered True when the device sent nothing back at all, so the miss is retried once
  ///        after PERIODIC_POLL_MISS_RETRY_MS before it counts.
  void apply_periodic_reading_(const std::string &device_id, DeviceReading reading, const std::optional<float> &value,
                               const std::string &detail_text, bool unanswered);

  /// Fold the end of a data-object read into the device record: a value is stored through
  /// apply_periodic_reading_(); a failed read, an empty open or an open the device declines with a status other than
  /// "no such object" (busy, another transfer open) counts as a miss and keeps the last value; an error reply to the
  /// open, the status "no such object" (3) or a read the hub itself refuses marks the reading absent until the next
  /// boot (one warning); a read another controller cut off is asked again soon without
  /// counting (ADR 0102).
  /// @param device_id Target device ID.
  /// @param reading The reading that was read.
  /// @param outcome How the transfer read ended.
  /// @param data The bytes the read returned.
  void apply_device_reading_answer_(const std::string &device_id, DeviceReading reading, TransferOutcome outcome,
                                    const std::vector<uint8_t> &data);

  /// The key under which the DEVICE_READING schedule holds a device's reading.
  [[nodiscard]] static std::string device_reading_key(const std::string &device_id, DeviceReading reading);

  /// Log that @p device_id reported nothing for a refresh interval and queue its status read.
  void queue_status_refresh_(const std::string &device_id);

  /// Queue every periodic poll that is due: walks one list of PeriodicPoll rows, each with what to
  /// queue for its due device or reading.
  void queue_due_periodic_polls_();
  /// Read every device with `linked_remotes` once, when a burst of linked-remote activity has been
  /// followed by `remote_activity_catchup_poll_ms` without an exchange (ADR 0080). Each read is a
  /// one-off status read marked state-unknown (StatusPollPolicy::start_catchup_read()); it opens no
  /// follow-up window of its own, an answer that says moving does. A read that gets no answer is read
  /// once more one catch-up delay later (StatusPollPolicy::take_due_catchup_retries(), ADR 0095); a
  /// second silence ends it.
  void queue_due_catchup_reads_();

  /// @brief What exchange_and_record_() found.
  enum class RequestResult : uint8_t {
    FAILED,               ///< No usable reply: silence.
    ACCEPTED_NO_PAYLOAD,  ///< The device accepted a request that needs a payload (not a CMD_EXECUTE) but sent none;
                          ///< bookkept as a failure exactly like FAILED, kept apart only so a caller can say so.
    UNCONFIRMED,          ///< A CMD_EXECUTE the device accepted but never closed; counted as success, no reply frame.
    ERROR_REPLY,          ///< The device refused with CMD_ERROR_RESP; handle_error_response_() has recorded it.
    REPLY,                ///< The device answered; the caller interprets the reply frame.
  };

  /// @brief Per-request knobs of exchange_and_record_().
  struct RequestOptions {
    bool warn_on_no_response;            ///< Log a warning when no response arrives.
    uint32_t retry_after_fail_ms;        ///< If non-zero, drives the status-poll backoff and failure streaks.
    decisions::AttemptRequest attempts;  ///< Attempt cap or read kind forwarded to send_and_receive_().
  };

  /// send_and_receive_() on the command channel, except that a device event report
  /// (decisions::is_device_event_report()) taken as the reply is recorded (record_device_event_())
  /// and the request asked once more; a second report in a row, or one answering an EXECUTE that is
  /// not safe to repeat, ends it as FAILED.
  /// @param request Outbound request frame.
  /// @param response Filled with the reply on SUCCESS_WITH_RESPONSE; never a device event report then.
  /// @param attempts Attempt budget per send, as for send_and_receive_().
  /// @return The exchange outcome.
  ExchangeOutcome exchange_past_device_event_(const IoFrame &request, IoFrame &response,
                                              const decisions::AttemptRequest &attempts);

  /// Run one authenticated request/response exchange on the standard command channel and record
  /// its outcome: exchange-outcome and timeout counters, link health, challenge-seen handling,
  /// CMD_ERROR_RESP handling, debug logs and (when requested) the status-poll backoff. It does not
  /// interpret a reply — callers decide what a REPLY means.
  /// @param device_id Target device ID.
  /// @param request Outbound request frame.
  /// @param[out] response The device's reply, valid for RequestResult::REPLY and ERROR_REPLY.
  /// @param options See RequestOptions.
  /// @return What happened, see RequestResult.
  RequestResult exchange_and_record_(const std::string &device_id, const IoFrame &request, IoFrame &response,
                                     const RequestOptions &options);

  /// Shared request/response helper for high-level operations: exchange_and_record_() plus
  /// update_device_status_() on a reply.
  /// @param device_id Target device ID.
  /// @param request Outbound request frame.
  /// @param warn_on_no_response If true, logs a warning when no response is received.
  /// @param retry_after_fail_ms If non-zero, schedules next status poll after this delay on failure.
  /// @param attempts Attempt cap or read kind, forwarded to send_and_receive_(). Defaults to the full
  ///        EXCHANGE_RETRY_COUNT; a cover's status poll passes its read kind
  ///        (decisions::scheduled_poll_read_kind()).
  /// @return true when the device replied, or when a CMD_EXECUTE was accepted without a reply —
  ///         every other command's unconfirmed acceptance is still a failure here (see
  ///         @ref ExchangeOutcome and decisions::retry_after_unconfirmed_accept_is_safe()).
  bool execute_request_and_update_(const std::string &device_id, const IoFrame &request, bool warn_on_no_response,
                                   uint32_t retry_after_fail_ms = 0,
                                   const decisions::AttemptRequest &attempts = EXCHANGE_RETRY_COUNT);

  /// @brief Everything one execute-family operation needs beyond its own guard and frame builder.
  struct ExecuteRequestSpec {
    const char *action;   ///< Verb/phrase for the "Sending ..." and rejection logs.
    bool settle_as_stop;  ///< Passed through to arm_execute_confirmation_poll_().
  };
  /// Funnel for the four execute-family operations: runs try_execute_operation_() and, on any
  /// false return (the command will not reach the device), withdraws the optimistic prediction the
  /// entity applied at control() time via DeviceRegistry::rollback_optimistic(). Wrapping rather
  /// than inlining the rollback keeps every current and future failure exit covered by one call.
  /// @param device_id Target device ID.
  /// @param spec Pre-formatted action phrase and the settle-as-stop flag.
  /// @param accepts Capability guard; returns false to reject the operation for this device.
  /// @param rejection_profile Expected-profile label for the rejection log.
  /// @param build Fills the request frame from the resolved device; returns false on failure.
  /// @return true when the exchange succeeded and the settle poll was armed.
  bool run_execute_operation_(const std::string &device_id, const ExecuteRequestSpec &spec,
                              const std::function<bool(const IoDevice &)> &accepts, const char *rejection_profile,
                              const std::function<bool(IoFrame &, const IoDevice &)> &build);

  /// Shared skeleton for the four execute-family operations (position, named command, tilt,
  /// position+tilt): device lookup + initialized guard, capability guard, poll-tracking start,
  /// "Sending ..." log, frame build, exchange, failure backoff, and the settle poll. The four
  /// public methods supply only their guard predicate, rejection profile label, and frame
  /// builder. Called only through run_execute_operation_(), which owns the failure rollback.
  /// @param device_id Target device ID.
  /// @param spec Pre-formatted action phrase and the settle-as-stop flag.
  /// @param accepts Capability guard; returns false to reject the operation for this device.
  /// @param rejection_profile Expected-profile label for the rejection log.
  /// @param build Fills the request frame from the resolved device; returns false on failure.
  /// @return true when the exchange succeeded and the settle poll was armed.
  bool try_execute_operation_(const std::string &device_id, const ExecuteRequestSpec &spec,
                              const std::function<bool(const IoDevice &)> &accepts, const char *rejection_profile,
                              const std::function<bool(IoFrame &, const IoDevice &)> &build);

  /// Attempts for a cover's status read: its read kind (a settling read or a look at a busy device,
  /// decisions::scheduled_poll_read_kind()), consuming the poll's STOP and confirming-read marks.
  /// @param device_id Device being polled.
  /// @param scheduler_managed True when StatusPollPolicy is tracking the device (a ladder stands behind the poll).
  /// @param state_unknown True for a catch-up read (StatusPollPolicy::take_state_unknown()).
  [[nodiscard]] decisions::AttemptRequest cover_poll_attempts_(const std::string &device_id, bool scheduler_managed,
                                                               bool state_unknown = false);

  /// Execute a named device command (STOP, FAVORITE, VENT, PARTIAL, PEDESTRIAN, FORCE_OPEN) via the
  /// authenticated exchange.
  /// A named position (NAMED_POSITIONS) goes out only to a device whose features include its bit.
  /// @param device_id Target device ID.
  /// @param cmd Named command to execute.
  /// @return true if device acknowledged; false otherwise, including a named position the device's
  ///         features lack (nothing is sent).
  bool execute_device_command_(const std::string &device_id, CoverCommand cmd);
  /// Shared bookkeeping for every 1W transmit: mark the radio busy for the duration of `send`,
  /// then record it as 1W activity so background polls back off for it exactly as they do for a
  /// remote's burst — the radio is equally busy either way. Every 1W execute must go through this;
  /// a future one that skips it would compile, pass, and silently break poll-deferral.
  /// @param send Callable that performs the actual transmit; takes no arguments.
  template<typename F> void execute_oneway_(F &&send) {
    this->busy_ = true;
    send();
    this->busy_ = false;
    this->oneway_receiver_.note_activity(millis());
  }
  /// Send a queued 1W named command. Unlike its 2W sibling this returns nothing: there is no
  /// acknowledgement to report, and success here would only mean "bytes left the radio".
  /// @param controller_id Controller-identity handle.
  /// @param cmd Named command to send.
  void execute_oneway_command_(const std::string &controller_id, CoverCommand cmd);
  /// Send a queued 1W numeric position. See execute_oneway_command_().
  /// @param controller_id Controller-identity handle.
  /// @param position Target position 0–100.
  void execute_oneway_position_(const std::string &controller_id, uint8_t position);
  /// Send a queued 1W enrollment (add-controller). See execute_oneway_command_().
  /// @param controller_id Controller-identity handle.
  void execute_oneway_enroll_(const std::string &controller_id);
  /// Send a queued 1W un-enrollment (remove-controller). See execute_oneway_command_().
  /// @param controller_id Controller-identity handle.
  void execute_oneway_unenroll_(const std::string &controller_id);
  /// Fire all registered device update callbacks for the given device ID.
  /// @param id Device ID that updated.
  void notify_device_update_(const std::string &id);
  /// @brief Build the calls the heater controller reaches the hub through (ADR 0083).
  ///
  /// Lives on the hub because the calls need protected access (`set_timeout()`) and the hub's private
  /// exchange and execute helpers; called from the initializer list, so every lambda only captures `this`.
  /// @return The injected calls.
  HeaterControllerHub make_heater_controller_hub_();
  /// @brief Build the calls the transfer reader reaches the hub through (ADR 0099).
  ///
  /// Lives on the hub for the same reasons as make_heater_controller_hub_(): the calls need protected access
  /// (`set_timeout()`) and the hub's private predicate and exchange helpers.
  /// @return The injected calls.
  TransferReaderHub make_transfer_reader_hub_();
  /// Apply backoff after a failed background status poll and log the result.
  /// @param device_id Target device ID.
  /// @param auth_like True when the failed exchange saw a 0x3C challenge.
  /// @param device_moving True when the failed request was a single-try status poll of a device
  ///        the hub believes is travelling; its first misses keep the normal cadence
  ///        (StatusPollPolicy::on_exchange_failed()).
  void schedule_background_poll_backoff_(const std::string &device_id, bool auth_like, bool device_moving = false);
  /// Pop next pending operation from the queue and execute it (set position, request status, discover).
  void process_pending_operation_();

  // --- Exchange helpers (thin wrappers delegating to ExchangeEngine) ---

  /// Log the last exchange debug snapshot (delegates to exchange_engine_).
  void log_exchange_debug_(const char *device_id) const { this->exchange_engine_.log_debug(device_id); }

  /// Log the last exchange debug snapshot for an accepted-but-unconfirmed exchange, at INFO.
  void log_exchange_unconfirmed_debug_(const char *device_id) const {
    this->exchange_engine_.log_debug_unconfirmed(device_id);
  }

  // --- Tuning ---
  /// Apply the current tuning configuration to the active radio driver.
  void apply_tuning_to_radio_();
  /// @brief Convert the deprecated byte-denominated preamble keys to durations on this radio (ADR 0097).
  void convert_legacy_preamble_bytes_();

  // --- Management actions (the native API calls management_actions_ directly) ---
  /// Register hub-level Home Assistant actions; called from setup().
  void register_management_actions_() { this->management_actions_.register_actions(); }

  // --- Frequency hopping ---
  void hop_frequency_();

  // --- Radio driver selection (called once from setup()) ---
  /// Select and construct the radio driver named by the required `radio_type` config field.
  ///
  /// Kept as its own method rather than inlined into setup(): the three-way chip branch
  /// (SX1276/SX1262/LR1121) is enough logic on its own that folding it into setup() pushes
  /// that function's cognitive complexity past clang-tidy's threshold.
  /// Validates the pins each driver needs and logs a clear error (without calling
  /// mark_failed() itself — the caller decides how to react) when a required pin is
  /// missing. On success, `*chip_name_out` is set to a static string naming the selected
  /// chip (used for logging), and the returned pointer is the heap-allocated (not yet
  /// initialized) driver instance.
  /// @param chip_name_out Output: human-readable chip name for logging (always set,
  ///                      even on failure, to the best-known name for error messages).
  /// @return Newly allocated RadioDriver, or nullptr if pin validation or allocation failed.
  RadioDriver *select_and_construct_radio_(const char **chip_name_out);

  /// @brief Emit the 1W controller identities to the config dump — node, class, and the resolved
  /// ACEI / broadcast (ADR 0031). Factored out of dump_config() to keep its cognitive complexity
  /// under the clang-tidy threshold.
  void dump_oneway_controllers_config_() const;

#ifdef IOHOME_LR1121_FIRMWARE_UPDATE
  // --- LR1121 firmware update: thin forwarders to the Lr1121FirmwareUpdateController collaborator
  //     (lr1121_firmware_update_controller.h). setup()/dump_config() keep calling these names; the
  //     orchestration and every safety invariant live in the collaborator's .cpp. ---
  /// Boot-time bootloader-version excursion — forwards. Called from setup() after
  /// select_and_construct_radio_() and before radio_->init().
  void run_lr1121_boot_time_bootloader_read_() { this->lr1121_firmware_update_.run_boot_time_bootloader_read(); }
  /// Compute and cache the flash verdict once radio_->init() has produced (or failed to produce)
  /// an installed-firmware-version read — forwards. Called from setup() regardless of whether
  /// init() succeeded.
  void cache_lr1121_flash_verdict_() { this->lr1121_firmware_update_.cache_flash_verdict(); }
  /// Emit the bootloader version and cached flash verdict to the config dump — forwards. Called
  /// from dump_config(), next to the existing radio_->dump_debug() call.
  void dump_lr1121_firmware_update_debug_() const { this->lr1121_firmware_update_.dump_debug(); }
#endif

  // --- Radio driver ---
  RadioDriver *radio_{nullptr};

  // --- Hardware pins (set by YAML codegen, passed to radio driver in setup) ---
  InternalGPIOPin *rst_pin_{nullptr};
  InternalGPIOPin *dio0_pin_{nullptr};        ///< SX1276 DIO0 interrupt
  InternalGPIOPin *dio4_pin_{nullptr};        ///< SX1276 DIO4 preamble detect (optional)
  InternalGPIOPin *dio1_pin_{nullptr};        ///< SX1262 DIO1 interrupt; also carries the LR1121's DIO9 IRQ line
  InternalGPIOPin *busy_pin_{nullptr};        ///< SX1262/LR1121 BUSY pin
  InternalGPIOPin *fem_en_pin_{nullptr};      ///< Front-end module enable
  InternalGPIOPin *vfem_pin_{nullptr};        ///< Front-end module power
  InternalGPIOPin *fem_pa_pin_{nullptr};      ///< Front-end module PA switch
  FemProfile fem_profile_{FemProfile::NONE};  ///< Which FEM part fem_pa_pin_ belongs to, if any.

  // --- Configuration (from YAML) ---
  std::string node_id_str_;
  std::string system_key_str_;
  std::string radio_type_;  ///< "sx1276", "sx1262", or "lr1121"; required by the YAML schema.
  /// Why setup() gave up on the radio, or empty. Printed by dump_config so log clients that connect
  /// after boot see the cause, not only ESPHome's generic "marked FAILED" line.
  std::string radio_failure_reason_;
  /// Set once loop() has reported a radio that latched a failure after setup, so the report (an
  /// error line, ESPHome's warning status and a republish of every device) is made once, not on
  /// every loop.
  bool radio_failure_reported_{false};
  /// ESPHome's request for a loop that runs without waiting for its interval, held while the radio is
  /// idle-listening so the idle channel hop runs on its own timer (ADR 0086). Decided by
  /// update_idle_loop_request_().
  HighFrequencyLoopRequester idle_loop_;
  uint8_t node_id_[NODE_ID_SIZE]{};
  uint8_t system_key_[AES_KEY_SIZE]{};
  uint8_t tx_power_{DEFAULT_TX_POWER_DBM};
  uint8_t pa_pin_{DEFAULT_PA_PIN_PA_BOOST};
  uint8_t tcxo_voltage_{DEFAULT_TCXO_VOLTAGE_SETTING_1P8V};  ///< SX1262/LR1121 TCXO voltage setting (default 1.8 V)

  // --- Runtime state ---
  bool initialized_{false};
  bool busy_{false};
  /// millis() when the last pairing attempt ended; 0 before the first one (see queue_discover_and_pair()).
  uint32_t last_pairing_end_ms_{0};
  bool radio_test_mode_{false};  ///< When true, loop() is suspended for loopback testing.
  TuningConfig tuning_{};        ///< Runtime tuning overrides.
  DeviceRegistry registry_;
  /// Invoked once after every pairing attempt completes; see set_pairing_result_callback().
  std::function<void()> pairing_result_callback_;
  /// Device events recorded since boot, the last one's text and its subscriber.
  DeviceEventLog device_event_log_;
  /// Whether diagnostic probes (DiagnosticProbes::probe_device()/probe_sweep()) are enabled.
  /// False by default so a build that didn't opt in via `diagnostic_probes: true` never sends an
  /// undecoded probe opcode. See set_diagnostic_probes_enabled().
  bool diagnostic_probes_enabled_{false};
  StatusPollPolicy poll_policy_;
  PeriodicPollPolicy status_refresh_policy_;       ///< Schedule of the opt-in status refresh at rest.
  PeriodicPollPolicy device_reading_poll_policy_;  ///< Schedule of the scheduled readings, by device_reading_key().
  /// One scheduled reading: the key it has in device_reading_poll_policy_ and what it names.
  struct DeviceReadingPoll {
    std::string key;
    std::string device_id;
    DeviceReading reading;
    bool in_flight{false};  ///< A data-object read of it is queued or running in the transfer reader (ADR 0108).
  };
  std::vector<DeviceReadingPoll> device_reading_polls_;  ///< Every reading with a schedule entry.
  OperationQueue op_queue_;
  /// Per-attempt pairing telemetry. PairingEngine records into it and, during an attempt, attaches it
  /// to ExchangeEngine as its TransmitObserver.
  PairingTelemetry pairing_telemetry_;
  /// Receive-side state of overheard 1W frames: burst window, duplicate filter, pairing-gesture sighting
  /// and sender allowlist. Declared before pairing_engine_, which holds a reference to its sighting, so
  /// member-init order matches the initializer list.
  OneWayReceiver oneway_receiver_;
  ExchangeEngine exchange_engine_;        ///< Owns all authenticated exchange and LBT/hop logic.
  PairingEngine pairing_engine_;          ///< Owns the three-phase device pairing flow.
  ManagementActions management_actions_;  ///< Owns rename, identify, force-open, scan_paired_devices, and other
                                          ///< hub-level HA actions.
  /// Owns the heater and water heater writes and register reads (ADR 0083). Declared after registry_, radio_,
  /// initialized_, node_id_ and exchange_engine_, which it references, so member-init order matches the
  /// initializer list.
  HeaterController heater_controller_;
  /// Owns the queued, non-blocking reads of device data objects (ADR 0099). Declared after node_id_ and
  /// heater_controller_ so member-init order matches the initializer list.
  TransferReader transfer_reader_;
  /// Owns the 1W controller identities, their rolling-sequence counters and the transmit burst.
  /// The third collaborator that drives the radio (ADR 0004), and the only one that awaits
  /// nothing — 1W has no reply to wait for.
  OneWayTransmitter oneway_transmitter_;
  /// Opt-in, receive-only 1W controller-key adoption listener (oneway_key_adoption.cpp). Armed via
  /// the "Recover 1W Controller Key" switch; observes an overheard CMD_ONEWAY_ADD_CONTROLLER and
  /// reports the key once, then disarms. Declared after oneway_transmitter_ so member-init order
  /// matches the initializer list.
  OnewayKeyAdoption oneway_key_adoption_;
  /// Device-role responder for the "Recover System Key" feature (key_extraction_responder.cpp).
  /// Owns the pairing_responder::ResponderContext, the throwaway-ID/auto-off/grace-window
  /// machinery, and the device-role reply TX. Declared after registry_/radio_/tuning_/node_id_
  /// (which it references) and after oneway_key_adoption_ so member-init order matches the
  /// initializer list.
  KeyExtractionResponder key_extraction_;
#ifdef IOHOME_LR1121_FIRMWARE_UPDATE
  /// Orchestrates the compile-gated LR1121 transceiver-firmware-update feature
  /// (lr1121_firmware_update_controller.cpp): boot-time bootloader read, cached flash verdict,
  /// two-press confirmation window, and the button-triggered flash/bootloader-rewrite sequences.
  /// Guarded member AND guarded initializer — an unguarded initializer for a guarded member is the
  /// classic way this breaks only under make firmware-test.
  Lr1121FirmwareUpdateController lr1121_firmware_update_;
#endif
  /// Subscribers to the per-command 1W report; one per "Last 1W Command" sensor.
  std::vector<OneWayCommandReportFn> oneway_report_callbacks_;
};

// ----------------------------------------------------------------------------
// Test-visible helpers (inline for host unit tests)
// ----------------------------------------------------------------------------

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
