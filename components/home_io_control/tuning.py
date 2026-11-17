## @file
## @brief ESPHome tuning schema and code generation for Home IO Control.
## @ingroup hioc_codegen
##
## Provides the YAML ``tuning:`` block under ``home_io_control:`` and optionally
## generates Home Assistant ``number`` and ``select`` entities so users can adjust
## pairing and radio parameters at runtime. Tuned values are volatile and reset on
## every boot.

import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number, select
from esphome.const import CONF_ID, CONF_NAME, ENTITY_CATEGORY_CONFIG
from esphome.core import ID

from .hub_names import home_io_control_ns

_LOGGER = logging.getLogger(__name__)

CONF_TUNING = "tuning"
CONF_UI_CONTROLS = "ui_controls"

# Fixed ID prefix for the generated tuning companion entities (see _inject_tuning_companion_ids).
_COMPANION_ID_BASE = "home_io_control"

# --- Radio / physical layer ---
CONF_SX1262_RX_BANDWIDTH = "sx1262_rx_bandwidth"
CONF_SX1262_POST_TX_SETTLE_US = "sx1262_post_tx_settle_us"
CONF_SX1276_RX_BANDWIDTH = "sx1276_rx_bandwidth"
CONF_SX1276_DISCOVERY_HOP_SLICE_MS = "sx1276_discovery_hop_slice_ms"
CONF_SX1262_DISCOVERY_HOP_SLICE_MS = "sx1262_discovery_hop_slice_ms"
CONF_LR1121_RX_BANDWIDTH = "lr1121_rx_bandwidth"
CONF_LR1121_POST_TX_SETTLE_US = "lr1121_post_tx_settle_us"
CONF_LR1121_DISCOVERY_HOP_SLICE_MS = "lr1121_discovery_hop_slice_ms"
CONF_RESPONSE_PREAMBLE_MS = "response_preamble_ms"
CONF_COLD_BROADCAST_REPLY_PREAMBLE_MS = "cold_broadcast_reply_preamble_ms"
CONF_START_PREAMBLE_MS = "start_preamble_ms"
CONF_LBT_MAX_RETRIES = "lbt_max_retries"  # deprecated, accepted and ignored (ADR 0096)
CONF_LBT_FORCE_WHEN_BUSY = "lbt_force_when_busy"
CONF_LBT_RSSI_THRESHOLD_DBM = "lbt_rssi_threshold_dbm"
CONF_LOW_POWER_WAKE_BELIEF = "low_power_wake_belief"
CONF_LOW_POWER_WAKE_MS = "low_power_wake_ms"
CONF_EXCHANGE_START_RESPONSE_WAIT_MS = "exchange_start_response_wait_ms"
CONF_EXCHANGE_EARLY_RESPONSE_WAIT_MS = "exchange_early_response_wait_ms"
CONF_EXCHANGE_RESPONSE_WAIT_MS = "exchange_response_wait_ms"
CONF_EXCHANGE_TOTAL_BUDGET_MS = "exchange_total_budget_ms"

# --- Polls after a linked-remote press (ADR 0080) ---
CONF_REMOTE_ACTIVITY_POLL_DELAY_MS = "remote_activity_poll_delay_ms"
CONF_REMOTE_ACTIVITY_CATCHUP_POLL_MS = "remote_activity_catchup_poll_ms"

# --- Pairing protocol ---
CONF_PAIRING_DISCOVERY_COMMANDS = "pairing_discovery_commands"
CONF_PAIRING_DISCOVERY_DESTINATION = "pairing_discovery_destination"
CONF_PAIRING_DISCOVERY_PAYLOAD = "pairing_discovery_payload"
CONF_PAIRING_DISCOVERY_LOW_POWER = "pairing_discovery_low_power"
CONF_PAIRING_DISCOVERY_ACK_CAPABLE = "pairing_discovery_ack_capable"
CONF_PAIRING_DISCOVERY_PREAMBLE_MS = "pairing_discovery_preamble_ms"
CONF_PAIRING_DISCOVERY_WAIT_MS = "pairing_discovery_wait_ms"
CONF_PAIRING_DISCOVERY_INITIAL_DWELL_MS = "pairing_discovery_initial_dwell_ms"
CONF_PAIRING_KEY_EXCHANGE_RETRIES = "pairing_key_exchange_retries"
CONF_SCAN_POWER_CLASSES = "scan_power_classes"
CONF_PAIRING_DISCOVER_CONFIRM = "pairing_discover_confirm"
CONF_PAIRING_DISCOVERY_LISTEN_CHANNELS = "pairing_discovery_listen_channels"
CONF_PAIRING_DISCOVERY_SEND_CHANNELS = "pairing_discovery_send_channels"
CONF_PAIRING_KEY_INIT_DELAY_MS = "pairing_key_init_delay_ms"
CONF_PAIRING_KEY_TRANSFER = "pairing_key_transfer"
CONF_KEY_EXTRACTION_TYPE_WORD = "key_extraction_type_word"
CONF_KEY_EXTRACTION_MULTI_INFO = "key_extraction_multi_info"

# C++ type references
TuningConfig = home_io_control_ns.class_("TuningConfig")
IOHomeTuningNumber = home_io_control_ns.class_(
    "IOHomeTuningNumber", number.Number, cg.Component
)
IOHomeTuningSelect = home_io_control_ns.class_(
    "IOHomeTuningSelect", select.Select, cg.Component
)

# C++ enums. These stringify to their fully-qualified C++ names (e.g.
# esphome::home_io_control::SX1262RxBandwidth::BW_117_3_KHZ) so generated code compiles in
# the global-namespace setup() function.
SX1262RxBandwidth = home_io_control_ns.enum("SX1262RxBandwidth", is_class=True)
SX1276RxBandwidth = home_io_control_ns.enum("SX1276RxBandwidth", is_class=True)
LR1121RxBandwidth = home_io_control_ns.enum("LR1121RxBandwidth", is_class=True)
DiscoveryCommand = home_io_control_ns.enum("DiscoveryCommand", is_class=True)
ScanPowerClasses = home_io_control_ns.enum("ScanPowerClasses", is_class=True)
DiscoverConfirmMode = home_io_control_ns.enum("DiscoverConfirmMode", is_class=True)
DiscoveryListenChannels = home_io_control_ns.enum("DiscoveryListenChannels", is_class=True)
DiscoverySendChannels = home_io_control_ns.enum("DiscoverySendChannels", is_class=True)
KeyTransferMode = home_io_control_ns.enum("KeyTransferMode", is_class=True)

