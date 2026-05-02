#pragma once

/// @file proto_frame.h
/// @brief IO-Homecontrol 2W protocol definitions.
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
/// The controller hops between channels every ~2.7ms when idle, listening for
/// incoming frames. Commands are typically sent on channel 2.
static constexpr uint32_t FREQ_CH1 = 868250000;  ///< Channel 1: 868.25 MHz (2W only)
static constexpr uint32_t FREQ_CH2 = 868950000;  ///< Channel 2: 868.95 MHz (1W and 2W, primary)
static constexpr uint32_t FREQ_CH3 = 869850000;  ///< Channel 3: 869.85 MHz (2W only)

/// Preamble is a sequence of 0xAA bytes that precedes every frame.
/// The first frame in an exchange uses a long preamble (1024 bytes = 8192 bits)
/// so the receiver has time to detect it while hopping. Subsequent frames in the
/// same exchange use a short preamble (8 bytes) since both sides are already
/// on the same channel. Solar-powered devices need the long preamble to wake up.
static constexpr uint16_t LONG_PREAMBLE = 1024;                ///< 1024 bytes for initial/start frames
static constexpr uint16_t SHORT_PREAMBLE = 8;                  ///< 8 bytes for response/continuation frames
static constexpr uint16_t SX1262_AUTH_RESPONSE_PREAMBLE = 46;  ///< SX1262-specific 0x3D preamble workaround

/// Timing constants for frequency hopping and response waiting.
static constexpr int32_t HOP_TIME_US = 2700;             ///< Time per channel when hopping (2.7ms)
static constexpr int32_t RESPONSE_CHANNEL_WAIT_MS = 50;  ///< Per-channel dwell while waiting for an exchange response
static constexpr int32_t RESPONSE_WAIT_MS = 500;         ///< Wait for response to non-start frame
static constexpr int32_t RESPONSE_START_WAIT_MS = 800;   ///< Wait for response to start frame (longer)
static constexpr int32_t RESPONSE_AUTH_WAIT_MS =
    RESPONSE_WAIT_MS;                                    ///< Wait for final response after challenge response
static constexpr int32_t EXCHANGE_RETRY_DELAY_MS = 100;  ///< Gap between retries within one HA command
static constexpr uint8_t EXCHANGE_RETRY_COUNT = 4;       ///< Attempts per command before reporting failure

// ============================================================================
// Frame Constants
// ============================================================================

static constexpr uint8_t NODE_ID_SIZE = 3;     ///< Device/node addresses are 3 bytes (e.g., "123ABC")
static constexpr uint8_t HMAC_SIZE = 6;        ///< Authentication HMAC is 6 bytes (truncated AES output)
static constexpr uint8_t AES_KEY_SIZE = 16;    ///< AES-128 key size
static constexpr uint8_t AES_BLOCK_SIZE = 16;  ///< AES block size
static constexpr uint8_t IV_SIZE = 16;         ///< Initialization vector size for AES
static constexpr uint8_t IV_PADDING = 0x55;    ///< Padding byte used in IV construction

static constexpr uint8_t FRAME_MIN_SIZE = 9;        ///< Minimum frame: CTRL0+CTRL1+DST(3)+SRC(3)+CMD(1)
static constexpr uint8_t FRAME_MAX_SIZE = 32;       ///< Maximum frame size (9 header + 23 data)
static constexpr uint8_t FRAME_MAX_DATA_SIZE = 23;  ///< Maximum data bytes after command ID

/// Control byte 0 (CTRL0) bit definitions.
/// CTRL0 encodes frame flags and the total frame length.
/// Bits [4:0] = frame_length - 1 (so 0x08 means 9 bytes total).
static constexpr uint8_t CTRL0_END = 0x80;          ///< Bit 7: last frame in exchange
static constexpr uint8_t CTRL0_START = 0x40;        ///< Bit 6: first frame in exchange (uses long preamble)
static constexpr uint8_t CTRL0_PROTOCOL_1W = 0x20;  ///< Bit 5: 1=OneWay protocol, 0=TwoWay protocol
static constexpr uint8_t CTRL0_LENGTH_MASK = 0x1F;  ///< Bits [4:0]: frame length - 1

