## @file
## @brief Schema, validation and code generation for ``home_io_control: oneway_controllers:``.
## @ingroup hioc_codegen
##
## A 1W controller identity is class-addressed (ADR 0027): its own address, key and target
## class, never an ``io_device_id``. This module derives node IDs, rejects address
## collisions (including against device-bound platforms, at final-validate time) and
## generates each identity's command buttons, enrollment button and last-command sensor.

import hashlib
import logging

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
# Aliased: this package has its own switch.py/button.py platform submodules, so the real ESPHome
# components are always imported under names that cannot be mistaken for them. In the package's
# __init__.py an unaliased `switch`/`button` would even be overwritten: __init__.py's namespace IS
# the package object, the slot ESPHome's loader binds `esphome.components.home_io_control.switch`
# into when it imports our platform file, so whichever import ran last would silently win.
from esphome.components import button as button_component
from esphome.components import text_sensor as text_sensor_component
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    CONF_PLATFORM,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.core import ID

from .hub_names import (
    CONF_BUTTON_IDS,
    CONF_COMMANDS,
    CONF_ENROLLMENT,
    CONF_ENROLLMENT_CLASSES,
    CONF_ENROLLMENT_WITH_MAC,
    CONF_ENROLL_BUTTON_ID,
    CONF_EXECUTE_ACEI,
    CONF_EXECUTE_BROADCAST,
    CONF_INITIAL_SEQUENCE,
    CONF_IO_DEVICE_TYPE,
    CONF_LAST_COMMAND_SENSOR_ID,
    CONF_LOW_POWER,
    CONF_MANUFACTURER,
    CONF_NODE_ID,
    CONF_NODE_ID_DERIVED,
    CONF_ONEWAY_CONTROLLERS,
    CONF_RADIO_TYPE,
    CONF_SYSTEM_KEY,
    IOHomeOneWayCommandButton,
    IOHomeOneWayEnrollButton,
    IOHomeOneWayLastCommandTextSensor,
    OneWayButtonAction,
)
from .hub_validators import (
    DEVICE_TYPE_OPTIONS,
    MANUFACTURER_OPTIONS,
    _ONEWAY_WIRE_PROFILE_MANUFACTURERS,
    validate_device_type,
    validate_manufacturer,
    validate_node_id,
    validate_system_key,
)

_LOGGER = logging.getLogger(__name__)


# Command names a user may list, mapped to the C++ enum. OPEN and CLOSE are positions on the
# wire, not distinct opcodes -- encode_oneway_action() (oneway_controller.h) is where that
# resolves, so this table stays a plain name->enum mapping.
ONEWAY_COMMANDS = {
    "open": OneWayButtonAction.OPEN,
    "close": OneWayButtonAction.CLOSE,
    "stop": OneWayButtonAction.STOP,
    "vent": OneWayButtonAction.VENT,
    "favorite": OneWayButtonAction.FAVORITE,
}


# A 1W controller identity. 1W is class-addressed — a command goes to a device *class*, never to
# a node — so there is no `io_device_id` here and none on the entities that will reference this
# handle. What distinguishes one 1W control surface from another is the controller doing the
# transmitting: its address, its key, and the class it speaks to. See ADR 0027 and
# oneway_controller.h.
#
# A newly-required key here also needs a matching field in the `oneway_controllers:` block
# build_oneway_adoption_report() (oneway_key_adoption.cpp) hand-emits for paste-and-reflash (ADR 0018).
# `make yaml-emitter-sync` (scripts/check-yaml-emitters.py) catches drift between the two
# statically.
def _no_duplicate_enrollment_classes(value):
    """Reject a repeated class in `enrollment_classes:` -- each just retransmits the same 0x30."""
    seen = set()
    for entry in value:
        if entry in seen:
            name = next(
                (n for n, v in DEVICE_TYPE_OPTIONS.items() if v == entry), hex(entry)
            )
            raise cv.Invalid(
                f"enrollment_classes has '{name}' more than once; each entry adds a burst to the "
                "enrollment gesture, so a repeat only wastes ~1 second of it"
            )
        seen.add(entry)
    return value


