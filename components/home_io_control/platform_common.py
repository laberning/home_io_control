## @file
## @brief Shared ESPHome codegen for IO-Homecontrol device-bound platforms.
## @ingroup hioc_codegen
##
## The cover, light, switch, lock, fan, climate and water_heater platforms all bind an ESPHome entity to a hub
## device: the same config keys, the same auto-generated companion diagnostic sensors
## (device name, active issue, RSSI, last contact, exchange failures, unconfirmed exchanges,
## last commanded by, last command source), and the same to_code() wiring (set_parent /
## set_device_id / device type / subtype / poll interval / linked remotes). This module is the
## single home for that shared logic so a change lands in one place instead of in each — platform
## files call one
## inject_companion_sensor_ids() post-validator and one create_companion_sensors()
## to_code() helper. Any other device-bound companion (a button, switch, select or extra sensor)
## is declared by a platform table through inject_companion_ids() and created with
## create_companion(); the platform files keep their tables, schema arguments and extra setters.

import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, sensor, switch, text_sensor
from esphome.const import (
    CONF_DEFAULT_TRANSITION_LENGTH,
    CONF_DISABLED_BY_DEFAULT,
    CONF_ID,
    CONF_NAME,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
)
from esphome.components.sensor import DEVICE_CLASS_SIGNAL_STRENGTH
from esphome.core import ID

from . import (
    home_io_control_ns,
    IOHomeControlComponent,
    CONF_HOME_IO_CONTROL_ID,
    CONF_LOW_POWER,
    device_type_expression,
    inherit_esphome_device,
    validate_device_id,
    validate_device_type,
    validate_linked_remote_entry,
    validate_status_poll_interval,
)
from .hub_names import (
    CONF_IO_DEVICE_ID,
    CONF_IO_DEVICE_TYPE,
    CONF_LINKED_REMOTES,
    CONF_SYSTEM_KEY,
)
from .hub_validators import (
    DEVICE_MODEL_OPTIONS,
    DEVICE_READINGS,
    READING_FEATURE_HINTS,
    READING_REMOVE_HINTS,
    check_feature_name,
    device_features,
    device_model_expression,
    device_models,
    device_type_name,
    kind_is_experimental,
    type_offers_feature,
    validate_device_model_name,
    validate_system_key,
)

_LOGGER = logging.getLogger(__name__)

# The published page that tells a user how to report a device; the anchor is the one the docs site
# generates for the "Reporting unsupported devices" heading of the contributing page.
REPORTING_DEVICES_URL = (
    "https://laberning.github.io/home_io_control/contributing.html#reporting-unsupported-devices"
)

# Shared YAML config keys used by every device-bound platform (CONF_IO_DEVICE_ID and
# CONF_LINKED_REMOTES come from hub_names.py).
CONF_DEVICE_TYPE = CONF_IO_DEVICE_TYPE
CONF_SUBTYPE = "io_subtype"
# A product model below io_device_type / io_subtype, for products that report the same wire type but
# need different handling (ADR 0067). Only a few (type, subtype) offer one (hub_validators.device_models()).
CONF_MODEL = "io_model"
CONF_STATUS_POLL_INTERVAL = "status_poll_interval"

# Internal config key for the companion device-name sensor ID (injected by post-validator).
CONF_DEVICE_NAME_SENSOR_ID = "_device_name_sensor_id"
# Internal config key for the companion active-issue sensor ID (injected by post-validator).
CONF_ACTIVE_ISSUE_SENSOR_ID = "_active_issue_sensor_id"
# Internal config keys for the companion link-health sensor IDs (injected by post-validator).
CONF_RSSI_SENSOR_ID = "_rssi_sensor_id"
CONF_LAST_CONTACT_SENSOR_ID = "_last_contact_sensor_id"
CONF_EXCHANGE_FAILURES_SENSOR_ID = "_exchange_failures_sensor_id"
CONF_UNCONFIRMED_EXCHANGES_SENSOR_ID = "_unconfirmed_exchanges_sensor_id"
# Internal config keys for the companion last-command sensor IDs (injected by post-validator).
CONF_LAST_COMMANDED_BY_SENSOR_ID = "_last_commanded_by_sensor_id"
CONF_LAST_COMMAND_SOURCE_SENSOR_ID = "_last_command_source_sensor_id"

