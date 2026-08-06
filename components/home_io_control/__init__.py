## @file
## @brief ESPHome hub schema and code generation for Home IO Control.
## @ingroup hioc_codegen
##
## Defines the top-level ``home_io_control:`` YAML schema, shared validators, and the
## generated C++ hub component wiring used by the platform modules.

import hashlib
import urllib.error
import urllib.request

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import spi
# Aliased: this package has its own switch.py/button.py submodules, so a plain `from
# esphome.components import switch` here would bind the name `switch` in this __init__.py's
# namespace — which, because __init__.py IS the esphome.components.home_io_control package
# object, is the exact same slot ESPHome's component loader later overwrites when it imports our
# own switch.py/button.py platform files as `esphome.components.home_io_control.switch` /
# `...button`. Without the alias, whichever import happens to run last silently wins, and
# to_code() (called even later, when tasks are flushed) can end up resolving `switch`/`button` to
# our own platform files instead of the real ESPHome components.
from esphome.components import button as button_component
from esphome.components import switch as switch_component
from esphome.const import CONF_ID, CONF_INVERTED, CONF_NAME, CONF_REF, CONF_SOURCE, ENTITY_CATEGORY_CONFIG
from esphome.core import CORE, ID
from esphome.helpers import write_file_if_changed

from . import lr1121_firmware
from . import tuning as tuning_module

DEPENDENCIES = ["api", "spi"]
AUTO_LOAD = ["button", "cover", "light", "lock", "number", "select", "sensor", "switch", "text_sensor"]
MULTI_CONF = False

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
CONF_TCXO_VOLTAGE = "tcxo_voltage"
CONF_EXPOSED_SENDERS = "exposed_senders"
CONF_ACCEPT_FOREIGN_PAIRING = "accept_foreign_pairing"
CONF_LR1121_FIRMWARE_UPDATE = "lr1121_firmware_update"
CONF_CHECKSUM_MD5 = "checksum_md5"
CONF_TARGET_VERSION = "target_version"
MIN_STATUS_POLL_INTERVAL_MS = 500

# Internal config key for the "Accept Foreign Pairing" companion switch ID (injected by
# post-validator, same pattern as tuning.py's companion entity IDs — ESPHome 2026.x sizes the
# runtime component vector from IDs known at the end of schema validation, so a companion
# entity created only in to_code() would silently drop; see tuning.py::_inject_tuning_companion_ids
# for the fuller rationale).
CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID = "_accept_foreign_pairing_switch_id"
# Internal config key for the "Flash LR1121 Radio Firmware" companion button ID (injected by
# post-validator; same rationale as CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID above).
CONF_LR1121_FIRMWARE_UPDATE_BUTTON_ID = "_lr1121_firmware_update_button_id"

home_io_control_ns = cg.esphome_ns.namespace("home_io_control")
IOHomeControlComponent = home_io_control_ns.class_(
    "IOHomeControlComponent", cg.Component, spi.SPIDevice
)
# Hub-level "Accept Foreign Pairing (Key Extraction)" switch (hub_key_extraction.cpp /
# platform_accept_foreign_pairing_switch.h). Deliberately NOT exposed via a `switch:` platform
# entry: earlier revisions dispatched on the presence/absence of `io_device_id` within switch.py,
# which meant an ordinary device-bound switch missing `io_device_id` by mistake would silently
# become this security-sensitive switch instead of failing validation. Gating it behind this
# boolean (created dynamically, like the `tuning:` UI controls) makes that class of mistake
# structurally impossible: there is no shared schema for the two to be confused under.
IOHomeAcceptForeignPairingSwitch = home_io_control_ns.class_(
    "IOHomeAcceptForeignPairingSwitch", switch_component.Switch, cg.Component
)
# Hub-level "Flash LR1121 Radio Firmware" button (hub_lr1121_firmware_update.cpp /
# platform_lr1121_firmware_update_button.h). Same "created dynamically from a home_io_control:
# sub-block, not a device-bound platform entry" shape as the switch above — there is no
# `io_device_id` to bind this to, it targets the hub's own radio.
IOHomeLr1121FirmwareUpdateButton = home_io_control_ns.class_(
    "IOHomeLr1121FirmwareUpdateButton", button_component.Button, cg.Component
)


