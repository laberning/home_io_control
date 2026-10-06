## @file
## @brief Shared vocabulary of the Home IO Control codegen: YAML keys and generated C++ handles.
## @ingroup hioc_codegen
##
## The ``CONF_*`` keys of the ``home_io_control:`` block and its ``oneway_controllers:`` /
## ``lr1121_firmware_update:`` sub-blocks (tuning.py keeps the ``tuning:`` keys), the
## ``home_io_control`` C++ namespace, and the class/enum handles of the hub and of the
## hub-level entities it generates. The other codegen modules import from here; this
## module imports none of them.

import esphome.codegen as cg
from esphome.components import spi
# Aliased: this package has its own switch.py/button.py platform submodules, so the real ESPHome
# components are always imported under names that cannot be mistaken for them. In the package's
# __init__.py an unaliased `switch`/`button` would even be overwritten: __init__.py's namespace IS
# the package object, the slot ESPHome's loader binds `esphome.components.home_io_control.switch`
# into when it imports our platform file, so whichever import ran last would silently win.
from esphome.components import button as button_component
from esphome.components import switch as switch_component
from esphome.components import text_sensor as text_sensor_component


CONF_HOME_IO_CONTROL_ID = "home_io_control_id"
CONF_RST_PIN = "rst_pin"
CONF_DIO0_PIN = "dio0_pin"
CONF_DIO4_PIN = "dio4_pin"
CONF_DIO1_PIN = "dio1_pin"
CONF_BUSY_PIN = "busy_pin"
CONF_NODE_ID = "node_id"
CONF_SYSTEM_KEY = "system_key"
CONF_TX_POWER = "tx_power"
CONF_PA_PIN = "pa_pin"
CONF_RADIO_TYPE = "radio_type"
CONF_FEM_EN_PIN = "fem_en_pin"
CONF_VFEM_PIN = "vfem_pin"
CONF_FEM_PA_PIN = "fem_pa_pin"
# Which RF front-end part fem_pa_pin (and, for the parts that have one, fem_en_pin) is wired to
# (ADR 0035) -- selects the driver's per-transmission switching behaviour and which pins that
# behaviour requires; never supplies a pin number itself. See FEM_REQUIRED_PINS in hub_validators.py.
# ("Part", not "chip" -- see FEM_PROFILES' own comment there for why.)
CONF_FEM = "fem"
CONF_TCXO_VOLTAGE = "tcxo_voltage"
CONF_EXPOSED_SENDERS = "exposed_senders"
CONF_ACCEPT_FOREIGN_PAIRING = "accept_foreign_pairing"
CONF_RECOVER_ONEWAY_KEY = "recover_oneway_key"
CONF_SCAN_PAIRED_DEVICES_BUTTON = "scan_paired_devices_button"
CONF_DISCOVER_AND_PAIR_BUTTON = "discover_and_pair_button"
CONF_ONEWAY_CONTROLLERS = "oneway_controllers"
# Internal marker recording that an identity's node_id was derived rather than configured, so
# validation errors and the boot log can say which it was.
CONF_NODE_ID_DERIVED = "_node_id_derived"
CONF_MANUFACTURER = "manufacturer"
# Same YAML key as platform_common.py's CONF_DEVICE_TYPE, spelled here rather than imported:
# platform_common imports from this package, whose modules import this one, so the dependency
# cannot run the other way.
CONF_IO_DEVICE_TYPE = "io_device_type"
CONF_INITIAL_SEQUENCE = "initial_sequence"
CONF_COMMANDS = "commands"
# Per-identity 1W wire overrides (ADR 0031). execute_acei: overrides the manufacturer-derived
# ACEI byte for CMD_EXECUTE; execute_broadcast: all|typed picks the all-devices (00 00 3F) vs
# typed-class destination — a handheld-remote-vs-class-bound axis, not a vendor axis.
CONF_EXECUTE_ACEI = "execute_acei"
CONF_EXECUTE_BROADCAST = "execute_broadcast"
# The build flag for this identity's "Enroll 1W Controller" button. Presence/absence is the whole
# gate -- adding or removing this line and reflashing is the enrollment feature's entire
# lifecycle, same shape as accept_foreign_pairing/recover_oneway_key.
CONF_ENROLLMENT = "enrollment"
# Whether the "Enroll 1W Controller" button's 0x30 carries a trailing MAC. Only meaningful with
# enrollment: true -- see ONEWAY_CONTROLLER_SCHEMA's own comment and create_1w_add_controller()'s
# @warning (proto_commands.h) for why real hardware disagrees on this byte.
CONF_ENROLLMENT_WITH_MAC = "enrollment_with_mac"
# Override for the device classes a VELUX enrollment 0x30 sweep targets. Unset -> the manufacturer
# profile's list (velux: {roller_shutter, awning, dual_shutter}). Ignored by the somfy gesture.
# See resolve_oneway_wire_profile() / effective_enrollment_classes() (oneway_controller.h), ADR 0032.
CONF_ENROLLMENT_CLASSES = "enrollment_classes"
# Same YAML key as platform_common.py's per-device 2W low_power (ADR 0029), tri-state here: unset
# keeps every 1W burst in the legacy shape (LONG_PREAMBLE on every copy, CTRL1 0x00); `false`/`true`
# opt an identity into the ADR 0038 shapes instead. One definition, shared: platform_common.py
# imports this constant rather than keeping its own copy. See ONEWAY_CONTROLLER_SCHEMA's own
# comment on this key for the full tri-state semantics, and ADR 0038.
CONF_LOW_POWER = "low_power"
# Injected at schema time, never user-supplied: the generated buttons' IDs and the identity's
# diagnostic sensor ID (ADR 0009).
CONF_BUTTON_IDS = "button_ids"
CONF_LAST_COMMAND_SENSOR_ID = "last_command_sensor_id"
# Only present when enrollment: true (ADR 0009 -- an ID created late in to_code() is silently
# dropped at runtime).
CONF_ENROLL_BUTTON_ID = "_enroll_button_id"
CONF_DIAGNOSTIC_PROBES = "diagnostic_probes"
CONF_LR1121_FIRMWARE_UPDATE = "lr1121_firmware_update"
CONF_LR1121_BOOTLOADER = "bootloader"
CONF_CHECKSUM_MD5 = "checksum_md5"
CONF_TARGET_VERSION = "target_version"
MIN_STATUS_POLL_INTERVAL_MS = 500
MIN_RAIN_SENSOR_POLL_INTERVAL_MS = 60000