# Map each YAML option string to its C++ enum value. Options are bare kHz numbers (the "kHz"
# unit lives in the entity name) for uniformity with the numeric parameters.
SX1262_BANDWIDTH_OPTIONS = {
    "39.0": SX1262RxBandwidth.BW_39_0_KHZ,
    "46.9": SX1262RxBandwidth.BW_46_9_KHZ,
    "58.6": SX1262RxBandwidth.BW_58_6_KHZ,
    "78.2": SX1262RxBandwidth.BW_78_2_KHZ,
    "117.3": SX1262RxBandwidth.BW_117_3_KHZ,
    "156.2": SX1262RxBandwidth.BW_156_2_KHZ,
    "187.2": SX1262RxBandwidth.BW_187_2_KHZ,
}

SX1276_BANDWIDTH_OPTIONS = {
    "20.8": SX1276RxBandwidth.BW_20_8_KHZ,
    "41.7": SX1276RxBandwidth.BW_41_7_KHZ,
    "62.5": SX1276RxBandwidth.BW_62_5_KHZ,
    "83.3": SX1276RxBandwidth.BW_83_3_KHZ,
    "125.0": SX1276RxBandwidth.BW_125_0_KHZ,
}

# LR1121 shares the Semtech GFSK bandwidth grid with SX1262, so LR1121RxBandwidth is
# byte-for-byte identical to SX1262RxBandwidth today (tuning_config.h; the
# Sx1262AndLr1121BandwidthTablesAgree test pins them together). They are kept as separate C++
# enums, and separate option dicts here, only so each chip's option set can diverge if a real
# chip difference ever demands it. The two narrowest options (39.0/46.9 kHz) are offered on
# both chips for probing below the default; both sit well below the 76.8 kHz (DSB)
# sizing floor for this waveform, so neither suits everyday use.
LR1121_BANDWIDTH_OPTIONS = {
    "39.0": LR1121RxBandwidth.BW_39_0_KHZ,
    "46.9": LR1121RxBandwidth.BW_46_9_KHZ,
    "58.6": LR1121RxBandwidth.BW_58_6_KHZ,
    "78.2": LR1121RxBandwidth.BW_78_2_KHZ,
    "117.3": LR1121RxBandwidth.BW_117_3_KHZ,
    "156.2": LR1121RxBandwidth.BW_156_2_KHZ,
    "187.2": LR1121RxBandwidth.BW_187_2_KHZ,
}

DISCOVERY_COMMAND_OPTIONS = {
    "0x28": DiscoveryCommand.DISCOVER,
    "0x2E": DiscoveryCommand.DISCOVER_ALT,
}
# 0x2A (CMD_DISCOVER_SPE_REQ) is deliberately excluded: it is a roll-call answered only by devices
# that already hold the controller's system key, so a device in learning mode never answers it.
# Offering it here could only add replies from already-paired devices to a pairing attempt, never
# help reach the unpaired one. The command byte itself stays valid — see
# DiscoveryCommand::DISCOVER_SPE in tuning_config.h.

# Home Assistant `select` entities are single-choice, but the discovery phase can send an
# ordered list of commands. These comma-separated presets expose the useful combinations as
# selectable options; the C++ dispatch (update_tuning_select) parses them back into the vector.
DISCOVERY_COMMAND_PRESETS = [
    "0x28",  # default 2W discovery
    "0x2E",  # alternate discovery — kept for completeness/experimentation, but broadcast 0x2E has
             # never drawn a response from any device this project has real evidence for; see the
             # CMD_DISCOVER_ALT_REQ doc comment in proto_constants.h. Not a recommended fix.
    "0x28,0x2E",  # both broadcasts
]

# Destinations a discovery broadcast may be addressed to. The first three are the class-less
# forms every controller in this project's corpus uses — a real Somfy TaHoma Switch's own 0x28
# goes to 0x00003B (corpus: somfy_tahoma_pairing_key_extraction_success_sx1276).
#
# The 0x0001xx pair are *typed* broadcasts for the lighting class: an io broadcast address encodes
# the device type as (type << 6) | subtype-mask, so LIGHT (0x06) gives 0x0001BF with the
# all-subtypes mask 0x3F and 0x0001BB with discovery's 0x3B. They are offered because the only
# system-opening sweep in the corpus is typed by class — a VELUX KLI 313 hitting 0x0000BF /
# 0x0000FF / 0x00037F (roller_shutter / vertical_exterior_awning / dual_roller_shutter) in
# velux_ssl_discovery_tahoma_pairing — while every discovery this project sends is class-less.
# These two are an experiment, not a fix: no light has answered one.
#
# 0x00FFFB is the discovery form for remote controllers (type 1023). An Atlantic Tatou heater that
# receives a key searches with it, and a Tatou offering its key answered that search (#199), so
# Discover & Pair with this destination can find such a heater and pull its key.
DISCOVERY_DESTINATION_OPTIONS = [
    "auto",
    "0x00003B",
    "0x00003F",
    "0x0001BB",
    "0x0001BF",
    "0x00FFFB",
]

PAIRING_DISCOVERY_PAYLOAD_OPTIONS = ["none", "0x00"]

# Which power classes scan_paired_devices() calls. Option strings mirror the roll-call report's
# power_save= values so the two share one vocabulary; the C++ side of these exact strings is
# scan_power_classes_to_string() / power_save_mode_name() (tuning_config.h / proto_constants.h).
# scripts/check-tuning-sync.py compares every enum select's option strings with its C++ function.
SCAN_POWER_CLASSES_OPTIONS = {
    "both": ScanPowerClasses.BOTH,
    "always_alive": ScanPowerClasses.ALWAYS_ALIVE,
    "low_power": ScanPowerClasses.LOW_POWER,
}