def _inject_accept_foreign_pairing_switch_id(config):
    if not config[CONF_ACCEPT_FOREIGN_PAIRING]:
        return config
    parent_id = config[CONF_ID]
    base = parent_id.id if parent_id.id else "home_io_control"
    config[CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID] = ID(
        f"{base}_accept_foreign_pairing_switch",
        is_declaration=True,
        type=IOHomeAcceptForeignPairingSwitch,
    )
    return config


def validate_lr1121_firmware_source(value):
    """Validate the lr1121_firmware_update `source:` shorthand at schema time.

    Only checks the shape (github://owner/repo/path[@ref]); the network fetch and MD5/image
    verification happen later, in to_code(), where a failure is still a build-time error but one
    that needs the network anyway.
    """
    value = cv.string_strict(value)
    try:
        lr1121_firmware.parse_github_source(value)
    except lr1121_firmware.Lr1121FirmwareError as err:
        raise cv.Invalid(str(err)) from err
    return value


def validate_checksum_md5(value):
    """Validate checksum_md5 as exactly 32 hex characters (MD5)."""
    value = cv.string_strict(value).lower()
    if len(value) != 32:
        raise cv.Invalid("checksum_md5 must be exactly 32 hex characters (MD5)")
    try:
        int(value, 16)
    except ValueError as err:
        raise cv.Invalid("checksum_md5 must be valid hexadecimal") from err
    return value


LR1121_FIRMWARE_UPDATE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_SOURCE): validate_lr1121_firmware_source,
        cv.Optional(CONF_REF): cv.string_strict,
        cv.Optional(CONF_CHECKSUM_MD5): validate_checksum_md5,
        # target_version exists solely as an escape hatch for a mirrored/renamed image whose
        # filename carries no version — NOT as a compatibility declaration. There is deliberately
        # no `requires_bootloader:` key; see the design record for why user-supplied data would be
        # the wrong shape for a safety check.
        cv.Optional(CONF_TARGET_VERSION): cv.hex_int,
    }
)


def _validate_lr1121_firmware_update(config):
    """Gate + inject the button ID for the optional lr1121_firmware_update: block.

    Only runs when the block is present. Rejects configurations that can't reach the LR1121
    bootloader at all (wrong radio_type, missing busy_pin) or that would silently invert the
    bootloader-entry level (busy_pin inverted: true — bootloader entry drives BUSY to a physical
    LOW; see radio_lr1121_firmware_updater.h). Also injects the flash button's companion ID at
    validation time — see CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID's comment above for why that
    can't wait until to_code().
    """
    if CONF_LR1121_FIRMWARE_UPDATE not in config:
        return config
    try:
        lr1121_firmware.validate_bootloader_reachability(
            radio_type=config[CONF_RADIO_TYPE],
            has_busy_pin=CONF_BUSY_PIN in config,
            busy_pin_inverted=config.get(CONF_BUSY_PIN, {}).get(CONF_INVERTED, False),
        )
    except lr1121_firmware.Lr1121FirmwareError as err:
        raise cv.Invalid(str(err)) from err

    parent_id = config[CONF_ID]
    base = parent_id.id if parent_id.id else "home_io_control"
    config[CONF_LR1121_FIRMWARE_UPDATE_BUTTON_ID] = ID(
        f"{base}_lr1121_firmware_update_button",
        is_declaration=True,
        type=IOHomeLr1121FirmwareUpdateButton,
    )
    return config


PA_PIN_OPTIONS = {
    "BOOST": 0x80,
    "RFO": 0x00,
}

RADIO_TYPE_OPTIONS = {
    "sx1276": "sx1276",
    "sx1262": "sx1262",
    "lr1121": "lr1121",
}

TCXO_VOLTAGE_OPTIONS = {
    "1_6V": 0x01,
    "1_7V": 0x02,
    "1_8V": 0x03,
    "2_2V": 0x04,
    "2_4V": 0x05,
    "2_7V": 0x06,
    "3_0V": 0x07,
    "3_3V": 0x08,
}

