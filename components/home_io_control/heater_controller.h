#pragma once

/// @file heater_controller.h
/// @brief Heating and hot water: the register heater's writes and reads, the water heater's reads and writes.
/// @ingroup hioc_hub
///
/// HeaterController owns every frame the hub sends to a heater or a water heater: the register heater's
/// CMD_WRITE_PRIVATE writes and register reads (ADR 0054), the level and temperature heaters' EXECUTEs, the
/// water heater's target, mode, boost and away writes with their read-backs, and the regular polls of all of
/// them. IOHomeControlComponent owns one instance, forwards the entities' and the `heating_control` action's
/// calls to it and hands a heater's regular poll to it (ADR 0083). The decoding of a heater's or water
/// heater's status answer is the stateless pair of functions at the end of this header.
///
/// The class reaches the hub through HeaterControllerHub, a handful of injected calls, and includes no
/// hub header. Unlike DiagnosticProbes (ADR 0072), one of those calls, HeaterControllerHub::status_read,
/// reaches the hub's status decoder: a water heater's status answer is an ordinary status reply.
///
/// The single-register reads (read_private_register(), read_dhw_register(), read_dhw_reading(),
/// read_dhw_status()), schedule_poll() and check_heater_model() are steps of the polls and writes; the hub
/// calls none of them. They are public so that their gates, timer keying and bounded runs are tested on
/// their own.