# Internal config key for the "Accept Foreign Pairing" companion switch ID (injected by
# post-validator, same pattern as tuning.py's companion entity IDs — ESPHome 2026.x sizes the
# runtime component vector from IDs known at the end of schema validation, so a companion
# entity created only in to_code() would silently drop; see tuning.py::_inject_tuning_companion_ids
# for the fuller rationale).
CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID = "_accept_foreign_pairing_switch_id"
# Internal config key for the "Recover 1W Controller Key" companion switch ID (injected by
# post-validator; same rationale as CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID above).
CONF_RECOVER_ONEWAY_KEY_SWITCH_ID = "_recover_oneway_key_switch_id"
# Internal config key for the "Flash LR1121 Radio Firmware" companion button ID (injected by
# post-validator; same rationale as CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID above).
CONF_LR1121_FIRMWARE_UPDATE_BUTTON_ID = "_lr1121_firmware_update_button_id"
# Internal config key for the "Scan Paired Devices" companion button ID (injected by
# post-validator; same rationale as CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID above).
CONF_SCAN_PAIRED_DEVICES_BUTTON_ID = "_scan_paired_devices_button_id"
# Internal config key for the "Discover & Pair" button ID (injected by post-validator; same
# rationale as CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID above).
CONF_DISCOVER_AND_PAIR_BUTTON_ID = "_discover_and_pair_button_id"
# Internal config key for the "Last Pairing Result" companion sensor ID that ships with the button
# above (injected by the same post-validator, gated on the same flag). Deliberately NOT shared
# with button.py's identically-purposed CONF_PAIRING_RESULT_SENSOR_ID: during the deprecation
# window (see button.py) both exist, keying different config dicts -- the hub block here, vs. a
# legacy button: entry there.
CONF_DISCOVER_AND_PAIR_RESULT_SENSOR_ID = "_discover_and_pair_result_sensor_id"
# Internal config key for the "Allow LR1121 Bootloader Rewrite (Irreversible)" companion switch ID
# (injected by post-validator; same rationale as CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID above --
# only present when lr1121_firmware_update.bootloader: is configured).
CONF_LR1121_BOOTLOADER_SWITCH_ID = "_lr1121_bootloader_switch_id"