DEVICE_TYPE_OPTIONS = {
    "unknown": 0x00,
    "venetian_blind": 0x01,
    "roller_shutter": 0x02,
    "awning": 0x03,
    "window_opener": 0x04,
    "garage_opener": 0x05,
    "light": 0x06,
    "gate_opener": 0x07,
    "rolling_door_opener": 0x08,
    "lock": 0x09,
    "blind": 0x0A,
    "screen": 0x0B,
    "dual_shutter": 0x0D,
    "heating_temperature_interface": 0x0E,
    "on_off_switch": 0x0F,
    "horizontal_awning": 0x10,
    "external_venetian_blind": 0x11,
    "louvre_blind": 0x12,
    "curtain_track": 0x13,
    "intrusion_alarm": 0x17,
    "swinging_shutter": 0x18,
}


def _resolve_device_type_token(token):
    """Resolve a lowercase, stripped device-type token (name or raw int/hex string) to 0-255.

    Single source of truth for the "named value from DEVICE_TYPE_OPTIONS, else raw integer"
    acceptance rule shared by validate_device_type() (io_device_type) and
    validate_linked_remote_entry() (the class:<device_type> linked-remotes form) — both accept
    the exact same set of device-type spellings, so the lookup lives here once.
    @raises ValueError if token is neither a known name nor a parseable integer.
    @raises cv.Invalid if token parses as an integer but is out of range 0-255.
    """
    if token in DEVICE_TYPE_OPTIONS:
        return DEVICE_TYPE_OPTIONS[token]
    return cv.int_range(min=0, max=0xFF)(int(token, 0))


def validate_device_type(value):
    """Validate io_device_type as a named string or integer 0-255."""
    if isinstance(value, int):
        return cv.int_range(min=0, max=0xFF)(value)

    if isinstance(value, str):
        normalized = cv.string_strict(value).strip().lower()
        try:
            return _resolve_device_type_token(normalized)
        except ValueError as err:
            raise cv.Invalid(
                "Device type must be a known name or an integer in the range 0..255 (for example 0x11)"
            ) from err

    raise cv.Invalid(
        "Device type must be a known name or an integer in the range 0..255"
    )


def device_type_expression(value):
    """Generate a C++ static_cast expression for a validated device type."""
    return cg.RawExpression(
        f"static_cast<esphome::home_io_control::DeviceType>(0x{value:02X})"
    )


def validate_node_id(value):
    """Validate node_id as exactly 6 hex characters (3 bytes)."""
    value = cv.string_strict(value).upper()
    if len(value) != 6:
        raise cv.Invalid("Node ID must be exactly 6 hex characters (3 bytes)")
    try:
        int(value, 16)
    except ValueError:
        raise cv.Invalid("Node ID must be valid hexadecimal")
    return value


def validate_system_key(value):
    """Validate system_key as exactly 32 hex characters (16 bytes)."""
    value = cv.string_strict(value).upper()
    if len(value) != 32:
        raise cv.Invalid("System key must be exactly 32 hex characters (16 bytes)")
    try:
        int(value, 16)
    except ValueError:
        raise cv.Invalid("System key must be valid hexadecimal")
    return value


def validate_device_id(value):
    """Validate io_device_id as exactly 6 hex characters (3 bytes)."""
    value = cv.string_strict(value).upper()
    if len(value) != 6:
        raise cv.Invalid("Device ID must be exactly 6 hex characters (3 bytes)")
    try:
        int(value, 16)
    except ValueError:
        raise cv.Invalid("Device ID must be valid hexadecimal")
    return value


def validate_linked_remote_entry(value):
    """Validate a linked_remotes entry: either a device ID or 'class:<device_type>'.

    The class form matches how 1W remotes address a typed broadcast (e.g. "all awnings")
    rather than a single node, so one entry can cover many same-type devices without
    enumerating each one. Shares _resolve_device_type_token() with validate_device_type()
    so a type without a named YAML alias yet (e.g. discovered via pairing) can still be
    class-linked. Normalized to 'class:0x<HH>' (uppercase hex) so wire_device_binding() can
    parse the type directly without a second DEVICE_TYPE_OPTIONS lookup; bare device IDs are
    validated exactly as before and behave identically.
    """
    if isinstance(value, str) and value.lower().startswith("class:"):
        type_token = value.split(":", 1)[1].strip().lower()
        try:
            type_value = _resolve_device_type_token(type_token)
        except ValueError as err:
            raise cv.Invalid(
                f"Unknown device class '{type_token}' in linked_remotes; expected one of: "
                + ", ".join(sorted(DEVICE_TYPE_OPTIONS))
                + ", or a raw integer such as 0x14"
            ) from err
        return f"class:0x{type_value:02X}"
    return validate_device_id(value)