ONEWAY_CONTROLLER_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.string_strict,
        # Optional and derived when omitted — see derive_oneway_node_id() for why asking the user
        # for one is an unanswerable question.
        cv.Optional(CONF_NODE_ID): validate_node_id,
        # Optional and inherits the hub's key when omitted. cv.sensitive matches the hub's own
        # system_key handling (see the main schema below) so the value is redacted from ESPHome's
        # config dump; without it a per-identity key would leak into logs verbatim.
        cv.Optional(CONF_SYSTEM_KEY): cv.sensitive(validate_system_key),
        # No default here -- see _validate_oneway_controllers() for why: it must become required,
        # not silently 0, whenever enrollment: true actually puts this byte on air. Named or raw
        # hex, same "known name, else escape hatch" shape as io_device_type below.
        cv.Optional(CONF_MANUFACTURER): validate_manufacturer,
        cv.Required(CONF_IO_DEVICE_TYPE): validate_device_type,
        # Seeds this identity's rolling counter on first use. This is the escape hatch for a
        # device that has stopped accepting commands because its stored counter ran ahead of
        # ours: bump this and reflash. Devices accept a forward jump only within a window (~1000
        # in the one documented receiver implementation), so a value that is too far ahead fails
        # exactly like one that is too far behind, and just as silently — move it in small steps.
        cv.Optional(CONF_INITIAL_SEQUENCE, default=0): cv.int_range(min=0, max=0xFFFF),
        # Which command buttons to generate. Validated against the known set rather than taken
        # as free text: a typo would otherwise produce a silently missing button, and 1W gives no
        # runtime signal that would ever reveal one.
        cv.Optional(CONF_COMMANDS, default=[]): cv.ensure_list(
            cv.one_of(*ONEWAY_COMMANDS, lower=True)
        ),
        # The build flag for this identity's "Enroll 1W Controller" button. See CONF_ENROLLMENT's
        # own comment for the lifecycle this presence/absence gates.
        cv.Optional(CONF_ENROLLMENT, default=False): cv.boolean,
        # Whether the enroll button's 0x30 carries a 6-byte MAC trailer. Real hardware disagrees:
        # most captures this project holds carry no MAC at all (default here, matching real Somfy
        # traffic), but a real Izymo has separately been shown to accept the MAC-bearing form too
        # (the published documentation vector's own shape) -- see create_1w_add_controller()'s
        # @warning (proto_commands.h). Untested manufacturers (e.g. Velux) may require one shape
        # or the other; this exists so trying the other one needs a YAML edit, not a code change.
        # No VELUX capture this project holds carries the MAC trailer, and on sx1276 a MAC-bearing
        # 0x30 has been measured to take about twice as long per burst -- see
        # _validate_oneway_controllers()'s warning below.
        cv.Optional(CONF_ENROLLMENT_WITH_MAC, default=False): cv.boolean,
        # The device classes a VELUX enrollment 0x30 sweep targets. Unset -> the manufacturer
        # profile default ({roller_shutter, awning, dual_shutter} for velux). Set it to narrow the
        # sweep, e.g. [awning], once you know which class your actuator listens on. Ignored by the
        # somfy gesture (which always uses io_device_type). Max 3 -- the C++ side is a fixed array.
        # `unknown` (0x00) is rejected: it is the C++ "not overridden" sentinel
        # (effective_enrollment_classes()), so [unknown] would silently expand back to the full
        # three-class sweep -- the opposite of narrowing it. Duplicates are rejected too: each just
        # retransmits the same frame and costs ~1s of the blocking gesture for nothing.
        cv.Optional(CONF_ENROLLMENT_CLASSES): cv.All(
            cv.ensure_list(cv.All(validate_device_type, cv.int_range(min=1, max=0xFF))),
            cv.Length(min=1, max=3),
            _no_duplicate_enrollment_classes,
        ),
        # Override the manufacturer-derived ACEI byte for this identity's 1W CMD_EXECUTE frames.
        # min=1 on purpose: 0 is the C++ "not overridden" sentinel (OneWayControllerIdentity::
        # execute_acei), so accepting execute_acei: 0 would be a silent no-op instead of an error.
        # cv.hex_int allows 0x61-style spelling; the range check runs after.
        cv.Optional(CONF_EXECUTE_ACEI): cv.All(cv.hex_int, cv.int_range(min=1, max=0xFF)),
        # "all" -> address the all-devices broadcast 00 00 3F for CMD_EXECUTE (what a handheld
        # cover remote of either vendor does); "typed" (default) -> the per-class address from
        # io_device_type (current behaviour). Not a vendor axis -- see ADR 0031.
        cv.Optional(CONF_EXECUTE_BROADCAST, default="typed"): cv.one_of(
            "typed", "all", lower=True
        ),
        # Tri-state, unlike the per-device 2W low_power (ADR 0029): no default here, because unset
        # and false mean different things for a 1W identity. Unset = every burst keeps the
        # legacy shape (LONG_PREAMBLE on every copy, CTRL1 0x00) -- byte- and
        # timing-identical to the hardware-validated Somfy path. false = every copy uses the live
        # `normal_start_preamble` tuning value instead, CTRL1 0x00 -- the ADR 0029 shape, for a
        # mains-powered receiver. true = copy 1 gets LONG_PREAMBLE + CTRL1_LOW_POWER, copies 2-4 the
        # normal preamble -- a wake-up copy for a solar/battery receiver. Applies to every 1W TX of
        # the identity: commands, positions, both enrollment gestures, un-enrollment. See ADR 0038.
        cv.Optional(CONF_LOW_POWER): cv.boolean,
    }
)