IOHomeDeviceNameTextSensor = home_io_control_ns.class_(
    "IOHomeDeviceNameTextSensor", text_sensor.TextSensor, cg.Component
)
IOHomeActiveIssueTextSensor = home_io_control_ns.class_(
    "IOHomeActiveIssueTextSensor", text_sensor.TextSensor, cg.Component
)
# The generic entities that show a named device reading (platform_device_readings.h) and the heater setting switch
# (platform_climate_controls.h), and the readings they are named after (proto_device_readings.h DeviceReading).
IOHomeDeviceReadingSensor = home_io_control_ns.class_(
    "IOHomeDeviceReadingSensor", sensor.Sensor, cg.Component
)
IOHomeDeviceReadingBinarySensor = home_io_control_ns.class_(
    "IOHomeDeviceReadingBinarySensor", binary_sensor.BinarySensor, cg.Component
)
IOHomeDeviceReadingTextSensor = home_io_control_ns.class_(
    "IOHomeDeviceReadingTextSensor", text_sensor.TextSensor, cg.Component
)
IOHomeHeaterSettingSwitch = home_io_control_ns.class_(
    "IOHomeHeaterSettingSwitch", switch.Switch, cg.Component
)
DeviceReading = home_io_control_ns.enum("DeviceReading", is_class=True)

IOHomeRssiSensor = home_io_control_ns.class_("IOHomeRssiSensor", sensor.Sensor, cg.Component)
IOHomeLastContactSensor = home_io_control_ns.class_(
    "IOHomeLastContactSensor", sensor.Sensor, cg.Component
)
IOHomeExchangeFailuresSensor = home_io_control_ns.class_(
    "IOHomeExchangeFailuresSensor", sensor.Sensor, cg.Component
)
IOHomeUnconfirmedExchangesSensor = home_io_control_ns.class_(
    "IOHomeUnconfirmedExchangesSensor", sensor.Sensor, cg.Component
)
IOHomeLastCommandedByTextSensor = home_io_control_ns.class_(
    "IOHomeLastCommandedByTextSensor", text_sensor.TextSensor, cg.Component
)
IOHomeLastCommandSourceTextSensor = home_io_control_ns.class_(
    "IOHomeLastCommandSourceTextSensor", text_sensor.TextSensor, cg.Component
)


# (id key, ID suffix, codegen class, condition) of every auto-generated companion sensor. The
# condition is None: they exist for every device-bound entity.
_COMPANION_SENSOR_IDS = (
    (CONF_DEVICE_NAME_SENSOR_ID, "device_name_sensor", IOHomeDeviceNameTextSensor, None),
    (CONF_ACTIVE_ISSUE_SENSOR_ID, "active_issue_sensor", IOHomeActiveIssueTextSensor, None),
    (CONF_RSSI_SENSOR_ID, "rssi_sensor", IOHomeRssiSensor, None),
    (CONF_LAST_CONTACT_SENSOR_ID, "last_contact_sensor", IOHomeLastContactSensor, None),
    (
        CONF_EXCHANGE_FAILURES_SENSOR_ID,
        "exchange_failures_sensor",
        IOHomeExchangeFailuresSensor,
        None,
    ),
    (
        CONF_UNCONFIRMED_EXCHANGES_SENSOR_ID,
        "unconfirmed_exchanges_sensor",
        IOHomeUnconfirmedExchangesSensor,
        None,
    ),
    (
        CONF_LAST_COMMANDED_BY_SENSOR_ID,
        "last_commanded_by_sensor",
        IOHomeLastCommandedByTextSensor,
        None,
    ),
    (
        CONF_LAST_COMMAND_SOURCE_SENSOR_ID,
        "last_command_source_sensor",
        IOHomeLastCommandSourceTextSensor,
        None,
    ),
)


def companion_entity_name(config, suffix):
    """Derive a companion entity's name from the parent entity name and a suffix."""
    base_name = config.get(CONF_NAME, "")
    if base_name:
        return f"{base_name} {suffix}"
    return suffix