# Whether/how PairingEngine sends CMD_DISCOVER_CONFIRM (0x2C) during pairing. Option strings
# mirror discover_confirm_mode_to_string() (tuning_config.h), compared by check-tuning-sync.py.
# "skip"/"send"/"send_with_ack", not "on"/"off":
# YAML 1.1 turns bare on/off into booleans.
DISCOVER_CONFIRM_MODE_OPTIONS = {
    "skip": DiscoverConfirmMode.SKIP,
    "send": DiscoverConfirmMode.SEND,
    "send_with_ack": DiscoverConfirmMode.SEND_WITH_ACK,
}

# Which channels the discovery response wait covers. `skip_request` (default) listens on the two
# channels that are not the one the request went out on; `all` includes it, at the cost of a third
# of the dwell on the other two. See DiscoveryListenChannels in tuning_config.h for why the
# default skips, and why that reasoning cannot settle the case of a device that never answers.
DISCOVERY_LISTEN_CHANNELS_OPTIONS = {
    "skip_request": DiscoveryListenChannels.SKIP_REQUEST,
    "all": DiscoveryListenChannels.ALL,
}

# Which channels the discovery requests go out on. `request` (default) sends every attempt on CH2;
# `all` sends the attempts of one command on CH2, CH1 and CH3 in turn. Option strings mirror
# discovery_send_channels_to_string() (tuning_config.h), compared by check-tuning-sync.py.
DISCOVERY_SEND_CHANNELS_OPTIONS = {
    "request": DiscoverySendChannels.REQUEST,
    "all": DiscoverySendChannels.ALL,
}

# Which direction pairing moves the system key in. `auto` (default) pulls from a device that answers
# discovery as a remote controller and pushes to any other; `push` sends our key; `pull` asks the
# device for its own key. Option strings mirror key_transfer_mode_to_string() (tuning_config.h),
# compared by check-tuning-sync.py. See KeyTransferMode for the details.
KEY_TRANSFER_MODE_OPTIONS = {
    "auto": KeyTransferMode.AUTO,
    "push": KeyTransferMode.PUSH,
    "pull": KeyTransferMode.PULL,
}


def _id_key(param_key):
    """Config-dict key under which a parameter's injected companion entity ID is stored."""
    return f"_{param_key}_id"


# UI entity names. Radio params are prefixed "Radio" and pairing params "Pairing" so that,
# under Home Assistant's Configuration section (entity_category=config), the two logical groups
# cluster together alphabetically.
UI_NAMES = {
    CONF_SX1262_RX_BANDWIDTH: "Radio SX1262 RX Bandwidth (kHz)",
    CONF_SX1262_POST_TX_SETTLE_US: "Radio SX1262 Post-TX Settle",
    CONF_SX1276_RX_BANDWIDTH: "Radio SX1276 RX Bandwidth (kHz)",
    CONF_SX1276_DISCOVERY_HOP_SLICE_MS: "Radio SX1276 Discovery Hop Slice",
    CONF_SX1262_DISCOVERY_HOP_SLICE_MS: "Radio SX1262 Discovery Hop Slice",
    CONF_LR1121_RX_BANDWIDTH: "Radio LR1121 RX Bandwidth (kHz)",
    CONF_LR1121_POST_TX_SETTLE_US: "Radio LR1121 Post-TX Settle",
    CONF_LR1121_DISCOVERY_HOP_SLICE_MS: "Radio LR1121 Discovery Hop Slice",
    CONF_RESPONSE_PREAMBLE_MS: "Radio Response Preamble",
    CONF_COLD_BROADCAST_REPLY_PREAMBLE_MS: "Radio Cold Broadcast Reply Preamble",
    CONF_START_PREAMBLE_MS: "Radio Start Preamble",
    CONF_LBT_FORCE_WHEN_BUSY: "Radio LBT Force When Busy",
    CONF_LBT_RSSI_THRESHOLD_DBM: "Radio LBT RSSI Threshold",
    CONF_LOW_POWER_WAKE_BELIEF: "Radio Low Power Wake Belief",
    CONF_LOW_POWER_WAKE_MS: "Radio Low Power Wake Preamble",
    CONF_EXCHANGE_START_RESPONSE_WAIT_MS: "Exchange Start Response Wait",
    CONF_EXCHANGE_EARLY_RESPONSE_WAIT_MS: "Exchange Early Response Wait",
    CONF_EXCHANGE_RESPONSE_WAIT_MS: "Exchange Response Wait",
    CONF_EXCHANGE_TOTAL_BUDGET_MS: "Exchange Total Budget",
    CONF_REMOTE_ACTIVITY_POLL_DELAY_MS: "Remote Activity Poll Delay",
    CONF_REMOTE_ACTIVITY_CATCHUP_POLL_MS: "Remote Activity Catch-up Poll",
    CONF_PAIRING_DISCOVERY_COMMANDS: "Pairing Discovery Commands",
    CONF_PAIRING_DISCOVERY_DESTINATION: "Pairing Discovery Destination",
    CONF_PAIRING_DISCOVERY_PAYLOAD: "Pairing Discovery Payload",
    CONF_PAIRING_DISCOVERY_LOW_POWER: "Pairing Discovery Low Power",
    CONF_PAIRING_DISCOVERY_ACK_CAPABLE: "Pairing Discovery ACK Capable",
    CONF_PAIRING_DISCOVERY_PREAMBLE_MS: "Pairing Discovery Preamble",
    CONF_PAIRING_DISCOVERY_WAIT_MS: "Pairing Discovery Wait",
    CONF_PAIRING_DISCOVERY_INITIAL_DWELL_MS: "Pairing Discovery Initial Dwell",
    CONF_PAIRING_KEY_EXCHANGE_RETRIES: "Pairing Key Exchange Retries",
    CONF_SCAN_POWER_CLASSES: "Pairing Scan Power Classes",
    CONF_PAIRING_DISCOVER_CONFIRM: "Pairing Discover Confirm",
    CONF_PAIRING_KEY_INIT_DELAY_MS: "Pairing Key Init Delay",
    CONF_PAIRING_DISCOVERY_LISTEN_CHANNELS: "Pairing Discovery Listen Channels",
    CONF_PAIRING_DISCOVERY_SEND_CHANNELS: "Pairing Discovery Send Channels",
    CONF_PAIRING_KEY_TRANSFER: "Pairing Key Transfer",
    CONF_KEY_EXTRACTION_TYPE_WORD: "Key Extraction Type Word",
    CONF_KEY_EXTRACTION_MULTI_INFO: "Key Extraction Multi Info",
}

