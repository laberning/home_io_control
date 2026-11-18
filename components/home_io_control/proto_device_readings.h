#pragma once

/// @file proto_device_readings.h
/// @brief The named readings a device can show, the capability each needs and where each value comes from.
/// @ingroup hioc_protocol
///
/// A reading is what one entity shows: a heater's energy counter, its occupancy, the minutes left of a towel
/// dryer's run. One table (DEVICE_READINGS in the .cpp) lists every reading by name with the capability a device
/// needs for it (DeviceFeature, ADR 0098) and its source. The source says how the value is obtained, so the
/// kinds stay apart and a new kind leaves the rows of the others alone:
///  - HeatingRegisterSource: one private register of a heater, read by the hub's poll when an entity asks for it
///    and decoded with a ValueCodec (proto_value_codecs.h), optionally only while another register holds a value;
///  - DerivedSource: a value worked out from other readings (the minutes left of whichever run is active), with the
///    registers it needs polled;
///  - DataObjectSource: a slice of a device's data object, read on a schedule through a transfer recipe
///    (transfer_reader.h) and decoded with a ValueCodec; the value is kept in the device record
///    (DeviceReadingValues), since a read takes seconds and answers later than the poll that asked.
///
/// What the hub polls and what is shown stay separate: the registers the poll reads are derived from the rows
/// (reading_polled_registers()), and every entity reads through reading_value(), wherever the source keeps the
/// value. The YAML key and the Home Assistant metadata of a reading are not here: the codegen mirror
/// (hub_validators.py `DEVICE_READINGS`) carries them, checked against this table by name, capability and source
/// registers (tests/sync/device_features_sync_test.cpp).

#include "proto_device_features.h"
#include "proto_device_model.h"
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
  CENTRAL_HEATING_MODE,   ///< 1 while a mixed towel dryer runs on central heating (its element stays off).
  OCCUPANCY_DETECTION,    ///< The heater's occupancy detection setting; a writable setting.
  OCCUPANCY,              ///< Someone detected in the room.
  OPEN_WINDOW_DETECTION,  ///< The heater's open-window detection setting; a writable setting.
  ENERGY_TOTAL,           ///< The energy the heater has counted, whole kWh.
  BOOST_DURATION,         ///< The boost duration set on a towel dryer, minutes.
  DRYING_DURATION,        ///< The duration of the last drying run a towel dryer started, minutes.
  BOOST_REMAINING,        ///< Minutes left of a running boost; read only while a boost runs.
  DRYING_REMAINING,       ///< Minutes left of a running drying run; read only while it runs.
  REMAINING_TIME,         ///< Minutes left of whichever run is active, 0 with none (derived).
  COUNT,                  ///< Number of readings; not a reading.
};

static_assert(static_cast<size_t>(DeviceReading::COUNT) <= DeviceReadingValues::SLOTS,
              "every reading needs a slot in DeviceReadingValues; raise SLOTS");

/// @brief A reading another register must show for a register read to run.
struct HeatingReadCondition {
  HeatingFunction
      fn;         ///< The register whose stored reading decides; an integer register (HeatingReadings::value_of()).
  uint8_t value;  ///< The value `fn` must hold.
};

/// @brief A reading that is one register of a heater.
struct HeatingRegisterSource {
  HeatingFunction fn;  ///< The register; its codec is the heater register table's.
  std::optional<HeatingReadCondition> only_while =
      std::nullopt;  ///< When set, the register is read only while this holds (the minutes left of a run, only
                     ///< while that run is on).
};

/// @brief A reading worked out from other readings of the same device.
struct DerivedSource {
  std::optional<float> (*value)(const IoDevice &);  ///< The value now, or nullopt while it cannot be told.
  std::span<const HeatingFunction> polls;           ///< The registers the poll must read for `value` to know.
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

/// @brief Where a reading's value comes from. Later kinds (a one-frame read) are further alternatives; the rows of
/// the existing kinds do not change.
using ReadingSource = std::variant<HeatingRegisterSource, DerivedSource, DataObjectSource>;

/// @brief One row of the reading table.
struct DeviceReadingRow {
  const char *name;       ///< Lower-case name for logs; its upper-case form is the key of the codegen mirror.
  ReadingSource source;   ///< Where the value comes from.
  DeviceReading reading;  ///< The reading.
  DeviceFeature feature;  ///< The capability a device kind needs for the reading to be accepted and read.
};

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
/// @details A heater register reads from the device's HeatingReadings; a derived reading works from them.
///          Binary readings are 0 and 1.
/// @param dev The device record.
/// @param reading The reading.
/// @return The value, or nullopt while it has not been read, was dropped or cannot be told.
std::optional<float> reading_value(const IoDevice &dev, DeviceReading reading);

/// @brief reading_value() over a table of the caller's, so a test can give a data-object row that production does not
/// have yet.
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

/// @brief The registers the hub's poll reads so a reading has a value.
/// @param row The reading's row.
/// @return The register of a register reading, the registers a derived reading names, none for a kind with no
///         register (a data-object reading is read by the transfer reader instead). The span points into static storage
///         (the table).
std::span<const HeatingFunction> reading_polled_registers(const DeviceReadingRow &row);

/// @brief The register a reading is, when it is one register of a heater.
/// @param reading The reading.
/// @return The register (what a writable setting writes), or nullopt for a derived reading or COUNT.
std::optional<HeatingFunction> reading_register(DeviceReading reading);

/// @brief Look up the reading that is one register, for its read condition.
/// @param fn A register.
/// @return The row whose source is that register, or nullptr when no reading is.
const DeviceReadingRow *find_register_reading(HeatingFunction fn);

/// @brief Is a register due for a read, given what the poll has stored so far?
/// @details A register with no condition, or no reading at all, is always due. One with a condition is due while
///          the condition's register holds the value; an unread condition register (-1) holds none.
/// @param fn The register.
/// @param readings What the hub has read from the heater (the condition's register is read earlier in the poll).
/// @return true when the poll should read the register.
bool heating_register_due(HeatingFunction fn, const HeatingReadings &readings);

/// @brief Forget the readings that are read only while a register holds a value, after a write of that register.
/// @details A written temporary state starts or ends a timed run, so the minutes left read for an earlier run are not
///          the new run's; the next poll reads them again. A poll that reads the register changes nothing here: it
///          reads the conditional registers right after, and clears them itself while the condition does not hold.
/// @param written The register that was written.
/// @param readings What the hub has read from the heater.
/// @return true when a reading held a value and was cleared.
bool clear_readings_read_only_while(HeatingFunction written, HeatingReadings &readings);

}  // namespace home_io_control
}  // namespace esphome