def companion_id_base(config, parent_id_key):
    """Return the shared ID prefix for a platform's companion entity IDs.

    ESPHome 2026.x sizes its runtime component vector (StaticVector) from the number of
    component IDs known at the end of schema validation — before to_code() runs.  If
    companion entities are only created inside to_code(), their IDs are not counted and
    the StaticVector overflows at runtime, silently dropping later components whose
    setup() then never executes.  Companion IDs must therefore be declared during
    validation, and they all share the prefix returned here.

    ``parent_id_key`` differs per platform: light reads the entity ID from
    CONF_OUTPUT_ID, while cover, switch and lock read it from CONF_ID. Falls back to the
    entity's CONF_ID (the sibling LightState, for light) when parent_id_key's own field wasn't
    manually set, then to a sanitized form of the entity name. Raises cv.Invalid when neither an
    explicit id: nor a non-empty name: is available to derive a unique prefix from — reachable via
    the `name: ""`/`name: None` device-name idiom (see validate_entity_name()) without an id:.
    """
    from esphome.helpers import sanitize

    parent_id = config[parent_id_key]
    if parent_id.id:
        return parent_id.id
    # Light's parent_id_key is CONF_OUTPUT_ID (the LightOutput), which has no YAML-settable id:
    # of its own — a user's `id:` lands on CONF_ID (the sibling LightState) instead. Check that
    # before giving up, so an explicit id: still anchors the empty-name idiom on a light. This is
    # a no-op for cover/switch/lock, where parent_id_key already *is* CONF_ID.
    if parent_id_key != CONF_ID:
        sibling_id = config.get(CONF_ID)
        if sibling_id is not None and sibling_id.id:
            return sibling_id.id
    # When no explicit id: is given, ESPHome auto-generates it after validation.
    # At this point .id may still be None, so derive from the entity name instead.
    if not config[CONF_NAME]:
        # An empty name is ESPHome's device-name idiom (see esphome/core/entity_helpers.py's
        # get_base_entity_object_id() — the entity then displays as just the sub-device's name).
        # Without an explicit id: to fall back on, every such entity on the same platform would
        # sanitize down to the same empty prefix and collide on companion IDs like
        # "_favorite_button"/"_device_name_sensor" instead of failing loudly.
        #
        # The message names both spellings: validate_entity_name() has already normalized
        # `name: None`/`name: none` to "" by now, so a user who wrote the literal would otherwise
        # be told their name is "empty" without that word appearing anywhere in their YAML.
        raise cv.Invalid(
            'An entity using the device-name idiom — name: "", or the YAML literal name: None / '
            "name: none, which normalize to the same empty name — must also declare an explicit "
            "id:, otherwise its companion entity IDs cannot be derived uniquely."
        )
    return sanitize(config[CONF_NAME]).lower()


def has_feature(feature):
    """Condition for a companion spec: the declared kind has the capability named `feature`.

    A config that declares no io_device_type has no capabilities, so the companion is not created
    (see declared_features()). A name that is no capability raises ValueError when the spec is built.
    """
    check_feature_name(feature)

    def condition(config):
        return declared_has_feature(config, feature)

    return condition


def inject_companion_ids(config, specs, parent_id_key):
    """Declare a platform's companion entity IDs during schema validation.

    ``specs`` is a tuple of ``(id key, ID suffix, codegen class, condition)``; each row whose
    condition holds puts an ``ID`` declared as ``<base>_<suffix>`` under ``config[id key]``, where
    ``<base>`` is companion_id_base(). The condition is ``None`` (always), a config key that must be
    present, or a function of the config (has_feature(), or e.g. a boolean option). The platform
    creates the entity from the same id key in to_code(), see create_companion(). See
    companion_id_base() for why the IDs must be declared at validation time.
    """
    base = companion_id_base(config, parent_id_key)
    for id_key, id_suffix, entity_class, condition in specs:
        if condition is None:
            wanted = True
        elif callable(condition):
            wanted = condition(config)
        else:
            wanted = condition in config
        if wanted:
            config[id_key] = ID(f"{base}_{id_suffix}", is_declaration=True, type=entity_class)
    return config


def inject_companion_sensor_ids(config, parent_id_key):
    """Declare every auto-generated companion sensor ID during schema validation.

    Shared post-validator body for every device-bound platform: inject_companion_ids() over
    _COMPANION_SENSOR_IDS.
    """
    return inject_companion_ids(config, _COMPANION_SENSOR_IDS, parent_id_key)


def validate_entity_name(value):
    """Validate a device-bound platform's `name:`, keeping it required but honoring ESPHome's
    device-name idiom (`name: ""` or the YAML literal `name: None`/`none` — both mean "this
    entity displays as just its sub-device's name", see companion_id_base()'s empty-name path).

    ENTITY_BASE_SCHEMA's own Optional(CONF_NAME) validator (`esphome.config_validation
    ._validate_entity_name`) never runs for these platforms — platform_schema_extension()
    overrides that key with a Required one — so its None -> "" conversion, its NAME_MAX_LENGTH
    check, and its '/' handling all have to be reapplied here. Delegating rather than
    reimplementing keeps `name: ""` / `name: None` behaving exactly as they do in every other
    ESPHome component, including upstream's own asymmetry: `name: None` additionally requires
    `esphome: friendly_name:` to be set (matching Home Assistant's own null-name convention),
    while `name: ""` does not.
    """
    # Private, but it is the only place these rules live; a vendored copy would silently drift.
    value = cv._validate_entity_name(value)
    # `_entity_base_validator` normally turns a None result into "" for us, but it only runs for
    # schemas that did not override CONF_NAME the way platform_schema_extension() does.
    return "" if value is None else value