#include "device_registry.h"
#include "exchange_engine.h"
#include "hub_hooks.h"
#include "proto_dhw.h"
#include "proto_frame.h"
#include "proto_heating.h"
#include "proto_heating_profiles.h"
#include "proto_private_registers.h"
#include "status_poll_policy.h"

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
  /// Schedules the retry of a failed poll on the hub's backoff ladder (StatusPollPolicy::on_exchange_failed()
  /// and its log line); `auth_like` is true when the failed exchange saw a 0x3C challenge.
  std::function<void(const std::string &device_id, bool auth_like)> poll_backoff;
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
  /// @param poll_policy  The status poll ladder and the tracking window a poll runs in.
  /// @param engine       Shared exchange engine, read for its debug snapshot only.
  /// @param hub          The injected hub calls.
  HeaterController(const uint8_t *node_id, const bool *initialized, RadioDriver *const *radio, DeviceRegistry &registry,
                   StatusPollPolicy &poll_policy, ExchangeEngine &engine, HeaterControllerHub hub);

  HeaterController(const HeaterController &) = delete;
  HeaterController &operator=(const HeaterController &) = delete;

  // === Regular polls ===

  /// @brief Is the device's regular poll a heater poll: a register heater, a water heater or a heater read
  /// with the ordinary status read.
  /// @param dev The device record.
  /// @return true when poll() runs the device's poll.
  [[nodiscard]] static bool polls(const IoDevice &dev);

  /// @brief Run one regular poll of a heater or water heater and arm the next one.
  ///
  /// The body depends on the device: a register heater reads its profile's poll registers (stopping at the
  /// first failure), then the programme's temperature in force, its boot registers until each has been read
  /// once, and on a kind with model rows the one-off model check once it has answered a read; a water heater
  /// reads its status (target, capabilities, boost and away), then its water temperature, mode, heating
  /// state, boost and away duration and energy registers, then its brand until it has been read once; any
  /// other heater is polled with the generic status read. A register or water heater failure moves the
  /// retry onto the StatusPollPolicy ladder; a status-read heater leaves that to the status read itself.
  /// Whatever the poll finds, or whether it could run at all (the hub is not initialised yet), the next
  /// regular poll is armed one interval out (the device's `status_poll_interval`, else
  /// HEATING_POLL_DEFAULT_INTERVAL_MS, see heater_poll_interval_ms()). A device that is not registered
  /// arms no timer. Called by the hub's request_device_status() for a device polls() accepts.
  /// @param device_id Target device ID (hex string).
  /// @return true when every read of the poll was answered; false when it could not run.
  bool poll(const std::string &device_id);

  /// @brief The one-off model check of a heater kind that has model rows (device_offers_models()).
  ///
  /// Reads the model name (register 0x0119) and logs it at INFO, and compares whether the towel-dryer mode
  /// register (0x0136) holds a value with the declared `io_model:` (decisions::classify_heater_model_hint()),
  /// logging a WARN when they disagree. Nothing it finds changes behaviour. Done once both reads were
  /// answered, or after decisions::HEATER_MODEL_CHECK_MAX_RUNS runs. A step of the register poll; public so
  /// its bounded-run contract is tested on its own.
  /// @param device_id Target device ID.
  /// @param max_tries Transmit-attempt cap of each read (the poll's).
  void check_heater_model(const std::string &device_id, uint8_t max_tries);

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

  /// @brief Send a planned climate write (plan_heating_write()) and read the register back.
  ///
  /// Dispatches to send_heating_command(), set_heating_level() or set_heating_temperature() by the plan's
  /// kind. When the device accepted, `on_accepted` runs first, then the plan's register is read back
  /// (EXCHANGE_RETRY_COUNT tries), so the caller has applied the commanded state before the read-back
  /// notifies it. A refused plan or a rejected write sends no read-back and runs no hook. A failed
  /// read-back is already logged by read_heating_register(); the next poll finds the register again.
  /// @param device_id Target device ID (hex string).
  /// @param plan The plan; one with no write (HeatingWritePlan::is_write()) is not sent.
  /// @param on_accepted Called once after an accepted write; may be empty.
  /// @return true when the device accepted the write.
  bool execute_heating_plan(const std::string &device_id, const HeatingWritePlan &plan,
                            const std::function<void()> &on_accepted);

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
  /// encode_private_register_read(), which cannot produce a write.
  /// @param device_id Target device ID.
  /// @param reg_hi High byte of the register.
  /// @param reg_lo Low byte of the register.
  /// @param label What is read, for the log ("model_name").
  /// @param[out] response The reply; its value bytes start at PRIVATE_REGISTER_VALUE_OFFSET (none for a
  ///        register the device does not hold).
  /// @param max_tries Transmit-attempt cap.
  /// @return true when the device answered with the register's echo.
  bool read_private_register(const std::string &device_id, uint8_t reg_hi, uint8_t reg_lo, const char *label,
                             IoFrame &response, uint8_t max_tries);

  /// @brief Arm the heater's regular-cadence poll timer (run by poll() and moved by an acknowledged write).
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
  /// Confirmed on an Atlantic Calypso Connecté (issue #198).
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
  /// Confirmed on an Atlantic Calypso Connecté (issue #198).
  /// @param device_id Target device ID (hex string).
  /// @param on true to start away, false to end it.
  /// @return true when the device accepted the EXECUTE; false for a refused or unacknowledged command.
  bool set_water_heater_away(const std::string &device_id, bool on);

  /// @brief Read one family-0x00 register of a water heater raw (read_private_register() gated on
  /// device_feature::WATER_HEATER).
  /// @param device_id Target device ID.
  /// @param index Low byte of the register.
  /// @param label What is read, for the log ("dhw_water_temperature").
  /// @param[out] response The reply; its value bytes start at PRIVATE_REGISTER_VALUE_OFFSET.
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
  /// @brief Read back the register a write changed.
  ///
  /// Like read_heating_register() with the read-back's tries, and for the comfort setpoint of a heater
  /// whose programme runs it also moves the stored temperature in force with it
  /// (in_force_after_comfort_change()), so what the card shows is the new target before the poll that
  /// reads that register.
  /// @param device_id Target device ID (hex string).
  /// @param fn Register to read back.
  void read_back_heating_register_(const std::string &device_id, HeatingFunction fn);

  /// @brief The read and store behind read_heating_register() and read_back_heating_register_().
  /// @param device_id Target device ID (hex string).
  /// @param fn Register to read.
  /// @param[out] value_out Decoded value; nullptr when only the stored reading is wanted.
  /// @param max_tries Transmit-attempt cap.
  /// @param follow_comfort True to carry the temperature in force along with a comfort setpoint read.
  /// @return true only if the device answered and the reply decoded to a value for `fn`.
  bool read_and_store_heating_register_(const std::string &device_id, HeatingFunction fn, float *value_out,
                                        uint8_t max_tries, bool follow_comfort);

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

  /// @brief What runs inside run_poll_(): the reads of one poll; receives the try cap of each read and
  /// returns true when all were answered.
  using PollBody = std::function<bool(uint8_t max_tries)>;

  /// Who runs the failure ladder after a poll (run_poll_()).
  enum class PollLadder : uint8_t {
    RUN_BY_POLL,  ///< The wrapper: clear the ladder on success, back off on failure (register and water heater).
    RUN_BY_BODY,  ///< The body's own status read already runs it (a heater read with the ordinary status read).
  };

  /// @brief Run one poll: everything around the body that the three heater polls share.
  ///
  /// Returns false without a timer when the device is not registered. Otherwise, when the hub is
  /// initialised, it opens the tracking window unless one is running, computes the try band
  /// (decisions::scheduled_poll_max_tries()), calls `body` and applies the ladder as `ladder` says. It
  /// always ends by arming the next regular poll (heater_poll_interval_ms()), also when the hub is not
  /// initialised yet or the body found nothing, so no way out of a poll stops the cadence.
  /// @param device_id Target device ID.
  /// @param ladder Who clears or backs off the failure ladder.
  /// @param body The reads of one poll.
  /// @return The body's result; false when the poll could not run.
  bool run_poll_(const std::string &device_id, PollLadder ladder, const PollBody &body);

  /// @brief Poll a register heater: its profile's poll registers (stopping at the first failure), the
  /// programme's temperature in force, the boot registers until each has been read once, and on a heater
  /// kind with model rows the one-off model check once the device has answered a read.
  /// @param device_id Target device ID.
  /// @return true when every poll register answered.
  bool poll_registers_(const std::string &device_id);

  /// @brief The reads of one register-heater poll, in the order poll_registers_() describes.
  /// @param device_id Target device ID.
  /// @param profile The device's register profile.
  /// @param max_tries Transmit-attempt cap of each read (the poll's try band).
  /// @return true when every poll register answered.
  bool read_poll_registers_(const std::string &device_id, const HeatingProfile &profile, uint8_t max_tries);

  /// @brief Read the temperature in force of a heater's programme, when the programme runs.
  /// @details Sends nothing unless programme_period_active() holds for the readings the poll just made.
  ///          When the temperature fits neither period it forgets the eco setback, so the boot registers
  ///          read it again.
  /// @param device_id Target device ID.
  /// @param profile The device's profile; its programme_period must not be nullptr.
  /// @param max_tries Transmit-attempt cap of the read (the poll's).
  void poll_programme_period_(const std::string &device_id, const HeatingProfile &profile, uint8_t max_tries);

  /// @brief Poll a heater without private registers with the ordinary status read: a heating-level heater
  /// (its level in the position field) and a temperature heater (its setpoint in the main parameter).
  /// @param device_id Target device ID.
  /// @return true when the status read was answered.
  bool poll_status_read_(const std::string &device_id);

  /// @brief Poll a water heater: the status read, then its registers (stopping at the first failure), then
  /// its brand until it has been read once (logged at INFO).
  /// @param device_id Target device ID.
  /// @return true when the status read and every register were answered.
  bool poll_dhw_(const std::string &device_id);

  const uint8_t *node_id_;     ///< Controller node ID, owned by the hub.
  const bool *initialized_;    ///< The hub's initialized flag.
  RadioDriver *const *radio_;  ///< The hub's radio pointer, for link-health stamps.
  DeviceRegistry &registry_;   ///< Device records and entity notification.
  StatusPollPolicy &policy_;   ///< The poll ladder and tracking window.
  ExchangeEngine &engine_;     ///< Debug snapshot of the last exchange, logged on a failed one.
  HeaterControllerHub hub_;    ///< The injected hub calls.
};

