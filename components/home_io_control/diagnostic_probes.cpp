/// @file diagnostic_probes.cpp
/// @brief Diagnostic probes: the probe table and frame builders, probe_device(), probe_sweep() and the
///        data-transport read.
/// @ingroup hioc_hub

#include "diagnostic_probes.h"

#include "entity_helpers.h"
#include "log_helpers.h"
#include "proto_commands.h"
#include "proto_device_types.h"
#include "proto_heating_profiles.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace esphome {
namespace home_io_control {

namespace {

// --- Diagnostic probe names (probe_device()/probe_sweep() `probe` argument) ---
constexpr const char *PROBE_NAME_PRIVATE_FN = "private_fn";          ///< Q0: create_private_function().
constexpr const char *PROBE_NAME_PRIVATE_FN_SUB = "private_fn_sub";  ///< CMD_PRIVATE fn 0x09, chosen second byte.
constexpr const char *PROBE_NAME_STATUS_EXT = "status_ext";          ///< Q1: create_get_status_extended().
constexpr const char *PROBE_NAME_STATUS_EXT_FN6 = "status_ext_fn6";  ///< Extended CMD_PRIVATE at function ID 0x06.
constexpr const char *PROBE_NAME_STATUS_EXT_FN9 = "status_ext_fn9";  ///< Extended CMD_PRIVATE at function ID 0x09.
constexpr const char *PROBE_NAME_GET_INFO1 = "get_info1";            ///< create_get_info1() (0x54, no payload).
constexpr const char *PROBE_NAME_GET_INFO2 = "get_info2";            ///< create_get_info2() (0x56, no payload).
constexpr const char *PROBE_NAME_GENERAL_INFO3 = "general_info3";    ///< Q2: create_general_info3().
constexpr const char *PROBE_NAME_PRIVATE2 = "private2";              ///< Q3 long form: create_private2_read().
constexpr const char *PROBE_NAME_PRIVATE2_SHORT = "private2_short";  ///< Q3 short form: create_private2_read().
constexpr const char *PROBE_NAME_STATUS_MP_FP = "status_mp_fp";      ///< create_status_mp_fp_read() (no index).
constexpr const char *PROBE_NAME_STATUS_FP_HIGH =
    "status_fp_high";  ///< create_status_mp_fp_read() with FPI1 0x00 and the index as the FPI2 bitmap.
constexpr const char *PROBE_NAME_LIMITATION =
    "limitation";  ///< create_limitation_status_read(); index 0 = minimum, 1 = maximum.
/// Highest `limitation` index: the two limit types of the KLF 200 API's get-limitation command
/// (LimitationType numbering). The index is never the on-air selector byte.
constexpr uint8_t PROBE_LIMITATION_MAX_INDEX = 1;
constexpr uint8_t PROBE_LIMITATION_INDEX_MINIMUM = 0;  ///< `limitation` index that reads the resulting minimum.
constexpr const char *PROBE_NAME_SUBSCRIBE_EVENT =
    "subscribe_event";  ///< create_subscribe_event(); index 0 = battery level event, 1 = local sensor event.
constexpr const char *PROBE_NAME_READ_OBJECT =
    "read_object";  ///< Data-transport read of one device object (ADR 0062); index = TRANSFER_READABLE_OBJECTS row.
constexpr const char *PROBE_NAME_REGISTER_00 =
    "register_00";  ///< Read of register 0x00<index> over CMD_WRITE_PRIVATE (read direction only).
constexpr const char *PROBE_NAME_REGISTER_01 =
    "register_01";  ///< Read of register 0x01<index> over CMD_WRITE_PRIVATE (read direction only).
/// Bytes of a register request that a `0x21` answer repeats: `0C 60 <hi> <lo>`.
constexpr uint8_t PROBE_REGISTER_HEADER_LENGTH = 4;
constexpr uint8_t PROBE_REGISTER_HIGH_00 = 0x00;  ///< High register byte of `register_00`; family untested.
constexpr uint8_t PROBE_REGISTER_HIGH_01 =
    0x01;  ///< High register byte of `register_01`; the family climate support reads.
/// Highest `read_object` index: the last row of TRANSFER_READABLE_OBJECTS.
constexpr uint8_t PROBE_READ_OBJECT_MAX_INDEX = TRANSFER_READABLE_OBJECT_COUNT - 1;
/// Pause before each block request and before the close. An RS100 Solar io missed every block request sent
/// right after its previous answer and answered every re-send, which left about 650 ms after that answer
/// (issue #156). Waiting that long first saves one long-preamble transmission per block.
constexpr uint32_t PROBE_READ_OBJECT_BLOCK_GAP_MS = 650;
/// Longest object `read_object` pulls: 24 blocks (432 bytes), the smallest whole-block cap that covers the
/// longest length a real device has announced (431 bytes, issue #156). A device announcing more is closed again
/// unread, so a misreading cannot turn into a long run of transmissions on the shared band. The cost: on that
/// low-power device a 14-block read blocked the loop for 16.4 s, so 24 blocks take about 28 s nominally and, with
/// up to 3 tries per block, can pass 60 s. The ESPHome API tolerates that (its server only drops a client after
/// 150 s without traffic), and the probe is manual, one object per call.
constexpr uint16_t PROBE_READ_OBJECT_MAX_BYTES = 24 * TRANSFER_BLOCK_SIZE;
/// Data bytes per log line when `read_object` prints what it read (short enough for the log's line limit).
constexpr uint8_t PROBE_READ_OBJECT_LOG_BYTES_PER_LINE = 24;
/// "object 0xNNNN class NNN handle 0xNN status NNN length NNNNN" plus the terminator.
constexpr size_t PROBE_READ_OBJECT_HEADER_BUFFER_SIZE = 72;
/// "event 0xNNNN, NNN data bytes" plus the terminator.
constexpr size_t PROBE_EVENT_HEADER_BUFFER_SIZE = 40;
/// Highest `subscribe_event` index: one per allowlisted event id. The index is never the on-air event id.
constexpr uint8_t PROBE_SUBSCRIBE_EVENT_MAX_INDEX = 1;
constexpr uint8_t PROBE_SUBSCRIBE_EVENT_INDEX_BATTERY = 0;  ///< `subscribe_event` index for EVENT_BATTERY_LEVEL_CHANGE.
constexpr const char *PROBE_NAME_UNSUBSCRIBE_EVENT =
    "unsubscribe_event";  ///< create_unsubscribe_event(); same index mapping as `subscribe_event`.
/// Function IDs the extended-shape probes hold fixed. Production software elsewhere describes
/// these two as a battery read; on real hardware (17 solar devices plus our own mains motors)
/// the *short* 3-byte form at these IDs returned position-family values, never a charge value.
/// The extended 4-byte shape at these IDs has never been sent by anything.
constexpr uint8_t PROBE_EXT_FUNCTION_ID_06 = 0x06;
constexpr uint8_t PROBE_EXT_FUNCTION_ID_09 = 0x09;
/// Function ID `private_fn_sub` holds fixed while `index` walks the second payload byte.
constexpr uint8_t PROBE_PRIVATE_FN_SUB_FUNCTION_ID = 0x09;
/// Extended-CMD_PRIVATE selector "status_ext" probes: the field-observed, never-decoded 0x80.
/// Selector 0x20 (tilt) already has a permanent builder/action (create_get_status_tilt()) and is
/// not part of this probe.
constexpr uint8_t PROBE_STATUS_EXT_SELECTOR = 0x80;
/// Bound on probe_sweep()'s index range — this transmits on a shared ISM band to what may be a
/// battery device; an unbounded sweep is antisocial and would keep the device awake needlessly.
constexpr uint8_t PROBE_SWEEP_MAX_INDICES = 16;
/// Spacing between probe_sweep() indices. Sweep steps are sequential and blocking, so they never
/// overlap regardless of this value; it exists for ISM-band duty cycling and to give a battery
/// device real idle time between reads.
constexpr uint32_t PROBE_SWEEP_DELAY_MS = 1000;

/// @brief Uniform wrapper signature every probe builder is adapted to, so PROBE_TABLE below can
/// hold one function-pointer type regardless of each create_*() builder's own parameter list.
/// `low_power` is the target device's YAML-declared power class, forwarded to each builder so a
/// probe to an always-alive device is shaped like one — the diagnostic subsystem has to work on
/// the very device class a silent-device investigation reaches for it.
using ProbeBuilderFn = bool (*)(IoFrame &, const uint8_t *, const uint8_t *, uint8_t index, bool low_power);

bool build_probe_private_fn(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index, bool low_power) {
  return create_private_function(f, own, dst, low_power, index);
}
bool build_probe_private_fn_sub(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index, bool low_power) {
  return create_private_function(f, own, dst, low_power, PROBE_PRIVATE_FN_SUB_FUNCTION_ID, index);
}
bool build_probe_status_ext(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index, bool low_power) {
  return create_get_status_extended(f, own, dst, low_power, PROBE_STATUS_EXT_SELECTOR, index);
}
bool build_probe_status_ext_fn6(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index, bool low_power) {
  return create_get_status_extended(f, own, dst, low_power, PROBE_STATUS_EXT_SELECTOR, index, PROBE_EXT_FUNCTION_ID_06);
}
bool build_probe_status_ext_fn9(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index, bool low_power) {
  return create_get_status_extended(f, own, dst, low_power, PROBE_STATUS_EXT_SELECTOR, index, PROBE_EXT_FUNCTION_ID_09);
}
bool build_probe_get_info1(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t /*index*/, bool low_power) {
  return create_get_info1(f, own, dst, low_power);
}
bool build_probe_get_info2(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t /*index*/, bool low_power) {
  return create_get_info2(f, own, dst, low_power);
}
bool build_probe_general_info3(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t /*index*/, bool low_power) {
  return create_general_info3(f, own, dst, low_power);
}
bool build_probe_private2_long(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index, bool low_power) {
  return create_private2_read(f, own, dst, index, /*long_form=*/true, low_power);
}
bool build_probe_private2_short(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index, bool low_power) {
  return create_private2_read(f, own, dst, index, /*long_form=*/false, low_power);
}
bool build_probe_status_mp_fp(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t /*index*/, bool low_power) {
  return create_status_mp_fp_read(f, own, dst, low_power);
}
/// `status_fp_high`: main parameter plus the FP9-FP16 selected by the index bitmap. The builder itself
/// refuses an empty or oversized selection, so the probe stays safe without probe_device()'s index_valid gate.
bool build_probe_status_fp_high(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index, bool low_power) {
  return create_status_mp_fp_read(f, own, dst, low_power, /*fpi1=*/0x00, /*fpi2=*/index);
}

/// True when `bitmap` selects between 1 and STATUS_MP_FP_PARAM_COUNT parameters.
bool status_fp_high_bitmap_valid(uint8_t bitmap) {
  uint8_t bits = 0;
  for (uint8_t v = bitmap; v != 0; v >>= 1)
    bits += v & 1U;
  return bits >= 1 && bits <= STATUS_MP_FP_PARAM_COUNT;
}

bool build_probe_limitation(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index, bool low_power) {
  if (index > PROBE_LIMITATION_MAX_INDEX) {
    return false;  // Unreachable through probe_device()/probe_sweep(), which gate on max_index.
  }
  const LimitationType type =
      index == PROBE_LIMITATION_INDEX_MINIMUM ? LimitationType::MINIMUM : LimitationType::MAXIMUM;
  return create_limitation_status_read(f, own, dst, type, low_power);
}

/// Maps the `subscribe_event` index onto an allowlisted event id; the id itself is checked again by
/// create_subscribe_event().
bool build_probe_subscribe_event(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index, bool low_power) {
  if (index > PROBE_SUBSCRIBE_EVENT_MAX_INDEX)
    return false;  // Unreachable through probe_device(), which gates on max_index.
  const uint16_t event = index == PROBE_SUBSCRIBE_EVENT_INDEX_BATTERY ? EVENT_BATTERY_LEVEL_CHANGE : EVENT_LOCAL_SENSOR;
  return create_subscribe_event(f, own, dst, event, low_power);
}

/// The `subscribe_event` index mapping, for the unsubscribe request; create_unsubscribe_event()
/// checks the id again.
bool build_probe_unsubscribe_event(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index, bool low_power) {
  if (index > PROBE_SUBSCRIBE_EVENT_MAX_INDEX)
    return false;  // Unreachable through probe_device(), which gates on max_index.
  const uint16_t event = index == PROBE_SUBSCRIBE_EVENT_INDEX_BATTERY ? EVENT_BATTERY_LEVEL_CHANGE : EVENT_LOCAL_SENSOR;
  return create_unsubscribe_event(f, own, dst, event, low_power);
}

/// Maps the `read_object` index onto its TRANSFER_READABLE_OBJECTS row; create_transfer_open() checks the
/// object against the table again.
bool build_probe_read_object(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index, bool low_power) {
  if (index > PROBE_READ_OBJECT_MAX_INDEX)
    return false;  // Unreachable through probe_device(), which gates on max_index.
  return create_transfer_open(f, own, dst, TRANSFER_READABLE_OBJECTS[index], low_power);
}

/// Shared body of the register probes: frames a read of register `<reg_hi><index>`. The payload comes from
/// encode_heating_register_read(), which cannot produce the write direction, so no probe input can put a
/// register write on the air (ADR 0063).
bool build_probe_register_read(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t reg_hi, uint8_t index,
                               bool low_power) {
  uint8_t payload[HEATING_PAYLOAD_MAX_SIZE];
  const size_t payload_len = encode_heating_register_read(reg_hi, index, payload);
  return create_write_private(f, own, dst, low_power, payload, payload_len);
}

bool build_probe_register_00(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index, bool low_power) {
  return build_probe_register_read(f, own, dst, PROBE_REGISTER_HIGH_00, index, low_power);
}

bool build_probe_register_01(IoFrame &f, const uint8_t *own, const uint8_t *dst, uint8_t index, bool low_power) {
  return build_probe_register_read(f, own, dst, PROBE_REGISTER_HIGH_01, index, low_power);
}

/// @brief One row per probe_device()/probe_sweep() `probe` argument value.
struct ProbeDescriptor {
  const char *name;
  ProbeBuilderFn builder;
  const char *index_help;  ///< Wording for the rejection when an index exceeds max_index (null: unbounded).
  bool needs_index;   ///< False for the probes whose builders take no index/selector ("general_info3", "get_info1",
                      ///< "get_info2", "status_mp_fp").
  uint8_t max_index;  ///< Highest index the probe accepts; the full byte range when index_help is null.
  bool sweepable;     ///< False for a probe that changes state in the device, or whose index is a bitmap rather than
                      ///< an ordinal: probe_sweep() refuses it.
  bool reads_blocks{false};  ///< True for `read_object`: an accepted first reply is followed by block reads.
  /// True for the register probes: refused unless device_accepts_register_probe() holds for the target.
  bool register_read{false};
  /// Optional check of the parsed index beyond max_index (a bitmap index); false is reported with
  /// index_out_of_range_message().
  bool (*index_valid)(uint8_t){nullptr};
};

/// @brief The full set of probes probe_device()/probe_sweep() can dispatch to.
///
/// Single source of truth for probe names. Adding, removing, or renaming a probe is a one-line
/// change here rather than a change to both probe_device()'s dispatch and its "index must be..."
/// error message. Deliberately has no "unknown4a" row -- see ADR 0024.
constexpr ProbeDescriptor PROBE_TABLE[] = {
    {PROBE_NAME_PRIVATE_FN, build_probe_private_fn, nullptr, true, UINT8_MAX, true},
    {PROBE_NAME_PRIVATE_FN_SUB, build_probe_private_fn_sub, nullptr, true, UINT8_MAX, true},
    {PROBE_NAME_STATUS_EXT, build_probe_status_ext, nullptr, true, UINT8_MAX, true},
    {PROBE_NAME_STATUS_EXT_FN6, build_probe_status_ext_fn6, nullptr, true, UINT8_MAX, true},
    {PROBE_NAME_STATUS_EXT_FN9, build_probe_status_ext_fn9, nullptr, true, UINT8_MAX, true},
    {PROBE_NAME_GET_INFO1, build_probe_get_info1, nullptr, false, UINT8_MAX, true},
    {PROBE_NAME_GET_INFO2, build_probe_get_info2, nullptr, false, UINT8_MAX, true},
    {PROBE_NAME_GENERAL_INFO3, build_probe_general_info3, nullptr, false, UINT8_MAX, true},
    {PROBE_NAME_PRIVATE2, build_probe_private2_long, nullptr, true, UINT8_MAX, true},
    {PROBE_NAME_PRIVATE2_SHORT, build_probe_private2_short, nullptr, true, UINT8_MAX, true},
    {PROBE_NAME_STATUS_MP_FP, build_probe_status_mp_fp, nullptr, false, UINT8_MAX, true},
    // The FP9-FP16 twin of status_mp_fp: the index is the FPI2 bitmap, not an ordinal, so it is not swept.
    {PROBE_NAME_STATUS_FP_HIGH, build_probe_status_fp_high,
     "FP9-FP16 bitmap, bit 7 = FP9 ... bit 0 = FP16, at most 7 bits (e.g. 0x43 = FP10, FP15, FP16)", true, UINT8_MAX,
     false, false, false, status_fp_high_bitmap_valid},
    {PROBE_NAME_LIMITATION, build_probe_limitation, "index 0 (minimum) or 1 (maximum)", true,
     PROBE_LIMITATION_MAX_INDEX, true},
    // The two probes that change device state (ADR 0060): one stores a subscription, the other is an
    // untested guess at removing one. Hence the two-event allowlist behind the index, and no sweeping.
    {PROBE_NAME_SUBSCRIBE_EVENT, build_probe_subscribe_event,
     "index 0 (battery level event 0x2001) or 1 (local sensor event 0x2005)", true, PROBE_SUBSCRIBE_EVENT_MAX_INDEX,
     false},
    {PROBE_NAME_UNSUBSCRIBE_EVENT, build_probe_unsubscribe_event,
     "index 0 (battery level event 0x2001) or 1 (local sensor event 0x2005)", true, PROBE_SUBSCRIBE_EVENT_MAX_INDEX,
     false},
    // The data-transport read (ADR 0062): a multi-frame session, so it is not swept; it changes nothing in
    // the device but a transfer handle that the probe closes again.
    {PROBE_NAME_READ_OBJECT, build_probe_read_object,
     "index 0 (actuator capacity 0x8100), 1 (actuator state 0x8103), 2 (name record 0x8003), 3 (general info 1, "
     "0x8001), 4 (general info 2, 0x8002), or 5 to 29 (further objects, see the diagnostic probes documentation)",
     true, PROBE_READ_OBJECT_MAX_INDEX, false, true},
    // The register reads (ADR 0063): one CMD_WRITE_PRIVATE frame in the read direction, for register-protocol
    // heaters and domestic hot water devices only. Read-only, so they sweep like the other single-frame probes.
    {PROBE_NAME_REGISTER_00, build_probe_register_00, nullptr, true, UINT8_MAX, true, false, true},
    {PROBE_NAME_REGISTER_01, build_probe_register_01, nullptr, true, UINT8_MAX, true, false, true},
};
constexpr uint8_t PROBE_TABLE_SIZE = sizeof(PROBE_TABLE) / sizeof(PROBE_TABLE[0]);

/// @brief Look up a probe by name.
/// @return Pointer into PROBE_TABLE, or nullptr if `probe` names none of its rows.
const ProbeDescriptor *find_probe_descriptor(const std::string &probe) {
  for (const auto &descriptor : PROBE_TABLE) {
    if (probe == descriptor.name)
      return &descriptor;
  }
  return nullptr;
}

/// @brief The error message probe_device()/probe_sweep() report for an unrecognized `probe`
/// argument, built from PROBE_TABLE so the list of names can never drift out of sync with it.
std::string unknown_probe_message(const std::string &probe) {
  std::string names;
  for (uint8_t i = 0; i < PROBE_TABLE_SIZE; i++) {
    if (i > 0)
      names += (i + 1 == PROBE_TABLE_SIZE) ? ", or " : ", ";
    names += PROBE_TABLE[i].name;
  }
  return "unknown probe \"" + probe + "\" (expected " + names + ")";
}

/// @brief The error message for an index above a probe's max_index (probe_device() and probe_sweep()).
std::string index_out_of_range_message(const ProbeDescriptor &descriptor) {
  return "probe \"" + std::string(descriptor.name) + "\" takes " + descriptor.index_help;
}

/// @brief True when `index` is within the probe's max_index and passes its optional bitmap check.
bool probe_index_in_range(const ProbeDescriptor &descriptor, uint8_t index) {
  if (index > descriptor.max_index)
    return false;
  return descriptor.index_valid == nullptr || descriptor.index_valid(index);
}

/// @brief Shared probe-name lookup for probe_device()/probe_sweep(): returns the descriptor, or
/// nullptr after writing the "unknown probe" message into @p result. The device-resolution and
/// index-parse steps stay per-method: probe_device keeps the resolved device (for the moving
/// check and node_id), probe_sweep discards it, and one parses a single index while the other
/// parses a first/last range.
const ProbeDescriptor *resolve_probe_descriptor(const std::string &probe, ManagementActionResult &result) {
  const ProbeDescriptor *descriptor = find_probe_descriptor(probe);
  if (descriptor == nullptr)
    result.message = unknown_probe_message(probe);
  return descriptor;
}

/// Space-separated hex of data[first, end).
std::string hex_range(const uint8_t *data, uint16_t first, uint16_t end) {
  std::string out;
  for (uint16_t i = first; i < end; i++) {
    if (i > first)
      out += ' ';
    out += format_hex_byte(data[i]);
  }
  return out;
}

/// @brief Parse a probe_device()/probe_sweep() index argument into a byte.
///
/// Accepts both a bare decimal string ("6") and a "0x"-prefixed hex string ("0x06") — the two
/// shapes a Home Assistant user is likely to type when copying a value out of a captured frame
/// dump. Rejects anything else (empty string, trailing garbage, out-of-range value) rather than
/// silently defaulting to 0, since a wrong index silently sent as 0 would misrepresent what was
/// actually probed.
/// @param text Argument as received from the native API call.
/// @param out Parsed byte on success; left untouched on failure.
/// @return true if `text` is exactly one well-formed byte value.
bool parse_probe_index(const std::string &text, uint8_t &out) {
  if (text.empty())
    return false;
  // A leading "0" followed by another digit (e.g. "010") is valid octal to strtoul() below and
  // would silently probe a different index than a user copying a byte value out of a hex dump
  // intended -- reject it explicitly rather than relying on strtoul()'s octal parsing, which only
  // rejects the invalid-octal-digit cases ("08", "09"), not the valid ones.
  if (text.size() > 1 && text[0] == '0' && text[1] != 'x' && text[1] != 'X')
    return false;
  errno = 0;
  // NOLINTNEXTLINE(misc-const-correctness): strtoul()/strtof() take a `char **` end pointer.
  char *end = nullptr;
  // Base 0: strtoul() itself recognizes a "0x"/"0X" prefix as hex and a bare "0" as decimal
  // zero, which is exactly the "6" / "0x06" / "0" shape probes need to accept.
  const uint32_t value = std::strtoul(text.c_str(), &end, 0);
  if (end != text.c_str() + text.size())
    return false;
  if (errno == ERANGE || value > std::numeric_limits<uint8_t>::max())
    return false;
  out = static_cast<uint8_t>(value);
  return true;
}

/// A register read means something only to a register-protocol device; a cover, light or lock has no such register
/// and is not sent the frame. By kind, not by name: IoDevice has no manufacturer. Terminal, because the kind does
/// not change partway through a sweep.
bool register_probe_refused(const ProbeDescriptor &descriptor, const IoDevice &dev, const std::string &probe,
                            ManagementActionResult &result) {
  if (!descriptor.register_read || device_accepts_register_probe(dev.type, dev.subtype))
    return false;
  result.message = "probe \"" + probe +
                   "\" reads registers and only applies to a register-protocol heater or a domestic hot water "
                   "device; this device is neither";
  result.terminal_refusal = true;
  return true;
}

/// Reports what a register probe drew back (nothing for any other probe). A `0x21` that repeats the request's `0C 60
/// <hi> <lo>` header splits the value off it into `result.decoded` and the message; the bytes are not interpreted,
/// because what a register holds is what the probe exists to find out. Always logs one line: device, register, outcome.
void report_register_reply(const ProbeDescriptor &descriptor, const IoFrame &request, const IoFrame &response,
                           const std::string &probe, ManagementActionResult &result) {
  if (!descriptor.register_read)
    return;
  const bool echoed = response.cmd == CMD_WRITE_PRIVATE_ACK && response.data_len >= PROBE_REGISTER_HEADER_LENGTH &&
                      std::memcmp(response.data, request.data, PROBE_REGISTER_HEADER_LENGTH) == 0;
  if (echoed) {
    std::string value;
    for (uint8_t i = PROBE_REGISTER_HEADER_LENGTH; i < response.data_len; i++)
      value += (i == PROBE_REGISTER_HEADER_LENGTH ? "" : " ") + format_hex_byte(response.data[i]);
    result.decoded = "value: " + value;
    result.message += " [" + result.decoded + "]";
  }
  ESP_LOGI(detail::TAG, "%s register 0x%02X%02X on %s: %s", probe.c_str(), request.data[2], request.data[3],
           result.device_id.c_str(), echoed ? result.decoded.c_str() : command_name(response.cmd));
}

}  // namespace

DiagnosticProbes::DiagnosticProbes(const uint8_t *node_id, ExchangeEngine &engine, DeviceRegistry &registry,
                                   const bool *initialized, DiagnosticProbesHub hub)
    : node_id_(node_id), engine_(engine), registry_(registry), initialized_(initialized), hub_(std::move(hub)) {}

ManagementActionResult DiagnosticProbes::probe_device(const std::string &device_id, const std::string &probe,
                                                      const std::string &index) {
  // resolve_management_device() reassigns its `result` argument wholesale (via make_management_result()),
  // so it must write into its own object rather than the one already carrying probe_name/index --
  // matches probe_sweep()'s resolve_result pattern below.
  ManagementActionResult resolve_result;
  auto *dev =
      resolve_management_device(MANAGEMENT_ACTION_PROBE_DEVICE, device_id, *initialized_, registry_, resolve_result);
  if (dev == nullptr) {
    resolve_result.probe_name = probe;
    resolve_result.probe_index = index;
    return resolve_result;
  }

  ManagementActionResult result = resolve_result;
  result.probe_name = probe;
  result.probe_index = index;

  if (!hub_.probes_enabled()) {
    result.message = "diagnostic probes are not enabled; set diagnostic_probes: true in YAML";
    result.terminal_refusal = true;
    return result;
  }
  // An unknown frame into a mid-transaction device state machine is the one avoidable way a
  // read-shaped probe could cause harm.
  if (!effective_is_stopped(*dev)) {
    result.message = "device is moving; refusing to probe mid-transaction";
    result.terminal_refusal = true;
    return result;
  }

  const ProbeDescriptor *descriptor = resolve_probe_descriptor(probe, result);
  if (descriptor == nullptr)
    return result;

  if (register_probe_refused(*descriptor, *dev, probe, result))
    return result;

  uint8_t index_byte = 0;
  if (descriptor->needs_index && !parse_probe_index(index, index_byte)) {
    result.message = "index must be a decimal or 0x-prefixed byte value (0-255)";
    return result;
  }
  if (descriptor->needs_index && !probe_index_in_range(*descriptor, index_byte)) {
    result.message = index_out_of_range_message(*descriptor);
    return result;
  }

  IoFrame request;
  if (!descriptor->builder(request, node_id_, dev->node_id, index_byte, dev->low_power)) {
    result.message = "failed to build probe request";
    return result;
  }
  if (request.cmd == CMD_SUBSCRIBE_EVENT && request.data_len == 3) {
    ESP_LOGI(detail::TAG, "%s %s %s event 0x%02X%02X (diagnostic)",
             request.data[0] == EVENT_SUBSCRIPTION_UNSUBSCRIBE ? "Unsubscribing" : "Subscribing",
             result.device_id.c_str(), request.data[0] == EVENT_SUBSCRIPTION_UNSUBSCRIBE ? "from" : "to",
             request.data[1], request.data[2]);
  }

  IoFrame response;
  // A probe exists to read back a payload, so -- like a status poll or name read, and unlike a
  // bare CMD_EXECUTE -- SUCCESS_UNCONFIRMED (device accepted the request but sent nothing back)
  // does not satisfy the caller here.
  const ExchangeOutcome outcome = engine_.send_and_receive(request, response, FREQ_CH2);
  if (outcome != ExchangeOutcome::SUCCESS_WITH_RESPONSE) {
    engine_.log_debug(result.device_id.c_str());
    result.message = outcome == ExchangeOutcome::SUCCESS_UNCONFIRMED
                         ? "device accepted the probe request but sent no response"
                         : "no reply after " + std::to_string(EXCHANGE_RETRY_COUNT) +
                               " attempts (device asleep, unreachable, or silently ignoring this opcode)";
    return result;
  }

  // Report the raw reply, not an interpretation -- the whole point of a probe is that we do not
  // know what these bytes mean yet. This never touches a device record's status: DiagnosticProbes
  // holds no hub pointer, only the three DiagnosticProbesHub calls, so a probe reply can never be
  // misread as a position update.
  uint8_t raw[FRAME_MAX_SIZE] = {0};
  const uint8_t raw_len = serialize(response, raw, sizeof(raw));
  char hex[FRAME_LOG_HEX_BUFFER_SIZE];
  render_frame_hex_redacted(raw, raw_len, hex, sizeof(hex));

  // Log through the same "io_capture" structured tag every other received frame uses
  // (log_component_capture(), log_helpers.h) rather than relying on IOHOME_FRAME_LOG -- that
  // build flag is opt-in (only set in the loopback/monitor configs under config/), and the
  // always-on receive path (process_received_packet_(), gated on !busy_) never sees a probe
  // reply at all, since the reply is consumed here, inside a blocking send_and_receive() call,
  // while busy_ is still true. Without this call a probe reply would only appear in the action's
  // hex message/event on a default build -- not a form scripts/corpus/ingest.py parses -- despite
  // the reply already having gone out over an authenticated, radio-verified exchange.
  detail::log_component_capture(hub_.radio(), "probe_rx", raw, raw_len, &response);

  result.success = true;
  result.has_response_cmd = true;
  result.response_cmd = response.cmd;
  result.response_hex = hex;
  result.message = "probe \"" + probe + "\" reply cmd=0x" + format_hex_byte(response.cmd) + " (" +
                   command_name(response.cmd) + ") hex=" + hex;
  // The status-reply table is a decoded part of the protocol, unlike the probe payload itself --
  // unlike the payload bytes, a result code is not something the probe exists to discover.
  if (response.cmd == CMD_ERROR_RESP && apply_error_response(response, result)) {
    result.message += " [" + std::string(command_result_name(result.result_code)) + ": " +
                      command_result_description(result.result_code) + "]";
  }
  // A device event report (the answer a `subscribe_event` probe draws) is the first report of the
  // subscription: it goes on the "Last Device Event" sensor next to the later ones, and its header is
  // named. The event data stays raw: it is what the probe exists to collect.
  StatusUpdateEvent event;
  if (hub_.record_reply_event(response) && decode_status_update_event(response, event)) {
    char header[PROBE_EVENT_HEADER_BUFFER_SIZE];
    snprintf(header, sizeof(header), "event 0x%04X, %u data bytes", static_cast<unsigned>(event.event_id),
             static_cast<unsigned>(event.data_len));
    result.decoded = header;
    result.message += " [" + result.decoded + "]";
  }
  report_register_reply(*descriptor, request, response, probe, result);
  // The data-transport read continues past its first answer; everything it reports is raw bytes.
  if (descriptor->reads_blocks)
    read_object_blocks_(*dev, response, TRANSFER_READABLE_OBJECTS[index_byte], result);
  // The decode depends on the reply, not on which probe drew it, so any probe that happens to
  // draw a limitation reply reports it. Rendered, never applied: the device record is untouched.
  if (response.cmd == CMD_LIMITATION_STATUS_RESP) {
    result.decoded = detail::describe_limitation_reply(response);
    result.message += " [" + result.decoded + "]";
  }
  return result;
}

void DiagnosticProbes::read_object_blocks_(const IoDevice &dev, const IoFrame &open_reply, const TransferObject &object,
                                           ManagementActionResult &result) {
  const uint16_t object_id = object.id;
  // Anything but an open answer (an error frame, say) is already reported by the caller.
  if (open_reply.cmd != CMD_TRANSFER_OPEN_RESP)
    return;
  TransferOpenReply open;
  if (!decode_transfer_open_reply(open_reply, open)) {
    result.message += " [not a well-formed transfer-open answer]";
    return;
  }
  char head[PROBE_READ_OBJECT_HEADER_BUFFER_SIZE];
  snprintf(head, sizeof(head), "object 0x%04X class %u handle 0x%02X status %u length %u",
           static_cast<unsigned>(object_id), static_cast<unsigned>(object.object_class),
           static_cast<unsigned>(open.handle), static_cast<unsigned>(open.status), static_cast<unsigned>(open.length));
  result.decoded = head;
  result.message += " [" + result.decoded + "]";
  if (open.status == TRANSFER_STATUS_NOTHING_TO_READ && open.length == 0) {
    result.message += " [object is empty (status 7)]";
    return;
  }
  if (!open.ok()) {
    result.message += " [device refused the read]";
    return;
  }
  if (open.length == 0) {
    result.message += " [object is empty]";
    return;
  }

  // Close the transfer on every path past this point: the device holds a handle until told otherwise.
  // Every block request and the close wait PROBE_READ_OBJECT_BLOCK_GAP_MS after the device's last answer.
  const auto pause_before_request = []() {
    App.feed_wdt();
    delay(PROBE_READ_OBJECT_BLOCK_GAP_MS);
  };
  const auto close_transfer = [&]() {
    IoFrame close_request;
    IoFrame close_reply;
    pause_before_request();
    if (create_transfer_read(close_request, node_id_, dev.node_id, open.handle, TRANSFER_CHUNK_CLOSE, dev.low_power))
      engine_.send_and_receive(close_request, close_reply, FREQ_CH2);
  };
  if (open.length > PROBE_READ_OBJECT_MAX_BYTES) {
    close_transfer();
    result.message += " [longer than " + std::to_string(PROBE_READ_OBJECT_MAX_BYTES) + " bytes, closed unread]";
    return;
  }

  uint8_t data[PROBE_READ_OBJECT_MAX_BYTES] = {0};
  uint16_t have = 0;
  const uint16_t chunks = transfer_chunk_count(open.length);
  bool complete = true;
  for (uint16_t chunk = chunks; chunk >= 1; chunk--) {
    IoFrame request;
    IoFrame reply;
    pause_before_request();
    if (!create_transfer_read(request, node_id_, dev.node_id, open.handle, chunk, dev.low_power) ||
        engine_.send_and_receive(request, reply, FREQ_CH2) != ExchangeOutcome::SUCCESS_WITH_RESPONSE) {
      complete = false;
      break;
    }
    const uint8_t *block = nullptr;
    uint8_t block_len = 0;
    // The blocks arrive counting down but their data runs forward; a block that echoes another chunk
    // is a late answer and ends the read rather than being spliced in.
    if (!decode_transfer_block(reply, open.handle, chunk, block, block_len) ||
        have + block_len > PROBE_READ_OBJECT_MAX_BYTES) {
      complete = false;
      break;
    }
    std::memcpy(data + have, block, block_len);
    have = static_cast<uint16_t>(have + block_len);
  }
  close_transfer();

  const std::string hex = hex_range(data, 0, have);
  result.response_hex = hex;
  ESP_LOGI(detail::TAG, "read_object %s object 0x%04X class %u: %u of %u bytes%s", result.device_id.c_str(),
           static_cast<unsigned>(object_id), static_cast<unsigned>(object.object_class), static_cast<unsigned>(have),
           static_cast<unsigned>(open.length), complete ? "" : " (incomplete)");
  // One log line per few bytes: the logger truncates long lines (512 bytes on hardware).
  for (uint16_t i = 0; i < have; i += PROBE_READ_OBJECT_LOG_BYTES_PER_LINE) {
    ESP_LOGI(detail::TAG, "  +%03X: %s", static_cast<unsigned>(i),
             hex_range(data, i, std::min<uint16_t>(have, i + PROBE_READ_OBJECT_LOG_BYTES_PER_LINE)).c_str());
  }
  result.message += (complete ? " data=" : " incomplete, data so far=") + hex;
}

ManagementActionResult DiagnosticProbes::probe_sweep(const std::string &device_id, const std::string &probe,
                                                     const std::string &first_index, const std::string &last_index) {
  ManagementActionResult result =
      make_management_result(MANAGEMENT_ACTION_PROBE_SWEEP, normalize_device_id_argument(device_id));
  result.probe_name = probe;

  // Validate the device and the probe name once, up front. Neither can start succeeding partway
  // through a range: an unregistered device or an unrecognized probe name fails identically for
  // every index, so without this check the loop below would run the full span and append the
  // same error line up to PROBE_SWEEP_MAX_INDICES times.
  ManagementActionResult resolve_result;
  if (resolve_management_device(MANAGEMENT_ACTION_PROBE_SWEEP, device_id, *initialized_, registry_, resolve_result) ==
      nullptr) {
    result.message = resolve_result.message;
    return result;
  }
  const ProbeDescriptor *descriptor = resolve_probe_descriptor(probe, result);
  if (descriptor == nullptr)
    return result;
  if (!descriptor->sweepable) {
    result.message = "probe \"" + probe + "\" cannot be swept; use probe_device";
    return result;
  }
  if (!descriptor->needs_index) {
    result.message = "probe \"" + probe + "\" takes no index; use probe_device";
    return result;
  }

  uint8_t first = 0;
  uint8_t last = 0;
  if (!parse_probe_index(first_index, first) || !parse_probe_index(last_index, last)) {
    result.message = "first_index/last_index must be decimal or 0x-prefixed byte values (0-255)";
    return result;
  }
  if (last < first) {
    result.message = "last_index must be >= first_index";
    return result;
  }
  if (last > descriptor->max_index) {
    result.message = index_out_of_range_message(*descriptor);
    return result;
  }
  const uint32_t span = static_cast<uint32_t>(last) - first + 1;
  if (span > PROBE_SWEEP_MAX_INDICES) {
    result.message =
        "sweep range too wide: " + std::to_string(span) + " indices, max " + std::to_string(PROBE_SWEEP_MAX_INDICES);
    return result;
  }

  std::string report;
  uint32_t answered_count = 0;
  bool stopped_early = false;
  for (uint32_t idx = first; idx <= last; idx++) {
    if (idx != first) {
      App.feed_wdt();
      delay(PROBE_SWEEP_DELAY_MS);
    }
    const std::string index_str = std::to_string(idx);
    const ManagementActionResult step = probe_device(device_id, probe, index_str);
    report += "index=0x" + format_hex_byte(static_cast<uint8_t>(idx)) + ": ";
    if (step.success) {
      answered_count++;
      report += "cmd=0x" + format_hex_byte(step.response_cmd) + " hex=" + step.response_hex;
      if (!step.decoded.empty()) {
        report += " [" + step.decoded + "]";
      }
      report += "\n";
    } else {
      report += step.message + "\n";
    }
    // terminal_refusal (diagnostic probes not enabled, device moving) applies to every remaining
    // index the same way it applied to this one -- stop rather than repeat it N more times. A
    // structured flag, not a search over `step.message`: probe_device() sets it explicitly, so
    // rewording a refusal message can never silently break this check.
    if (step.terminal_refusal) {
      report += "(stopping sweep: " + step.message + ")\n";
      stopped_early = true;
      break;
    }
  }

  // A sweep that ran its full requested range is a success even if nothing answered -- "no
  // device answered any index" is a valid, reportable outcome, the same philosophy
  // scan_paired_devices() uses for zero replies. A sweep cut short by a terminal refusal did not
  // complete what was asked of it and must not report success.
  result.success = !stopped_early;
  // Renders the same 0x-prefixed form as the report body, so the Home Assistant event carries the
  // swept range rather than a blank `index` field left over from the per-index probe_device()
  // results this loop discards.
  result.probe_index = "0x" + format_hex_byte(first) + "-0x" + format_hex_byte(last);
  result.message = "Sweep \"" + probe + "\" over [0x" + format_hex_byte(first) + ",0x" + format_hex_byte(last) +
                   "]: " + std::to_string(answered_count) + " answered\n" + report;
  return result;
}

}  // namespace home_io_control
}  // namespace esphome