def platform_schema_extension():
    """Return the shared schema keys every device-bound platform extends with."""
    return {
        cv.Required(CONF_NAME): validate_entity_name,
        cv.GenerateID(CONF_HOME_IO_CONTROL_ID): cv.use_id(IOHomeControlComponent),
        cv.Required(CONF_IO_DEVICE_ID): validate_device_id,
        cv.Optional(CONF_DEVICE_TYPE): validate_device_type,
        cv.Optional(CONF_SUBTYPE): cv.int_range(min=0, max=63),
        cv.Optional(CONF_MODEL): validate_device_model_name,
        cv.Optional(CONF_LINKED_REMOTES): cv.ensure_list(validate_linked_remote_entry),
        cv.Optional(CONF_STATUS_POLL_INTERVAL): validate_status_poll_interval,
        cv.Optional(CONF_LOW_POWER): cv.boolean,
        # A key this one device keeps of its own, for a device that does not share the hub's
        # (ADR 0053). Same validator as the hub's system_key; `!secret` works as for any string.
        # cv.sensitive, as on the hub and the 1W controllers, so `esphome config` and the
        # dashboard mask the key instead of printing it.
        cv.Optional(CONF_SYSTEM_KEY): cv.sensitive(validate_system_key),
    }


def config_model(config):
    """The DEVICE_MODEL_OPTIONS value of the entry's io_model:, 0 when it declares none."""
    return DEVICE_MODEL_OPTIONS.get(config.get(CONF_MODEL), 0)


def _kind_label(device_type, subtype, model=None):
    """The declared kind as a user wrote it: the io_device_type name (or hex value) and, when set,
    io_subtype and io_model."""
    name = device_type_name(device_type) or f"0x{device_type:02X}"
    details = []
    if subtype != 0:
        details.append(f"io_subtype {subtype}")
    if model is not None:
        details.append(f"io_model {model}")
    return f"{name} ({', '.join(details)})" if details else name


def validate_device_model(config):
    """Accept io_model: only where the declared io_device_type / io_subtype offers that model.

    A post-validator for every device-bound platform. A model is a row below a (type, subtype)
    (ADR 0067), so it means nothing without both keys; the error names the models the declared kind
    offers, or says it offers none.
    """
    if CONF_MODEL not in config:
        return config
    model = config[CONF_MODEL]
    if CONF_DEVICE_TYPE not in config or CONF_SUBTYPE not in config:
        raise cv.Invalid(
            f"io_model: {model} needs io_device_type and io_subtype: a model is a product below a "
            "device type and subtype",
            path=[CONF_MODEL],
        )
    device_type = config[CONF_DEVICE_TYPE]
    subtype = config[CONF_SUBTYPE]
    offered = device_models(device_type, subtype)
    if model not in offered:
        choices = f"it offers: {', '.join(offered)}" if offered else "it has no models"
        raise cv.Invalid(
            f"io_model: {model} is not a model of {_kind_label(device_type, subtype)}; {choices}",
            path=[CONF_MODEL],
        )
    return config


def warn_if_experimental_kind(config):
    """Warn, without rejecting, that the declared kind is not yet confirmed on a real device.

    A post-validator for every device-bound platform. The config is legal and still transmits, so
    this logs one warning (the pattern of the other warn-only validators) that names the kind and
    points at the page for reporting a result. A config that declares no kind passes silently.
    """
    if CONF_DEVICE_TYPE in config:
        device_type = config[CONF_DEVICE_TYPE]
        subtype = config.get(CONF_SUBTYPE, 0)
        if kind_is_experimental(device_type, subtype):
            _LOGGER.warning(
                "home_io_control: %s is not yet confirmed on a real device, so it may not behave as "
                "described. If you try it, please report the result: %s",
                _kind_label(device_type, subtype, config.get(CONF_MODEL)),
                REPORTING_DEVICES_URL,
            )
    return config


def deprecated_key(key, replacement):
    """Build a post-validator that warns, without rejecting, about a key that is being retired.

    The first stage of retiring a key: the config stays legal and behaves as before, and one
    warning names the entity, says the key will become a config error, and gives the replacement.
    The second stage swaps this validator for one that raises cv.Invalid with the same replacement.

    @param key The YAML key being retired.
    @param replacement Called with the validated config when the key is present; returns the
        sentence(s) saying what to write instead and what changes in Home Assistant.
    """

    def validator(config):
        if key in config:
            _LOGGER.warning(
                "home_io_control: '%s' uses '%s:', which is deprecated and will become a config "
                "error. %s",
                config.get(CONF_NAME) or config[CONF_ID].id or "this entity",
                key,
                replacement(config),
            )
        return config

    return validator