# Numeric parameters: key -> (min, max, step, unit). Single source of truth for both the
# YAML validation range and the Home Assistant `number` entity bounds, so the two cannot drift.
_NUMBER_PARAMS = {
    # Every preamble is a duration, the same on every radio (ADR 0097); each driver converts it to
    # programmed bytes. The floor 1.5 ms is about SHORT_PREAMBLE (8 bytes, 1.7 ms on a raw-stream
    # radio): the lowest value that leaves the peer's detector anything to lock on to. The ceiling
    # 70 ms is about 256 bytes on the radio that sends 10 bit periods per byte, and below LONG_PREAMBLE
    # bytes on every radio (preamble_wake_sync_test.cpp pins that, so the value can never be taken
    # for a wake-up burst). Step 0.5 ms puts the 3.5 ms default on the number entity's grid.
    CONF_RESPONSE_PREAMBLE_MS: (1.5, 70, 0.5, "ms"),
    CONF_SX1262_POST_TX_SETTLE_US: (0, 2000, 10, "µs"),
    CONF_SX1276_DISCOVERY_HOP_SLICE_MS: (5, 200, 1, "ms"),
    # Floor is 0, not a physically meaningful minimum for the chip: coverage degrades gradually as
    # the dwell shortens and only truly collapses at the literal 0 ms edge case, where
    # wait_for_packet(..., 0) returns before any guard can observe activity at all. Left open so
    # that floor stays empirically checkable rather than assumed. See
    # SX1262_DISCOVERY_HOP_SLICE_MS in tuning_config.h for why the default itself is short.
    CONF_SX1262_DISCOVERY_HOP_SLICE_MS: (0, 500, 1, "ms"),
    # LR1121 numeric ranges reuse the SX1262 bounds — same chip-family physical constraints, and
    # a validated SX1262 value encodes protocol-side reality more than a chip quirk.
    CONF_LR1121_POST_TX_SETTLE_US: (0, 2000, 10, "µs"),
    CONF_LR1121_DISCOVERY_HOP_SLICE_MS: (0, 500, 1, "ms"),
    # Same (1.5, 70) floor/ceiling as the response preamble, for the same reasons. The default is
    # 20 ms (COLD_BROADCAST_REPLY_PREAMBLE_MS in proto_timing.h): above the ~13.9 ms preamble real
    # devices put on a discovery reply, so a hopping receiver catches this reply at least as reliably.
    CONF_COLD_BROADCAST_REPLY_PREAMBLE_MS: (1.5, 70, 0.5, "ms"),
    # Same (1.5, 70) floor/ceiling as the response preamble. The default is 10 ms
    # (START_PREAMBLE_MS in proto_timing.h): brand-new devices have been seen failing at 1/4/8 B
    # (docs/configuration/tuning.md), so the floor is a proven-safe lower bound for the knob, not a
    # sensible default.
    CONF_START_PREAMBLE_MS: (1.5, 70, 0.5, "ms"),
    # Floor is the response preamble's 1.5 ms. The ceiling 300 ms reaches the wake-up burst
    # (LONG_PREAMBLE bytes, 213 ms on a raw-stream radio and 267 ms on the SX1276) on every radio:
    # an explicit opt-in to a wake-up discovery, which this one knob can reach and the others cannot.
    # It exists to let a stuck pairing attempt go *shorter* than the long wake-up burst (issue
    # #27/#87's precedent); nothing calls for raising it further.
    CONF_PAIRING_DISCOVERY_PREAMBLE_MS: (1.5, 300, 0.5, "ms"),
    # The wake-up preamble on a directed start frame to a `low_power` target believed at rest. The
    # default is LOW_POWER_WAKE_MS (600, proto_timing.h, ADR 0097): a VELUX KLI 110 remote sends about
    # 600 ms of wake-up, and a resting VELUX SSL solar roller shutter answered every try at that
    # length. The floor 300 ms is at least LONG_PREAMBLE bytes on every radio (1152 bytes on the
    # SX1276), which keeps every value a wake-up burst for tx_wake_for(); the ceiling 1000 ms is
    # about four seconds of air for the three attempts. A low-power target keeps its three attempts
    # whatever their length costs: the exchange is bounded by that count, not by
    # exchange_total_budget_ms (ADR 0095).
    CONF_LOW_POWER_WAKE_MS: (300, 1000, 10, "ms"),
    CONF_LBT_RSSI_THRESHOLD_DBM: (-95, -70, 1, "dBm"),
    # Ceilings are generous because the right value is a property of the target device, not of
    # the radio: measured RS100 solar reply latencies reach ~3 s (see RESPONSE_START_WAIT_MS).
    # Every millisecond here is loop-blocking time on a *failed* exchange only (ADR 0013).
    CONF_EXCHANGE_START_RESPONSE_WAIT_MS: (200, 4000, 50, "ms"),
    # Listen time of every attempt but the last when a mains device is asked more than once. A
    # mains device answers within ~100 ms or not at all (corpus replies; bench replies 33-49 ms), so
    # the floor 150 keeps a margin over the slowest of them; the default (200) is the largest value
    # at which nine attempts still fit the default budget (EXCHANGE_EARLY_RESPONSE_WAIT_MS). Ceiling
    # 1000: above the start window it has no effect, and fewer attempts fit the higher it goes.
    CONF_EXCHANGE_EARLY_RESPONSE_WAIT_MS: (150, 1000, 50, "ms"),
    CONF_EXCHANGE_RESPONSE_WAIT_MS: (200, 4000, 50, "ms"),
    # Ceiling on a whole exchange. Every millisecond over ~2550 blocks the ESPHome loop past its
    # own warning threshold, so raise this only when persistence matters more than responsiveness.
    CONF_EXCHANGE_TOTAL_BUDGET_MS: (500, 12000, 100, "ms"),
    # Up to two minutes: long enough to poll once after a slow solar screen's whole travel instead of
    # tracking it. 0 polls as soon as the 1W quiet period lets it.
    CONF_REMOTE_ACTIVITY_POLL_DELAY_MS: (0, 120000, 100, "ms"),
    # 0 = off (the default). Up to ten minutes, the follow-up window's length (ADR 0061).
    CONF_REMOTE_ACTIVITY_CATCHUP_POLL_MS: (0, 600000, 1000, "ms"),
    CONF_PAIRING_DISCOVERY_WAIT_MS: (500, 5000, 50, "ms"),
    CONF_PAIRING_DISCOVERY_INITIAL_DWELL_MS: (0, 500, 10, "ms"),
    CONF_PAIRING_KEY_EXCHANGE_RETRIES: (1, 5, 1, ""),
    # Ceiling covers KLR300's observed 0x2C -> 0x31 gap (~8.1 s) with headroom; step 100 matches
    # the other pairing-phase millisecond knobs.
    CONF_PAIRING_KEY_INIT_DELAY_MS: (0, 10000, 100, "ms"),
    # The two bytes of the key-extraction responder's discovery answer (0x29) that describe the
    # device: the packed type word and the Multi Information Byte. Written in hex in YAML (see
    # _HEX_PARAMS); the Home Assistant number entity shows the same value in decimal.
    CONF_KEY_EXTRACTION_TYPE_WORD: (0, 0xFFFF, 1, ""),
    CONF_KEY_EXTRACTION_MULTI_INFO: (0, 0xFF, 1, ""),
}

