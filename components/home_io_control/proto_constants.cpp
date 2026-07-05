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
    case CMD_SET_SENSOR:
      return "SET_SENSOR";
    case CMD_SET_SENSOR_ACK:
      return "SET_SENSOR_ACK";
    case CMD_WRITE_PRIVATE:
      return "WRITE_PRIVATE";
    case CMD_WRITE_PRIVATE_ACK:
      return "WRITE_PRIVATE_ACK";
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
    case CMD_KEY_INIT:
      return "KEY_INIT";
    case CMD_KEY_TRANSFER:
      return "KEY_TRANSFER";
    case CMD_KEY_CONFIRM:
      return "KEY_CONFIRM";
    case CMD_ADDRESS_REQ:
      return "ADDRESS_REQ";
    case CMD_ADDRESS_RESP:
      return "ADDRESS_RESP";
    case CMD_LAUNCH_KEY_TRANSFER:
      return "LAUNCH_KEY_TRANSFER";
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
    case CMD_GET_INFO2:
      return "GET_INFO2";
    case CMD_GET_INFO2_RESP:
      return "GET_INFO2_RESP";
    case CMD_SET_CONFIG1:
      return "SET_CONFIG1";
    case CMD_SET_CONFIG1_RESP:
      return "SET_CONFIG1_RESP";
    case CMD_STATUS_UPDATE:
      return "STATUS_UPDATE";
    case CMD_STATUS_UPDATE_RESP:
      return "STATUS_UPDATE_RESP";
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
    case ATT_CLASS_5S:
      return "5s";
    case ATT_CLASS_10S:
      return "10s";
    case ATT_CLASS_20S:
      return "20s";
    case ATT_CLASS_40S:
      return "40s";
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