# ESPHome's light_schema() defaults to a 1 s transition, which fades a brightness change client-side by
# calling write_state() repeatedly over that window. Here each write_state() is a radio exchange (about
# 300 ms), so a fade becomes a stream of superseding commands, and each device reply can restart it: on real
# hardware (the Izymo dimmer) it looped indefinitely. Every brightness light of this component extends its
# schema with this, so changes apply at once unless the YAML asks for a fade.
INSTANT_LIGHT_TRANSITION = {
    cv.Optional(CONF_DEFAULT_TRANSITION_LENGTH, default="0s"): cv.positive_time_period_milliseconds
}


def declared_features(config):
    """The capabilities (a frozenset of names) of the kind an entry declares (io_device_type,
    io_subtype, io_model).

    Empty when the entry declares no io_device_type: nothing at validation time says what the device
    is, so it has no feature a validator may rely on.
    """
    if CONF_DEVICE_TYPE not in config:
        return frozenset()
    return device_features(config[CONF_DEVICE_TYPE], config.get(CONF_SUBTYPE, 0), config_model(config))


def declared_has_feature(config, feature):
    """True when the kind an entry declares has the capability named `feature` (see declared_features())."""
    return check_feature_name(feature) in declared_features(config)


# A heater's or a water heater's state moves over minutes and its poll costs one radio exchange per register,
# so these platforms raise the status_poll_interval floor well above the cover floor
# (hub_names.MIN_STATUS_POLL_INTERVAL_MS). Without the key the hub polls them every five minutes.
MIN_REGISTER_POLL_INTERVAL_MS = 60000


def validate_register_poll_interval(config, platform):
    """Raise the status_poll_interval floor to one minute on a platform whose poll reads registers.

    A post-validator for climate and water_heater, bound to the platform name with functools.partial so the
    message says which one. The key itself is accepted by the shared platform schema.
    """
    interval = config.get(CONF_STATUS_POLL_INTERVAL)
    if interval is not None and interval.total_milliseconds < MIN_REGISTER_POLL_INTERVAL_MS:
        raise cv.Invalid(
            f"status_poll_interval must be at least {MIN_REGISTER_POLL_INTERVAL_MS}ms for the "
            f"{platform} platform: its poll is one radio exchange per register",
            path=[CONF_STATUS_POLL_INTERVAL],
        )
    return config


def require_feature(config, key, active, *, required=None, forbidden=None, reason):
    """Reject a YAML key the declared device kind cannot use.

    One gate for every feature-dependent key. It only fires when the config declares
    ``io_device_type`` (without a kind nothing says what the device can do), ``active`` is true
    (the caller decides, because keys differ: ``silent:`` is active by presence, ``dimmable:``
    only when true), and the kind lacks the ``required`` capability or has the ``forbidden`` one
    (each a capability name, or None for no condition). ``reason`` says what to do about it.
    """
    if not active or CONF_DEVICE_TYPE not in config:
        return
    features = declared_features(config)
    if (required is None or check_feature_name(required) in features) and (
        forbidden is None or check_feature_name(forbidden) not in features
    ):
        return
    label = _kind_label(config[CONF_DEVICE_TYPE], config.get(CONF_SUBTYPE, 0), config.get(CONF_MODEL))
    raise cv.Invalid(f"{key} is not available on {label}: {reason}")


async def wire_device_binding(var, parent, config):
    """Emit the shared to_code() wiring that binds an entity to its hub device.

    Covers set_parent / set_device_id, the optional device type / subtype / model / status
    poll interval / low-power class, and the linked-remotes registration loop —
    identical across every device-bound platform.
    """
    cg.add(var.set_parent(parent))
    cg.add(var.set_device_id(config[CONF_IO_DEVICE_ID]))

    if CONF_DEVICE_TYPE in config:
        cg.add(var.set_device_type(device_type_expression(config[CONF_DEVICE_TYPE])))
    if CONF_SUBTYPE in config:
        cg.add(var.set_subtype(config[CONF_SUBTYPE]))
    if CONF_MODEL in config:
        cg.add(var.set_device_model(device_model_expression(config_model(config))))
    if CONF_STATUS_POLL_INTERVAL in config:
        cg.add(
            var.set_status_poll_interval(
                config[CONF_STATUS_POLL_INTERVAL].total_milliseconds
            )
        )
    if CONF_LOW_POWER in config:
        cg.add(var.set_low_power(config[CONF_LOW_POWER]))
    if CONF_SYSTEM_KEY in config:
        cg.add(var.set_system_key(config[CONF_SYSTEM_KEY]))

    if CONF_LINKED_REMOTES in config:
        for remote_id in config[CONF_LINKED_REMOTES]:
            if remote_id.startswith("class:"):
                # validate_linked_remote_entry() already normalized this to 'class:0x<HH>'.
                type_value = int(remote_id.split(":", 1)[1], 16)
                cg.add(
                    parent.add_linked_remote_class(
                        device_type_expression(type_value),
                        config[CONF_IO_DEVICE_ID],
                    )
                )
            else:
                cg.add(parent.add_linked_remote(remote_id, config[CONF_IO_DEVICE_ID]))