# Numeric parameters with a fractional value (the preamble durations, 3.5 ms by default): validated as
# floats, so the YAML value and the number entity keep their fraction.
_FLOAT_PARAMS = frozenset(
    {
        CONF_RESPONSE_PREAMBLE_MS,
        CONF_COLD_BROADCAST_REPLY_PREAMBLE_MS,
        CONF_START_PREAMBLE_MS,
        CONF_PAIRING_DISCOVERY_PREAMBLE_MS,
    }
)

# Plain boolean parameters: the C++ TuningConfig field name equals the YAML key, and every one is
# a simple On/Off select — driving the schema, the select options, and the to_code() assignment
# from one list each, the same way _NUMBER_PARAMS drives the numeric ones below.
#
# pairing_discovery_ack_capable is opt-in: see the init_frame() doc in proto_frame.h for why
# CTRL1_ACK must never become an unconditional default. This knob scopes it to the discovery
# broadcast only, off by default.
#
# low_power_wake_belief is the opposite: on by default, and a diagnostic off-switch. Setting it to
# false restores the fixed long wake-up preamble on every try to a low_power device.
#
# lbt_force_when_busy is opt-in: by default a request never goes out onto a channel the hub measured
# busy (ADR 0096). On, a request whose access time ran out with every channel busy is sent on the
# quietest one.
_BOOL_PARAMS = (
    CONF_PAIRING_DISCOVERY_LOW_POWER,
    CONF_PAIRING_DISCOVERY_ACK_CAPABLE,
    CONF_LOW_POWER_WAKE_BELIEF,
    CONF_LBT_FORCE_WHEN_BUSY,
)
_BOOL_SELECT_OPTIONS = ["Off", "On"]

# Select parameters: key -> ordered option list. Single source of truth for both the
# companion-ID injection and the entity creation.
_SELECT_OPTIONS = {
    CONF_PAIRING_DISCOVERY_COMMANDS: DISCOVERY_COMMAND_PRESETS,
    CONF_PAIRING_DISCOVERY_DESTINATION: DISCOVERY_DESTINATION_OPTIONS,
    CONF_PAIRING_DISCOVERY_PAYLOAD: PAIRING_DISCOVERY_PAYLOAD_OPTIONS,
    # check-tuning-sync.py's AST scan only understands plain `CONF_*: value` entries in this dict
    # (no `**` unpacking), so the boolean rows stay written out individually here even though
    # _BOOL_PARAMS drives the schema and to_code() loops below.
    CONF_PAIRING_DISCOVERY_LOW_POWER: _BOOL_SELECT_OPTIONS,
    CONF_PAIRING_DISCOVERY_ACK_CAPABLE: _BOOL_SELECT_OPTIONS,
    CONF_SX1262_RX_BANDWIDTH: list(SX1262_BANDWIDTH_OPTIONS),
    CONF_SX1276_RX_BANDWIDTH: list(SX1276_BANDWIDTH_OPTIONS),
    CONF_LR1121_RX_BANDWIDTH: list(LR1121_BANDWIDTH_OPTIONS),
    CONF_LOW_POWER_WAKE_BELIEF: _BOOL_SELECT_OPTIONS,
    CONF_LBT_FORCE_WHEN_BUSY: _BOOL_SELECT_OPTIONS,
    CONF_SCAN_POWER_CLASSES: list(SCAN_POWER_CLASSES_OPTIONS),
    CONF_PAIRING_DISCOVER_CONFIRM: list(DISCOVER_CONFIRM_MODE_OPTIONS),
    CONF_PAIRING_DISCOVERY_LISTEN_CHANNELS: list(DISCOVERY_LISTEN_CHANNELS_OPTIONS),
    CONF_PAIRING_DISCOVERY_SEND_CHANNELS: list(DISCOVERY_SEND_CHANNELS_OPTIONS),
    CONF_PAIRING_KEY_TRANSFER: list(KEY_TRANSFER_MODE_OPTIONS),
}


def _one_of_string(param_name, options, coerce_number=False):
    """Build a validator that requires a string value present in `options`.

    Works for both list and dict `options` (dict membership tests the keys), so a single
    factory replaces a hand-written validator per option set while keeping a per-parameter
    error message. With `coerce_number`, a numeric YAML value (e.g. ``117.3``) is accepted and
    normalized to its string form, so bare-number options do not force the user to quote them.
    """

    def validate(value):
        if coerce_number and isinstance(value, (int, float)) and not isinstance(value, bool):
            value = str(value)
        value = cv.string_strict(value)
        if value not in options:
            raise cv.Invalid(f"{param_name} must be one of {list(options)}")
        return value

    return validate