/// Control byte 1 (CTRL1) bit definitions.
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
static constexpr uint8_t CMD_GET_INFO2 = 0x56;       ///< Request device type/model info
static constexpr uint8_t CMD_GET_INFO2_RESP = 0x57;  ///< Device type/model response

// Configuration and status update commands
static constexpr uint8_t CMD_SET_CONFIG1 = 0x6F;         ///< Configure device to auto-send status updates
static constexpr uint8_t CMD_SET_CONFIG1_RESP = 0x70;    ///< Config response
static constexpr uint8_t CMD_STATUS_UPDATE = 0x71;       ///< Device-initiated status update (needs auth)
static constexpr uint8_t CMD_STATUS_UPDATE_RESP = 0x72;  ///< Acknowledge status update
static constexpr uint8_t CMD_ERROR_RESP = 0xFE;          ///< Error response to any command

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

/// Status byte flags in CMD_PRIVATE_RESP and CMD_STATUS_UPDATE.
static constexpr uint8_t STATUS_STOPPED = 0x01;   ///< Byte 0 bit 0: device is not moving
static constexpr uint8_t STATUS_EXPECTED = 0x80;  ///< Byte 1 bit 7: device will send auto status update

// ============================================================================
// Cryptographic Constants
// ============================================================================

/// The transfer key is a hardcoded key used ONLY during pairing to obfuscate
/// the system key during over-the-air transfer. It is NOT the system key.
/// This is the same across all IO-Homecontrol devices worldwide.
static constexpr uint8_t TRANSFER_KEY[AES_KEY_SIZE] = {0x34, 0xC3, 0x46, 0x6E, 0xD8, 0x8F, 0x4E, 0x8E,
                                                       0x16, 0xAA, 0x47, 0x39, 0x49, 0x88, 0x43, 0x73};

/// Broadcast address for device discovery (0x00003B).
static constexpr uint8_t BROADCAST_DISCOVER[NODE_ID_SIZE] = {0x00, 0x00, 0x3B};

// ============================================================================
// Frame Structure
// ============================================================================

/// An IO-Homecontrol frame as parsed from the radio.
/// Over the air: [CTRL0][CTRL1][DST 3B][SRC 3B][CMD][DATA 0-23B][CRC 2B]
/// The CRC is handled by the SX1276 hardware (IoHomeOn mode) and is not
/// included in this struct.
struct IoFrame {
  uint8_t ctrl0;                      ///< Control byte 0: flags + length
  uint8_t ctrl1;                      ///< Control byte 1: low power, beacon, etc.
  uint8_t dst[NODE_ID_SIZE];          ///< Destination node ID (3 bytes)
  uint8_t src[NODE_ID_SIZE];          ///< Source node ID (3 bytes)
  uint8_t cmd;                        ///< Command ID
  uint8_t data[FRAME_MAX_DATA_SIZE];  ///< Command parameters (0-23 bytes)
  uint8_t data_len;                   ///< Actual length of data
};

// --- Frame construction and parsing ---
void init_frame(IoFrame &f, bool is_2w = true, bool start = false, bool end = false, bool low_power = false);
void set_dst(IoFrame &f, const uint8_t id[NODE_ID_SIZE]);
void set_src(IoFrame &f, const uint8_t id[NODE_ID_SIZE]);
bool set_cmd(IoFrame &f, uint8_t cmd, const uint8_t *params = nullptr, uint8_t params_len = 0);
uint8_t frame_length(const IoFrame &f);
bool is_start(const IoFrame &f);
bool is_end(const IoFrame &f);
uint8_t serialize(const IoFrame &f, uint8_t *buf, uint8_t buf_size);
bool parse(const uint8_t *buf, uint8_t buf_len, IoFrame &f);

// ============================================================================
// Device Types — from the KLF 200 API / Velux specification
// ============================================================================

