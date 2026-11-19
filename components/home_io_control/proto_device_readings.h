#pragma once

/// @file proto_device_readings.h
/// @brief The named readings a device can show, the capability each needs and where each value comes from.
/// @ingroup hioc_protocol
///
/// A reading is what the hub keeps of one value of a device: a heater's room temperature, its energy counter, the
/// minutes left of a towel dryer's run, the heating level its status reply reports, a water heater's water
/// temperature, a cover's rain state, sun level and battery. One table (DEVICE_READINGS in the
/// .cpp) lists every reading by name with its source and, for a reading an entity of the YAML can ask for, the
/// capability a device needs (DeviceFeature, ADR 0098). The source says how the value is obtained, so the kinds stay
/// apart and a new kind leaves the rows of the others alone:
///  - RegisterSource: one private register of a heater or a water heater, decoded with a ValueCodec
///    (proto_value_codecs.h), or one slice of it with a codec of its own; optionally only while another register
///    holds a value;
///  - StatusMainParameterSource: the main parameter of a status reply, as a heating level or a temperature;
///  - StatusFunctionalParameterSource: one of the functional parameters FP9-FP16 of a status reply, as the whole word
///    or one two-bit field of it;
///  - DerivedSource: a value worked out from other readings (the minutes left of whichever run is active), with the
///    registers it needs polled;
///  - DataObjectSource: a slice of a device's data object, read on a schedule through a transfer recipe
///    (transfer_reader.h) and decoded with a ValueCodec;
///  - EventSource: the data of a device event, the answer to an event subscription sent on a schedule (ADR 0064), a
///    slice of it decoded with a ValueForm;
///  - LimitationSource: the answer to a limitation read sent on a schedule (a window's rain protection);
///  - StatusBatterySource: the coarse battery level in byte 1 of a status reply.
///
/// A reading that is read on a schedule (data object, event, limitation) also says what its value becomes when the
/// reads keep missing (ReadingStale).
///
/// Every value but a derived one is kept in the device record, by reading (DeviceReadingValues), since a read takes
/// seconds and answers later than the poll that asked, and a status reply answers without being asked for. What the
/// hub polls and what is shown stay separate: the registers the poll reads are derived from the rows
/// (reading_polled_registers(), device_poll_reads()), and every consumer (the climate card, the window sensor, the
/// reading entities) reads through reading_value(), wherever the source keeps the value. The YAML key and the Home
/// Assistant metadata of a reading (its entity, unit, device class, and the name of a companion entity made for it)
/// are not here: the codegen mirror (hub_validators.py `DEVICE_READINGS`) carries them, checked against this table by
/// name, capability, source kind, registers and value kind (tests/sync/device_features_sync_test.cpp). What stays in
/// C++ is what the entity needs at run time: the names of a named value (`value_name`).
///
/// Outside the table, with the reason: the model-name register (text, logged once), the registers the hub only logs
/// (PAIRED_DEVICES, SETPOINT_BLOCK), the raw probes (`register_00`, `register_01`, `read_object`; raw by design), and a
/// cover's position, tilt and remaining time (fields of the status frame the cover entity shows, not readings of a
/// register).

#include "proto_constants.h"
#include "proto_device_features.h"
#include "proto_device_model.h"
#include "proto_frame.h"
#include "proto_heating.h"
#include "proto_value_codecs.h"