_validate_bandwidth = _one_of_string(
    CONF_SX1262_RX_BANDWIDTH, SX1262_BANDWIDTH_OPTIONS, coerce_number=True
)
_validate_sx1276_bandwidth = _one_of_string(
    CONF_SX1276_RX_BANDWIDTH, SX1276_BANDWIDTH_OPTIONS, coerce_number=True
)
_validate_lr1121_bandwidth = _one_of_string(
    CONF_LR1121_RX_BANDWIDTH, LR1121_BANDWIDTH_OPTIONS, coerce_number=True
)
_validate_discovery_command = _one_of_string(
    f"{CONF_PAIRING_DISCOVERY_COMMANDS} entries", DISCOVERY_COMMAND_OPTIONS
)
_validate_discovery_destination = _one_of_string(
    CONF_PAIRING_DISCOVERY_DESTINATION, DISCOVERY_DESTINATION_OPTIONS
)
_validate_discovery_payload = _one_of_string(
    CONF_PAIRING_DISCOVERY_PAYLOAD, PAIRING_DISCOVERY_PAYLOAD_OPTIONS
)
_validate_scan_power_classes = _one_of_string(
    CONF_SCAN_POWER_CLASSES, SCAN_POWER_CLASSES_OPTIONS
)
_validate_discover_confirm_mode = _one_of_string(
    CONF_PAIRING_DISCOVER_CONFIRM, DISCOVER_CONFIRM_MODE_OPTIONS
)
_validate_discovery_listen_channels = _one_of_string(
    CONF_PAIRING_DISCOVERY_LISTEN_CHANNELS, DISCOVERY_LISTEN_CHANNELS_OPTIONS
)
_validate_discovery_send_channels = _one_of_string(
    CONF_PAIRING_DISCOVERY_SEND_CHANNELS, DISCOVERY_SEND_CHANNELS_OPTIONS
)
_validate_key_transfer_mode = _one_of_string(
    CONF_PAIRING_KEY_TRANSFER, KEY_TRANSFER_MODE_OPTIONS
)


def _deprecated_lbt_max_retries(value):
    """Accept the retired ``lbt_max_retries`` and say it no longer does anything.

    The checks of a request now end by time, not by count (ADR 0096), so the value has no effect.
    The key stays legal so an existing config keeps compiling; it also stays within its old range
    so a typo is still reported. No code is generated for it.
    """
    value = cv.int_range(min=0, max=10)(value)
    _LOGGER.warning(
        "home_io_control: tuning '%s:' is deprecated and has no effect: a request now waits for a "
        "free channel for a fixed time instead of counting checks. Remove it, and use '%s: true' if "
        "you want a send onto a busy channel after that time.",
        CONF_LBT_MAX_RETRIES,
        CONF_LBT_FORCE_WHEN_BUSY,
    )
    return value


# Deprecated byte-denominated preamble keys (ADR 0097): old key -> (min, max, duration key it
# became, TuningConfig::LegacyPreambleBytes field, radio_type it applies to or None for every radio).
# The ranges are the byte ranges the keys always had. The byte value is handed to C++ unconverted:
# only the radio driver knows how long a byte lasts on its coder, so setup() turns it into the
# duration it has always given on that radio. A response key of another radio is validated and
# warned about but has no effect, as before.
_LEGACY_PREAMBLE_KEYS = {
    "low_power_wake_preamble": (1024, 4096, CONF_LOW_POWER_WAKE_MS, "low_power_wake", None),
    "normal_start_preamble": (8, 256, CONF_START_PREAMBLE_MS, "start", None),
    "sx1262_response_preamble": (8, 256, CONF_RESPONSE_PREAMBLE_MS, "response", "sx1262"),
    "sx1276_response_preamble": (8, 256, CONF_RESPONSE_PREAMBLE_MS, "response", "sx1276"),
    "lr1121_response_preamble": (8, 256, CONF_RESPONSE_PREAMBLE_MS, "response", "lr1121"),
    "cold_broadcast_reply_preamble": (8, 256, CONF_COLD_BROADCAST_REPLY_PREAMBLE_MS, "cold_broadcast_reply", None),
    "pairing_discovery_preamble": (8, 1024, CONF_PAIRING_DISCOVERY_PREAMBLE_MS, "discovery", None),
}


def _deprecated_preamble_bytes(old_key, low, high, new_key):
    """Build the validator of a deprecated byte-denominated preamble key.

    The key keeps its old byte range so a typo is still reported, and says once that the value is a
    duration now. The value stays a byte count; the driver converts it (see `_LEGACY_PREAMBLE_KEYS`).
    """
    in_range = cv.int_range(min=low, max=high)

    def validate(value):
        value = in_range(value)
        _LOGGER.warning(
            "home_io_control: tuning '%s:' is deprecated: every preamble is set in milliseconds now, "
            "the same on every radio. It is converted to the duration it gives on this radio; use "
            "'%s:' instead (see the tuning documentation).",
            old_key,
            new_key,
        )
        return value

    return validate


def _legacy_preamble_assignments(config, radio_type):
    """The `TuningConfig::legacy_preamble_bytes` fields to set: (field, bytes) per deprecated key given.

    Handed over unconverted: setup() turns each byte count into the duration it gives on the radio
    (ADR 0097). A response key written for another radio never applied to this one and is left out.
    """
    return [
        (f"legacy_preamble_bytes.{field}", config[old_key])
        for old_key, (_lo, _hi, _new_key, field, key_radio) in _LEGACY_PREAMBLE_KEYS.items()
        if old_key in config and key_radio in (None, radio_type)
    ]


def _refuse_old_and_new_preamble_keys(config):
    """Refuse a deprecated byte key given together with the duration key that replaces it."""
    for old_key, (_lo, _hi, new_key, _field, _radio) in _LEGACY_PREAMBLE_KEYS.items():
        if old_key in config and new_key in config:
            raise cv.Invalid(
                f"tuning: '{old_key}' (bytes, deprecated) and '{new_key}' (milliseconds) set the same "
                f"preamble; remove '{old_key}'"
            )
    return config


# Numeric parameters that are protocol bytes, written in hex in YAML: key -> hex digits.
_HEX_PARAMS = {
    CONF_KEY_EXTRACTION_TYPE_WORD: 4,
    CONF_KEY_EXTRACTION_MULTI_INFO: 2,
}


