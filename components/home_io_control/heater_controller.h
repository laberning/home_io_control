#pragma once

/// @file heater_controller.h
/// @brief Heating and hot water: the register heater's writes and reads, the water heater's reads and writes.
/// @ingroup hioc_hub
///
/// HeaterController owns every frame the hub sends to a heater or a water heater on the climate and
/// water heater entities' behalf: the register heater's CMD_WRITE_PRIVATE writes and register reads
/// (ADR 0054), the level and temperature heaters' EXECUTEs, and the water heater's target, mode, boost
/// and away writes with their read-backs. IOHomeControlComponent owns one instance and forwards the
/// entities' and the `heating_control` action's calls to it (ADR 0083).
///
/// The class reaches the hub through HeaterControllerHub, a handful of injected calls, and includes no
/// hub header. Unlike DiagnosticProbes (ADR 0072), one of those calls, HeaterControllerHub::status_read,
/// reaches the hub's status decoder: a water heater's status answer is an ordinary status reply.

#include "device_registry.h"
#include "exchange_engine.h"
#include "hub_hooks.h"
#include "proto_dhw.h"
#include "proto_frame.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace esphome {
namespace home_io_control {

class RadioDriver;

/// @brief Which of the hub's execute funnels runs a heater's EXECUTE.
enum class HeaterExecuteKind : uint8_t {
  WITH_ROLLBACK,  ///< The cover-shaped funnel: a failed command also withdraws the entity's optimistic prediction.
  TRY,            ///< The bare operation: no prediction to withdraw (a water heater never moves).
};

/// Guard of an execute operation: true for a device the operation may be sent to.
using HeaterExecuteAcceptsFn = std::function<bool(const IoDevice &)>;
/// Builds an execute operation's request frame for a device; false when the frame cannot be built.
using HeaterExecuteBuildFn = std::function<bool(IoFrame &, const IoDevice &)>;

/// @brief The hub capabilities a HeaterController needs, injected by the hub in its initializer list.
struct HeaterControllerHub {
  /// Runs one authenticated exchange on the command channel with the shared exchange bookkeeping
  /// (exchange counters, link health of a failed ending, result code of an error reply, device event
  /// reports) and no poll backoff. True only when the device answered with a reply frame in `response`.
  std::function<bool(const std::string &device_id, const IoFrame &request, IoFrame &response, uint8_t max_tries)>
      exchange;
  /// Runs one EXECUTE through the hub's execute funnel (gate, "Sending ..." log, exchange, settle poll)
  /// and returns whether the device accepted it.
  std::function<bool(const std::string &device_id, const char *action, const HeaterExecuteAcceptsFn &accepts,
                     const char *rejection_profile, const HeaterExecuteBuildFn &build, HeaterExecuteKind kind)>
      execute;
  /// Sends a status request and folds the answer into the device record through the hub's status decoder.
  /// True when the device answered.
  std::function<bool(const std::string &device_id, const IoFrame &request, uint32_t retry_after_fail_ms,
                     uint8_t max_tries)>
      status_read;
  NumericTimeoutFn set_timeout_by_id;  ///< Arms a numeric, replace-on-same-id timer on the hub's scheduler.
  std::function<void(const std::string &device_id)> queue_status_request;  ///< Queues a status poll of a device.
};

/// @brief Sends the frames of heaters and water heaters and decodes their register answers.
///
/// Non-copyable: it holds references into hub members.
/// @ingroup hioc_hub
class HeaterController {
 public:
  /// @param node_id      Controller 3-byte node ID, owned by the hub.
  /// @param initialized  Pointer to the hub's initialized flag.
  /// @param radio        Pointer to the hub's radio pointer (read per call, so a later assignment is seen).
  /// @param registry     Device registry for device lookups and entity notification.
  /// @param engine       Shared exchange engine, read for its debug snapshot only.
  /// @param hub          The injected hub calls.
  HeaterController(const uint8_t *node_id, const bool *initialized, RadioDriver *const *radio, DeviceRegistry &registry,
                   ExchangeEngine &engine, HeaterControllerHub hub);

  HeaterController(const HeaterController &) = delete;
  HeaterController &operator=(const HeaterController &) = delete;

  // === Register heater ===