const char *command_result_name(uint8_t result) {
  switch (result) {
    case RESULT_UNKNOWN_STATUS_REPLY:
      return "UNKNOWN_STATUS_REPLY";
    case RESULT_COMMAND_COMPLETED_OK:
      return "COMMAND_COMPLETED_OK";
    case RESULT_NO_CONTACT:
      return "NO_CONTACT";
    case RESULT_MANUALLY_OPERATED:
      return "MANUALLY_OPERATED";
    case RESULT_BLOCKED:
      return "BLOCKED";
    case RESULT_WRONG_SYSTEMKEY:
      return "WRONG_SYSTEMKEY";
    case RESULT_PRIORITY_LEVEL_LOCKED:
      return "PRIORITY_LEVEL_LOCKED";
    case RESULT_REACHED_WRONG_POSITION:
      return "REACHED_WRONG_POSITION";
    case RESULT_ERROR_DURING_EXECUTION:
      return "ERROR_DURING_EXECUTION";
    case RESULT_NO_EXECUTION:
      return "NO_EXECUTION";
    case RESULT_CALIBRATING:
      return "CALIBRATING";
    case RESULT_POWER_CONSUMPTION_TOO_HIGH:
      return "POWER_CONSUMPTION_TOO_HIGH";
    case RESULT_POWER_CONSUMPTION_TOO_LOW:
      return "POWER_CONSUMPTION_TOO_LOW";
    case RESULT_LOCK_POSITION_OPEN:
      return "LOCK_POSITION_OPEN";
    case RESULT_MOTION_TIME_TOO_LONG:
      return "MOTION_TIME_TOO_LONG";
    case RESULT_THERMAL_PROTECTION:
      return "THERMAL_PROTECTION";
    case RESULT_PRODUCT_NOT_OPERATIONAL:
      return "PRODUCT_NOT_OPERATIONAL";
    case RESULT_FILTER_MAINTENANCE_NEEDED:
      return "FILTER_MAINTENANCE_NEEDED";
    case RESULT_BATTERY_LEVEL:
      return "BATTERY_LEVEL";
    case RESULT_TARGET_MODIFIED:
      return "TARGET_MODIFIED";
    case RESULT_MODE_NOT_IMPLEMENTED:
      return "MODE_NOT_IMPLEMENTED";
    case RESULT_COMMAND_INCOMPATIBLE_TO_MOVEMENT:
      return "COMMAND_INCOMPATIBLE_TO_MOVEMENT";
    case RESULT_USER_ACTION:
      return "USER_ACTION";
    case RESULT_DEAD_BOLT_ERROR:
      return "DEAD_BOLT_ERROR";
    case RESULT_AUTOMATIC_CYCLE_ENGAGED:
      return "AUTOMATIC_CYCLE_ENGAGED";
    case RESULT_WRONG_LOAD_CONNECTED:
      return "WRONG_LOAD_CONNECTED";
    case RESULT_COLOUR_NOT_REACHABLE:
      return "COLOUR_NOT_REACHABLE";
    case RESULT_TARGET_NOT_REACHABLE:
      return "TARGET_NOT_REACHABLE";
    case RESULT_BAD_INDEX_RECEIVED:
      return "BAD_INDEX_RECEIVED";
    case RESULT_COMMAND_OVERRULED:
      return "COMMAND_OVERRULED";
    case RESULT_NODE_WAITING_FOR_POWER:
      return "NODE_WAITING_FOR_POWER";
    case RESULT_NODE_LOCKED:
      return "NODE_LOCKED";
    case RESULT_WRONG_POSITION:
      return "WRONG_POSITION";
    case RESULT_LIMITS_NOT_SET:
      return "LIMITS_NOT_SET";
    case RESULT_IP_NOT_SET:
      return "IP_NOT_SET";
    case RESULT_OUT_OF_RANGE:
      return "OUT_OF_RANGE";
    case RESULT_PRIORITY_LOCKED_NON_EXEC:
      return "PRIORITY_LOCKED";
    case RESULT_INFORMATION_CODE:
      return "INFORMATION_CODE";
    case RESULT_PARAMETER_LIMITED:
      return "PARAMETER_LIMITED";
    case RESULT_LIMITATION_BY_LOCAL_USER:
      return "LIMITATION_BY_LOCAL_USER";
    case RESULT_LIMITATION_BY_USER:
      return "LIMITATION_BY_USER";
    case RESULT_LIMITATION_BY_RAIN:
      return "LIMITATION_BY_RAIN";
    case RESULT_LIMITATION_BY_TIMER:
      return "LIMITATION_BY_TIMER";
    case RESULT_LIMITATION_BY_SCD:
      return "LIMITATION_BY_SCD";
    case RESULT_LIMITATION_BY_UPS:
      return "LIMITATION_BY_UPS";
    case RESULT_LIMITATION_BY_UNKNOWN_DEVICE:
      return "LIMITATION_BY_UNKNOWN_DEVICE";
    case RESULT_LIMITATION_BY_SAAC:
      return "LIMITATION_BY_SAAC";
    case RESULT_LIMITATION_BY_WIND:
      return "LIMITATION_BY_WIND";
    case RESULT_LIMITATION_BY_MYSELF:
      return "LIMITATION_BY_MYSELF";
    case RESULT_LIMITATION_BY_AUTOMATIC_CYCLE:
      return "LIMITATION_BY_AUTOMATIC_CYCLE";
    case RESULT_LIMITATION_BY_EMERGENCY:
      return "LIMITATION_BY_EMERGENCY";
    default:
      return "UNKNOWN_RESULT_CODE";
  }
}