def validate_status_poll_interval(value):
    """Validate status_poll_interval is at least MIN_STATUS_POLL_INTERVAL_MS."""
    value = cv.positive_time_period_milliseconds(value)
    if value.total_milliseconds < MIN_STATUS_POLL_INTERVAL_MS:
        raise cv.Invalid(
            f"status_poll_interval must be at least {MIN_STATUS_POLL_INTERVAL_MS}ms"
        )
    return value


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(IOHomeControlComponent),
            cv.Required(CONF_RST_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_DIO0_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_DIO4_PIN): pins.internal_gpio_input_pin_schema,
            # The chip's IRQ line: SX1262's DIO1, or LR1121's DIO9
            cv.Optional(CONF_DIO1_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_BUSY_PIN): pins.internal_gpio_input_pin_schema,
            cv.Required(CONF_NODE_ID): validate_node_id,
            cv.Required(CONF_SYSTEM_KEY): cv.sensitive(validate_system_key),
            cv.Optional(CONF_TX_POWER, default=17): cv.int_range(min=0, max=22),
            cv.Optional(CONF_PA_PIN, default="BOOST"): cv.enum(
                PA_PIN_OPTIONS, upper=True
            ),
            cv.Required(CONF_RADIO_TYPE): cv.enum(RADIO_TYPE_OPTIONS, lower=True),
            cv.Optional(CONF_FEM_EN_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_VFEM_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_FEM_PA_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_TCXO_VOLTAGE, default="1_8V"): cv.enum(
                TCXO_VOLTAGE_OPTIONS, upper=True
            ),
            cv.Optional(CONF_EXPOSED_SENDERS, default=[]): cv.ensure_list(
                validate_device_id
            ),
            cv.Optional(CONF_ACCEPT_FOREIGN_PAIRING, default=False): cv.boolean,
            cv.Optional(CONF_LR1121_FIRMWARE_UPDATE): LR1121_FIRMWARE_UPDATE_SCHEMA,
            cv.Optional(tuning_module.CONF_TUNING): tuning_module.TUNING_CONFIG_SCHEMA,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(spi.spi_device_schema(True, 8e6, "mode0")),
    _inject_accept_foreign_pairing_switch_id,
    _validate_lr1121_firmware_update,
)


async def to_code(config):
    # Hub-level management actions and result events are compiled behind native API
    # feature flags. Home IO Control enables the required compile-time switches here
    # so users only need a normal `api:` block in YAML.
    # ESPHome 2026.x additionally gates user-defined actions behind
    # USE_API_USER_DEFINED_ACTIONS.
    cg.add_define("USE_API_USER_DEFINED_ACTIONS")
    cg.add_define("USE_API_CUSTOM_SERVICES")
    cg.add_define("USE_API_HOMEASSISTANT_SERVICES")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await spi.register_spi_device(var, config)

    rst_pin = await cg.gpio_pin_expression(config[CONF_RST_PIN])
    cg.add(var.set_rst_pin(rst_pin))

    if CONF_DIO0_PIN in config:
        dio0_pin = await cg.gpio_pin_expression(config[CONF_DIO0_PIN])
        cg.add(var.set_dio0_pin(dio0_pin))

    if CONF_DIO4_PIN in config:
        dio4_pin = await cg.gpio_pin_expression(config[CONF_DIO4_PIN])
        cg.add(var.set_dio4_pin(dio4_pin))

    if CONF_DIO1_PIN in config:
        dio1_pin = await cg.gpio_pin_expression(config[CONF_DIO1_PIN])
        cg.add(var.set_dio1_pin(dio1_pin))

    if CONF_BUSY_PIN in config:
        busy_pin = await cg.gpio_pin_expression(config[CONF_BUSY_PIN])
        cg.add(var.set_busy_pin(busy_pin))

    if CONF_FEM_EN_PIN in config:
        fem_en_pin = await cg.gpio_pin_expression(config[CONF_FEM_EN_PIN])
        cg.add(var.set_fem_en_pin(fem_en_pin))

    if CONF_VFEM_PIN in config:
        vfem_pin = await cg.gpio_pin_expression(config[CONF_VFEM_PIN])
        cg.add(var.set_vfem_pin(vfem_pin))

    if CONF_FEM_PA_PIN in config:
        fem_pa_pin = await cg.gpio_pin_expression(config[CONF_FEM_PA_PIN])
        cg.add(var.set_fem_pa_pin(fem_pa_pin))

    cg.add(var.set_node_id(config[CONF_NODE_ID]))
    cg.add(var.set_system_key(config[CONF_SYSTEM_KEY]))
    cg.add(var.set_tx_power(config[CONF_TX_POWER]))
    cg.add(var.set_pa_pin(config[CONF_PA_PIN]))

    cg.add(var.set_radio_type(config[CONF_RADIO_TYPE]))

    cg.add(var.set_tcxo_voltage(config[CONF_TCXO_VOLTAGE]))

    for sender_id in config[CONF_EXPOSED_SENDERS]:
        cg.add(var.add_exposed_sender(sender_id))

    if config[CONF_ACCEPT_FOREIGN_PAIRING]:
        await _create_accept_foreign_pairing_switch(config, var)

    if CONF_LR1121_FIRMWARE_UPDATE in config:
        await _create_lr1121_firmware_update(config, var)

    if tuning_module.CONF_TUNING in config:
        await tuning_module.to_code(config[tuning_module.CONF_TUNING], var)