def derive_oneway_node_id(hub_node_id, identity_id):
    """Derive a stable 3-byte 1W source address from the hub's node_id and an identity handle.

    `node_id` is optional on a `oneway_controllers:` entry because asking a user to invent a
    3-byte radio address is an unanswerable question — nothing tells them which addresses are
    safe, and colliding with a real remote in range silently desyncs both transmitters' rolling
    sequence counters. Deriving one removes the decision while leaving an explicit value
    available to anyone who needs it.

    The derivation is done here, at schema time, rather than at runtime on the device, so that a
    derived address participates in the same collision checks as a configured one and a clash
    fails the build instead of surfacing as a device that silently ignores commands. It is a
    pure function of (hub node_id, identity id), so it is stable across builds and reproducible
    from the YAML alone.

    Uses BLAKE2b rather than Python's hash(), which is salted per-process and would produce a
    different address on every compile.
    """
    digest = hashlib.blake2b(
        f"{hub_node_id}:{identity_id}".encode(), digest_size=3
    ).digest()
    return f"{digest[0]:02X}{digest[1]:02X}{digest[2]:02X}"


def _hex_byte_array(hex_string):
    """Render a hex string as a C++ brace-initialiser list of bytes."""
    values = ", ".join(
        f"0x{hex_string[i : i + 2]}" for i in range(0, len(hex_string), 2)
    )
    return f"{{{values}}}"


def _enrollment_classes_initialiser(identity):
    """Render `enrollment_classes:` as a 3-element std::array<DeviceType,3> brace-initialiser.

    Unset, or fewer than 3, is padded with UNKNOWN (0x00) -- the sentinel effective_enrollment_classes()
    (oneway_controller.h) reads as "not overridden here" / "skip this slot".
    """
    padded = (list(identity.get(CONF_ENROLLMENT_CLASSES, [])) + [0, 0, 0])[:3]
    entries = ", ".join(
        f"static_cast<esphome::home_io_control::DeviceType>(0x{value:02X})"
        for value in padded
    )
    return f"{{{entries}}}"