#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace esphome {
namespace home_io_control {

/// @brief One named reading. Adding one is an enumerator before COUNT, its row in the table and its entry in the
/// codegen mirror.
enum class DeviceReading : uint8_t {
  CENTRAL_HEATING_MODE,        ///< 1 while a mixed towel dryer runs on central heating (its element stays off).
  OCCUPANCY_DETECTION,         ///< The heater's occupancy detection setting; a writable setting.
  OCCUPANCY,                   ///< Someone detected in the room.
  OPEN_WINDOW_DETECTION,       ///< The heater's open-window detection setting; a writable setting.
  ENERGY_TOTAL,                ///< The energy the heater has counted, whole kWh.
  BOOST_DURATION,              ///< The boost duration set on a towel dryer, minutes.
  DRYING_DURATION,             ///< The duration of the last drying run a towel dryer started, minutes.
  BOOST_REMAINING,             ///< Minutes left of a running boost; read only while a boost runs.
  DRYING_REMAINING,            ///< Minutes left of a running drying run; read only while it runs.
  REMAINING_TIME,              ///< Minutes left of whichever run is active, 0 with none (derived).
  ROOM_TEMPERATURE,            ///< The room temperature the heater measures, degrees; NaN on a sensor fault.
  SETPOINT,                    ///< The comfort setpoint the heater holds, degrees.
  MAX_SETPOINT,                ///< The highest setpoint the heater accepts, degrees.
  MODE,                        ///< The heater's operating mode (HeatingMode).
  REGULATOR_STATE,             ///< The regulator's state (HeatingRegulatorState).
  WINDOW_OPEN,                 ///< The heater's window sensor, 1 open and 0 closed.
  TOWEL_MODE,                  ///< A towel dryer's mode (TowelDryerMode).
  TEMPORARY_STATE,             ///< A towel dryer's temporary state, the timed run it is in (HeatingTemporaryState).
  WORKING_RATE,                ///< The share of the time the element heats, 0-100 %.
  TOWEL_TEMPERATURE_IN_FORCE,  ///< The temperature a towel dryer holds right now, degrees.
  TEMPERATURE_IN_FORCE,        ///< The temperature a radiator holds right now, degrees.
  DEROGATED_SETPOINT,          ///< A heater's temporary override of its programme, degrees; 0 while none is active.
  NIGHT_LIGHT,                 ///< A heater's night light, 1 on and 0 off.
  NIGHT_LIGHT_BRIGHTNESS,      ///< The night light's brightness register as a percentage, 0-100.
  HEATING_LEVEL,               ///< The heating level, or setpoint alias, a heater's status reply reports (low byte).
  STATUS_SETPOINT,             ///< The setpoint a status reply reports, degrees, rounded to tenths: a temperature
                               ///< heater's setpoint, a water heater's target.
  HOT_WATER_TEMPERATURE,       ///< A water heater's water temperature, degrees.
  HOT_WATER_MODE,              ///< A water heater's operating mode (DhwMode).
  HOT_WATER_HEATING,           ///< The water heater heats at all (heat pump, element or both).
  HOT_WATER_ELECTRIC_BACKUP,   ///< The water heater's electric element runs.
  HOT_WATER_HEAT_PUMP,         ///< The water heater's heat pump runs.
  HOT_WATER_BOOST_DURATION,    ///< A water heater's boost duration register. Logged, never a state.
  HOT_WATER_AWAY_DURATION,     ///< A water heater's away duration register, days. Logged, never a state.
  HOT_WATER_ENERGY,            ///< The energy a water heater has consumed since installation, kWh.
  HOT_WATER_BRAND,             ///< The brand letter of a water heater. Logged once.
  HOT_WATER_CAPABILITIES,      ///< The operating-mode capabilities a water heater reports (FP15 word).
  HOT_WATER_BOOST,             ///< A water heater's boost is on, as its operating-mode word (FP16) reports it.
  HOT_WATER_AWAY,              ///< A water heater's away mode is on, as its operating-mode word (FP16) reports it.
  RAIN,                        ///< A window's rain protection, 1 while its minimum limit is rain-sized and 0 below it.
  SUN_LEVEL,                   ///< The sun on a solar cover's panel, lux (an estimate, SUN_VALUE_LUX_PER_STEP).
  BATTERY_LEVEL,               ///< A battery-powered cover's charge level, percent, from its event.
  BATTERY_STATE,               ///< A cover's coarse battery level from its status reply (BatteryState).
  BATTERY_LOW,                 ///< 1 while a cover reports a low battery and 0 while it reports another level.
  HEAT_PUMP_HEATING_ENERGY,    ///< The energy an Atlantic heat pump has counted for heating, whole kWh.
  HEAT_PUMP_HOT_WATER_ENERGY,  ///< The energy an Atlantic heat pump has counted for hot water, whole kWh.
  HEAT_PUMP_OUTDOOR_TEMPERATURE,  ///< The outdoor temperature an Atlantic heat pump measures, degrees.
  COUNT,                          ///< Number of readings; not a reading.
};

static_assert(static_cast<size_t>(DeviceReading::COUNT) <= DeviceReadingValues::SLOTS,
              "every reading needs a slot in DeviceReadingValues; raise SLOTS");

/// @brief A reading another register must show for a register read to run.
struct HeatingReadCondition {
  DeviceRegister fn;  ///< The register whose stored reading decides; an integer register.
  uint8_t value;      ///< The value `fn` must hold.
};

/// @brief A slice of a register's value, with a codec of its own: the register holds several facts in different bytes.
struct RegisterPart {
  uint8_t first;     ///< Index of the first byte of the slice in the value bytes.
  uint8_t length;    ///< Number of bytes of the slice; the reading has no value when the reply is shorter.
  ValueCodec codec;  ///< How the slice maps onto a number.
};

/// @brief A reading that is one register of a heater or a water heater, or a part of it.
/// @details Several readings may name the same register, each with its own part: one read of the register stores
///          them all (store_register_reply()). A reading without a part is the register's value, decoded with the
///          register's codec.
struct RegisterSource {
  DeviceRegister fn;  ///< The register; its codec is the register table's.
  std::optional<HeatingReadCondition> only_while =
      std::nullopt;  ///< When set, the register is read only while this holds (the minutes left of a run, only
                     ///< while that run is on).
  std::optional<RegisterPart> part =
      std::nullopt;  ///< When set, the reading is this slice of the value, not the register's own value.
};

/// @brief How a status reply's main parameter becomes a reading.
enum class StatusValueForm : uint8_t {
  LEVEL,        ///< The low byte, when the high byte is the prefix of the heater's profile (a heating level or an
                ///< alias): HeatingProfile::mp_prefix.
  TEMPERATURE,  ///< A temperature encoded over the device's range (IoDevice::temperature_range_min_c and `_max_c`).
};

/// @brief A reading that is the main parameter of a status reply of a heater commanded with an EXECUTE.
/// @details Not read by a poll of its own: the status read answers it, and the hub's status decoder
///          (apply_heating_main_parameter(), apply_register_heater_status_reply(), apply_dhw_status_reply()) stores it.
///          The two bytes are decoded with ValueCodec::U16_BE and `form` says what they mean. A device that answers no
///          status read (DeviceFeature::NO_STATUS_READ) never gets one.
struct StatusMainParameterSource {
  StatusValueForm form;  ///< What the main parameter means for this reading.
};

/// @brief How a functional parameter of a status reply becomes a reading.
enum class StatusParameterForm : uint8_t {
  WORD,      ///< The whole 16-bit value.
  FIELD_ON,  ///< A two-bit field of the value, shifted by StatusFunctionalParameterSource::shift: 1 when the field
             ///< reads 1 (on), 0 for any other value (keep, off).
};

/// @brief A reading that is one of the functional parameters FP9-FP16 of a status reply.
/// @details Not read by a poll of its own: the status read that asked for the parameter answers it, and the status
///          decoder (store_status_parameter_readings()) stores it.
struct StatusFunctionalParameterSource {
  uint8_t fp;                ///< The functional parameter, 9-16.
  StatusParameterForm form;  ///< What the value means for this reading.
  uint8_t shift{0};          ///< FIELD_ON: lowest bit of the field.
};

/// @brief A reading worked out from other readings of the same device.
struct DerivedSource {
  std::optional<float> (*value)(const IoDevice &);  ///< The value now, or nullopt while it cannot be told.
  std::span<const DeviceRegister> polls;            ///< The registers the poll must read for `value` to know.
  ValueCodec codec{ValueCodec::NONE};               ///< What kind of number `value` gives: BINARY for a flag, which
                                                    ///< a binary sensor shows; NONE for any other number.
};

/// @brief A reading that is a slice of a data object of the device, read on a schedule.
/// @details The read is one TransferReader read of `recipe` at `offset` (a row of the recipe table the reader holds,
///          and an offset that recipe allows); the value is `codec` applied to the bytes `first` to
///          `first + length - 1` of what came back. The hub keeps it in IoDevice::reading_values.
struct DataObjectSource {
  uint8_t recipe;      ///< Row of the transfer recipe table (TRANSFER_RECIPES in production).
  uint16_t offset;     ///< Offset the object is opened at; one the recipe allows.
  uint8_t first;       ///< Index of the first byte of the slice in the data read.
  uint8_t length;      ///< Number of bytes of the slice; at least what the codec reads.
  ValueCodec codec;    ///< How the slice maps onto a number.
  uint16_t max_value;  ///< Highest value of a bounded codec (decode_value()); 0 for the others.
};

/// @brief A reading that is the data of a device event, the answer to an event subscription.
/// @details The hub sends the subscription again on every scheduled read (ADR 0064) and the device answers with the
///          event's current value in a CMD_STATUS_UPDATE, which is the reading and nothing else: it never reaches the
///          position decode. The answer is accepted only when it is that event, carries exactly `data_length` data
///          bytes and, where the row names one, starts with `type_byte`; any other shape is not decoded
///          (decode_event_reading()).
struct EventSource {
  uint16_t event;                    ///< The event the hub subscribes to and the answer carries (EVENT_LOCAL_SENSOR).
  uint8_t data_length;               ///< Exact number of event data bytes of the captured answer.
  std::optional<uint8_t> type_byte;  ///< The first event data byte, when the answer must carry a given one (the
                                     ///< sensor type of EVENT_LOCAL_SENSOR).
  uint8_t value_offset;              ///< Index in the event data of the first byte of the value.
  ValueForm form;                    ///< How the bytes from `value_offset` map onto a number.
  uint16_t factor{1};                ///< The decoded number is multiplied by this: natural units per step.
};

/// @brief A reading that is the answer to a limitation read (CMD_LIMITATION_STATUS_REQ), sent on a schedule.
/// @details The reply is accepted only inside the exchange that asked and never reaches the status decode, so the
///          read cannot move a device's position or target. The row's value is the rain flag the reply's value gives
///          (decisions::rain_from_limitation_reply()); a reply that says nothing of rain is no value.
struct LimitationSource {
  LimitationType type;  ///< Which limit the hub asks for.
};

/// @brief A reading that is the coarse battery level in byte 1 of a status reply (decode_battery_state()).
/// @details Not read by a poll of its own: a status reply that answers the hub's own request or an authenticated
///          status update of the device stores it, never an execute acknowledgement (its layout follows the request)
///          or an overheard frame (ADR 0022). A reply that shows no battery marks the reading absent.
struct StatusBatterySource {};

/// @brief Where a reading's value comes from. Rows of an existing kind are plain additions; a new kind is a further
/// alternative here with its branches wherever the table is walked by kind.
using ReadingSource = std::variant<RegisterSource, DerivedSource, DataObjectSource, StatusMainParameterSource,
                                   StatusFunctionalParameterSource, EventSource, LimitationSource, StatusBatterySource>;

/// @brief What becomes of a scheduled reading's value when its reads keep missing.
enum class ReadingStale : uint8_t {
  KEEP,  ///< The last value stays shown: a counter only grows, so an old value is still the best one.
  DROP,  ///< The value is dropped after PERIODIC_POLL_FAILURES_BEFORE_UNKNOWN misses in a row: a state that
         ///< changes (rain, sun, charge) is no longer known, and a shown "dry" must mean a reply said so.
};

/// @brief When a device's poll reads a reading by itself.
/// @details Which devices read it is decided by the row's capability or, for a row without one, by the heating
///          profile (heating_profile_reads()); this is the cadence on a device that does. Within one policy the
///          order of the rows is the order of the reads (device_poll_reads()). A reading read on a schedule of its own
///          (a data object, an event, a limitation) is never consulted by the heater poll.
enum class ReadingPoll : uint8_t {
  EVERY_POLL,           ///< Every poll; the first read that fails ends the poll and fails it.
  EVERY_POLL_MAY_MISS,  ///< Every poll, after the readings of EVERY_POLL; a miss keeps the earlier value and costs a
                        ///< read, not the poll (a setting the heater's own controller changes, a part a model may
                        ///< lack).
  ONCE_AFTER_BOOT,      ///< Until it holds a value; a property that changes rarely.
  WHEN_ASKED,           ///< Where an entity of the YAML declares the reading, until the heater answers it empty; a
                        ///< register that is read only while another holds a value is skipped, and its earlier
                        ///< value dropped, while it does not.
  NOT_POLLED,           ///< No heater poll reads it for its own sake: a status reply carries it, another reading it
                        ///< is derived from does, or the programme rule and a write's read-back read it.
};

/// @brief What a reading's number is, which decides the entity that shows it.
enum class ReadingValueKind : uint8_t {
  FLAG,    ///< 0 or 1: a binary sensor shows it, a switch shows and writes it.
  NAMED,   ///< A whole number with a name for each value the row knows (DeviceReadingRow::value_name): a text sensor.
  NUMBER,  ///< Any other number: a sensor.
};

/// @brief Which temperature of the climate card a reading serves, for a profile that shows it.
enum class CardUse : uint8_t {
  NONE,                 ///< No fixed role: the profile names the register (mode, preset, action, programme).
  CURRENT_TEMPERATURE,  ///< The card's current temperature (HeatingFeatures::current_temperature).
  TARGET_TEMPERATURE,   ///< The card's target and its bounds (HeatingFeatures::target_temperature).
};

/// @brief One row of the reading table.
struct DeviceReadingRow {
  const char *name;       ///< Lower-case name for logs; its upper-case form is the key of the codegen mirror.
  ReadingSource source;   ///< Where the value comes from.
  DeviceReading reading;  ///< The reading.
  ReadingPoll poll;       ///< When the hub reads it by itself.
  std::optional<DeviceFeature> feature =
      std::nullopt;  ///< The capability a device kind needs to have the reading; none for a reading the heater's
                     ///< climate card keeps by its profile (heating_profile_reads()), which no key of the YAML
                     ///< declares. A status reading's capability says the device reports it (a heating level).
  const char *(*value_name)(uint8_t value) =
      nullptr;  ///< The name of each value a NAMED reading takes (nullptr for a value that has none); unset for the
                ///< other kinds. The entity that shows the reading publishes the name, never the number.
  CardUse card{CardUse::NONE};  ///< The role the climate card gives the reading; read by heating_profile_reads().
  ReadingStale stale{ReadingStale::KEEP};  ///< What a scheduled reading's value becomes after repeated misses; the
                                           ///< other kinds never consult it.
};

/// @brief What kind of number a reading is.
/// @details A row with a value-name function is NAMED; otherwise a flag codec (the register's, the data object's or
///          the event's, or a derived reading's own) makes it a FLAG, as does the answer to a limitation read (the
///          rain flag), and anything else is a NUMBER. The generated code and the entity that shows the reading
///          follow it (the codegen mirror's `entity`, checked in tests/sync/device_features_sync_test.cpp).
/// @param row The reading's row.
/// @return The kind.
[[nodiscard]] ReadingValueKind reading_value_kind(const DeviceReadingRow &row);

/// @brief Whether a scheduled poll that brings the value it already had is worth a debug line.
/// @details A flag (rain) is polled often and rarely changes, so repeating it would only fill the log; a number or a
///          named value says something even when it is the same.
/// @param row The reading's row.
/// @return False for a flag reading, true otherwise.
[[nodiscard]] bool reading_logs_unchanged(const DeviceReadingRow &row);

/// @brief Look up the row of a reading.
/// @param reading The reading.
/// @return The row, or nullptr for COUNT or a value outside the enumeration.
const DeviceReadingRow *find_device_reading(DeviceReading reading);

/// @brief Look up the row of a reading in a table of the caller's (the hub's reading table, which a test replaces).
/// @param reading The reading.
/// @param table The rows to look in.
/// @return The row, or nullptr when the table has none for the reading.
const DeviceReadingRow *find_device_reading(DeviceReading reading, std::span<const DeviceReadingRow> table);

/// @brief Every reading, in table order, for code that checks the whole table.
std::span<const DeviceReadingRow> device_readings();

/// @brief The current value of a reading for a device, wherever its source keeps it.
/// @details A derived reading is worked out from other readings; every other kind is the value the device record
///          keeps (IoDevice::reading_values). Binary readings are 0 and 1. A value that is NaN is a value (a sensor
///          the device reports as faulty).
/// @param dev The device record.
/// @param reading The reading.
/// @return The value, or nullopt while it has not been read, was dropped or cannot be told.
std::optional<float> reading_value(const IoDevice &dev, DeviceReading reading);

/// @brief reading_value() over a table of the caller's, so a test can give a row that production does not have.
/// @param dev The device record.
/// @param reading The reading.
/// @param table The rows to look the reading up in.
/// @return The value, or nullopt while it has not been read, was dropped or cannot be told.
std::optional<float> reading_value(const IoDevice &dev, DeviceReading reading, std::span<const DeviceReadingRow> table);

/// @brief The slot of a reading in a device's DeviceReadingValues.
/// @param reading The reading.
/// @return The reading's number in the table's enumeration.
[[nodiscard]] constexpr size_t reading_slot(DeviceReading reading) { return static_cast<size_t>(reading); }

/// @brief The value a data-object read carries for a reading.
/// @param source The reading's slice and codec.
/// @param data The bytes the transfer read returned.
/// @return The decoded value, or nullopt when the slice lies beyond the data or the bytes are the codec's no-value
///         pattern.
std::optional<float> decode_data_object_reading(const DataObjectSource &source, std::span<const uint8_t> data);

/// @brief The value an event report gives for an event reading.
/// @details The frame must be a well-formed CMD_STATUS_UPDATE (decode_status_update_event()) carrying the source's
///          event with exactly its data length and, when the source names one, its type byte. The value is the
///          source's form applied from its offset, multiplied by its factor.
/// @param source The reading's event and layout.
/// @param frame The device's answer to the event subscription.
/// @return The value in natural units, or nullopt for any other event, shape or a value the form refuses (a level
///         above its bound).
std::optional<float> decode_event_reading(const EventSource &source, const IoFrame &frame);

/// @brief The coarse battery level the device record holds (DeviceReading::BATTERY_STATE).
/// @param dev The device record.
/// @return The level, or BatteryState::NOT_REPORTED while no status reply showed one.
[[nodiscard]] BatteryState battery_state_of(const IoDevice &dev);

/// @brief The value a status reply's main parameter gives for a status reading.
/// @details The two bytes are decoded with ValueCodec::U16_BE, then the reading's StatusValueForm says what they mean:
///          a level is the low byte when the high byte is `level_prefix`, a temperature is the main parameter decoded
///          over the range and rounded to tenths of a degree (a step of the scale is under a thousandth of a degree,
///          so 20 C would read 19.9998 unrounded) and clamped into the range, so a range end that is no whole tenth
///          stays a setpoint the range accepts.
/// @param reading A reading with a StatusMainParameterSource (DeviceReading::HEATING_LEVEL or STATUS_SETPOINT).
/// @param main_parameter The two bytes of the main parameter as the status reply carries them.
/// @param level_prefix High byte of a level or alias (HeatingProfile::mp_prefix, HEATING_LEVEL_MP_PREFIX).
/// @param range_min_c Lower end of the device's temperature encoding range (device_temperature_range()).
/// @param range_max_c Upper end of it.
/// @return The value (a level's low byte, degrees Celsius), or nullopt when `reading` is not a status reading, the
///         bytes are too few or the main parameter is not the kind of value the reading is.
std::optional<float> decode_status_reading(DeviceReading reading, std::span<const uint8_t> main_parameter,
                                           uint8_t level_prefix, float range_min_c, float range_max_c);

/// @brief Store the readings that are functional parameters of a status reply.
/// @details Each reading with a StatusFunctionalParameterSource whose parameter the reply carries
///          (decode_status_functional_parameter()) is stored; a reading whose parameter the reply does not carry keeps
///          what it held.
/// @param dev The device record.
/// @param reply A CMD_PRIVATE_RESP from the device.
/// @return The number of readings stored.
size_t store_status_parameter_readings(IoDevice &dev, const IoFrame &reply);

/// @brief What store_status_main_parameter() did.
struct StatusStore {
  std::optional<float> value;  ///< The value now stored; nullopt when the main parameter is none of this reading.
  bool changed{false};         ///< The stored value differs from what the reading held before.
};

/// @brief Decode a status reply's main parameter for a status reading and store it.
/// @details The range is the device's (device_temperature_range()). A main parameter that is none of this reading
///          stores nothing and leaves the reading as it was: the caller decides whether that clears it.
/// @param dev The device record.
/// @param reading A reading with a StatusMainParameterSource.
/// @param main_parameter The two bytes of the main parameter.
/// @param level_prefix High byte of a level or alias (decode_status_reading()).
/// @return The value stored and whether it changed.
StatusStore store_status_main_parameter(IoDevice &dev, DeviceReading reading, std::span<const uint8_t> main_parameter,
                                        uint8_t level_prefix);

/// @brief The registers the hub's poll reads so a reading has a value.
/// @param row The reading's row.
/// @return The register of a register reading, the registers a derived reading names, none for a kind with no
///         register (a data-object, event or limitation reading is read on its own schedule, a status reading
///         answers the status read).
///         The span points into static storage (the table).
std::span<const DeviceRegister> reading_polled_registers(const DeviceReadingRow &row);

/// @brief The register a reading is, when its source is a register.
/// @param reading The reading.
/// @return The register (what a writable setting writes; for a reading that is one part of a register, that register),
///         or nullopt for a reading of another source kind or COUNT.
std::optional<DeviceRegister> reading_register(DeviceReading reading);

/// @brief Look up the reading that is a register, for its read condition and its stored value.
/// @param fn A register.
/// @return The first row in table order whose source is that register (the register's own value, or its first part),
///         or nullptr when no reading is.
const DeviceReadingRow *find_register_reading(DeviceRegister fn);

/// @brief Is a reading read on a schedule of its own: a data object, an event or a limitation?
/// @param row The reading's row.
/// @return true for those three source kinds; the others are read by the heater poll or answered by a status reply.
[[nodiscard]] bool reading_is_scheduled(const DeviceReadingRow &row);

/// @brief Can a device have a reading: does it have the capability the row names, if it names one?
/// @details Unlike device_has_reading_capability(), a row with no capability passes: a cover's readings are asked for
///          by YAML keys and need no capability of the kind.
/// @param dev The device record (its resolved features).
/// @param row The reading's row.
/// @return false only when the row names a capability the device lacks.
[[nodiscard]] bool device_can_have_reading(const IoDevice &dev, const DeviceReadingRow &row);

/// @brief Does a device have the capability an entity needs to ask for a reading?
/// @param dev The device record (its resolved features).
/// @param row The reading's row.
/// @return true when the row names a capability and the device has it; false for a reading no entity asks for.
[[nodiscard]] bool device_has_reading_capability(const IoDevice &dev, const DeviceReadingRow &row);

/// @brief The value of a reading as a whole number: a mode, a state, a flag, a percentage, a level.
/// @param dev The device record.
/// @param reading The reading.
/// @return The value, or -1 when it has none (see reading_value()) or it is NaN.
int reading_wire(const IoDevice &dev, DeviceReading reading);

/// @brief The value a device holds for a register, whatever reading it is.
/// @param dev The device record.
/// @param fn The register.
/// @return The value, or nullopt when no reading is the register or it has no value (never read, cleared, absent).
std::optional<float> register_value(const IoDevice &dev, DeviceRegister fn);

/// @brief The value of a register read as a whole number: a mode, a state, a flag, a percentage.
/// @param dev The device record.
/// @param fn The register.
/// @return The value, or -1 when it has none (see register_value()).
int register_wire(const IoDevice &dev, DeviceRegister fn);

/// @brief register_wire() for a register a profile may not name.
/// @param dev The device record.
/// @param fn The register, or none.
/// @return The value, or -1 when it has none or there is no register.
int register_wire(const IoDevice &dev, std::optional<DeviceRegister> fn);

// === The store door ===

/// @brief Keep a value in a reading of a device; the reading is no longer absent and counts as reported.
/// @param dev The device record.
/// @param reading The reading.
/// @param value The value (NaN is a value: a sensor the device reports as faulty).
void store_reading(IoDevice &dev, DeviceReading reading, float value);

/// @brief Forget the value of a reading; it reads as never read again. Absent, asked and reported stay.
/// @param dev The device record.
/// @param reading The reading.
void clear_reading(IoDevice &dev, DeviceReading reading);

/// @brief Record that the device does not have a reading; a value held before is dropped.
/// @param dev The device record.
/// @param reading The reading.
void mark_reading_absent(IoDevice &dev, DeviceReading reading);

/// @param dev The device record.
/// @param reading The reading.
/// @return true once the device was found not to have the reading (until a value is stored).
[[nodiscard]] bool reading_absent(const IoDevice &dev, DeviceReading reading);

/// @param dev The device record.
/// @param reading The reading.
/// @return true once a value was stored for the reading in this boot, also when it was cleared since.
[[nodiscard]] bool reading_reported(const IoDevice &dev, DeviceReading reading);

/// @brief Declare that an entity wants a reading.
/// @param dev The device record.
/// @param reading The reading.
void ask_reading(IoDevice &dev, DeviceReading reading);

/// @param dev The device record.
/// @param reading The reading.
/// @return true while an entity wants the reading.
[[nodiscard]] bool reading_asked(const IoDevice &dev, DeviceReading reading);

/// @brief Keep what a device answered for a register, in every reading that is the register or a part of it.
/// @details Each reading decodes the value bytes by its part's codec, or by the register's codec without a part, and
///          keeps the result. A reading whose bytes the reply does not carry (a cut-off value, a byte the codec
///          refuses) keeps what it held.
/// @param dev The device record.
/// @param fn The register.
/// @param value_bytes The value bytes of the reply (register_reply_value()).
/// @return true when at least one reading now holds a value from this reply; false when none did, which is how a
///         register the device does not hold, or a value no reading takes, shows.
bool store_register_reply(IoDevice &dev, DeviceRegister fn, std::span<const uint8_t> value_bytes);

/// @brief Forget the value of every reading that is a register or a part of it; they read as never read again.
/// @param dev The device record.
/// @param fn The register.
/// @return true when a reading is the register.
bool clear_register_reading(IoDevice &dev, DeviceRegister fn);

/// @brief Declare that an entity wants a register read at every poll (every reading of it).
/// @param dev The device record.
/// @param fn The register.
void ask_register_reading(IoDevice &dev, DeviceRegister fn);

/// @brief Does the poll read a register for its entity: declared, and not found absent?
/// @param dev The device record.
/// @param fn The register.
/// @return true when an entity asked for the reading that is the register and the heater has not answered it empty.
bool register_reading_wanted(const IoDevice &dev, DeviceRegister fn);

/// @brief Record that the heater answered a read of a register without a value: the poll stops reading it until the
/// next boot or until another read (an action, a write's read-back) answers it with a value, and its value is dropped.
/// @param dev The device record.
/// @param fn The register.
void mark_register_reading_absent(IoDevice &dev, DeviceRegister fn);

/// @brief Is a register due for a read, given what the poll has stored so far?
/// @details A register with no condition, or no reading at all, is always due. One with a condition is due while
///          the condition's register holds the value; an unread condition register holds none.
/// @param fn The register.
/// @param dev The device record (the condition's register is read earlier in the poll).
/// @return true when the poll should read the register.
bool heating_register_due(DeviceRegister fn, const IoDevice &dev);

/// @brief Forget the readings that are read only while a register holds a value, after a write of that register.
/// @details A written temporary state starts or ends a timed run, so the minutes left read for an earlier run are not
///          the new run's; the next poll reads them again. A poll that reads the register changes nothing here: it
///          reads the conditional registers right after, and clears them itself while the condition does not hold.
/// @param written The register that was written.
/// @param dev The device record.
/// @return true when a reading held a value and was cleared.
bool clear_readings_read_only_while(DeviceRegister written, IoDevice &dev);

}  // namespace home_io_control
}  // namespace esphome