async def _bind_companion(config, parent, new_entity, entity_config, **new_kwargs):
    """Create, register and bind one companion from its validated config; return the entity."""
    inherit_esphome_device(entity_config, config)
    entity = await new_entity(entity_config, **new_kwargs)
    await cg.register_component(entity, entity_config)
    cg.add(entity.set_parent(parent))
    cg.add(entity.set_device_id(config[CONF_IO_DEVICE_ID]))
    return entity


async def create_companion(
    config, parent, *, schema, new_entity, id_key, name, overrides=None, **new_kwargs
):
    """Create one device-bound companion entity and return it, so a caller can add its own setters.

    The entity is built from the platform's schema for its kind (``schema``, e.g.
    ``sensor.sensor_schema(IOHomeRssiSensor, ...)``, extended here with ``cv.COMPONENT_SCHEMA``) over
    ``{id, name, **overrides}``. Its ID is the one inject_companion_ids() declared under ``id_key`` in
    ``config``. The entity joins the parent's ESPHome sub-device, is registered as a component and
    bound to the hub (``set_parent``, ``set_device_id``). ``new_entity`` is the platform's
    ``new_<kind>()`` coroutine and receives ``new_kwargs`` (the select's options).
    """
    entity_config = schema.extend(cv.COMPONENT_SCHEMA)(
        {CONF_ID: config[id_key], CONF_NAME: name, **(overrides or {})}
    )
    return await _bind_companion(config, parent, new_entity, entity_config, **new_kwargs)


async def create_user_companion(config, parent, *, new_entity, entity_config):
    """Create one device-bound companion whose config the user's YAML already supplied.

    For opt-in companions declared as nested schemas of the platform (the water heater's energy
    sensor, ...): they carry their own ID, name and options, so only the shared binding of
    create_companion() applies. A sub-device the companion sets itself is kept.
    """
    return await _bind_companion(config, parent, new_entity, entity_config)


# === Device readings ===
#
# The mirror DEVICE_READINGS says, for every reading, what the codegen builds and how it is declared. A reading with a
# YAML key is an optional entity of a main entity: the climate entity takes the heater's, the `sensor:` platform the
# data objects'. Each key is an entity of its own that shows the reading; it costs the hub a read of the reading's
# registers at every poll (or a scheduled transfer read of its data object, or nothing for a status reading) and only
# some kinds have them, so it needs the capability of its row. A reading with a companion is an entity every device
# with the capability gets, named after the main entity. The table drives the schema, the capability check, the IDs
# and the code, so a new reading is a row there and in C++, never new codegen.

# The mirror's source kinds each platform offers its readings for.
HEATER_READING_SOURCES = ("register", "derived", "status")
DATA_OBJECT_READING_SOURCES = ("data_object",)

# The class of the entity that shows a reading, by the mirror's entity kind.
_READING_ENTITY_CLASSES = {
    "sensor": IOHomeDeviceReadingSensor,
    "binary_sensor": IOHomeDeviceReadingBinarySensor,
    "switch": IOHomeHeaterSettingSwitch,
    "text_sensor": IOHomeDeviceReadingTextSensor,
}
_READING_ENTITY_CREATORS = {
    "sensor": sensor.new_sensor,
    "binary_sensor": binary_sensor.new_binary_sensor,
    "switch": switch.new_switch,
    "text_sensor": text_sensor.new_text_sensor,
}


def _reading_keys(sources=None):
    """[(reading name, mirror entry)] of every reading a main entity declares by a YAML key, in table order; only
    those whose source is in `sources` when it is given."""
    return [
        (name, entry)
        for name, entry in DEVICE_READINGS.items()
        if entry["key"] is not None and (sources is None or entry["source"] in sources)
    ]


def _reading_companions(sources):
    """[(reading name, mirror entry)] of every reading an entity is made for without a key, in table order; only
    those whose source is in `sources`."""
    return [
        (name, entry)
        for name, entry in DEVICE_READINGS.items()
        if "companion" in entry and entry["source"] in sources
    ]