def _power_class_override_expression(identity):
    """Map `low_power:` to the C++ `std::optional<OneWayPowerClass>` the identity stores.

    Tri-state, and the three states are emitted as three distinct values rather than collapsed
    here: unset -> `{}` (empty optional), `false` -> ALWAYS_ALIVE, `true` -> LOW_POWER.

    An unset key deliberately does **not** pick a shape at codegen time. Resolving it needs the
    manufacturer profile, which lives in C++ (`resolve_oneway_wire_profile()`), and splitting that
    lookup across two languages is how the ACEI and enrollment-class defaults would drift from this
    one. `effective_power_class()` is the single resolver. See ADR 0041, and ADR 0038 for the
    shapes themselves.
    """
    if CONF_LOW_POWER not in identity:
        return "{}"
    name = "LOW_POWER" if identity[CONF_LOW_POWER] else "ALWAYS_ALIVE"
    return f"esphome::home_io_control::OneWayPowerClass::{name}"


def oneway_controller_expression(identity, hub_node_id):
    """Generate the C++ OneWayControllerIdentity initialiser for one configured identity.

    Emitted as a designated initialiser so the generated code reads like the YAML that produced
    it, and so adding a field to the struct cannot silently shift an existing value.
    """
    derived = identity.get(CONF_NODE_ID_DERIVED, False)
    fields = ", ".join(
        [
            f'.id = "{identity[CONF_ID]}"',
            f".node_id = {_hex_byte_array(identity[CONF_NODE_ID])}",
            f".system_key = {_hex_byte_array(identity[CONF_SYSTEM_KEY])}",
            f".manufacturer = 0x{identity[CONF_MANUFACTURER]:02X}",
            f".io_device_type = static_cast<esphome::home_io_control::DeviceType>"
            f"(0x{identity[CONF_IO_DEVICE_TYPE]:02X})",
            f".initial_sequence = 0x{identity[CONF_INITIAL_SEQUENCE]:04X}",
            f".node_id_derived = {'true' if derived else 'false'}",
            f".enrollment_with_mac = {'true' if identity[CONF_ENROLLMENT_WITH_MAC] else 'false'}",
            f".execute_acei = 0x{identity.get(CONF_EXECUTE_ACEI, 0):02X}",
            f".execute_broadcast_all = "
            f"{'true' if identity[CONF_EXECUTE_BROADCAST] == 'all' else 'false'}",
            f".enrollment_classes = {_enrollment_classes_initialiser(identity)}",
            f".power_class_override = {_power_class_override_expression(identity)}",
        ]
    )
    if derived:
        _LOGGER.info(
            "home_io_control: oneway_controllers '%s' node_id derived from hub %s -> %s",
            identity[CONF_ID],
            hub_node_id,
            identity[CONF_NODE_ID],
        )
    return cg.RawExpression(
        f"esphome::home_io_control::OneWayControllerIdentity{{{fields}}}"
    )


def _reject_node_id_collision(identity_id, node_id, seen_node_ids, derived):
    """Raise if `node_id` is already claimed in `seen_node_ids`; no-op otherwise.

    Shared between _validate_oneway_controllers() (collisions against the hub's own node_id and
    other oneway_controllers entries) and _final_validate_oneway_controller_addresses()
    (collisions against `linked_remotes:`/`io_device_id:` declared elsewhere in the same YAML),
    so both raise identically-worded errors regardless of which side of the config the other
    claimant lives on.
    """
    if node_id not in seen_node_ids:
        return
    owner = seen_node_ids[node_id]
    hint = (
        " (this address was derived; set node_id: explicitly to resolve the clash)"
        if derived
        else ""
    )
    raise cv.Invalid(
        f"oneway_controllers id '{identity_id}' uses node_id {node_id}, which collides with "
        f"{owner}{hint}. Two transmitters sharing an address share a rolling sequence "
        f"counter, which silently desyncs both."
    )