async def _create_accept_foreign_pairing_switch(config, var):
    """Create the hub-level "Accept Foreign Pairing (Key Extraction)" switch.

    Mirrors tuning.py's _create_number()/_create_select(): normalize a bare {id, name} dict
    through switch_schema()+COMPONENT_SCHEMA so it carries the entity/component defaults
    register_switch()/register_component() require, matching the neighboring pattern rather than
    hand-assembling a config dict shape of its own.
    """
    entity_config = switch_component.switch_schema(
        IOHomeAcceptForeignPairingSwitch,
        default_restore_mode="ALWAYS_OFF",  # never auto-arm after a reboot
        entity_category=ENTITY_CATEGORY_CONFIG,
    ).extend(cv.COMPONENT_SCHEMA)(
        {
            CONF_ID: config[CONF_ACCEPT_FOREIGN_PAIRING_SWITCH_ID],
            CONF_NAME: "Accept Foreign Pairing (Key Extraction)",
        }
    )
    entity = await switch_component.new_switch(entity_config)
    await cg.register_component(entity, entity_config)
    cg.add(entity.set_parent(var))


def _cached_http_fetch(cache_dir):
    """Build a `fetch(url, expected_hash=None) -> bytes` callable for
    lr1121_firmware.fetch_and_verify(), backed by an on-disk cache so repeat and offline builds
    don't re-download the same source (design plan §5.2).

    The cache key incorporates `expected_hash` (the MD5 fetch_and_verify() already resolved from
    the `.md5` sidecar or `checksum_md5:` before calling this for the `.bin`) rather than being
    `sha256(url)` alone. With the default `ref: HEAD` the URL never changes, so a plain
    url-only key means a corrupt/truncated download poisons the cache permanently -- no config
    change can ever invalidate it, since nothing about the request changes on retry. Folding the
    expected hash in means correcting a wrong `checksum_md5:` (or a fixed upstream sidecar) misses
    the poisoned entry and forces a fresh download. The `.md5` sidecar fetch itself has no
    expected_hash to key on (chicken-and-egg -- it's what supplies one for the .bin) and is cached
    under the URL alone; a corrupted sidecar is a much smaller/rarer risk than a corrupted 64+ KB
    binary, and the cache directory below is a manual escape hatch either way.

    Data that fails its own hash check is deliberately never written to the cache (verify-before-store):
    a transient network corruption then simply retries cleanly on the next build, with no
    config change needed at all.
    """
    cache_dir.mkdir(parents=True, exist_ok=True)

    def fetch(url, expected_hash=None):
        cache_key = hashlib.sha256(f"{url}|{expected_hash or ''}".encode("utf-8")).hexdigest()
        cache_path = cache_dir / cache_key
        if cache_path.exists():
            return cache_path.read_bytes()
        try:
            with urllib.request.urlopen(url, timeout=30) as response:  # noqa: S310
                data = response.read()
        except urllib.error.HTTPError as err:
            if err.code == 404:
                raise lr1121_firmware.Lr1121FirmwareNotFoundError(url) from err
            raise lr1121_firmware.Lr1121FirmwareError(f"HTTP {err.code} fetching {url}") from err
        except urllib.error.URLError as err:
            raise lr1121_firmware.Lr1121FirmwareError(f"Failed to fetch {url}: {err}") from err
        if expected_hash is None or hashlib.md5(data).hexdigest() == expected_hash:  # noqa: S324
            cache_path.write_bytes(data)
        return data

    return fetch