  /// @brief Run one heating function on the heater's private register protocol (CMD_WRITE_PRIVATE
  /// 0x20): a write for a settable register, a read for a read-only one (`power_on`, `midnight_sync`).
  ///
  /// Both the `heating_control` hub action and the climate entity's writes reach this; together with
  /// read_heating_register() it is the only place register heating frames are built and sent.
  ///
  /// Flow: registry lookup -> device_uses_heating_registers() gate (rejected via
  /// detail::log_rejected_operation()) -> encode_heating_payload() -> create_write_private() (with the
  /// device's `low_power` flag) -> exchange(). It is deliberately not routed through the status read
  /// or the execute funnel: those are cover- and position-shaped (status decode, poll backoff), and a
  /// register ack carries no state to decode (ADR 0083). A CMD_WRITE_PRIVATE_ACK (0x21) reply is success;
  /// anything else (including CMD_ERROR_RESP) is failure. The exchange keeps the bookkeeping every
  /// exchange shares (Last Contact, Exchange Failures, Last Result Code, device event reports; ADR 0082).
  /// A caller that wants the value the heater now holds reads it back with read_heating_register().
  /// An acknowledged write also moves the heater's next poll to HEATING_POST_WRITE_POLL_DELAY_MS from
  /// now, so the regulator state (heating / idle) follows the change.
  /// @param device_id Target device ID (hex string).
  /// @param fn Heating function to send.
  /// @param value Function-specific value (degrees C, a HeatingMode as float, 0/1, or ignored), see
  ///        encode_heating_payload().
  /// @return true only if the device answered with CMD_WRITE_PRIVATE_ACK.
  bool send_heating_command(const std::string &device_id, HeatingFunction fn, float value);

  /// @brief Set the heating level of a heater that is commanded with levels (the plain electrical heater).
  ///
  /// A main-parameter-only EXECUTE through the hub's execute funnel; the settle poll that follows reads
  /// the level back with the ordinary status read, and the climate entity shows that reading.
  /// \todo Experimental: unconfirmed on hardware.
  /// @param device_id Target device ID (hex string).
  /// @param level Heating level low byte (heating_level::*).
  /// @return true when the device accepted the command.
  bool set_heating_level(const std::string &device_id, uint8_t level);

  /// @brief Command a heater that carries a temperature or an alias in its main parameter (the heating
  /// temperature interface and the heat pump).
  ///
  /// A main-parameter-only EXECUTE, or one that also carries functional parameter 9 or 16 when the
  /// profile sends mode flags, through the hub's execute funnel; the settle poll that follows reads the
  /// state back with the ordinary status read.
  /// \todo Experimental: unconfirmed on hardware.
  /// @param device_id Target device ID (hex string).
  /// @param main_parameter The 16-bit main parameter (encode_temperature_main_parameter() or an alias).
  /// @param fp The functional parameter that accompanies it, if any (kind NONE sends a
  ///        main-parameter-only EXECUTE).
  /// @return true when the device accepted the command.
  bool set_heating_temperature(const std::string &device_id, uint16_t main_parameter, const HeatingFp &fp);

  /// @brief Set the temperature encoding range of a heater that carries a temperature in its main
  /// parameter (YAML `min_temperature:` / `max_temperature:`).
  /// @param device_id Target device ID.
  /// @param min_c Lower end of the range in degrees Celsius.
  /// @param max_c Upper end; must exceed `min_c`.
  void set_device_temperature_range(const std::string &device_id, float min_c, float max_c);

  /// @brief Read one register of a heater and fold the decoded value into IoDevice::heating.
  ///
  /// Same gate as send_heating_command(); the read itself is read_private_register(). A reply that
  /// decodes (decode_heating_reply()) updates the device record and notifies the entity callbacks, so
  /// what an entity shows is what the heater reported.
  /// @param device_id Target device ID (hex string).
  /// @param fn Register to read; must be readable (see heating_function_is_readable()).
  /// @param[out] value_out Decoded value (NaN for a room-temperature sensor fault); pass nullptr when
  ///        only the stored reading is wanted.
  /// @param max_tries Transmit-attempt cap: EXCHANGE_RETRY_COUNT for a read the user waits on (the
  ///        action, the read-back of a write), the scheduled-poll band for the background poll.
  /// @return true only if the device answered and the reply decoded to a value for `fn`.
  bool read_heating_register(const std::string &device_id, HeatingFunction fn, float *value_out, uint8_t max_tries);

  /// @brief Read one private register raw: build the read request, run the exchange and check that the
  /// reply echoes the register.
  ///
  /// The one place a register read is framed and sent; read_heating_register() decodes on top of it, and
  /// the reads no HeatingFunction describes (a model name in ASCII, "is there a value at all") use it
  /// directly. Gated by device_accepts_register_probe(); the request is built by
  /// encode_heating_register_read(), which cannot produce a write.
  /// @param device_id Target device ID.
  /// @param reg_hi High byte of the register.
  /// @param reg_lo Low byte of the register.
  /// @param label What is read, for the log ("model_name").
  /// @param[out] response The reply; its value bytes start at HEATING_REPLY_VALUE_OFFSET (none for a
  ///        register the device does not hold).
  /// @param max_tries Transmit-attempt cap.
  /// @return true when the device answered with the register's echo.
  bool read_private_register(const std::string &device_id, uint8_t reg_hi, uint8_t reg_lo, const char *label,
                             IoFrame &response, uint8_t max_tries);