// === Decoding a heater's status answer ===

/// @brief Record what a heater commanded with an EXECUTE reports as its current main parameter.
///
/// A heating-level heater reports a level; a temperature heater reports an alias (kept in
/// HeatingReadings::level) or a plain temperature (HeatingReadings::setpoint_c, decoded over the
/// device's encoding range, which also clears a stale alias). A device without a heating profile and a
/// register heater are left alone.
/// @param id Device ID for the log line.
/// @param dev Device record to update.
/// @param current_main_parameter The 16-bit current main parameter of a reply to a status poll.
void apply_heating_main_parameter(const std::string &id, IoDevice &dev, uint16_t current_main_parameter);

/// @brief Apply a water heater's status reply: its main parameter is the target temperature; the operating-mode
/// read (FP15, FP16) adds its capabilities and the boost and away state.
///
/// A water heater has no position, no movement and no battery: the cover parts of the status path (the
/// stopped flag, target/current position, the follow-up window, the moving evidence, the last-command
/// record) must not touch it, so it never reaches them. Only a reply to a status read carries the target;
/// the ack of an EXECUTE echoes the request and is not decoded.
/// @param id Device ID for the log line.
/// @param dev Device record to update.
/// @param frame A CMD_PRIVATE_RESP from the heater.
/// @param trust_position True for a reply to a status read, false for the ack of an EXECUTE.
void apply_dhw_status_reply(const std::string &id, IoDevice &dev, const IoFrame &frame, bool trust_position);

}  // namespace home_io_control
}  // namespace esphome
