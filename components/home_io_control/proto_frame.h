#pragma once

/// @file proto_frame.h
/// @brief IO-Homecontrol 2W protocol definitions.
/// @ingroup hioc_protocol
///
/// IO-Homecontrol is a proprietary wireless protocol used by Somfy, Velux, and other
/// manufacturers for controlling shutters, awnings, blinds, and similar devices.
/// "2W" means two-way: the controller sends commands and receives status feedback.
///
/// The protocol uses FSK modulation at 868 MHz with frequency hopping across 3 channels.
/// Communication is encrypted with AES-128 and authenticated with a 6-byte HMAC.
/// Each installation has a unique 16-byte "system key" shared between controller and devices.

#include <cstdint>
#include <cstring>
#include <string>

namespace esphome {
namespace home_io_control {

// ============================================================================
// Physical Layer — Radio Parameters
// ============================================================================

/// The protocol uses 3 frequency channels in the 868 MHz ISM band.
/// IO-Homecontrol uses 3 channels in the 868 MHz SRD band. In 1W (one-way) mode,
/// only CH2 is used. In 2W (two-way) mode, the controller hops across all three
/// channels every ~2.7ms when idle. Commands are sent on CH2; responses may arrive
/// on any channel within the exchange wait window.
static constexpr uint32_t FREQ_CH1 = 868250000;  ///< Channel 1: 868.25 MHz (2W only)
static constexpr uint32_t FREQ_CH2 = 868950000;  ///< Channel 2: 868.95 MHz (1W and 2W, TX channel)
static constexpr uint32_t FREQ_CH3 = 869850000;  ///< Channel 3: 869.85 MHz (2W only)

/// Preamble is a sequence of 0xAA bytes that precedes every frame.
/// The first frame in an exchange uses a long preamble (1024 bytes = 8192 bits)
/// so the receiver has time to detect it while hopping. Subsequent frames in the
/// same exchange use a short preamble (8 bytes) since both sides are already
/// on the same channel. Solar-powered devices need the long preamble to wake up.
static constexpr uint16_t LONG_PREAMBLE = 1024;  ///< 1024 bytes for initial/start frames
static constexpr uint16_t SHORT_PREAMBLE = 8;    ///< 8 bytes for response/continuation frames
/// SX1262-specific preamble for the outbound 0x3D challenge response.
///
/// The SX1262 path has to rebuild the IO-homecontrol framing details in software and the
/// 0x3D auth response is sent immediately after we switch from RX to TX upon receiving the
/// device's 0x3C challenge. Real-device tuning showed that 64 preamble bytes gives the peer
/// enough lock-on margin in that tight turn-around window, while leaving the proven SX1276
/// short-response waveform untouched.
static constexpr uint16_t SX1262_AUTH_RESPONSE_PREAMBLE = 64;

/// SX1262-specific per-channel dwell while waiting for authenticated exchange responses.
///
/// A 50 ms dwell was short enough that the controller could hop away from the request channel
/// just before the device emitted its post-auth reply. Using 90 ms keeps the SX1262 receiver on
/// that channel long enough for the observed device turn-around after 0x3D, without inflating the
/// overall 300/500 ms exchange windows for the rest of the protocol.
static constexpr int32_t SX1262_EXCHANGE_RESPONSE_WAIT_SLICE_MS = 90;

/// Timing constants for frequency hopping and response waiting.
static constexpr int32_t HOP_TIME_US = 2700;             ///< Time per channel when hopping (2.7ms)
static constexpr int32_t RESPONSE_CHANNEL_WAIT_MS = 50;  ///< Per-channel dwell while waiting for an exchange response
static constexpr int32_t RESPONSE_WAIT_MS = 500;         ///< Wait for response to non-start frame
static constexpr int32_t RESPONSE_START_WAIT_MS = 300;   ///< Wait for response to start frame (longer)
static constexpr int32_t RESPONSE_AUTH_WAIT_MS =
    RESPONSE_WAIT_MS;                                    ///< Wait for final response after challenge response
static constexpr int32_t EXCHANGE_RETRY_DELAY_MS = 250;  ///< Gap between retries within one HA command
static constexpr uint8_t EXCHANGE_RETRY_COUNT = 3;       ///< Attempts per command before reporting failure

/// Listen-before-talk (LBT) parameters for ETSI EN 300 220 compliance.
/// Before transmitting, the radio checks that the channel RSSI is below the
/// threshold. If the channel is busy, TX is deferred by LBT_RETRY_DELAY_MS
/// up to LBT_MAX_RETRIES times.
static constexpr int16_t LBT_RSSI_THRESHOLD_DBM = -90;  ///< Channel-free threshold (ETSI: ≤ -90 dBm)
static constexpr uint8_t LBT_MAX_RETRIES = 5;           ///< Max carrier-sense attempts before TX anyway
static constexpr uint8_t LBT_RETRY_DELAY_MS = 5;        ///< Backoff between LBT checks (≥ 5ms per ETSI)

// ============================================================================
// Frame Constants
// ============================================================================

static constexpr uint8_t NODE_ID_SIZE = 3;  ///< Device/node addresses are 3 bytes (e.g., "123ABC")
static constexpr uint8_t NODE_ID_STRING_SIZE = NODE_ID_SIZE * 2 + 1;  ///< Uppercase hex node ID plus null terminator
static constexpr uint8_t HMAC_SIZE = 6;        ///< Authentication HMAC is 6 bytes (truncated AES output)
static constexpr uint8_t AES_KEY_SIZE = 16;    ///< AES-128 key size
static constexpr uint8_t AES_BLOCK_SIZE = 16;  ///< AES block size
static constexpr uint8_t IV_SIZE = 16;         ///< Initialization vector size for AES
static constexpr uint8_t IV_PADDING = 0x55;    ///< Padding byte used in IV construction
static constexpr uint8_t BITS_PER_BYTE = 8;    ///< Number of bits in one protocol byte

static constexpr uint8_t FRAME_MIN_SIZE = 9;        ///< Minimum frame: CTRL0+CTRL1+DST(3)+SRC(3)+CMD(1)
static constexpr uint8_t FRAME_MAX_SIZE = 32;       ///< Maximum frame size (9 header + 23 data)
static constexpr uint8_t FRAME_MAX_DATA_SIZE = 23;  ///< Maximum data bytes after command ID

/// Control byte 0 (CTRL0) bit definitions.
/// CTRL0 encodes frame flags and the total frame length.
/// Bits [4:0] = frame_length - 1 (so 0x08 means 9 bytes total).
/// - START (bit 6): first frame in an exchange; uses long preamble (1024 bytes).
/// - END (bit 7): last frame in an exchange; set on responses and command completions.
/// - 1W (bit 5): 1=OneWay protocol (no response expected), 0=TwoWay (response expected).
/// For 2W operation, the controller sets START on initial command and device replies with END; subsequent frames in an
/// authenticated exchange also carry END.
static constexpr uint8_t CTRL0_END = 0x80;          ///< Bit 7: last frame in exchange
static constexpr uint8_t CTRL0_START = 0x40;        ///< Bit 6: first frame in exchange (uses long preamble)
static constexpr uint8_t CTRL0_PROTOCOL_1W = 0x20;  ///< Bit 5: 1=OneWay protocol, 0=TwoWay protocol
static constexpr uint8_t CTRL0_LENGTH_MASK = 0x1F;  ///< Bits [4:0]: frame length - 1

/// Control byte 1 (CTRL1) bit definitions.
/// - LOW_POWER (bit 5): device is battery/solar powered; may sleep and requires long preamble to wake.
static constexpr uint8_t CTRL1_LOW_POWER = 0x20;  ///< Bit 5: low-power device (e.g., solar-powered)

// ============================================================================
// Command IDs
// ============================================================================

// Normal operation commands
static constexpr uint8_t CMD_EXECUTE = 0x00;       ///< Set position/open/close/stop — requires authentication
static constexpr uint8_t CMD_PRIVATE = 0x03;       ///< Get device status — no authentication needed
static constexpr uint8_t CMD_PRIVATE_RESP = 0x04;  ///< Response to 0x00 and 0x03 (contains position data)

// Discovery and pairing commands
static constexpr uint8_t CMD_DISCOVER_REQ = 0x28;          ///< Broadcast discovery request
static constexpr uint8_t CMD_DISCOVER_RESP = 0x29;         ///< Device responds with its ID and type
static constexpr uint8_t CMD_DISCOVER_SPE_REQ = 0x2A;      ///< Discover sub-devices (e.g., light on garage door)
static constexpr uint8_t CMD_DISCOVER_SPE_RESP = 0x2B;     ///< Sub-device response
static constexpr uint8_t CMD_DISCOVER_CONFIRM = 0x2C;      ///< Confirm discovery to device
static constexpr uint8_t CMD_DISCOVER_CONFIRM_ACK = 0x2D;  ///< Device acknowledges confirmation

// Key exchange commands (used during pairing)
static constexpr uint8_t CMD_KEY_INIT = 0x31;      ///< Initiate key transfer to device
static constexpr uint8_t CMD_KEY_TRANSFER = 0x32;  ///< Send encrypted system key to device
static constexpr uint8_t CMD_KEY_CONFIRM = 0x33;   ///< Device confirms key was received

// Authentication commands (challenge-response for secured commands)
static constexpr uint8_t CMD_CHALLENGE_REQ = 0x3C;   ///< Device sends 6-byte random challenge
static constexpr uint8_t CMD_CHALLENGE_RESP = 0x3D;  ///< Controller responds with HMAC proof

// Device info commands
static constexpr uint8_t CMD_GET_NAME = 0x50;        ///< Request device name
static constexpr uint8_t CMD_GET_NAME_RESP = 0x51;   ///< Device name response
static constexpr uint8_t CMD_SET_NAME = 0x52;        ///< Set device name (authenticated)
static constexpr uint8_t CMD_SET_NAME_RESP = 0x53;   ///< Device-name write response
static constexpr uint8_t CMD_GET_INFO2 = 0x56;       ///< Request device type/model info
static constexpr uint8_t CMD_GET_INFO2_RESP = 0x57;  ///< Device type/model response

// Configuration and status update commands
static constexpr uint8_t CMD_SET_CONFIG1 = 0x6F;         ///< Configure device to auto-send status updates
static constexpr uint8_t CMD_SET_CONFIG1_RESP = 0x70;    ///< Config response
static constexpr uint8_t CMD_STATUS_UPDATE = 0x71;       ///< Device-initiated status update (needs auth)
static constexpr uint8_t CMD_STATUS_UPDATE_RESP = 0x72;  ///< Acknowledge status update
static constexpr uint8_t CMD_ERROR_RESP = 0xFE;          ///< Error response to any command

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
// Position and Status Constants
// ============================================================================

/// Position values in the IO protocol.
/// Normal positions are 0-100 (0=fully open, 100=fully closed).
/// Special values above 100 are control commands.
static constexpr uint8_t POS_STOP = 0xD2;      ///< Stop movement
static constexpr uint8_t POS_UNKNOWN = 0xD4;   ///< Position unknown
static constexpr uint8_t POS_FAVORITE = 0xD8;  ///< Move to favorite/"My" position

/// In status responses, position is encoded as a 16-bit value where
/// 0x0000 = fully open (0%) and 0xC800 = fully closed (100%).
static constexpr uint16_t STATUS_POS_MAX = 0xC800;
/// Target-reached tolerance expressed in raw IO-homecontrol position units.
/// 100 raw units out of 51200 full-scale is about 0.195%, so this only absorbs
/// tiny target/current mismatches from device rounding or early stopped flags.
static constexpr uint16_t STATUS_POS_TOLERANCE_RAW = 100;

/// Status byte flags in CMD_PRIVATE_RESP and CMD_STATUS_UPDATE.
static constexpr uint8_t STATUS_STOPPED = 0x01;        ///< Byte 0 bit 0: device is not moving
static constexpr uint8_t STATUS_EXPECTED = 0x80;       ///< Byte 1 bit 7: device will send auto status update
static constexpr uint8_t STATUS_TILT_SELECTOR = 0x20;  ///< Extended status payload marker for tilt-capable devices

/// Packed device metadata uses two bytes where the high 8 bits carry the upper type bits and the
/// low byte carries both the remaining type bits and the 6-bit manufacturer subtype.
static constexpr uint8_t DEVICE_METADATA_SIZE = 2;
static constexpr uint8_t DEVICE_TYPE_LOW_BITS_SHIFT = 2;
static constexpr uint8_t DEVICE_TYPE_HIGH_BITS_SHIFT = 6;
static constexpr uint8_t DEVICE_SUBTYPE_MASK = 0x3F;

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

// ============================================================================
// Frame Structure
// ============================================================================

/// @brief Parsed IO‑Homecontrol frame (CTRL0/1 + addresses + command + data).
/// @ingroup hioc_protocol
///
/// Over the air layout: [CTRL0][CTRL1][DST 3B][SRC 3B][CMD][DATA 0-23B][CRC 2B].
/// The CRC is handled by hardware on SX1276 (IoHomeOn) and by software on SX1262;
/// it is not included in this struct.
struct IoFrame {
  uint8_t ctrl0;                      ///< Control byte 0: flags + length.
  uint8_t ctrl1;                      ///< Control byte 1: low power, beacon, etc.
  uint8_t dst[NODE_ID_SIZE];          ///< Destination node ID (3 bytes).
  uint8_t src[NODE_ID_SIZE];          ///< Source node ID (3 bytes).
  uint8_t cmd;                        ///< Command ID.
  uint8_t data[FRAME_MAX_DATA_SIZE];  ///< Command parameters (0–23 bytes).
  uint8_t data_len;                   ///< Actual length of data.
};

// --- Frame construction and parsing ---
/// Initialize an IoFrame header (ctrl0/ctrl1) with flags.
/// @param f Frame to initialize.
/// @param is_2w True for 2‑way (default), false for 1‑way.
/// @param start Set START flag (first frame in exchange).
/// @param end Set END flag (final frame in exchange).
/// @param low_power Set LOW_POWER flag.
void init_frame(IoFrame &f, bool is_2w = true, bool start = false, bool end = false, bool low_power = false);
/// Set destination node ID.
/// @param f Frame to modify.
/// @param id 3‑byte destination address.
void set_dst(IoFrame &f, const uint8_t id[NODE_ID_SIZE]);
/// Set source node ID.
/// @param f Frame to modify.
/// @param id 3‑byte source address.
void set_src(IoFrame &f, const uint8_t id[NODE_ID_SIZE]);
/// Set command and payload.
/// @param f Frame to modify.
/// @param cmd Command ID.
/// @param params Pointer to payload bytes (may be nullptr for zero‑length).
/// @param params_len Payload length (0–23).
/// @return true if frame fits within size limits; false otherwise.
bool set_cmd(IoFrame &f, uint8_t cmd, const uint8_t *params = nullptr, uint8_t params_len = 0);
/// Get total frame length from ctrl0.
/// @param f Parsed frame.
/// @return Length in bytes.
uint8_t frame_length(const IoFrame &f);
/// Check START flag.
/// @param f Parsed frame.
/// @return true if START flag is set.
bool is_start(const IoFrame &f);
/// Check END flag.
/// @param f Parsed frame.
/// @return true if END flag is set.
bool is_end(const IoFrame &f);
/// Serialize a parsed frame into a wire buffer (without CRC).
/// @param f Parsed frame.
/// @param buf Output buffer (must be at least frame_length(f) bytes).
/// @param buf_size Size of buf.
/// @return Number of bytes written, or 0 on failure.
uint8_t serialize(const IoFrame &f, uint8_t *buf, uint8_t buf_size);
/// Parse a wire buffer into a parsed IoFrame (validates length and CTRL0).
/// @param buf Raw byte buffer.
/// @param buf_len Number of bytes in buf.
/// @param f Output parsed frame.
/// @return true if parse succeeded; false otherwise.
bool parse(const uint8_t *buf, uint8_t buf_len, IoFrame &f);

// ============================================================================
// Device Types — from the IO-Homecontrol specification
// ============================================================================

/// @brief Device type identifiers reported by IO‑Homecontrol products.
/// The numeric values follow the official specification. Do not reassign or reorder these.
enum class DeviceType : uint8_t {
  UNKNOWN = 0x00,                        ///< Unknown/unspecified device.
  VENETIAN_BLIND = 0x01,                 ///< Venetian blind.
  ROLLER_SHUTTER = 0x02,                 ///< Roller shutter.
  AWNING = 0x03,                         ///< Awning.
  WINDOW_OPENER = 0x04,                  ///< Window opening actuator.
  GARAGE_OPENER = 0x05,                  ///< Garage door opener.
  LIGHT = 0x06,                          ///< Binary light.
  GATE_OPENER = 0x07,                    ///< Gate opener.
  ROLLING_DOOR_OPENER = 0x08,            ///< Rolling door opener.
  LOCK = 0x09,                           ///< Lock.
  BLIND = 0x0A,                          ///< Generic blind.
  SCREEN = 0x0B,                         ///< Insect/privacy screen.
  BEACON = 0x0C,                         ///< Beacon (unpaired/announcement).
  DUAL_SHUTTER = 0x0D,                   ///< Dual-section shutter.
  HEATING_TEMPERATURE_INTERFACE = 0x0E,  ///< Heating temperature interface.
  ON_OFF_SWITCH = 0x0F,                  ///< Generic on/off switch.
  HORIZONTAL_AWNING = 0x10,              ///< Horizontal awning (open/close inverted).
  EXTERNAL_VENETIAN_BLIND = 0x11,        ///< External venetian blind.
  LOUVRE_BLIND = 0x12,                   ///< Louvre blind.
  CURTAIN_TRACK = 0x13,                  ///< Curtain track.
  VENTILATION_POINT = 0x14,              ///< Ventilation point.
  EXTERIOR_HEATING = 0x15,               ///< Exterior heating.
  HEAT_PUMP = 0x16,                      ///< Heat pump.
  INTRUSION_ALARM = 0x17,                ///< Intrusion alarm.
  SWINGING_SHUTTER = 0x18,               ///< Swinging shutter.
};

/// @brief High‑level capability class derived from DeviceType.
enum class DeviceCapabilityClass : uint8_t {
  UNKNOWN = 0x00,  ///< Unknown capability.
  COVER = 0x01,    ///< Position‑controlled cover (shutter/blind/awning).
  LIGHT = 0x02,    ///< Binary on/off light.
  SWITCH = 0x03,   ///< Binary on/off switch.
  SENSOR = 0x04,   ///< Sensor device.
  BEACON = 0x05,   ///< Beacon.
  CLIMATE = 0x06,  ///< Climate device (heating/cooling).
  LOCK = 0x07,     ///< Lock.
};

/// @brief Convert a DeviceType to a lowercase string identifier.
/// @param type Device type enum.
/// @return Null‑terminated string name (e.g., "roller_shutter").
const char *device_type_name(DeviceType type);

/// @brief Map a raw IO‑Homecontrol type to the closest ESPHome/Home Assistant entity family.
/// @param type Raw device type.
/// @return Capability class (COVER, LIGHT, SWITCH, etc.).
DeviceCapabilityClass device_capability_class(DeviceType type);

/// @brief Get a human‑readable name for a capability class.
/// @param type Device type (unused, kept for signature compatibility).
/// @return String like "cover", "light", "switch", "unknown".
const char *device_capability_class_name(DeviceType type);

/// @brief Does this device type support precise position control (0–100)?
/// @param type Device type.
/// @return true for cover‑family devices.
bool device_supports_position_control(DeviceType type);

/// @brief Does this device type support binary on/off control?
/// @param type Device type.
/// @return true for lights and switches.
bool device_supports_binary_control(DeviceType type);

/// @brief Does this device type support status request commands (0x03)?
/// @param type Device type.
/// @return true for covers, binary devices, and lock devices.
bool device_supports_status_requests(DeviceType type);

/// @brief Does this device type support binary lock/unlock control via execute commands?
/// @param type Device type.
/// @return true for lock devices.
bool device_supports_lock_control(DeviceType type);

/// @brief Does this device type support tilt (slat angle) control?
/// @param type Device type.
/// @return true for venetian blinds, blinds, external venetian blinds, louvre blinds.
bool device_supports_tilt(DeviceType type);

/// @brief Decode a protocol-packed device type from two metadata bytes.
/// @param type_msb First metadata byte.
/// @param type_subtype Second metadata byte containing the remaining type bits and subtype.
/// @return Decoded device type.
DeviceType decode_packed_device_type(uint8_t type_msb, uint8_t type_subtype);

/// @brief Decode a protocol-packed device subtype from the second metadata byte.
/// @param type_subtype Second metadata byte containing subtype in bits [5:0].
/// @return Manufacturer-specific subtype.
uint8_t decode_packed_device_subtype(uint8_t type_subtype);

/// @brief Human‑readable operation profile name for a device type.
/// Used for logging and diagnostics.
/// @param type Device type.
/// @return String such as "cover_position", "cover_position_tilt", "binary_on_off", "lock", etc.
const char *device_operation_profile_name(DeviceType type);

// ============================================================================
// Device State
// ============================================================================

/// @brief Sentinel value meaning "position is not known yet".
/// Matches POS_UNKNOWN (0xD4 = 212 decimal) for easy debugging.
static constexpr float UNKNOWN_POSITION = 212.0F;
static constexpr uint8_t DEVICE_NAME_BUFFER_SIZE = 32;       ///< Device name storage including null terminator
static constexpr uint8_t DEVICE_NAME_WRITE_CHAR_LIMIT = 15;  ///< Reference write limit before the trailing null.
static constexpr uint8_t DEVICE_NAME_WRITE_PAYLOAD_SIZE =
    DEVICE_NAME_WRITE_CHAR_LIMIT + 1;  ///< Fixed write payload: 15 visible chars plus trailing null/padding.
static constexpr uint16_t LATIN1_CODEPOINT_MAX = 0x00FF;  ///< Highest Unicode code point representable in Latin-1.

/// @brief Validation result for outbound device-name writes.
enum class DeviceNameValidationError : uint8_t {
  NONE = 0x00,              ///< Name is valid and encodable.
  EMPTY = 0x01,             ///< Name is empty after normalization.
  TOO_LONG = 0x02,          ///< Name exceeds the 15-character write limit.
  INVALID_UTF8 = 0x03,      ///< Name contains malformed UTF-8 bytes.
  UNSUPPORTED_CHAR = 0x04,  ///< Name contains characters outside Latin-1.
};

/// @brief Runtime state of a paired IO‑Homecontrol device.
struct IoDevice {
  uint8_t node_id[NODE_ID_SIZE]{};            ///< Device's 3‑byte radio address.
  DeviceType type{DeviceType::UNKNOWN};       ///< Device type (shutter, awning, etc.).
  uint8_t subtype{0};                         ///< Device subtype (manufacturer‑specific).
  char name[DEVICE_NAME_BUFFER_SIZE]{};       ///< Cached UTF-8 device name decoded from Latin-1 wire payloads.
  float position{UNKNOWN_POSITION};           ///< Current position: 0=open, 100=closed, or UNKNOWN_POSITION.
  float tilt{UNKNOWN_POSITION};               ///< Current tilt: 0=closed, 100=open, or UNKNOWN_POSITION.
  float target{UNKNOWN_POSITION};             ///< Target position the device is moving toward.
  bool is_stopped{true};                      ///< True if device is not moving.
  bool inverted{false};                       ///< True if open/close positions are swapped (e.g., horizontal awning).
  bool single_follow_up_poll_pending{false};  ///< True when one legacy settle poll should still be scheduled.
  uint32_t last_status{0};                    ///< millis() timestamp of last received status.
  uint32_t status_poll_interval_ms{0};        ///< Configured follow-up poll interval while a state change is expected.
  uint32_t next_update{0};                    ///< millis() timestamp when we should poll for status next.
  uint32_t poll_deadline{0};        ///< Hard stop for bounded follow-up polling after a command or remote activity.
  uint8_t status_poll_failures{0};  ///< Consecutive background status-poll failures without a valid reply.
  uint8_t auth_poll_failures{0};    ///< Consecutive background poll failures that reached 0x3C auth challenge.
};

/// @brief Convert a hex string (e.g., "123ABC") to a byte array.
/// @param hex Hex string (must be exactly len*2 characters).
/// @param out Output buffer (at least len bytes).
/// @param len Number of bytes to produce.
/// @return true on success; false if hex length mismatch or non‑hex characters.
bool hex_to_bytes(const std::string &hex, uint8_t *out, uint8_t len);
/// @brief Format a 3‑byte node ID as a 6‑character uppercase hex string.
/// @param id 3‑byte node ID.
/// @return Hex string (e.g., "123ABC").
std::string node_id_to_string(const uint8_t id[NODE_ID_SIZE]);
/// @brief Decode a device-name payload from IO-homecontrol's Latin-1 wire format into UTF-8.
/// Some devices prepend an extra byte before the first character and many pad the payload with
/// trailing 0x00 or 0x20 bytes. This helper normalizes those quirks and truncates the result to
/// fit DEVICE_NAME_BUFFER_SIZE - 1 bytes when copied into IoDevice::name.
/// @param data Raw payload pointer from CMD_GET_NAME_RESP.
/// @param len Raw payload length in bytes.
/// @return Normalized UTF-8 name, or an empty string when the payload carries no usable characters.
std::string decode_device_name_payload(const uint8_t *data, uint8_t len);
/// @brief Trim leading and trailing ASCII whitespace from a string.
/// This is shared by device-name validation and management actions so both paths normalize input
/// consistently before comparing or writing device metadata.
/// @param value Input string.
/// @return Copy of the string with leading and trailing ASCII whitespace removed.
std::string trim_ascii_whitespace(const std::string &value);
/// @brief Validate and encode a user-supplied UTF-8 device name into the fixed Latin-1 write payload.
/// The outbound payload is a fixed 16-byte field: up to 15 Latin-1 characters followed by trailing
/// zero padding. Leading and trailing ASCII whitespace are trimmed before validation so write-back
/// verification against the device's padded storage is deterministic.
/// @param name User-supplied UTF-8 device name.
/// @param payload Output buffer for the fixed write payload (16 bytes, zero-padded on success).
/// @param normalized_name Output normalized UTF-8 name used for later verification/logging.
/// @return Validation status indicating success or the reason the name cannot be written.
DeviceNameValidationError encode_device_name_payload(const std::string &name,
                                                     uint8_t payload[DEVICE_NAME_WRITE_PAYLOAD_SIZE],
                                                     std::string &normalized_name);
/// @brief Return a stable symbolic name for a device-name validation result.
/// @param error Validation result.
/// @return Uppercase symbolic name.
const char *device_name_validation_error_name(DeviceNameValidationError error);
/// @brief Return a human-readable explanation for a device-name validation result.
/// @param error Validation result.
/// @return Short message suitable for logs and Home Assistant action events.
const char *device_name_validation_error_description(DeviceNameValidationError error);
/// @brief Determine whether a device type has inverted position mapping by default.
/// @param type Device type.
/// @return true for horizontal awnings; false otherwise.
bool default_inverted_for_type(DeviceType type);
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
/// @brief Decode target/current position values from a status frame.
/// @param target_raw 16‑bit raw target value.
/// @param current_raw 16‑bit raw current value.
/// @param is_stopped True if device reports stopped.
/// @param target Output target position (0–100 or UNKNOWN_POSITION).
/// @param position Output current position (0–100 or UNKNOWN_POSITION).
void decode_position_report(uint16_t target_raw, uint16_t current_raw, bool is_stopped, float &target, float &position);
/// @brief Has the device reached its target within tolerance?
/// @param target Target position (0–100 or UNKNOWN_POSITION).
/// @param position Current position (0–100 or UNKNOWN_POSITION).
/// @return true if positions match within STATUS_POS_TOLERANCE_RAW.
bool has_reached_target_position(float target, float position);
/// @brief Decode tilt angle from raw 16‑bit value.
/// @param tilt_raw Raw tilt value from status frame.
/// @return Tilt percentage (0 = closed, 100 = open) or UNKNOWN_POSITION.
float decode_tilt_report(uint16_t tilt_raw);
/// @brief Compute CRC‑CCITT (poly 0x1021, init 0x0000) over a buffer.
/// On SX1276 this is done in hardware; on SX1262 it is computed in software.
/// @param data Pointer to data bytes.
/// @param len Number of bytes.
/// @return 16‑bit CRC value.
uint16_t crc_ccitt(const uint8_t *data, uint8_t len);

}  // namespace home_io_control
}  // namespace esphome