enum class DeviceType : uint8_t {
  UNKNOWN = 0x00,
  ADJUSTABLE_SLAT_SHUTTER = 0x09,
  VENETIAN_BLIND = 0x01,
  ROLLER_SHUTTER = 0x02,
  SCREEN = 0x0B,
  AWNING = 0x03,
  WINDOW_OPENER = 0x04,
  GARAGE_OPENER = 0x05,
  LIGHT = 0x06,
  GATE_OPENER = 0x07,
  ROLLING_DOOR_OPENER = 0x08,
  BLIND = 0x0A,
  DUAL_SHUTTER = 0x0D,
  ON_OFF_SWITCH = 0x0F,
  HORIZONTAL_AWNING = 0x10,  ///< Note: horizontal awnings have inverted open/close
  EXTERIOR_BLIND = 0x14,
  EXTERNAL_VENETIAN_BLIND = 0x11,
  LOUVRE_BLIND = 0x12,
  CURTAIN = 0x15,
  CURTAIN_TRACK = 0x13,
  PERGOLA = 0x16,
  EXTERIOR_SCREEN = 0x17,
  SWINGING_SHUTTER = 0x18,
  LOCK = 0x19,
  HEATING = 0x1A,
  BEACON = 0x1B,
  SENSOR = 0x1C,
};

enum class DeviceCapabilityClass : uint8_t {
  UNKNOWN = 0x00,
  COVER = 0x01,
  LIGHT = 0x02,
  SWITCH = 0x03,
  SENSOR = 0x04,
  BEACON = 0x05,
  CLIMATE = 0x06,
  LOCK = 0x07,
};

const char *device_type_name(DeviceType type);
/// Map a raw IO-homecontrol type to the closest ESPHome/Home Assistant entity family.
DeviceCapabilityClass device_capability_class(DeviceType type);
const char *device_capability_class_name(DeviceType type);
/// Operation helpers answer the stricter question of which runtime commands we have enough
/// evidence to expose safely for a known device family.
bool device_supports_position_control(DeviceType type);
bool device_supports_binary_control(DeviceType type);
bool device_supports_status_requests(DeviceType type);
const char *device_operation_profile_name(DeviceType type);

// ============================================================================
// Device State
// ============================================================================

/// Sentinel value meaning "position is not known yet".
/// Matches POS_UNKNOWN (0xD4 = 212 decimal) for easy debugging.
static constexpr float UNKNOWN_POSITION = 212.0F;

/// Runtime state of a paired IO-Homecontrol device.
struct IoDevice {
  uint8_t node_id[NODE_ID_SIZE]{};       ///< Device's 3-byte radio address
  DeviceType type{DeviceType::UNKNOWN};  ///< Device type (shutter, awning, etc.)
  uint8_t subtype{0};                    ///< Device subtype (manufacturer-specific)
  char name[32]{};                       ///< Device name (from device, Latin-1 encoded)
  float position{UNKNOWN_POSITION};      ///< Current position: 0=open, 100=closed, or UNKNOWN_POSITION
  float target{UNKNOWN_POSITION};        ///< Target position the device is moving toward
  bool is_stopped{true};                 ///< True if device is not currently moving
  bool inverted{false};                  ///< True if open/close positions are swapped (e.g., horizontal awning)
  uint32_t last_status{0};               ///< millis() timestamp of last received status
  uint32_t next_update{0};               ///< millis() timestamp when we should poll for status next
};

/// Convert a hex string (e.g., "123ABC") to a byte array.
bool hex_to_bytes(const std::string &hex, uint8_t *out, uint8_t len);
std::string node_id_to_string(const uint8_t id[NODE_ID_SIZE]);
bool default_inverted_for_type(DeviceType type);
void decode_position_report(uint16_t target_raw, uint16_t current_raw, bool is_stopped, float &target, float &position);
uint16_t crc_ccitt(const uint8_t *data, uint8_t len);

}  // namespace home_io_control
}  // namespace esphome