def _hex_value(param_name, digits):
    """Build a validator for a protocol byte or word: an int (YAML ``0xDC``) or a hex string.

    Accepts ``0xDC``, ``"0xDC"`` and ``"DC"`` (spaces allowed, so ``"01 80"`` copied from a frame
    dump works), and rejects anything wider than `digits` hex digits.
    """
    max_value = (1 << (4 * digits)) - 1

    def validate(value):
        if isinstance(value, bool):
            raise cv.Invalid(f"{param_name} must be a hex value such as 0x{'0' * (digits - 2)}DC")
        if isinstance(value, str):
            text = value.replace(" ", "").lower()
            text = text.removeprefix("0x")
            try:
                value = int(text, 16)
            except ValueError as err:
                raise cv.Invalid(f"{param_name} must be a hex value, got '{value}'") from err
        if not isinstance(value, int) or not 0 <= value <= max_value:
            raise cv.Invalid(f"{param_name} must be between 0x00 and 0x{max_value:0{digits}X}")
        return value

    return validate


def _parse_destination_to_bytes(value):
    """Convert an explicit destination string like '0x00003B' to a list of 3 byte values."""
    digits = value[2:]  # strip the '0x' prefix
    return [int(digits[i : i + 2], 16) for i in range(0, 6, 2)]


def _parse_payload(value):
    """Convert a payload option string like '0x00' to its numeric byte value."""
    return int(value, 16)


def _number_validator(key, low, high):
    """Range validator of a numeric parameter: a float for a duration in ms, an int otherwise."""
    if key in _FLOAT_PARAMS:
        return cv.float_range(min=low, max=high)
    return cv.int_range(min=low, max=high)


# Tuning sub-schema (imported and extended by the hub __init__.py).
#
# Only `ui_controls` carries a Python default (it is a feature toggle, not a tunable). Every
# tunable is a plain cv.Optional with NO default: when a key is omitted, the C++ TuningConfig
# field keeps its canonical default from proto_frame.h, which is the single source of truth.
# to_code() only overrides fields that are actually present in the validated config.
TUNING_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_UI_CONTROLS, default=False): cv.boolean,
        cv.Optional(CONF_SX1262_RX_BANDWIDTH): _validate_bandwidth,
        cv.Optional(CONF_SX1276_RX_BANDWIDTH): _validate_sx1276_bandwidth,
        cv.Optional(CONF_LR1121_RX_BANDWIDTH): _validate_lr1121_bandwidth,
        cv.Optional(CONF_PAIRING_DISCOVERY_COMMANDS): cv.All(
            cv.ensure_list(_validate_discovery_command),
            cv.Length(min=1),
        ),
        cv.Optional(CONF_PAIRING_DISCOVERY_DESTINATION): _validate_discovery_destination,
        cv.Optional(CONF_PAIRING_DISCOVERY_PAYLOAD): _validate_discovery_payload,
        cv.Optional(CONF_SCAN_POWER_CLASSES): _validate_scan_power_classes,
        cv.Optional(CONF_PAIRING_DISCOVER_CONFIRM): _validate_discover_confirm_mode,
        cv.Optional(
            CONF_PAIRING_DISCOVERY_LISTEN_CHANNELS
        ): _validate_discovery_listen_channels,
        cv.Optional(
            CONF_PAIRING_DISCOVERY_SEND_CHANNELS
        ): _validate_discovery_send_channels,
        cv.Optional(CONF_PAIRING_KEY_TRANSFER): _validate_key_transfer_mode,
        **{cv.Optional(key): cv.boolean for key in _BOOL_PARAMS},
        cv.Optional(CONF_LBT_MAX_RETRIES): _deprecated_lbt_max_retries,
        # Numeric parameters share their range with the number-entity bounds via _NUMBER_PARAMS.
        **{
            cv.Optional(key): _number_validator(key, lo, hi)
            for key, (lo, hi, _step, _unit) in _NUMBER_PARAMS.items()
            if key not in _HEX_PARAMS
        },
        **{cv.Optional(key): _hex_value(key, digits) for key, digits in _HEX_PARAMS.items()},
        **{
            cv.Optional(old_key): _deprecated_preamble_bytes(old_key, lo, hi, new_key)
            for old_key, (lo, hi, new_key, _field, _radio) in _LEGACY_PREAMBLE_KEYS.items()
        },
    }
)


def _inject_tuning_companion_ids(config):
    """Declare companion entity IDs for tuning UI controls.

    ESPHome 2026.x sizes the runtime component vector from the number of IDs
    declared during validation. If UI entities were created only in to_code(),
    they could be silently dropped. This post-validator injects declared IDs
    before the vector is sized.
    """
    if not config[CONF_UI_CONTROLS]:
        return config

    # The tuning block is a sub-schema of home_io_control and does not carry the parent's
    # CONF_ID, so the companion entity IDs use a fixed prefix. It only needs to be unique
    # among the generated tuning entities.
    base = _COMPANION_ID_BASE

    for key in _SELECT_OPTIONS:
        config[_id_key(key)] = ID(f"{base}_{key}", is_declaration=True, type=IOHomeTuningSelect)
    for key in _NUMBER_PARAMS:
        config[_id_key(key)] = ID(f"{base}_{key}", is_declaration=True, type=IOHomeTuningNumber)

    return config


TUNING_CONFIG_SCHEMA = cv.All(
    TUNING_SCHEMA, _refuse_old_and_new_preamble_keys, _inject_tuning_companion_ids
)


def _cpp_bool(value):
    """Format a Python bool as a C++ boolean literal."""
    return "true" if value else "false"


def _assign(struct, field, value):
    """Emit a `struct.field = value;` C++ assignment statement at codegen time.

    ESPHome's MockObj does not support Python attribute assignment, so struct fields
    are populated with raw assignment statements. `value` must already be valid C++
    (an int, or a string such as an enum value or brace-init list).
    """
    cg.add(cg.RawExpression(f"{struct}.{field} = {value}"))


# Enum parameters: (YAML key, C++ TuningConfig field, option string -> C++ enumerator), each assigned
# when the user gave the key.
_ENUM_ASSIGNMENTS = (
    (CONF_SX1262_RX_BANDWIDTH, "sx1262_rx_bandwidth", SX1262_BANDWIDTH_OPTIONS),
    (CONF_SX1276_RX_BANDWIDTH, "sx1276_rx_bandwidth", SX1276_BANDWIDTH_OPTIONS),
    (CONF_LR1121_RX_BANDWIDTH, "lr1121_rx_bandwidth", LR1121_BANDWIDTH_OPTIONS),
    (CONF_SCAN_POWER_CLASSES, "scan_power_classes", SCAN_POWER_CLASSES_OPTIONS),
    (CONF_PAIRING_DISCOVER_CONFIRM, "pairing_discover_confirm", DISCOVER_CONFIRM_MODE_OPTIONS),
    (CONF_PAIRING_DISCOVERY_LISTEN_CHANNELS, "pairing_discovery_listen_channels", DISCOVERY_LISTEN_CHANNELS_OPTIONS),
    (CONF_PAIRING_DISCOVERY_SEND_CHANNELS, "pairing_discovery_send_channels", DISCOVERY_SEND_CHANNELS_OPTIONS),
    (CONF_PAIRING_KEY_TRANSFER, "pairing_key_transfer", KEY_TRANSFER_MODE_OPTIONS),
)