def _validate_oneway_controllers(config):
    """Resolve per-identity defaults and reject address/handle collisions at compile time.

    Runs as a post-validator on the whole hub config because every rule here needs the hub's own
    `node_id`/`system_key`, which a per-entry validator cannot see.

    Only checks addresses visible within `home_io_control:` itself (its own `node_id` and every
    configured `oneway_controllers` entry) — see _final_validate_oneway_controller_addresses()
    below for the matching check against `linked_remotes:`/`io_device_id:` declared elsewhere in
    the same YAML, which needs the full cross-component config and so cannot run here. Even
    together the two cannot see a real remote the user has never mentioned to this config at all;
    see derive_oneway_node_id()'s own docstring for why that residual risk cannot be validated
    away.
    """
    identities = config.get(CONF_ONEWAY_CONTROLLERS, [])
    if not identities:
        return config

    hub_node_id = config[CONF_NODE_ID]
    seen_ids = set()
    # Maps resolved address -> the human-readable owner, so a collision message can name both
    # sides rather than just reporting that "an" address is taken.
    seen_node_ids = {hub_node_id: "the hub's own node_id"}

    for identity in identities:
        identity_id = identity[CONF_ID]
        if identity_id in seen_ids:
            raise cv.Invalid(
                f"Duplicate oneway_controllers id '{identity_id}' — each identity needs its own handle"
            )
        seen_ids.add(identity_id)

        if CONF_NODE_ID not in identity:
            identity[CONF_NODE_ID] = derive_oneway_node_id(hub_node_id, identity_id)
            identity[CONF_NODE_ID_DERIVED] = True

        node_id = identity[CONF_NODE_ID]
        _reject_node_id_collision(identity_id, node_id, seen_node_ids, identity.get(CONF_NODE_ID_DERIVED))
        seen_node_ids[node_id] = f"oneway_controllers id '{identity_id}'"

        # A per-identity key is optional so that identities on the hub's own network need not
        # repeat it; an adopted foreign network's key is what makes the override necessary.
        if CONF_SYSTEM_KEY not in identity:
            identity[CONF_SYSTEM_KEY] = config[CONF_SYSTEM_KEY]

        # manufacturer becomes required, not silently 0, whenever enrollment: true actually puts
        # this byte on the air in a CMD_ONEWAY_ADD_CONTROLLER frame -- 1W gives no error a wrong
        # value would ever surface as, so the mistake has to be caught here instead. Everywhere
        # else it is genuinely unused today, so defaulting to 0 there is harmless.
        if identity[CONF_ENROLLMENT] and CONF_MANUFACTURER not in identity:
            raise cv.Invalid(
                f"oneway_controllers id '{identity_id}' has enrollment: true but no manufacturer: "
                "set. Find the value from a key-adoption report for this network (the 'Recover "
                "1W Controller Key' switch prints it), or from the device's own documentation."
            )
        # Record whether the user set manufacturer: explicitly, before it is defaulted to 0. The
        # 1W wire-profile warning below only fires for an *explicit* unprofiled vendor -- an
        # omitted manufacturer is the common back-compat case and maps silently to Somfy.
        manufacturer_explicit = CONF_MANUFACTURER in identity
        if CONF_MANUFACTURER not in identity:
            identity[CONF_MANUFACTURER] = 0

        # manufacturer: now also drives the 1W CMD_EXECUTE ACEI byte (ADR 0031). We only have a
        # verified wire profile for Somfy and Velux; any other explicitly-named vendor falls back
        # to the Somfy-shaped default, which is a guess. Warn -- but not with cv.Invalid, since an
        # unprofiled vendor is a legal config that still transmits -- and only for an identity that
        # can actually emit an EXECUTE (has commands: or enrollment:); an inert identity's wire
        # shape does not matter yet.
        identity_can_transmit = bool(identity[CONF_COMMANDS]) or identity[CONF_ENROLLMENT]
        if (
            manufacturer_explicit
            and identity[CONF_MANUFACTURER] not in _ONEWAY_WIRE_PROFILE_MANUFACTURERS
            and identity_can_transmit
        ):
            _LOGGER.warning(
                "home_io_control: oneway_controllers '%s' has manufacturer 0x%02X, which has no "
                "1W wire profile -- using the Somfy-shaped defaults (ACEI 0x43). Set execute_acei: "
                "explicitly if that is wrong for your device.",
                identity_id,
                identity[CONF_MANUFACTURER],
            )

        # enrollment_with_mac: true on sx1276 has been measured to roughly double the 0x30 burst's
        # airtime, and no VELUX capture this project holds carries the MAC at all -- warn, don't
        # reject: it is still a legal config, and other radios/vendors are unaffected.
        if (
            identity[CONF_ENROLLMENT]
            and identity[CONF_ENROLLMENT_WITH_MAC]
            and config[CONF_RADIO_TYPE] == "sx1276"
        ):
            _LOGGER.warning(
                "home_io_control: oneway_controllers '%s' has enrollment_with_mac: true on "
                "radio_type: sx1276, where the MAC-bearing 0x30 has been measured to take about "
                "twice as long per burst. No VELUX capture carries this MAC either. Consider "
                "leaving it false unless your hardware specifically needs the MAC-bearing form.",
                identity_id,
            )

        # The velux profile's default 0x30 sweep is the exterior-shading set a KLI 310/313 uses
        # ({roller_shutter, awning, dual_shutter}). An interior-blind remote (KLI 312) sweeps
        # blind/venetian_blind instead, so a screen/blind identity that enrolls with the default
        # sweep almost certainly misses the actuator. Enrollment ignores io_device_type,
        # so point the user at enrollment_classes: and at the KLI's own 0x2E lines that name the classes.
        if (
            identity[CONF_MANUFACTURER] == MANUFACTURER_OPTIONS["velux"]
            and identity[CONF_ENROLLMENT]
            and CONF_ENROLLMENT_CLASSES not in identity
            and identity[CONF_IO_DEVICE_TYPE]
            in (
                DEVICE_TYPE_OPTIONS["screen"],
                DEVICE_TYPE_OPTIONS["blind"],
                DEVICE_TYPE_OPTIONS["venetian_blind"],
            )
        ):
            _LOGGER.warning(
                "home_io_control: oneway_controllers '%s' is a VELUX %s with enrollment: true, but "
                "enrollment_classes: is unset, so the enrollment 0x30 sweep uses the exterior-shading "
                "default roller_shutter/awning/dual_shutter and ignores io_device_type. This device may "
                "enroll on other classes: set enrollment_classes: (a KLI 312 interior blind uses "
                "[blind, venetian_blind]). To find yours, press Gear on the existing remote and use "
                "the classes named in its 'rx 1W remote ... (0x2E)' DEBUG log lines -- see 'Finding "
                "your enrollment classes' in the 1W transmit docs.",
                identity_id,
                next(
                    name
                    for name, value in DEVICE_TYPE_OPTIONS.items()
                    if value == identity[CONF_IO_DEVICE_TYPE]
                ),
            )

        # enrollment_classes: only feeds the VELUX enrollment gesture. On a somfy/unprofiled
        # identity, or one with no enroll button at all, it is silently inert -- flag it, the same
        # way the screen/blind case above is flagged.
        if CONF_ENROLLMENT_CLASSES in identity and (
            identity[CONF_MANUFACTURER] != MANUFACTURER_OPTIONS["velux"]
            or not identity[CONF_ENROLLMENT]
        ):
            _LOGGER.warning(
                "home_io_control: oneway_controllers '%s' sets enrollment_classes: but it only "
                "affects the VELUX enrollment gesture (needs manufacturer: velux AND "
                "enrollment: true) -- it is ignored here.",
                identity_id,
            )

        # Entity IDs are declared here, at validation time, not in to_code(): an ID created late
        # is silently dropped at runtime (ADR 0009). The `<identity_id>_<command>` shape is a
        # documented contract, not an implementation detail -- users compose `time_based` covers
        # against these IDs and cannot do that against IDs they can't predict.
        identity[CONF_BUTTON_IDS] = {
            command: ID(
                f"{identity_id}_{command}",
                is_declaration=True,
                type=IOHomeOneWayCommandButton,
            )
            for command in identity[CONF_COMMANDS]
        }
        identity[CONF_LAST_COMMAND_SENSOR_ID] = ID(
            f"{identity_id}_last_1w_command",
            is_declaration=True,
            type=IOHomeOneWayLastCommandTextSensor,
        )
        if identity[CONF_ENROLLMENT]:
            identity[CONF_ENROLL_BUTTON_ID] = ID(
                f"{identity_id}_enroll",
                is_declaration=True,
                type=IOHomeOneWayEnrollButton,
            )

    return config