  /// @brief Arm the heater's regular-cadence poll timer.
  ///
  /// Own timer id (decisions::heating_poll_timer_id()), so a remote-activity poll scheduled for the same
  /// device never replaces it. The callback queues a status poll through HeaterControllerHub.
  /// @param device_id ID of the heater.
  /// @param delay_ms Delay in milliseconds before the poll is queued.
  void schedule_poll(const std::string &device_id, uint32_t delay_ms);

  // === Water heater (device_feature::WATER_HEATER) ===

  /// @brief Set a water heater's target temperature.
  ///
  /// A main-parameter-only EXECUTE with the target on the relative scale over DHW_TARGET_MIN_C..
  /// DHW_TARGET_MAX_C (encode_dhw_target()), through the bare execute operation without the cover's
  /// movement bookkeeping. The settle poll that follows reads the target back from the status reply.
  /// Gated on device_feature::WATER_HEATER. The heater's display follows the target (issue #198).
  /// @param device_id Target device ID (hex string).
  /// @param celsius Target in degrees Celsius; a value outside the heater's range is refused, not clamped.
  /// @return true when the device accepted the command.
  bool set_water_heater_target(const std::string &device_id, float celsius);

  /// @brief Set a water heater's operating mode: a write of register 0x0006 (encode_dhw_mode_write()).
  ///
  /// The register is read back at once, so the entity shows what the heater then holds, and so is the
  /// target (the status read of the poll), which the mode decides. The next poll moves to
  /// HEATING_POST_WRITE_POLL_DELAY_MS from now so the heating state follows the change. Gated on
  /// device_feature::WATER_HEATER. The heater's display follows the mode (issue #198).
  /// @param device_id Target device ID (hex string).
  /// @param mode Mode to write.
  /// @return true only if the device acknowledged the write; a missed read-back does not change that.
  bool set_water_heater_mode(const std::string &device_id, DhwMode mode);

  /// @brief Start or end a water heater's boost: a duration write of register 0x0000, then an EXECUTE with
  /// the operating-mode word FP16 (DHW_OPERATING_MODE_BOOST_ON, or DHW_OPERATING_MODE_OFF to end).
  ///
  /// See set_dhw_operating_mode_() for the sequence. Ending a boost also ends away, because the off word
  /// switches both (ADR 0081). Gated on device_feature::WATER_HEATER.
  /// \todo Experimental: confirm on a real water heater that this starts and ends a boost.
  /// @param device_id Target device ID (hex string).
  /// @param on true to start a boost, false to end it.
  /// @return true when the device accepted the EXECUTE; false for a refused or unacknowledged command.
  bool set_water_heater_boost(const std::string &device_id, bool on);

  /// @brief Start or end a water heater's away (holiday) mode: a duration write of register 0x0005 (255,
  /// until turned off), then an EXECUTE with FP16 DHW_OPERATING_MODE_AWAY_ON, or DHW_OPERATING_MODE_OFF
  /// to end.
  ///
  /// See set_dhw_operating_mode_() for the sequence. Ending away also ends a boost (ADR 0081). Gated on
  /// device_feature::WATER_HEATER.
  /// \todo Experimental: confirm on a real water heater that this starts and ends the holiday mode.
  /// @param device_id Target device ID (hex string).
  /// @param on true to start away, false to end it.
  /// @return true when the device accepted the EXECUTE; false for a refused or unacknowledged command.
  bool set_water_heater_away(const std::string &device_id, bool on);

  /// @brief Read one family-0x00 register of a water heater raw (read_private_register() gated on
  /// device_feature::WATER_HEATER).
  /// @param device_id Target device ID.
  /// @param index Low byte of the register.
  /// @param label What is read, for the log ("dhw_water_temperature").
  /// @param[out] response The reply; its value bytes start at HEATING_REPLY_VALUE_OFFSET.
  /// @param max_tries Transmit-attempt cap forwarded to read_private_register().
  /// @return true when the device answered with the register's echo.
  bool read_dhw_register(const std::string &device_id, uint8_t index, const char *label, IoFrame &response,
                         uint8_t max_tries);

  /// @brief Read one family-0x00 register of a water heater and fold its value into IoDevice::dhw.
  /// @details A value the decoder does not take (an empty value, a mode byte never seen) is logged at
  ///          DEBUG and leaves the reading as it was; it is still an answer, not a failed read.
  /// @param device_id Target device ID.
  /// @param index One of the DHW_REG_* registers.
  /// @param max_tries Transmit-attempt cap.
  /// @return true when the device answered.
  bool read_dhw_reading(const std::string &device_id, uint8_t index, uint8_t max_tries);

