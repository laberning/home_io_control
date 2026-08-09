#pragma once

/// @file proto_constants.h
/// @brief IO-Homecontrol command IDs, result codes and protocol enumerations.
/// @ingroup hioc_protocol
///
/// Command bytes, CMD_ERROR_RESP result codes, wire position/status flags,
/// cryptographic constants, and the manufacturer/originator/ACEI/discovery
/// lookups. These describe *what* travels on the wire, independent of the
/// frame container (proto_frame.h) and the device model (proto_device_model.h).

#include "proto_sizes.h"

#include <cstdint>

namespace esphome {
namespace home_io_control {

// ============================================================================
// Command IDs
// ============================================================================

// Normal operation commands
static constexpr uint8_t CMD_EXECUTE = 0x00;        ///< Set position/open/close/stop — requires authentication
static constexpr uint8_t CMD_ACTIVATE_MODE = 0x01;  ///< Activate device mode (scene, ventilation) — requires auth
static constexpr uint8_t CMD_PRIVATE = 0x03;        ///< Get device status — no authentication needed
static constexpr uint8_t CMD_PRIVATE_RESP = 0x04;   ///< Response to 0x00 and 0x03 (contains position data)

// Sensor and private register commands
static constexpr uint8_t CMD_SET_SENSOR = 0x19;      ///< Inject sensor value into a device
static constexpr uint8_t CMD_SET_SENSOR_ACK = 0x1A;  ///< Acknowledgment to CMD_SET_SENSOR

// Device identification
static constexpr uint8_t CMD_IDENTIFY = 0x1E;  ///< Device physical identification / jog — requires authentication

static constexpr uint8_t CMD_WRITE_PRIVATE = 0x20;      ///< Write private register (climate/heating devices)
static constexpr uint8_t CMD_WRITE_PRIVATE_ACK = 0x21;  ///< Acknowledgment to CMD_WRITE_PRIVATE

// Discovery and pairing commands
static constexpr uint8_t CMD_DISCOVER_REQ = 0x28;   ///< Broadcast discovery request
static constexpr uint8_t CMD_DISCOVER_RESP = 0x29;  ///< Device responds with its ID and type
static constexpr uint8_t CMD_DISCOVER_SPE_REQ =
    0x2A;  ///< Discover sub-devices (e.g., light on garage door). Its 12-byte payload
           ///< authenticates itself in a single frame — 6 random challenge bytes followed by a
           ///< 6-byte HMAC over the command byte alone — instead of the usual 0x3C/0x3D round
           ///< trip, which is what lets it be broadcast. Confirmed against real bytes in
           ///< tests/corpus/captures/velux_kux100/pairing_full.yaml (recomputed under that
           ///< installation's key before the capture was re-keyed). create_discovery_request()
           ///< (proto_commands.cpp) builds it to match. Nothing consumes the reply:
           ///< classify_pairing_discovery_response() accepts only 0x29, so a device answering
           ///< 0x2B would be discarded — sending 0x2A as a discovery command can therefore only
           ///< work today if the device replies 0x29, which no capture yet shows either way.
static constexpr uint8_t CMD_DISCOVER_SPE_RESP = 0x2B;     ///< Sub-device response
static constexpr uint8_t CMD_DISCOVER_CONFIRM = 0x2C;      ///< Confirm discovery to device
static constexpr uint8_t CMD_DISCOVER_CONFIRM_ACK = 0x2D;  ///< Device acknowledges confirmation
static constexpr uint8_t CMD_DISCOVER_ALT_REQ =
    0x2E;  ///< Alternate discovery. Broadcast (to 0x00003F) draws no response at all on every
           ///< device this project has real evidence for — a Somfy Izymo dimmer
           ///< (tests/corpus/captures/somfy_dimmer/discover_alt_no_response.yaml) and a Velux
           ///< KLR200/KUX100 pair (tests/corpus/captures/velux_kux100/
           ///< discover_alt_broadcast_no_response.yaml) both went unanswered; the older
           ///< "response is 0x29" guess never had real evidence and appears to have been wrong.
           ///< Directly *addressed* to a known device instead of broadcast, it does draw a
           ///< response, but a 0x3C/0x3D challenge-response followed by CMD_DISCOVER_ALT_RESP
           ///< (0x2F), not 0x29 — see tests/corpus/captures/velux_kux100/
           ///< discover_alt_addressed_challenge_response.yaml.
static constexpr uint8_t CMD_DISCOVER_ALT_RESP =
    0x2F;  ///< Reply to an addressed (non-broadcast) CMD_DISCOVER_ALT_REQ, following a
           ///< 0x3C/0x3D challenge-response. See CMD_DISCOVER_ALT_REQ's comment and
           ///< tests/corpus/captures/velux_kux100/discover_alt_addressed_challenge_response.yaml
           ///< — the only capture this project has of it. Not otherwise used anywhere in this
           ///< codebase (no dispatch logic added).
static constexpr uint8_t CMD_ONEWAY_REMOVE =
    0x39;  ///< 1W "remove controller" (un-pair a 1W remote from a device); same payload shape
           ///< as 0x2E. Reference: analysis/completed/pairing_lab.md field capture, "CMD 0x39".

// Key exchange commands (used during pairing)
static constexpr uint8_t CMD_KEY_INIT = 0x31;      ///< Initiate key transfer to device
static constexpr uint8_t CMD_KEY_TRANSFER = 0x32;  ///< Send encrypted system key to device
static constexpr uint8_t CMD_KEY_CONFIRM = 0x33;   ///< Device confirms key was received

// Address and device-initiated key exchange
static constexpr uint8_t CMD_ADDRESS_REQ = 0x36;  ///< Address assignment request
static constexpr uint8_t CMD_ADDRESS_RESP =
    0x37;  ///< Address assignment response: the device returns its own 3-byte backbone address,
           ///< byte-identical to the one it reported at data[2..4]
           ///< (DISCOVERY_RESP_BACKBONE_OFFSET) of its CMD_DISCOVER_RESP earlier in the same
           ///< session — an independent confirmation of that offset. Only capture of this pair:
           ///< tests/corpus/captures/velux_kux100/pairing_full.yaml, where a Velux KLR200 closes
           ///< pairing with 0x36 and then challenges the 0x37 it gets back (see
           ///< CMD_CHALLENGE_REQ). Neither command is sent or handled anywhere in this codebase.
static constexpr uint8_t CMD_LAUNCH_KEY_TRANSFER = 0x38;  ///< Device-initiated key transfer request

// Authentication commands (challenge-response for secured commands)
static constexpr uint8_t CMD_CHALLENGE_REQ =
    0x3C;  ///< 6-byte random challenge. Usually a device challenging a controller's command —
           ///< the only direction this codebase implements — but the protocol is symmetric and
           ///< controllers challenge devices too: in
           ///< tests/corpus/captures/velux_kux100/pairing_full.yaml a KLR200 issues 0x3C against
           ///< the device's own CMD_ADDRESS_RESP.
static constexpr uint8_t CMD_CHALLENGE_RESP =
    0x3D;  ///< HMAC proof answering a 0x3C. Whoever is challenged authenticates *its own*
           ///< preceding frame: the transcript is [cmd, data...] of the challenged party's last
           ///< frame (create_challenge_resp()), never the challenger's. That holds in both
           ///< directions — the device-side 0x3D in pairing_full.yaml (over its own 0x37) was
           ///< recomputed under that installation's recovered key and confirmed before the
           ///< capture was re-keyed, so it is measured, not assumed by symmetry.

// Device info commands
static constexpr uint8_t CMD_GET_NAME = 0x50;       ///< Request device name
static constexpr uint8_t CMD_GET_NAME_RESP = 0x51;  ///< Device name response
static constexpr uint8_t CMD_SET_NAME = 0x52;       ///< Set device name (authenticated)
static constexpr uint8_t CMD_SET_NAME_RESP = 0x53;  ///< Device-name write response
static constexpr uint8_t CMD_GET_INFO1 =
    0x54;  ///< Request device general info 1 (unimplemented; we only use 0x56/0x57)
static constexpr uint8_t CMD_GET_INFO1_RESP =
    0x55;  ///< Device general info 1 response (unimplemented; we only use 0x56/0x57)
static constexpr uint8_t CMD_GET_INFO2 = 0x56;       ///< Request device type/model info
static constexpr uint8_t CMD_GET_INFO2_RESP = 0x57;  ///< Device type/model response

// Configuration and status update commands
static constexpr uint8_t CMD_SET_CONFIG1 = 0x6F;         ///< Configure device to auto-send status updates
static constexpr uint8_t CMD_SET_CONFIG1_RESP = 0x70;    ///< Config response
static constexpr uint8_t CMD_STATUS_UPDATE = 0x71;       ///< Device-initiated status update (needs auth)
static constexpr uint8_t CMD_STATUS_UPDATE_RESP = 0x72;  ///< Acknowledge status update

static constexpr uint8_t CMD_SEND_RAW_MESSAGE = 0xF0;    ///< Send raw message / "find hardware" (service)
static constexpr uint8_t CMD_READ_GROUPS = 0xF1;         ///< Actuator: read groups / service ACK
static constexpr uint8_t CMD_REBOOT = 0xF2;              ///< Reboot / service status
static constexpr uint8_t CMD_SERVICE_STATUS_ACK = 0xF3;  ///< Service status ACK

static constexpr uint8_t CMD_ERROR_RESP = 0xFE;  ///< Error response to any command

// Command-result / limitation codes carried in CMD_ERROR_RESP DATA[0].
static constexpr uint8_t RESULT_UNKNOWN_STATUS_REPLY = 0x00;        ///< Device returned an unknown status reply.
static constexpr uint8_t RESULT_COMMAND_COMPLETED_OK = 0x01;        ///< No errors detected.
static constexpr uint8_t RESULT_NO_CONTACT = 0x02;                  ///< No communication to node.
static constexpr uint8_t RESULT_MANUALLY_OPERATED = 0x03;           ///< Manually operated by a user.
static constexpr uint8_t RESULT_BLOCKED = 0x04;                     ///< Node blocked by an object.
static constexpr uint8_t RESULT_WRONG_SYSTEMKEY = 0x05;             ///< Node contains the wrong system key.
static constexpr uint8_t RESULT_PRIORITY_LEVEL_LOCKED = 0x06;       ///< Node is locked on this priority level.
static constexpr uint8_t RESULT_REACHED_WRONG_POSITION = 0x07;      ///< Node stopped in another position than expected.
static constexpr uint8_t RESULT_ERROR_DURING_EXECUTION = 0x08;      ///< Generic execution failure.
static constexpr uint8_t RESULT_NO_EXECUTION = 0x09;                ///< Node did not move.
static constexpr uint8_t RESULT_CALIBRATING = 0x0A;                 ///< Node is calibrating.
static constexpr uint8_t RESULT_POWER_CONSUMPTION_TOO_HIGH = 0x0B;  ///< Node power consumption is too high.
static constexpr uint8_t RESULT_POWER_CONSUMPTION_TOO_LOW = 0x0C;   ///< Node power consumption is too low.
static constexpr uint8_t RESULT_LOCK_POSITION_OPEN = 0x0D;          ///< Lock command failed because the door is open.
static constexpr uint8_t RESULT_MOTION_TIME_TOO_LONG = 0x0E;        ///< Target was not reached in time.
static constexpr uint8_t RESULT_THERMAL_PROTECTION = 0x0F;          ///< Node entered thermal protection mode.
static constexpr uint8_t RESULT_PRODUCT_NOT_OPERATIONAL = 0x10;     ///< Node is not currently operational.
static constexpr uint8_t RESULT_FILTER_MAINTENANCE_NEEDED = 0x11;   ///< Filter needs maintenance.
static constexpr uint8_t RESULT_BATTERY_LEVEL = 0x12;               ///< Battery level is low.
static constexpr uint8_t RESULT_TARGET_MODIFIED = 0x13;             ///< Node modified the requested target value.
static constexpr uint8_t RESULT_MODE_NOT_IMPLEMENTED = 0x14;        ///< Mode is not supported by the node.
static constexpr uint8_t RESULT_COMMAND_INCOMPATIBLE_TO_MOVEMENT = 0x15;  ///< Command cannot move the node that way.
static constexpr uint8_t RESULT_USER_ACTION = 0x16;                       ///< User action overrode the command.
static constexpr uint8_t RESULT_DEAD_BOLT_ERROR = 0x17;                   ///< Dead bolt error.
static constexpr uint8_t RESULT_AUTOMATIC_CYCLE_ENGAGED = 0x18;           ///< Node entered automatic cycle mode.
static constexpr uint8_t RESULT_WRONG_LOAD_CONNECTED = 0x19;              ///< Wrong load connected to node.
static constexpr uint8_t RESULT_COLOUR_NOT_REACHABLE = 0x1A;              ///< Requested colour not reachable.
static constexpr uint8_t RESULT_TARGET_NOT_REACHABLE = 0x1B;              ///< Requested target not reachable.
static constexpr uint8_t RESULT_BAD_INDEX_RECEIVED = 0x1C;                ///< Invalid index received.
static constexpr uint8_t RESULT_COMMAND_OVERRULED = 0x1D;                 ///< Command was overruled by a newer command.
static constexpr uint8_t RESULT_NODE_WAITING_FOR_POWER = 0x1E;            ///< Node is waiting for power.
static constexpr uint8_t RESULT_NODE_LOCKED = 0x20;                       ///< Node is locked.
static constexpr uint8_t RESULT_WRONG_POSITION = 0x21;                    ///< Node reports wrong position.
static constexpr uint8_t RESULT_LIMITS_NOT_SET = 0x22;                    ///< Device limits are not set.
static constexpr uint8_t RESULT_IP_NOT_SET = 0x23;                        ///< Intermediate position is not set.
static constexpr uint8_t RESULT_OUT_OF_RANGE = 0x24;                      ///< Requested value is out of range.
static constexpr uint8_t RESULT_PRIORITY_LOCKED_NON_EXEC =
    0x38;  ///< Priority locked, command not executed (ACEI priority too low).
static constexpr uint8_t RESULT_INFORMATION_CODE = 0xDF;              ///< Information-only code with unknown semantics.
static constexpr uint8_t RESULT_PARAMETER_LIMITED = 0xE0;             ///< Parameter limited by an unknown device.
static constexpr uint8_t RESULT_LIMITATION_BY_LOCAL_USER = 0xE1;      ///< Parameter limited by local button.
static constexpr uint8_t RESULT_LIMITATION_BY_USER = 0xE2;            ///< Parameter limited by a remote control.
static constexpr uint8_t RESULT_LIMITATION_BY_RAIN = 0xE3;            ///< Parameter limited by a rain sensor.
static constexpr uint8_t RESULT_LIMITATION_BY_TIMER = 0xE4;           ///< Parameter limited by a timer.
static constexpr uint8_t RESULT_LIMITATION_BY_SCD = 0xE5;             ///< Parameter limited by a security actuator.
static constexpr uint8_t RESULT_LIMITATION_BY_UPS = 0xE6;             ///< Parameter limited by a power supply.
static constexpr uint8_t RESULT_LIMITATION_BY_UNKNOWN_DEVICE = 0xE7;  ///< Parameter limited by an unknown device.
static constexpr uint8_t RESULT_LIMITATION_BY_SAAC = 0xEA;  ///< Parameter limited by a standalone automatic controller.
static constexpr uint8_t RESULT_LIMITATION_BY_WIND = 0xEB;  ///< Parameter limited by a wind sensor.
static constexpr uint8_t RESULT_LIMITATION_BY_MYSELF = 0xEC;           ///< Parameter limited by the node itself.
static constexpr uint8_t RESULT_LIMITATION_BY_AUTOMATIC_CYCLE = 0xED;  ///< Parameter limited by an automatic cycle.
static constexpr uint8_t RESULT_LIMITATION_BY_EMERGENCY = 0xEE;        ///< Parameter limited by an emergency.

// ============================================================================
// Position and Status Wire Constants
// ============================================================================

/// Position values in the IO protocol.
/// Normal positions are 0-100 (0=fully open, 100=fully closed).
/// Special values above 100 are control commands encoded as the "main" parameter
/// byte in CMD_EXECUTE payloads. These are internal wire constants — callers should
/// prefer CoverCommand for type-safe command dispatch.
static constexpr uint8_t POS_STOP = 0xD2;      ///< Wire value: stop movement.
static constexpr uint8_t POS_UNKNOWN = 0xD4;   ///< Wire value: position unknown / keep current.
static constexpr uint8_t POS_FAVORITE = 0xD8;  ///< Wire value: move to favorite/"My" position.

/// @brief Wire value for the secured target position command.
///
/// Moves the actuator to its pre-programmed secured/safety position from the
/// Execution Parameter Buffer. Typically sent by environmental sensors (wind, rain)
/// to retract an awning or close a shutter to a wind-safe state.
static constexpr uint8_t POS_SECURED_TARGET = 0xD1;

/// @brief Wire value for the default position command.
///
/// Moves the actuator to its factory or user-configured default position.
static constexpr uint8_t POS_DEFAULT = 0xD3;

/// @brief Ambiguous wire value used only for passive 1W-traffic intent decoding
/// (decode_1w_main_intent() / oneway_intent_to_target() in proto_codecs.cpp).
///
/// 0x64 (100) is simultaneously the ordinary doubled-position wire value for 50% and a value
/// some physical 1W remotes send for their "force open" button — the protocol has no dedicated
/// override code, so the two are indistinguishable on the wire. For passively decoding someone
/// else's remote traffic, "FORCE_OPEN" is the more useful diagnostic label (physical remotes
/// rarely send a numeric 50%). This is NOT used by any outbound builder in this codebase:
/// real-hardware testing confirmed that sending main=0x64 as an outbound 2W
/// CMD_EXECUTE command makes a real device move to 50% open, not bypass anything — see
/// create_force_open() in proto_commands.cpp for the actual (ACEI-priority-based) force-open
/// implementation.
static constexpr uint8_t POS_FORCE_OPEN = 0x64;

/// @brief Modifier byte for the ventilation command.
///
/// Both favorite and ventilation use POS_FAVORITE (0xD8) as the main parameter byte,
/// but ventilation sets the secondary byte (main[1]) to 0x03 while favorite leaves it 0x00.
static constexpr uint8_t POS_VENT_MODIFIER = 0x03;

/// Status byte flags in CMD_PRIVATE_RESP and CMD_STATUS_UPDATE.
static constexpr uint8_t STATUS_STOPPED = 0x01;        ///< Byte 0 bit 0: device is not moving
static constexpr uint8_t STATUS_EXPECTED = 0x80;       ///< Byte 1 bit 7: device will send auto status update
static constexpr uint8_t STATUS_TILT_SELECTOR = 0x20;  ///< Extended status payload marker for tilt-capable devices

// ============================================================================
// Cryptographic Constants
// ============================================================================

/// The transfer key is a hardcoded key used ONLY during pairing to obfuscate
/// the system key during over-the-air transfer. It is NOT the system key.
/// This is the same across all IO-Homecontrol devices worldwide.
static constexpr uint8_t TRANSFER_KEY[AES_KEY_SIZE] = {0x34, 0xC3, 0x46, 0x6E, 0xD8, 0x8F, 0x4E, 0x8E,
                                                       0x16, 0xAA, 0x47, 0x39, 0x49, 0x88, 0x43, 0x73};
static constexpr uint16_t CRC_POLYNOMIAL_REVERSED = 0x8408;  ///< Reversed CRC-CCITT polynomial used by IO-homecontrol
static constexpr uint16_t CRC_LSB_MASK = 0x0001;             ///< Least-significant-bit mask for reflected CRC update

/// Broadcast address for device discovery (0x00003B).
/// Used as destination in CMD_DISCOVER_REQ frames to trigger all pairable devices to respond.
static constexpr uint8_t BROADCAST_DISCOVER[NODE_ID_SIZE] = {0x00, 0x00, 0x3B};

/// Alternate discovery / 1W broadcast address (0x00003F).
/// Used as destination for CMD_DISCOVER_ALT_REQ (0x2E) alternate discovery, and the address
/// on which devices in 1W-triggered pairing mode listen. Distinct from the 2W discovery
/// broadcast BROADCAST_DISCOVER (0x00003B).
static constexpr uint8_t BROADCAST_DISCOVER_ALT[NODE_ID_SIZE] = {0x00, 0x00, 0x3F};

// ============================================================================
// Command Name Lookup
// ============================================================================

/// @brief Get a human-readable name for any IO-Homecontrol command ID.
///
/// Returns a short uppercase identifier suitable for log lines (e.g., "EXECUTE",
/// "DISCOVER_REQ", "CHALLENGE_RESP"). Unknown commands return "UNKNOWN_CMD".
/// @param cmd Command byte from the frame header.
/// @return Null-terminated string.
const char *command_name(uint8_t cmd);

// ============================================================================
// Manufacturer ID Lookup
// ============================================================================

/// @brief Maximum manufacturer ID with a known name in the lookup table.
static constexpr uint8_t MANUFACTURER_ID_MAX = 12;

/// @brief IO-Homecontrol manufacturer ID constants.
///
/// These 1-based identifiers are assigned by the IO-Homecontrol alliance and
/// appear in the discovery response payload at DISCOVERY_RESP_MANUFACTURER_OFFSET.
/// @{
static constexpr uint8_t MANUFACTURER_VELUX = 1;            ///< VELUX (roof windows, skylights).
static constexpr uint8_t MANUFACTURER_SOMFY = 2;            ///< Somfy (shutters, awnings, blinds).
static constexpr uint8_t MANUFACTURER_HONEYWELL = 3;        ///< Honeywell.
static constexpr uint8_t MANUFACTURER_HORMANN = 4;          ///< Hörmann (garage doors, gates).
static constexpr uint8_t MANUFACTURER_ASSA_ABLOY = 5;       ///< ASSA ABLOY (locks, access).
static constexpr uint8_t MANUFACTURER_NIKO = 6;             ///< Niko (switches, home automation).
static constexpr uint8_t MANUFACTURER_WINDOW_MASTER = 7;    ///< WINDOW MASTER (ventilation).
static constexpr uint8_t MANUFACTURER_RENSON = 8;           ///< Renson (ventilation, sun protection).
static constexpr uint8_t MANUFACTURER_CIAT = 9;             ///< CIAT (HVAC).
static constexpr uint8_t MANUFACTURER_SECUYOU = 10;         ///< Secuyou (security).
static constexpr uint8_t MANUFACTURER_OVERKIZ = 11;         ///< OVERKIZ (Somfy connectivity platform).
static constexpr uint8_t MANUFACTURER_ATLANTIC_GROUP = 12;  ///< Atlantic Group (heating, hot water).
/// @}

/// @brief Get a human-readable manufacturer name from the protocol manufacturer byte.
///
/// The manufacturer ID is a 1-based index assigned by the IO-Homecontrol alliance.
/// IDs outside the known range return "unknown". When an unknown ID appears at runtime,
/// the pairing flow logs a warning suggesting the user file a GitHub issue.
/// @param id Manufacturer ID byte (1–12 for known manufacturers).
/// @return Null-terminated lowercase string such as "unknown", or mixed-case name like "Somfy".
const char *manufacturer_name(uint8_t id);

// ============================================================================
// Command Originator Codes
// ============================================================================

/// @brief Command originator codes indicating what or who triggered a command.
///
/// The originator byte is the first byte of the CMD_EXECUTE payload. It tells
/// the actuator (and any eavesdropping controller) who initiated the movement.
/// This is useful for understanding device-initiated status updates.
/// @{
static constexpr uint8_t ORIGINATOR_LOCAL_USER = 0x00;        ///< User pressed a button on the actuator.
static constexpr uint8_t ORIGINATOR_USER_REMOTE = 0x01;       ///< User sent command from a remote control.
static constexpr uint8_t ORIGINATOR_RAIN_SENSOR = 0x02;       ///< Rain sensor triggered the movement.
static constexpr uint8_t ORIGINATOR_TIMER = 0x03;             ///< Timer or schedule triggered the movement.
static constexpr uint8_t ORIGINATOR_SECURITY = 0x04;          ///< Security controlling device (SCD) action.
static constexpr uint8_t ORIGINATOR_UPS = 0x05;               ///< Uninterruptible power supply action.
static constexpr uint8_t ORIGINATOR_SMART_CONTROLLER = 0x06;  ///< Smart function controller.
static constexpr uint8_t ORIGINATOR_LIFESTYLE = 0x07;         ///< Lifestyle scenario controller.
static constexpr uint8_t ORIGINATOR_SAAC = 0x08;              ///< Stand-alone automatic controller (SAAC).
static constexpr uint8_t ORIGINATOR_WIND_SENSOR = 0x09;       ///< Wind sensor triggered the movement.
static constexpr uint8_t ORIGINATOR_LOAD_SHEDDING = 0x0B;     ///< Load-shedding manager.
static constexpr uint8_t ORIGINATOR_LOCAL_LIGHT = 0x0C;       ///< Local light sensor.
static constexpr uint8_t ORIGINATOR_ENVIRONMENT = 0x0D;       ///< Unspecified environment sensor.
static constexpr uint8_t ORIGINATOR_MYSELF = 0x10;            ///< Actuator decided to move by itself.
static constexpr uint8_t ORIGINATOR_AUTOMATIC_CYCLE = 0xFE;   ///< Automatic cycle / external access.
static constexpr uint8_t ORIGINATOR_EMERGENCY = 0xFF;         ///< Emergency command (never disabled).
/// @}

/// @brief Get a human-readable name for a command originator byte.
///
/// @param originator Originator code from the first data byte of CMD_EXECUTE.
/// @return Null-terminated string such as "rain_sensor" or "user_remote".
const char *originator_name(uint8_t originator);

// ============================================================================
// ACEI (Application Command Execution Interface)
// ============================================================================

/// @brief ACEI byte bit-field definitions.
///
/// The ACEI byte is the second byte of the CMD_EXECUTE payload. It encodes the
/// priority level and service class of the command, controlling which commands
/// can override others. Devices reject commands with lower priority than their
/// current locked level (resulting in RESULT_PRIORITY_LEVEL_LOCKED).
/// @{
static constexpr uint8_t ACEI_VALID_BIT = 0x01;      ///< Bit 0: command validity flag.
static constexpr uint8_t ACEI_EXTENDED_MASK = 0x06;  ///< Bits [2:1]: extended field.
static constexpr uint8_t ACEI_EXTENDED_SHIFT = 1;    ///< Shift for extended field extraction.
static constexpr uint8_t ACEI_SERVICE_MASK = 0x18;   ///< Bits [4:3]: service type.
static constexpr uint8_t ACEI_SERVICE_SHIFT = 3;     ///< Shift for service field extraction.
static constexpr uint8_t ACEI_LEVEL_MASK = 0xE0;     ///< Bits [7:5]: priority level (0–7).
static constexpr uint8_t ACEI_LEVEL_SHIFT = 5;       ///< Shift for priority level extraction.
/// @}

/// @brief ACEI priority level values (0–7).
///
/// These values are extracted from the ACEI byte via (acei & ACEI_LEVEL_MASK) >> ACEI_LEVEL_SHIFT.
/// @{
static constexpr uint8_t ACEI_LEVEL_PROTECTION_HUMAN = 0;   ///< Personal safety (highest, overrides all).
static constexpr uint8_t ACEI_LEVEL_PROTECTION_SENSOR = 1;  ///< Goods/environment protection via sensors.
static constexpr uint8_t ACEI_LEVEL_USER_HIGH = 2;          ///< High-priority user controller.
static constexpr uint8_t ACEI_LEVEL_USER_DEFAULT = 3;       ///< Default remote controller priority.
static constexpr uint8_t ACEI_LEVEL_COMFORT_1 = 4;          ///< Comfort automation level 1.
static constexpr uint8_t ACEI_LEVEL_COMFORT_2 = 5;          ///< Comfort automation level 2.
static constexpr uint8_t ACEI_LEVEL_AUTO_SAAC = 6;          ///< Stand-alone automatic controller.
static constexpr uint8_t ACEI_LEVEL_AUTO_DEFAULT = 7;       ///< Default automatic level (lowest).
/// @}

/// @brief Get a human-readable name for an ACEI priority level (0–7).
///
/// Priority levels form a hierarchy: level 0 (human protection) is highest
/// and overrides all others. Level 3 is the default for remote controllers.
/// @param level Priority level value (0–7).
/// @return Null-terminated string such as "user_default" or "protection_sensor".
const char *acei_level_name(uint8_t level);

// ============================================================================
// Discovery Response Extended Fields
// ============================================================================

/// @brief Byte offsets within CMD_DISCOVER_RESP (0x29) payload data.
///
/// The full discovery response carries up to 9 bytes of device metadata:
/// bytes 0–1 hold the packed device type/subtype (already parsed by
/// decode_packed_device_type()), and bytes 2–8 hold additional fields.
/// @{
static constexpr uint8_t DISCOVERY_RESP_BACKBONE_OFFSET = 2;      ///< Backbone address starts at data[2] (3 bytes);
                                                                  ///< cross-confirmed by CMD_ADDRESS_RESP (0x37),
                                                                  ///< which returns the same 3 bytes for the same
                                                                  ///< device (see that constant's comment).
static constexpr uint8_t DISCOVERY_RESP_MANUFACTURER_OFFSET = 5;  ///< Manufacturer ID at data[5].
static constexpr uint8_t DISCOVERY_RESP_FLAGS_OFFSET = 6;         ///< Flags byte at data[6].
static constexpr uint8_t DISCOVERY_RESP_TIMESTAMP_OFFSET = 7;     ///< Timestamp starts at data[7] (2 bytes).
static constexpr uint8_t DISCOVERY_RESP_FULL_SIZE = 9;            ///< Full discovery response payload size.
/// @}

// === Multi Information Byte (Discovery Response data[6]) ===
// The flags byte in the discovery response encodes device capabilities and timing
// characteristics that help a controller tune its interaction with the actuator.

/// @brief Bit masks and shifts for the Multi Information Byte fields.
/// @{
static constexpr uint8_t DISCOVERY_FLAGS_ATT_MASK = 0xC0;         ///< Bits [7:6]: actuator turnaround time class.
static constexpr uint8_t DISCOVERY_FLAGS_ATT_SHIFT = 6;           ///< Shift for ATT field extraction.
static constexpr uint8_t DISCOVERY_FLAGS_SYNC_CTRL_GRP = 0x20;    ///< Bit 5: supports sync control group.
static constexpr uint8_t DISCOVERY_FLAGS_RF_SUPPORT = 0x08;       ///< Bit 3: RF support in node (0=yes, 1=no).
static constexpr uint8_t DISCOVERY_FLAGS_POWER_SAVE_MASK = 0x03;  ///< Bits [1:0]: power save mode.
/// @}

/// @brief Actuator Turnaround Time (ATT) class values.
///
/// Indicates the maximum time window in which the actuator normally responds after
/// receiving a command. Extracted from the Multi Information Byte via
/// `(flags & DISCOVERY_FLAGS_ATT_MASK) >> DISCOVERY_FLAGS_ATT_SHIFT`.
/// @{
static constexpr uint8_t ATT_CLASS_5S = 0;   ///< Response within 5 seconds.
static constexpr uint8_t ATT_CLASS_10S = 1;  ///< Response within 10 seconds.
static constexpr uint8_t ATT_CLASS_20S = 2;  ///< Response within 20 seconds.
static constexpr uint8_t ATT_CLASS_40S = 3;  ///< Response within 40 seconds.
/// @}

/// @brief Power save mode values from the Multi Information Byte.
///
/// Extracted from `flags & DISCOVERY_FLAGS_POWER_SAVE_MASK`.
/// Devices in low-power mode require long preamble (1024 bytes) to wake their receiver.
/// @{
static constexpr uint8_t POWER_SAVE_ALWAYS_ALIVE = 0;  ///< Device is always listening — short preamble works.
static constexpr uint8_t POWER_SAVE_LOW_POWER = 1;     ///< Device sleeps — needs long preamble to wake.
/// @}

/// @brief Get a human-readable turnaround time string for an ATT class value.
/// @param att_class ATT class (0–3) extracted from the Multi Information Byte.
/// @return Null-terminated string such as "5s", "10s", "20s", or "40s".
const char *att_class_name(uint8_t att_class);

/// @brief Get a human-readable power save mode name.
/// @param mode Power save value (0–1) extracted from the Multi Information Byte.
/// @return Null-terminated string such as "always_alive" or "low_power".
const char *power_save_mode_name(uint8_t mode);

// ============================================================================
// Command Result Codes
// ============================================================================

/// @brief Return a stable symbolic name for a CMD_ERROR_RESP result code.
/// @param result Result byte from CMD_ERROR_RESP data[0].
/// @return Uppercase symbolic name, or "UNKNOWN_RESULT_CODE" when unmapped.
const char *command_result_name(uint8_t result);
/// @brief Return a human-readable explanation for a CMD_ERROR_RESP result code.
/// @param result Result byte from CMD_ERROR_RESP data[0].
/// @return Short description suitable for warn-level logs.
const char *command_result_description(uint8_t result);
/// @brief Check whether a result code represents an environmental or control limitation.
/// @param result Result byte from CMD_ERROR_RESP data[0].
/// @return true when the response reports a limitation rather than a generic execution error.
bool is_limitation_result(uint8_t result);

}  // namespace home_io_control
}  // namespace esphome