def _render_lr1121_firmware_header(image):
    """Render the verified firmware image as a C++ header (design plan §5.3).

    Each raw 4-byte chunk of the `.bin` is exactly one big-endian word as Semtech's own image
    format already lays it out, so this only has to slice and format, not transform, the bytes.
    `inline const` (not `constexpr`) for the array: it is never used in a constant expression, so
    forcing constant-evaluation of up to ~61k elements would only cost compile time; `const` at
    namespace scope still lands in `.rodata` (flash) on ESP32, not RAM.
    """
    words = [f"0x{int.from_bytes(image.data[i : i + 4], 'big'):08X}" for i in range(0, len(image.data), 4)]
    words_per_line = 8
    body_lines = [
        "    " + ", ".join(words[i : i + words_per_line]) + "," for i in range(0, len(words), words_per_line)
    ]
    return "\n".join(
        [
            "#pragma once",
            "// Auto-generated by the home_io_control lr1121_firmware_update build step. Do not edit.",
            "#include <cstddef>",
            "#include <cstdint>",
            "",
            "namespace esphome {",
            "namespace home_io_control {",
            "",
            "inline const uint32_t LR1121_FIRMWARE_UPDATE_IMAGE[] = {",
            *body_lines,
            "};",
            f"inline constexpr size_t LR1121_FIRMWARE_UPDATE_IMAGE_WORDS = {len(words)};",
            f"inline constexpr uint16_t LR1121_FIRMWARE_UPDATE_TARGET_VERSION = 0x{image.version:04X};",
            "",
            "}  // namespace home_io_control",
            "}  // namespace esphome",
            "",
        ]
    )


async def _create_lr1121_firmware_update(config, var):
    """Fetch/verify the configured firmware image, generate its header, set the build flag that
    gates the whole feature, and create the "Flash LR1121 Radio Firmware" button.

    The block's mere presence in YAML is the build flag (design plan §5.4) — there is no
    separate enable switch, so entering/leaving flash mode is a recompile + OTA each way.
    """
    fw_config = config[CONF_LR1121_FIRMWARE_UPDATE]
    cache_dir = CORE.data_dir / "lr1121_firmware_cache"
    try:
        image = lr1121_firmware.fetch_and_verify(
            source=fw_config[CONF_SOURCE],
            ref=fw_config.get(CONF_REF),
            checksum_md5=fw_config.get(CONF_CHECKSUM_MD5),
            target_version=fw_config.get(CONF_TARGET_VERSION),
            fetch=_cached_http_fetch(cache_dir),
        )
    except lr1121_firmware.Lr1121FirmwareError as err:
        raise cv.Invalid(f"lr1121_firmware_update: {err}") from err

    header_path = CORE.relative_src_path("lr1121_firmware_update_image.h")
    write_file_if_changed(header_path, _render_lr1121_firmware_header(image))

    cg.add_define("IOHOME_LR1121_FIRMWARE_UPDATE")

    entity_config = button_component.button_schema(
        IOHomeLr1121FirmwareUpdateButton,
        entity_category=ENTITY_CATEGORY_CONFIG,
    ).extend(cv.COMPONENT_SCHEMA)(
        {
            CONF_ID: config[CONF_LR1121_FIRMWARE_UPDATE_BUTTON_ID],
            CONF_NAME: "Flash LR1121 Radio Firmware",
        }
    )
    entity = await button_component.new_button(entity_config)
    await cg.register_component(entity, entity_config)
    cg.add(entity.set_parent(var))