  /// @brief Read a water heater's target, capabilities and boost and away state: the status read
  /// DHW_STATUS_FPI1 / DHW_STATUS_FPI2 (create_status_mp_fp_read()), whose reply the hub's status
  /// decoder folds into IoDevice::dhw.
  /// @param device_id Target device ID.
  /// @param max_tries Transmit-attempt cap.
  /// @return true when the device answered.
  bool read_dhw_status(const std::string &device_id, uint8_t max_tries);

 private:
  /// @brief The exchange every heating frame shares: run it through HeaterControllerHub::exchange (the
  /// shared bookkeeping, no poll backoff since the heater poll keeps its own), stamp link health of a
  /// reply, notify the entities and report whether the device answered with CMD_WRITE_PRIVATE_ACK
  /// (ADR 0082).
  /// @param device_id Target device ID.
  /// @param label What was sent, for the log ("set_temperature", "read room_temperature").
  /// @param request The framed CMD_WRITE_PRIVATE request.
  /// @param[out] response The device's reply; valid when true is returned.
  /// @param max_tries Transmit-attempt cap.
  /// @return true only if the reply is a CMD_WRITE_PRIVATE_ACK.
  bool exchange_heating_(const std::string &device_id, const char *label, const IoFrame &request, IoFrame &response,
                         uint8_t max_tries);

  /// @brief Write one family-0x00 register of a water heater, then read it back.
  ///
  /// Shared by the mode setter and the boost and away duration writes, each of which encodes its own
  /// payload. Gated on device_feature::WATER_HEATER; the write has the full retry budget, the read-back
  /// (read_dhw_reading()) updates IoDevice::dhw, and the next poll moves to
  /// HEATING_POST_WRITE_POLL_DELAY_MS from now.
  /// @param device_id Target device ID.
  /// @param operation What is written, for the log ("hot water mode").
  /// @param index The register written, which is also the one read back.
  /// @param payload The encoded write (`0C 61 00 <index> <value>`).
  /// @param payload_len Its length.
  /// @return true only if the device acknowledged the write; a missed read-back does not change that.
  bool write_dhw_register_(const std::string &device_id, const char *operation, uint8_t index, const uint8_t *payload,
                           size_t payload_len);

  /// @brief Switch a water heater's boost or away through the operating-mode word (ADR 0081).
  ///
  /// Refused and logged when FP15 has been read and lacks the feature's capability bit. On: the duration
  /// register write (its ack is logged, a missed one does not stop the sequence), then one EXECUTE with
  /// MAIN_PARAMETER_KEEP and the feature's FP16. Off: the EXECUTE with DHW_OPERATING_MODE_OFF only. After
  /// an accepted EXECUTE the status is read at once and the next poll moves to
  /// HEATING_POST_WRITE_POLL_DELAY_MS from now.
  /// @param device_id Target device ID.
  /// @param which Boost or away.
  /// @param on true to start it, false to end it (which ends the other too).
  /// @return true when the device accepted the EXECUTE.
  bool set_dhw_operating_mode_(const std::string &device_id, DhwOperatingFeature which, bool on);

  /// @brief The water heater `device_id` names, or nullptr when it is unknown, the hub is not set up yet,
  /// or the device is no water heater (that case is logged). The gate of every water heater write.
  /// @param device_id Target device ID.
  /// @param operation What is being written, for the rejection log line.
  /// @return The device record, or nullptr.
  const IoDevice *water_heater_for_write_(const std::string &device_id, const char *operation);

  /// @brief Send one water heater EXECUTE (target or operating mode) through the bare execute operation,
  /// which gates on device_feature::WATER_HEATER and arms the settle poll; on acceptance the next poll
  /// moves to HEATING_POST_WRITE_POLL_DELAY_MS from now so the heating state follows.
  /// @param device_id Target device ID.
  /// @param action Operation name for the logs.
  /// @param build Builds the EXECUTE for the device.
  /// @return true when the device accepted the EXECUTE.
  bool execute_dhw_(const std::string &device_id, const char *action, const HeaterExecuteBuildFn &build);

  const uint8_t *node_id_;     ///< Controller node ID, owned by the hub.
  const bool *initialized_;    ///< The hub's initialized flag.
  RadioDriver *const *radio_;  ///< The hub's radio pointer, for link-health stamps.
  DeviceRegistry &registry_;   ///< Device records and entity notification.
  ExchangeEngine &engine_;     ///< Debug snapshot of the last exchange, logged on a failed one.
  HeaterControllerHub hub_;    ///< The injected hub calls.
};

}  // namespace home_io_control
}  // namespace esphome