# cover:/light:/lock:/switch: are the device-bound platforms that can carry an io_device_id: or a
# linked_remotes: entry (platform_common.py). button:/number:/select:/sensor:/text_sensor: are
# either not device-bound or, for this component, hub-level only.
_DEVICE_BOUND_DOMAINS = ("cover", "light", "lock", "switch")


def _collect_declared_device_addresses(full_config):
    """Map every node ID declared as an `io_device_id:` or a bare `linked_remotes:` entry, across
    every `home_io_control` entity in `full_config`, to a human-readable owner string.

    Only entries with `platform: home_io_control` are considered — the domains in
    _DEVICE_BOUND_DOMAINS are shared with every other component that provides a cover/light/
    lock/switch platform, and those have nothing to do with this component's address space.
    `class:` linked_remotes entries name a device *type*, not a node, so they carry no address to
    collide with and are skipped.

    CONF_IO_DEVICE_ID/CONF_LINKED_REMOTES are imported locally from platform_common rather than at
    module level: platform_common imports from the package (`from . import ...`), whose
    __init__.py imports this module, so a module-level import here would be circular. By the time this function actually runs (final
    validation, after every used platform module has already been imported), the cycle has
    already resolved and the import is a plain cache hit.
    """
    from .platform_common import CONF_IO_DEVICE_ID, CONF_LINKED_REMOTES

    addresses = {}
    for domain in _DEVICE_BOUND_DOMAINS:
        for entry in full_config.get(domain, []):
            if not isinstance(entry, dict) or entry.get(CONF_PLATFORM) != "home_io_control":
                continue
            owner_name = entry.get(CONF_NAME) or entry.get(CONF_ID) or "<unnamed>"
            device_id = entry.get(CONF_IO_DEVICE_ID)
            if device_id:
                addresses[device_id] = f"{domain} '{owner_name}' io_device_id"
            for remote in entry.get(CONF_LINKED_REMOTES, []):
                if remote.startswith("class:"):
                    continue
                addresses[remote] = f"{domain} '{owner_name}' linked_remotes"
    return addresses