def _reading_entity_schema(entry):
    """The schema of the entity that shows a reading: its kind and Home Assistant metadata come from the mirror."""
    display = {
        argument: entry[field]
        for field, argument in (
            ("unit", "unit_of_measurement"),
            ("accuracy", "accuracy_decimals"),
            ("device_class", "device_class"),
            ("state_class", "state_class"),
        )
        if field in entry
    }
    kind = entry["entity"]
    entity_class = _READING_ENTITY_CLASSES.get(kind)
    if entity_class is None:
        raise ValueError(f"a reading with an entity needs an entity kind, not {kind!r}")
    if kind == "sensor":
        schema = sensor.sensor_schema(entity_class, **display)
    elif kind == "binary_sensor":
        schema = binary_sensor.binary_sensor_schema(entity_class, **display)
    elif kind == "text_sensor":
        schema = text_sensor.text_sensor_schema(entity_class, **display)
    else:
        # Restore mode DISABLED: the switch shows what the device reports, never a restored guess.
        schema = switch.switch_schema(
            entity_class,
            default_restore_mode="DISABLED",
            entity_category=ENTITY_CATEGORY_CONFIG,
        )
    return schema.extend(cv.COMPONENT_SCHEMA)


def reading_keys_schema(sources):
    """The optional YAML keys of the readings a platform offers, `{cv.Optional(key): entity schema}`, to splice into
    a main entity's schema. `sources` are the mirror source kinds the platform shows (HEATER_READING_SOURCES,
    DATA_OBJECT_READING_SOURCES). The entity IDs are declared by the schemas' own GenerateID."""
    return {cv.Optional(entry["key"]): _reading_entity_schema(entry) for _, entry in _reading_keys(sources)}


def validate_reading_keys(config):
    """Accept each reading key only on a kind with the capability of its reading.

    A key is rejected when no io_device_type is declared: the registers would otherwise be read from a device
    nothing says has them. The message names the kind to declare (READING_FEATURE_HINTS), or, where the declared type
    has the capability on other kinds and READING_REMOVE_HINTS says so, tells the user to remove the key.
    """
    for _, entry in _reading_keys():
        key = entry["key"]
        feature = entry["feature"]
        if key not in config or declared_has_feature(config, feature):
            continue
        remove_hint = READING_REMOVE_HINTS.get(feature)
        if remove_hint is not None and type_offers_feature(config.get(CONF_DEVICE_TYPE), feature):
            label = _kind_label(config[CONF_DEVICE_TYPE], config.get(CONF_SUBTYPE, 0), config.get(CONF_MODEL))
            raise cv.Invalid(f"{key} is not available on {label}: {remove_hint.format(key=key)}", path=[key])
        raise cv.Invalid(f"{key} needs {READING_FEATURE_HINTS[feature]}", path=[key])
    return config


def inject_reading_companion_ids(config, parent_id_key, sources=HEATER_READING_SOURCES):
    """Declare the IDs of the companion entities of the readings a device has, during schema validation.

    One entity per reading whose mirror entry has a companion and whose capability the declared kind has
    (has_feature()): a kind with no declared type gets none. The ID is ``<base>_<companion_id>``, under the config key
    ``_<companion_id>_id``, and its type is the class of the entity kind. See companion_id_base() for why this is
    done at validation time and inject_companion_ids() for the declaration.
    """
    specs = tuple(
        (
            _reading_companion_id_key(entry),
            entry["companion_id"],
            _READING_ENTITY_CLASSES[entry["entity"]],
            has_feature(entry["feature"]),
        )
        for _, entry in _reading_companions(sources)
    )
    return inject_companion_ids(config, specs, parent_id_key)


def _reading_companion_id_key(entry):
    """The config key the declared ID of a reading's companion is kept under."""
    return f"_{entry['companion_id']}_id"


async def reading_companions_to_code(config, parent, sources=HEATER_READING_SOURCES):
    """Create the companion entities declared by inject_reading_companion_ids(), named after the main entity and
    bound to the reading they show."""
    for name, entry in _reading_companions(sources):
        id_key = _reading_companion_id_key(entry)
        if id_key not in config:
            continue
        overrides = {CONF_DISABLED_BY_DEFAULT: True} if entry.get("disabled_by_default") else None
        companion = await create_companion(
            config,
            parent,
            schema=_reading_entity_schema(entry),
            new_entity=_READING_ENTITY_CREATORS[entry["entity"]],
            id_key=id_key,
            name=companion_entity_name(config, entry["companion"]),
            overrides=overrides,
        )
        cg.add(companion.set_reading(getattr(DeviceReading, name)))


async def reading_keys_to_code(var, parent, config):
    """Emit the code for the reading keys a main entity declares.

    For each key present the main entity asks the hub to poll the reading's registers (`add_device_reading()`), or to
    read its data object on a schedule (`add_data_object_reading()`, with the reading's default interval, which the
    entry's `update_interval:` replaces on the entity), and the entity that shows the reading is created, named after
    it. A status reading (the heating level) is read by the status read, so the hub is asked nothing.
    """
    for name, entry in _reading_keys():
        key = entry["key"]
        if key not in config:
            continue
        reading = getattr(DeviceReading, name)
        if entry["source"] in DATA_OBJECT_READING_SOURCES:
            cg.add(var.add_data_object_reading(reading, entry["update_interval_ms"]))
        elif entry["source"] != "status":
            cg.add(var.add_device_reading(reading))
        entity = await create_user_companion(
            config,
            parent,
            new_entity=_READING_ENTITY_CREATORS[entry["entity"]],
            entity_config=config[key],
        )
        cg.add(entity.set_reading(reading))


