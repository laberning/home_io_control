/// @file proto_constants.cpp
/// @brief Name/description lookups for IO-Homecontrol commands, results and enumerations.
/// @ingroup hioc_protocol

#include "proto_constants.h"

namespace esphome {
namespace home_io_control {

const char *command_name(uint8_t cmd) {
  switch (cmd) {
    case CMD_EXECUTE:
      return "EXECUTE";
    case CMD_ACTIVATE_MODE:
      return "ACTIVATE_MODE";
    case CMD_PRIVATE:
      return "PRIVATE";
    case CMD_PRIVATE_RESP:
      return "PRIVATE_RESP";
    case CMD_PRIVATE2:
      return "PRIVATE2";
    case CMD_PRIVATE2_RESP:
      return "PRIVATE2_RESP";
    case CMD_PRIORITY_LEVEL_REQ:
      return "PRIORITY_LEVEL_REQ";
    case CMD_PRIORITY_LEVEL_RESP:
      return "PRIORITY_LEVEL_RESP";
    case CMD_IDENTIFY:
      return "IDENTIFY";
    case CMD_WRITE_PRIVATE:
      return "WRITE_PRIVATE";
    case CMD_WRITE_PRIVATE_ACK:
      return "WRITE_PRIVATE_ACK";
    case CMD_LIMITATION_STATUS_REQ:
      return "LIMITATION_STATUS_REQ";
    case CMD_LIMITATION_STATUS_RESP:
      return "LIMITATION_STATUS_RESP";
    case CMD_DISCOVER_REQ:
      return "DISCOVER_REQ";
    case CMD_DISCOVER_RESP:
      return "DISCOVER_RESP";
    case CMD_DISCOVER_SPE_REQ:
      return "DISCOVER_SPE_REQ";
    case CMD_DISCOVER_SPE_RESP:
      return "DISCOVER_SPE_RESP";
    case CMD_DISCOVER_CONFIRM:
      return "DISCOVER_CONFIRM";
    case CMD_DISCOVER_CONFIRM_ACK:
      return "DISCOVER_CONFIRM_ACK";
    case CMD_DISCOVER_ALT_REQ:
      return "DISCOVER_ALT_REQ";
    case CMD_DISCOVER_ALT_RESP:
      return "DISCOVER_ALT_RESP";
    case CMD_ONEWAY_ADD_CONTROLLER:
      return "ONEWAY_ADD_CONTROLLER";
    case CMD_ONEWAY_REMOVE:
      return "ONEWAY_REMOVE";
    case CMD_KEY_INIT:
      return "KEY_INIT";
    case CMD_KEY_TRANSFER:
      return "KEY_TRANSFER";
    case CMD_KEY_CONFIRM:
      return "KEY_CONFIRM";
    case CMD_NODE_VERIFY_REQ:
      return "NODE_VERIFY_REQ";
    case CMD_NODE_VERIFY_RESP:
      return "NODE_VERIFY_RESP";
    case CMD_LAUNCH_KEY_TRANSFER:
      return "LAUNCH_KEY_TRANSFER";
    case CMD_TRANSFER_OPEN:
      return "TRANSFER_OPEN";
    case CMD_TRANSFER_OPEN_RESP:
      return "TRANSFER_OPEN_RESP";
    case CMD_UNKNOWN4A_REQ:
      return "UNKNOWN4A_REQ";
    case CMD_UNKNOWN4A_RESP:
      return "UNKNOWN4A_RESP";
    case CMD_CHALLENGE_REQ:
      return "CHALLENGE_REQ";
    case CMD_CHALLENGE_RESP:
      return "CHALLENGE_RESP";
    case CMD_GET_NAME:
      return "GET_NAME";
    case CMD_GET_NAME_RESP:
      return "GET_NAME_RESP";
    case CMD_SET_NAME:
      return "SET_NAME";
    case CMD_SET_NAME_RESP:
      return "SET_NAME_RESP";
    case CMD_GET_INFO1:
      return "GET_INFO1";
    case CMD_GET_INFO1_RESP:
      return "GET_INFO1_RESP";
    case CMD_GET_INFO2:
      return "GET_INFO2";
    case CMD_GET_INFO2_RESP:
      return "GET_INFO2_RESP";
    case CMD_GET_GENERAL_INFO3:
      return "GET_GENERAL_INFO3";
    case CMD_GET_GENERAL_INFO3_RESP:
      return "GET_GENERAL_INFO3_RESP";
    case CMD_SET_CONFIG1:
      return "SET_CONFIG1";
    case CMD_SET_CONFIG1_RESP:
      return "SET_CONFIG1_RESP";
    case CMD_SUBSCRIBE_EVENT:
      return "SUBSCRIBE_EVENT";
    case CMD_STATUS_UPDATE:
      return "STATUS_UPDATE";
    case CMD_STATUS_UPDATE_RESP:
      return "STATUS_UPDATE_RESP";
    case CMD_SEND_RAW_MESSAGE:
      return "SEND_RAW_MESSAGE";
    case CMD_READ_GROUPS:
      return "READ_GROUPS";
    case CMD_REBOOT:
      return "REBOOT";
    case CMD_SERVICE_STATUS_ACK:
      return "SERVICE_STATUS_ACK";
    case CMD_ERROR_RESP:
      return "ERROR_RESP";
    default:
      return "UNKNOWN_CMD";
  }
}

const char *manufacturer_name(uint8_t id) {
  switch (id) {
    case MANUFACTURER_VELUX:
      return "VELUX";
    case MANUFACTURER_SOMFY:
      return "Somfy";
    case MANUFACTURER_HONEYWELL:
      return "Honeywell";
    case MANUFACTURER_HORMANN:
      return "Hörmann";
    case MANUFACTURER_ASSA_ABLOY:
      return "ASSA ABLOY";
    case MANUFACTURER_NIKO:
      return "Niko";
    case MANUFACTURER_WINDOW_MASTER:
      return "WINDOW MASTER";
    case MANUFACTURER_RENSON:
      return "Renson";
    case MANUFACTURER_CIAT:
      return "CIAT";
    case MANUFACTURER_SECUYOU:
      return "Secuyou";
    case MANUFACTURER_OVERKIZ:
      return "OVERKIZ";
    case MANUFACTURER_ATLANTIC_GROUP:
      return "Atlantic Group";
    default:
      return "unknown";
  }
}

const char *att_class_name(uint8_t att_class) {
  switch (att_class) {
    case ATT_CLASS_5MS:
      return "5ms";
    case ATT_CLASS_10MS:
      return "10ms";
    case ATT_CLASS_20MS:
      return "20ms";
    case ATT_CLASS_40MS:
      return "40ms";
    default:
      return "unknown";
  }
}

const char *power_save_mode_name(uint8_t mode) {
  switch (mode) {
    case POWER_SAVE_ALWAYS_ALIVE:
      return "always_alive";
    case POWER_SAVE_LOW_POWER:
      return "low_power";
    default:
      return "unknown";
  }
}

const char *originator_name(uint8_t originator) {
  switch (originator) {
    case ORIGINATOR_LOCAL_USER:
      return "local_user";
    case ORIGINATOR_USER_REMOTE:
      return "user_remote";
    case ORIGINATOR_RAIN_SENSOR:
      return "rain_sensor";
    case ORIGINATOR_TIMER:
      return "timer";
    case ORIGINATOR_SECURITY:
      return "security";
    case ORIGINATOR_UPS:
      return "ups";
    case ORIGINATOR_SMART_CONTROLLER:
      return "smart_controller";
    case ORIGINATOR_LIFESTYLE:
      return "lifestyle";
    case ORIGINATOR_SAAC:
      return "saac";
    case ORIGINATOR_WIND_SENSOR:
      return "wind_sensor";
    case ORIGINATOR_LOAD_SHEDDING:
      return "load_shedding";
    case ORIGINATOR_LOCAL_LIGHT:
      return "local_light";
    case ORIGINATOR_ENVIRONMENT:
      return "environment";
    case ORIGINATOR_MYSELF:
      return "myself";
    case ORIGINATOR_AUTOMATIC_CYCLE:
      return "automatic_cycle";
    case ORIGINATOR_EMERGENCY:
      return "emergency";
    default:
      return "unknown";
  }
}

const char *acei_level_name(uint8_t level) {
  switch (level) {
    case ACEI_LEVEL_PROTECTION_HUMAN:
      return "protection_human";
    case ACEI_LEVEL_PROTECTION_SENSOR:
      return "protection_sensor";
    case ACEI_LEVEL_USER_HIGH:
      return "user_high";
    case ACEI_LEVEL_USER_DEFAULT:
      return "user_default";
    case ACEI_LEVEL_COMFORT_1:
      return "comfort_1";
    case ACEI_LEVEL_COMFORT_2:
      return "comfort_2";
    case ACEI_LEVEL_AUTO_SAAC:
      return "auto_saac";
    case ACEI_LEVEL_AUTO_DEFAULT:
      return "auto_default";
    default:
      return "unknown";
  }
}

// === Result codes ===

namespace {

/// One row per whole-byte result value: its symbolic name and a warn-level description.
struct CommandResultInfo {
  uint8_t result;
  const char *name;
  const char *description;
};

constexpr CommandResultInfo COMMAND_RESULTS[] = {
    {RESULT_UNKNOWN_STATUS_REPLY, "UNKNOWN_STATUS_REPLY", "unknown reply"},
    {RESULT_COMMAND_COMPLETED_OK, "COMMAND_COMPLETED_OK", "no errors detected"},
    {RESULT_NO_CONTACT, "NO_CONTACT", "no communication to node"},
    {RESULT_MANUALLY_OPERATED, "MANUALLY_OPERATED", "manually operated by a user"},
    {RESULT_BLOCKED, "BLOCKED", "node has been blocked by an object"},
    {RESULT_WRONG_SYSTEMKEY, "WRONG_SYSTEMKEY", "node contains the wrong system key"},
    {RESULT_PRIORITY_LEVEL_LOCKED, "PRIORITY_LEVEL_LOCKED", "node is locked on this priority level"},
    {RESULT_REACHED_WRONG_POSITION, "REACHED_WRONG_POSITION", "node stopped in another position than expected"},
    {RESULT_NO_EXECUTION, "NO_EXECUTION", "no movement of the node parameter"},
    {RESULT_CALIBRATING, "CALIBRATING", "node is calibrating the parameters"},
    {RESULT_POWER_CONSUMPTION_TOO_HIGH, "POWER_CONSUMPTION_TOO_HIGH", "node power consumption is too high"},
    {RESULT_POWER_CONSUMPTION_TOO_LOW, "POWER_CONSUMPTION_TOO_LOW", "node power consumption is too low"},
    {RESULT_LOCK_POSITION_OPEN, "LOCK_POSITION_OPEN", "door open during lock command"},
    {RESULT_MOTION_TIME_TOO_LONG, "MOTION_TIME_TOO_LONG", "target was not reached in time"},
    {RESULT_THERMAL_PROTECTION, "THERMAL_PROTECTION", "node has gone into thermal protection mode"},
    {RESULT_FILTER_MAINTENANCE_NEEDED, "FILTER_MAINTENANCE_NEEDED", "filter needs maintenance"},
    {RESULT_BATTERY_LEVEL, "BATTERY_LEVEL", "battery level is low"},
    {RESULT_TARGET_MODIFIED, "TARGET_MODIFIED", "node modified the requested target value"},
    {RESULT_MODE_NOT_IMPLEMENTED, "MODE_NOT_IMPLEMENTED", "node does not support the received mode"},
    {RESULT_COMMAND_INCOMPATIBLE_TO_MOVEMENT, "COMMAND_INCOMPATIBLE_TO_MOVEMENT",
     "node cannot move in the requested direction"},
    {RESULT_USER_ACTION, "USER_ACTION", "user action overrode the command"},
    {RESULT_DEAD_BOLT_ERROR, "DEAD_BOLT_ERROR", "dead bolt error"},
    {RESULT_WRONG_LOAD_CONNECTED, "WRONG_LOAD_CONNECTED", "wrong load connected to node"},
    {RESULT_COLOUR_NOT_REACHABLE, "COLOUR_NOT_REACHABLE", "node cannot reach the requested colour"},
    {RESULT_TARGET_NOT_REACHABLE, "TARGET_NOT_REACHABLE", "node cannot reach the requested target position"},
    {RESULT_BAD_INDEX_RECEIVED, "BAD_INDEX_RECEIVED", "invalid index received"},
    {RESULT_COMMAND_OVERRULED, "COMMAND_OVERRULED", "command was overruled by a newer command"},
    {RESULT_NODE_WAITING_FOR_POWER, "NODE_WAITING_FOR_POWER", "node is waiting for power"},
    {RESULT_WRONG_POSITION, "WRONG_POSITION", "wrong position"},
    {RESULT_LIMITS_NOT_SET, "LIMITS_NOT_SET", "limits are not set"},
    {RESULT_IP_NOT_SET, "IP_NOT_SET", "intermediate position is not set"},
    {RESULT_OUT_OF_RANGE, "OUT_OF_RANGE", "requested value is out of range"},
    {RESULT_COMMAND_UNKNOWN, "COMMAND_UNKNOWN", "the device does not offer this command in its current state"},
    {RESULT_MODE_OR_STATE_UNKNOWN, "MODE_OR_STATE_UNKNOWN", "mode or state number unknown to the device"},
    {RESULT_BAD_MAC, "BAD_MAC", "the device rejected our challenge answer (wrong system key?)"},
    {RESULT_SETUP_REQUIRED, "SETUP_REQUIRED", "the device needs setting up first"},
    {RESULT_PARAMETER_UNKNOWN, "PARAMETER_UNKNOWN", "parameter, alias or access method unknown to the device"},
    {RESULT_ORIGINATOR_DISABLED, "ORIGINATOR_DISABLED", "command originator disabled on the device"},
    {RESULT_PRIORITY_LOCKED, "PRIORITY_LOCKED", "command priority too low, node rejected execution"},
    {RESULT_PRIORITY_SERVICE_NOT_SUPPORTED, "PRIORITY_SERVICE_NOT_SUPPORTED",
     "priority service not supported by the device"},
    {RESULT_PARAMETER_LIMITATION_ADJUSTMENT, "PARAMETER_LIMITATION_ADJUSTMENT",
     "the device adjusted the parameter to a limitation"},
    {RESULT_PARAMETER_INCOHERENCE_ADJUSTMENT, "PARAMETER_INCOHERENCE_ADJUSTMENT",
     "the device adjusted an incoherent parameter"},
    {RESULT_TOTAL_INCOHERENCE, "TOTAL_INCOHERENCE", "the request does not fit what the device implements"},
    {RESULT_SERVICE_FULL, "SERVICE_FULL", "the device has no room left for this request"},
    {RESULT_ALIAS_INCOHERENCE, "ALIAS_INCOHERENCE", "alias incoherence"},
    {RESULT_SESSION_INTERRUPTED, "SESSION_INTERRUPTED", "session interrupted"},
    {RESULT_NOT_IMPLEMENTED, "NOT_IMPLEMENTED", "not implemented by the device"},
    {RESULT_COMMAND_REJECTED, "COMMAND_REJECTED", "the device rejected the command"},
    {RESULT_ALREADY_CONFIGURED, "ALREADY_CONFIGURED", "the device is already configured"},
    {RESULT_INFORMATION_CODE, "INFORMATION_CODE", "information-only result with unknown semantics"},
    {RESULT_PARAMETER_LIMITED, "PARAMETER_LIMITED", "parameter was limited by an unknown device"},
    {RESULT_LIMITATION_BY_LOCAL_USER, "LIMITATION_BY_LOCAL_USER", "parameter was limited by the local button"},
    {RESULT_LIMITATION_BY_USER, "LIMITATION_BY_USER", "parameter was limited by a remote control"},
    {RESULT_LIMITATION_BY_RAIN, "LIMITATION_BY_RAIN", "parameter was limited by a rain sensor"},
    {RESULT_LIMITATION_BY_TIMER, "LIMITATION_BY_TIMER", "parameter was limited by a timer"},
    {RESULT_LIMITATION_BY_SCD, "LIMITATION_BY_SCD", "parameter was limited by a security controlling actuator"},
    {RESULT_LIMITATION_BY_UPS, "LIMITATION_BY_UPS", "parameter was limited by a power supply"},
    {RESULT_LIMITATION_BY_UNKNOWN_DEVICE, "LIMITATION_BY_UNKNOWN_DEVICE", "parameter was limited by an unknown device"},
    {RESULT_LIMITATION_BY_SAAC, "LIMITATION_BY_SAAC", "parameter was limited by a standalone automatic controller"},
    {RESULT_LIMITATION_BY_WIND, "LIMITATION_BY_WIND", "parameter was limited by a wind sensor"},
    {RESULT_LIMITATION_BY_MYSELF, "LIMITATION_BY_MYSELF", "parameter was limited by the node itself"},
    {RESULT_LIMITATION_BY_AUTOMATIC_CYCLE, "LIMITATION_BY_AUTOMATIC_CYCLE",
     "parameter was limited by an automatic cycle"},
    {RESULT_LIMITATION_BY_EMERGENCY, "LIMITATION_BY_EMERGENCY", "parameter was limited by an emergency"},
};

/// @return The table row for `result`, or nullptr when the byte is unmapped.
const CommandResultInfo *find_result(uint8_t result) {
  for (const CommandResultInfo &row : COMMAND_RESULTS) {
    if (row.result == result)
      return &row;
  }
  return nullptr;
}

}  // namespace

const char *command_result_name(uint8_t result) {
  const CommandResultInfo *row = find_result(result);
  return row != nullptr ? row->name : "UNKNOWN_RESULT_CODE";
}

const char *result_code_name(uint8_t code) {
  if (code == 0 || code > ERROR_RESP_CODE_MAX)
    return "UNKNOWN_RESULT_CODE";
  return command_result_name(static_cast<uint8_t>(code << ERROR_RESP_CODE_SHIFT));
}

const char *command_result_description(uint8_t result) {
  const CommandResultInfo *row = find_result(result);
  return row != nullptr ? row->description : "unknown result code";
}

bool is_limitation_result(uint8_t result) {
  switch (result) {
    case RESULT_PARAMETER_LIMITED:
    case RESULT_LIMITATION_BY_LOCAL_USER:
    case RESULT_LIMITATION_BY_USER:
    case RESULT_LIMITATION_BY_RAIN:
    case RESULT_LIMITATION_BY_TIMER:
    case RESULT_LIMITATION_BY_SCD:
    case RESULT_LIMITATION_BY_UPS:
    case RESULT_LIMITATION_BY_UNKNOWN_DEVICE:
    case RESULT_LIMITATION_BY_SAAC:
    case RESULT_LIMITATION_BY_WIND:
    case RESULT_LIMITATION_BY_MYSELF:
    case RESULT_LIMITATION_BY_AUTOMATIC_CYCLE:
    case RESULT_LIMITATION_BY_EMERGENCY:
      return true;
    default:
      return false;
  }
}

}  // namespace home_io_control
}  // namespace esphome