def _final_validate_oneway_controller_addresses(config):
    """Extend the oneway_controllers address-collision check to addresses declared outside
    `home_io_control:` — a `linked_remotes:` entry or an `io_device_id:` on some other entity in
    this same YAML.

    Runs as FINAL_VALIDATE_SCHEMA rather than inside _validate_oneway_controllers() because only
    final validation has access to the full cross-component config (`fv.full_config`) —
    `cover:`/`light:`/`lock:`/`switch:` entries are validated independently of
    `home_io_control:`'s own CONFIG_SCHEMA and are not visible to it. By this point
    _validate_oneway_controllers() has already run, so every identity's `node_id` (derived or
    explicit) is resolved.
    """
    identities = config.get(CONF_ONEWAY_CONTROLLERS, [])
    if not identities:
        return config

    declared = _collect_declared_device_addresses(fv.full_config.get())
    for identity in identities:
        _reject_node_id_collision(
            identity[CONF_ID], identity[CONF_NODE_ID], declared, identity.get(CONF_NODE_ID_DERIVED)
        )
    return config


async def _create_oneway_controller_entities(identity, var):
    """Create one identity's command buttons and its "Last 1W Command" diagnostic sensor.

    Same normalization as _create_hub_arming_switch() (hub_entities.py): run a bare {id, name}
    dict through the platform's own schema so it carries the entity/component defaults register_*() require.

    Entity names derive from the identity handle and the command ("awning_remote" + "open" ->
    "Awning Remote Open"), mirroring how the cover's favourite/vent companions derive theirs. The
    *IDs* follow the documented `<identity_id>_<command>` rule instead, because those are what a
    `time_based` cover composes against.
    """
    friendly_identity = identity[CONF_ID].replace("_", " ").title()

    for command, button_id in identity[CONF_BUTTON_IDS].items():
        entity_config = button_component.button_schema(
            IOHomeOneWayCommandButton,
        ).extend(cv.COMPONENT_SCHEMA)(
            {
                CONF_ID: button_id,
                CONF_NAME: f"{friendly_identity} {command.replace('_', ' ').title()}",
            }
        )
        entity = await button_component.new_button(entity_config)
        await cg.register_component(entity, entity_config)
        cg.add(entity.set_parent(var))
        cg.add(entity.set_controller_id(identity[CONF_ID]))
        cg.add(entity.set_action(ONEWAY_COMMANDS[command]))

    # Always created, even with no buttons: an identity driven only by the
    # `oneway_set_position` action still needs somewhere to show what it sent, and with no reply
    # frame this sensor is the only place that can ever appear.
    sensor_config = text_sensor_component.text_sensor_schema(
        IOHomeOneWayLastCommandTextSensor,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ).extend(cv.COMPONENT_SCHEMA)(
        {
            CONF_ID: identity[CONF_LAST_COMMAND_SENSOR_ID],
            CONF_NAME: f"{friendly_identity} Last 1W Command",
        }
    )
    sensor = await text_sensor_component.new_text_sensor(sensor_config)
    await cg.register_component(sensor, sensor_config)
    cg.add(sensor.set_parent(var))
    cg.add(sensor.set_controller_id(identity[CONF_ID]))

    if identity[CONF_ENROLLMENT]:
        # entity_category: config, not a switch behind an arming flag: the receiver's own 2s PROG
        # hold is the real interlock -- a hub cannot enroll into a device nobody has walked up to
        # (ADR 0026). ADR 0021's bootloader precedent doesn't apply here (that ADR is for an
        # irreversible write; un-enrollment exists here as a rollback path).
        #
        # No "(May Replace Existing Remotes)" caveat: enrolling a new identity onto a device is
        # additive, not destructive -- an existing remote keeps working alongside a newly enrolled
        # hub. Un-enrollment (`0x39`, the `oneway_remove_controller` action) has not been confirmed
        # to work on real hardware yet -- see that action's own doxygen (management_actions.h) --
        # so "reversible" is the design intent, not yet a demonstrated fact.
        enroll_config = button_component.button_schema(
            IOHomeOneWayEnrollButton,
            entity_category=ENTITY_CATEGORY_CONFIG,
        ).extend(cv.COMPONENT_SCHEMA)(
            {
                CONF_ID: identity[CONF_ENROLL_BUTTON_ID],
                CONF_NAME: f"{friendly_identity} Enroll 1W Controller",
            }
        )
        enroll_entity = await button_component.new_button(enroll_config)
        await cg.register_component(enroll_entity, enroll_config)
        cg.add(enroll_entity.set_parent(var))
        cg.add(enroll_entity.set_controller_id(identity[CONF_ID]))