const char *command_result_description(uint8_t result) {
  switch (result) {
    case RESULT_UNKNOWN_STATUS_REPLY:
      return "unknown reply";
    case RESULT_COMMAND_COMPLETED_OK:
      return "no errors detected";
    case RESULT_NO_CONTACT:
      return "no communication to node";
    case RESULT_MANUALLY_OPERATED:
      return "manually operated by a user";
    case RESULT_BLOCKED:
      return "node has been blocked by an object";
    case RESULT_WRONG_SYSTEMKEY:
      return "node contains the wrong system key";
    case RESULT_PRIORITY_LEVEL_LOCKED:
      return "node is locked on this priority level";
    case RESULT_REACHED_WRONG_POSITION:
      return "node stopped in another position than expected";
    case RESULT_ERROR_DURING_EXECUTION:
      return "an error occurred during command execution";
    case RESULT_NO_EXECUTION:
      return "no movement of the node parameter";
    case RESULT_CALIBRATING:
      return "node is calibrating the parameters";
    case RESULT_POWER_CONSUMPTION_TOO_HIGH:
      return "node power consumption is too high";
    case RESULT_POWER_CONSUMPTION_TOO_LOW:
      return "node power consumption is too low";
    case RESULT_LOCK_POSITION_OPEN:
      return "door open during lock command";
    case RESULT_MOTION_TIME_TOO_LONG:
      return "target was not reached in time";
    case RESULT_THERMAL_PROTECTION:
      return "node has gone into thermal protection mode";
    case RESULT_PRODUCT_NOT_OPERATIONAL:
      return "node is not currently operational";
    case RESULT_FILTER_MAINTENANCE_NEEDED:
      return "filter needs maintenance";
    case RESULT_BATTERY_LEVEL:
      return "battery level is low";
    case RESULT_TARGET_MODIFIED:
      return "node modified the requested target value";
    case RESULT_MODE_NOT_IMPLEMENTED:
      return "node does not support the received mode";
    case RESULT_COMMAND_INCOMPATIBLE_TO_MOVEMENT:
      return "node cannot move in the requested direction";
    case RESULT_USER_ACTION:
      return "user action overrode the command";
    case RESULT_DEAD_BOLT_ERROR:
      return "dead bolt error";
    case RESULT_AUTOMATIC_CYCLE_ENGAGED:
      return "node has gone into automatic cycle mode";
    case RESULT_WRONG_LOAD_CONNECTED:
      return "wrong load connected to node";
    case RESULT_COLOUR_NOT_REACHABLE:
      return "node cannot reach the requested colour";
    case RESULT_TARGET_NOT_REACHABLE:
      return "node cannot reach the requested target position";
    case RESULT_BAD_INDEX_RECEIVED:
      return "invalid index received";
    case RESULT_COMMAND_OVERRULED:
      return "command was overruled by a newer command";
    case RESULT_NODE_WAITING_FOR_POWER:
      return "node is waiting for power";
    case RESULT_NODE_LOCKED:
      return "node is locked";
    case RESULT_WRONG_POSITION:
      return "wrong position";
    case RESULT_LIMITS_NOT_SET:
      return "limits are not set";
    case RESULT_IP_NOT_SET:
      return "intermediate position is not set";
    case RESULT_OUT_OF_RANGE:
      return "requested value is out of range";
    case RESULT_PRIORITY_LOCKED_NON_EXEC:
      return "command priority too low, node rejected execution";
    case RESULT_INFORMATION_CODE:
      return "information-only result with unknown semantics";
    case RESULT_PARAMETER_LIMITED:
      return "parameter was limited by an unknown device";
    case RESULT_LIMITATION_BY_LOCAL_USER:
      return "parameter was limited by the local button";
    case RESULT_LIMITATION_BY_USER:
      return "parameter was limited by a remote control";
    case RESULT_LIMITATION_BY_RAIN:
      return "parameter was limited by a rain sensor";
    case RESULT_LIMITATION_BY_TIMER:
      return "parameter was limited by a timer";
    case RESULT_LIMITATION_BY_SCD:
      return "parameter was limited by a security controlling actuator";
    case RESULT_LIMITATION_BY_UPS:
      return "parameter was limited by a power supply";
    case RESULT_LIMITATION_BY_UNKNOWN_DEVICE:
      return "parameter was limited by an unknown device";
    case RESULT_LIMITATION_BY_SAAC:
      return "parameter was limited by a standalone automatic controller";
    case RESULT_LIMITATION_BY_WIND:
      return "parameter was limited by a wind sensor";
    case RESULT_LIMITATION_BY_MYSELF:
      return "parameter was limited by the node itself";
    case RESULT_LIMITATION_BY_AUTOMATIC_CYCLE:
      return "parameter was limited by an automatic cycle";
    case RESULT_LIMITATION_BY_EMERGENCY:
      return "parameter was limited by an emergency";
    default:
      return "unknown result code";
  }
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