def _apply_tuning_config(config, var, radio_type):
    """Generate C++ code that builds a TuningConfig from the validated YAML.

    Only keys the user actually provided are emitted; every omitted key keeps the
    default baked into the C++ TuningConfig struct (sourced from proto_frame.h), so
    the defaults are never restated here.
    """
    tuning_id = cv.declare_id(TuningConfig)("tuning_config")
    tuning = cg.new_variable(tuning_id, cg.RawExpression(f"{TuningConfig}()"))

    # Presence of the block is what activates the override layer.
    _assign(tuning, "active", "true")

    # --- Parameters needing custom handling (enum/list/byte/bool) ---
    for key, field, options in _ENUM_ASSIGNMENTS:
        if key in config:
            _assign(tuning, field, options[config[key]])

    # Ordered discovery commands. Clear the struct default before appending so a
    # user-provided list replaces it rather than extending it.
    if CONF_PAIRING_DISCOVERY_COMMANDS in config:
        cg.add(tuning.pairing_discovery_commands.clear())
        for cmd in config[CONF_PAIRING_DISCOVERY_COMMANDS]:
            cg.add(
                tuning.pairing_discovery_commands.push_back(
                    DISCOVERY_COMMAND_OPTIONS[cmd]
                )
            )

    if CONF_PAIRING_DISCOVERY_DESTINATION in config:
        dest = config[CONF_PAIRING_DISCOVERY_DESTINATION]
        if dest == "auto":
            _assign(tuning, "pairing_discovery_destination_auto", "true")
        else:
            _assign(tuning, "pairing_discovery_destination_auto", "false")
            b = _parse_destination_to_bytes(dest)
            _assign(
                tuning,
                "pairing_discovery_destination",
                f"{{0x{b[0]:02X}, 0x{b[1]:02X}, 0x{b[2]:02X}}}",
            )

    if CONF_PAIRING_DISCOVERY_PAYLOAD in config:
        payload = config[CONF_PAIRING_DISCOVERY_PAYLOAD]
        if payload == "none":
            _assign(tuning, "pairing_discovery_payload_enabled", "false")
        else:
            _assign(tuning, "pairing_discovery_payload_enabled", "true")
            _assign(tuning, "pairing_discovery_payload", f"0x{_parse_payload(payload):02X}")

    # --- Plain boolean parameters: the C++ struct field name equals the YAML key. ---
    for key in _BOOL_PARAMS:
        if key in config:
            _assign(tuning, key, _cpp_bool(config[key]))

    # --- Plain integer parameters: the C++ struct field name equals the YAML key. ---
    for key in _NUMBER_PARAMS:
        if key in config:
            _assign(tuning, key, config[key])

    # Without the flag, discovery follows the start preamble (or the wake-up preamble under
    # pairing_discovery_low_power) instead of using the emitted value.
    if CONF_PAIRING_DISCOVERY_PREAMBLE_MS in config:
        _assign(tuning, "pairing_discovery_preamble_set", "true")

    for field, value in _legacy_preamble_assignments(config, radio_type):
        _assign(tuning, field, value)

    cg.add(var.set_tuning_config(tuning))


async def _create_tuning_entities(config, var):
    """Generate number/select entities for the tuning parameters when enabled."""
    if not config[CONF_UI_CONTROLS]:
        return

    # --- Select entities (options sourced from _SELECT_OPTIONS) ---
    for key, options in _SELECT_OPTIONS.items():
        await _create_select(config, var, key, options)

    # --- Number entities (bounds sourced from _NUMBER_PARAMS) ---
    for key, (min_value, max_value, step, unit) in _NUMBER_PARAMS.items():
        await _create_number(
            config, var, key, min_value=min_value, max_value=max_value, step=step, unit=unit
        )


async def _create_number(
    config, var, key, min_value, max_value, step, unit=""
):
    """Create a single IOHomeTuningNumber entity.

    The bare {id, name} dict is normalized through number_schema()+COMPONENT_SCHEMA so it
    carries the entity/component defaults (disabled_by_default, setup_priority, ...) that
    register_number()/register_component() require.
    """
    schema = number.number_schema(
        IOHomeTuningNumber,
        unit_of_measurement=unit,
        entity_category=ENTITY_CATEGORY_CONFIG,
    )
    entity_config = schema.extend(cv.COMPONENT_SCHEMA)(
        {
            CONF_ID: config[_id_key(key)],
            CONF_NAME: UI_NAMES[key],
        }
    )
    entity = await number.new_number(
        entity_config,
        var,
        key,
        min_value=min_value,
        max_value=max_value,
        step=step,
    )
    await cg.register_component(entity, entity_config)


async def _create_select(config, var, key, options):
    """Create a single IOHomeTuningSelect entity.

    The bare {id, name} dict is normalized through select_schema()+COMPONENT_SCHEMA so it
    carries the entity/component defaults that register_select()/register_component() require.
    """
    entity_config = select.select_schema(
        IOHomeTuningSelect, entity_category=ENTITY_CATEGORY_CONFIG
    ).extend(cv.COMPONENT_SCHEMA)(
        {
            CONF_ID: config[_id_key(key)],
            CONF_NAME: UI_NAMES[key],
        }
    )
    entity = await select.new_select(
        entity_config,
        var,
        key,
        options=options,
    )
    await cg.register_component(entity, entity_config)


async def to_code(config, var, radio_type):
    """Generate code for the tuning block.

    `radio_type` is the hub's `radio_type:`; it decides which deprecated response key applies.
    """
    _apply_tuning_config(config, var, radio_type)
    await _create_tuning_entities(config, var)