async def _create_diagnostic_text_sensor(config, parent, id_key, suffix, disabled_by_default):
    """Create one companion diagnostic `text_sensor:`."""
    await create_companion(
        config,
        parent,
        schema=text_sensor.text_sensor_schema(
            config[id_key].type, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        new_entity=text_sensor.new_text_sensor,
        id_key=id_key,
        name=companion_entity_name(config, suffix),
        overrides={CONF_DISABLED_BY_DEFAULT: disabled_by_default},
    )


async def _create_link_health_sensor(config, parent, id_key, suffix, **sensor_kwargs):
    """Create one link-health `sensor:` companion.

    All of them (RSSI, Last Contact, Exchange Failures, Unconfirmed Exchanges) are numeric,
    diagnostic, and disabled by default (noise control); only the name and sensor-specific schema keys
    (unit/device_class/state_class/accuracy_decimals) differ between them, so those are the
    only things each call in create_companion_sensors() supplies.
    """
    await create_companion(
        config,
        parent,
        schema=sensor.sensor_schema(
            config[id_key].type, entity_category=ENTITY_CATEGORY_DIAGNOSTIC, **sensor_kwargs
        ),
        new_entity=sensor.new_sensor,
        id_key=id_key,
        name=companion_entity_name(config, suffix),
        overrides={CONF_DISABLED_BY_DEFAULT: True},
    )


async def create_companion_sensors(config, parent):
    """Create and register every auto-generated companion diagnostic sensor.

    Single to_code() entry point for the device-bound platforms (the counterpart of
    inject_companion_sensor_ids()), so adding a companion touches this module only:

    - Device Name: disabled by default (clutter control).
    - Active Issue: the one enabled-by-default companion — it is the headline diagnostic
      value that turns a silently-ignored command into a self-explained one (e.g. a
      wind/rain lockout), so users should see it without an opt-in step. Empty except while
      an issue is outstanding (a refusal, a derived rain limitation, or a radio that stopped
      responding); see IOHomeActiveIssueTextSensor.
    - RSSI / Last Contact / Exchange Failures: numeric link-health diagnostics, disabled by
      default (noise control). Last Contact publishes seconds since the last frame from the
      device (an age, not a Home Assistant timestamp) and keeps counting up between frames via
      its own heartbeat; see IOHomeLastContactSensor.
    - Last Commanded By / Last Command Source: who/what last commanded the device, disabled by
      default (noise control). Free — decoded from bytes already present in every status reply,
      no extra radio traffic; see IOHomeLastCommandedByTextSensor.
    """
    await _create_diagnostic_text_sensor(
        config, parent, CONF_DEVICE_NAME_SENSOR_ID, "Device Name", disabled_by_default=True
    )
    await _create_diagnostic_text_sensor(
        config, parent, CONF_ACTIVE_ISSUE_SENSOR_ID, "Active Issue", disabled_by_default=False
    )
    await _create_link_health_sensor(
        config,
        parent,
        CONF_RSSI_SENSOR_ID,
        "RSSI",
        unit_of_measurement="dBm",
        device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=0,
    )
    await _create_link_health_sensor(
        config,
        parent,
        CONF_LAST_CONTACT_SENSOR_ID,
        "Last Contact",
        unit_of_measurement="s",
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=0,
    )
    await _create_link_health_sensor(
        config,
        parent,
        CONF_EXCHANGE_FAILURES_SENSOR_ID,
        "Exchange Failures",
        state_class=STATE_CLASS_TOTAL_INCREASING,
        accuracy_decimals=0,
    )
    await _create_link_health_sensor(
        config,
        parent,
        CONF_UNCONFIRMED_EXCHANGES_SENSOR_ID,
        "Unconfirmed Exchanges",
        state_class=STATE_CLASS_TOTAL_INCREASING,
        accuracy_decimals=0,
    )
    await _create_diagnostic_text_sensor(
        config, parent, CONF_LAST_COMMANDED_BY_SENSOR_ID, "Last Commanded By", disabled_by_default=True
    )
    await _create_diagnostic_text_sensor(
        config, parent, CONF_LAST_COMMAND_SOURCE_SENSOR_ID, "Last Command Source", disabled_by_default=True
    )