home_io_control_ns = cg.esphome_ns.namespace("home_io_control")
IOHomeControlComponent = home_io_control_ns.class_(
    "IOHomeControlComponent", cg.Component, spi.SPIDevice
)
# Hub-level "Recover System Key" switch (key extraction, key_extraction_responder.cpp /
# platform_hub_controls.h). Deliberately NOT exposed via a `switch:` platform entry: earlier
# revisions dispatched on the presence/absence of `io_device_id` within switch.py, which meant an
# ordinary device-bound switch missing `io_device_id` by mistake would silently become this
# security-sensitive switch instead of failing validation. Gating it behind this boolean (created
# dynamically, like the `tuning:` UI controls) makes that class of mistake structurally
# impossible: there is no shared schema for the two to be confused under.
IOHomeAcceptForeignPairingSwitch = home_io_control_ns.class_(
    "IOHomeAcceptForeignPairingSwitch", switch_component.Switch, cg.Component
)
# Hub-level "Recover 1W Controller Key" switch (key adoption, oneway_key_adoption.cpp /
# platform_hub_controls.h). Same dynamically-created, hub-bound shape and rationale as the switch
# above. Independent of accept_foreign_pairing — the two arm different listeners (2W pairing
# responder vs. 1W add-controller broadcast).
IOHomeRecoverOneWayKeySwitch = home_io_control_ns.class_(
    "IOHomeRecoverOneWayKeySwitch", switch_component.Switch, cg.Component
)
# Hub-level "Flash LR1121 Radio Firmware" button (lr1121_firmware_update_controller.cpp /
# platform_lr1121_controls.h). Same "created dynamically from a home_io_control: sub-block, not a
# device-bound platform entry" shape as the switch above — there is no `io_device_id` to bind this
# to, it targets the hub's own radio.
IOHomeLr1121FirmwareUpdateButton = home_io_control_ns.class_(
    "IOHomeLr1121FirmwareUpdateButton", button_component.Button, cg.Component
)
# Hub-level "Scan Paired Devices" button (management_actions.cpp / platform_hub_controls.h). An
# additional trigger for the already-registered `scan_paired_devices` native API action, which is
# unchanged. Same "created dynamically from the home_io_control: block" shape as the entities
# above -- there is no `io_device_id` to bind a roll-call to.
IOHomeScanPairedDevicesButton = home_io_control_ns.class_(
    "IOHomeScanPairedDevicesButton", button_component.Button, cg.Component
)
# Hub-level "Discover & Pair" button and its companion "Last Pairing Result" diagnostic sensor
# (platform_hub_controls.h). Created from `home_io_control.discover_and_pair_button: true`, the
# same shape as the entity above. Declared here (rather than in button.py, which historically owned
# both) because the deprecated `button:` platform (button.py) also still instantiates them for the
# duration of its deprecation window and imports both names from the package.
IOHomeDiscoverButton = home_io_control_ns.class_(
    "IOHomeDiscoverButton", button_component.Button, cg.Component
)
IOHomePairingResultTextSensor = home_io_control_ns.class_(
    "IOHomePairingResultTextSensor", text_sensor_component.TextSensor, cg.Component
)
# Generated 1W command buttons and their per-identity diagnostic sensor
# (platform_oneway_entities.h). Created from the `oneway_controllers:` block, never a `button:`
# entry, for the same reason as the switch above.
IOHomeOneWayCommandButton = home_io_control_ns.class_(
    "IOHomeOneWayCommandButton", button_component.Button, cg.Component
)
IOHomeOneWayLastCommandTextSensor = home_io_control_ns.class_(
    "IOHomeOneWayLastCommandTextSensor", text_sensor_component.TextSensor, cg.Component
)
# Generated 1W enrollment button (platform_oneway_entities.h), one per identity with
# `enrollment: true`. Same "created from the hub block, never a `button:` entry" reasoning as
# IOHomeOneWayCommandButton above -- see that class's comment.
IOHomeOneWayEnrollButton = home_io_control_ns.class_(
    "IOHomeOneWayEnrollButton", button_component.Button, cg.Component
)
OneWayButtonAction = home_io_control_ns.enum("OneWayButtonAction", is_class=True)


# Hub-level "Allow LR1121 Bootloader Rewrite (Irreversible)" arming switch
# (lr1121_firmware_update_controller.cpp / platform_lr1121_controls.h). Same dynamically-created,
# hub-bound shape as the two entities above; created only when lr1121_firmware_update.bootloader:
# is configured (see _create_lr1121_bootloader_update()).
IOHomeLr1121BootloaderRewriteSwitch = home_io_control_ns.class_(
    "IOHomeLr1121BootloaderRewriteSwitch", switch_component.Switch, cg.Component
)
