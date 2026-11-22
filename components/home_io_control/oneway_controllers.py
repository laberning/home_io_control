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
from dataclasses import dataclass

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
# Aliased so they cannot be mistaken for this package's own platform modules (see hub_names.py).
from esphome.components import button as button_component
from esphome.components import cover as cover_component
from esphome.components import light as light_component
from esphome.components import text_sensor as text_sensor_component
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    CONF_OUTPUT_ID,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.core import ID

from .hub_names import (
    CONF_BUTTON_IDS,
    CONF_COMMANDS,
    CONF_COVER,
    CONF_CLOSE_DURATION,
    CONF_COVER_ENTITY_ID,
    CONF_DIMMABLE,
    CONF_DRIVES,
    CONF_ENROLLMENT,
    CONF_ENROLLMENT_CLASSES,
    CONF_ENROLLMENT_WITH_MAC,
    CONF_ENROLL_BUTTON_ID,
    CONF_EXECUTE_ACEI,
    CONF_EXECUTE_BROADCAST,
    CONF_FOLLOWS,
    CONF_INITIAL_SEQUENCE,
    CONF_INVERT_POSITION,
    CONF_IO_DEVICE_ID,
    CONF_IO_DEVICE_TYPE,
    CONF_LAST_COMMAND_SENSOR_ID,
    CONF_LIGHT,
    CONF_LIGHT_ENTITY_ID,
    CONF_LIGHT_GAMMA_TABLE_ID,
    CONF_LIGHT_STATE_ID,
    CONF_LINKED_REMOTES,
    CONF_LOW_POWER,
    CONF_MANUFACTURER,
    CONF_NODE_ID,
    CONF_NODE_ID_DERIVED,
    CONF_ONEWAY_CONTROLLERS,
    CONF_OFF_DURATION,
    CONF_ON_DURATION,
    CONF_OPEN_DURATION,
    CONF_RADIO_TYPE,
    CONF_SET_POSITION,
    CONF_SYSTEM_KEY,
    IOHomeOneWayCommandButton,
    IOHomeOneWayCover,
    IOHomeOneWayEnrollButton,
    IOHomeOneWayLastCommandTextSensor,
    IOHomeOneWayLight,
    OneWayButtonAction,
    OneWaySliderWay,
)
from .hub_validators import (
    DEVICE_TYPE_MAX,
    INSTANT_LIGHT_TRANSITION,
    declared_device_entries,
    DEVICE_TYPE_OPTIONS,
    MANUFACTURER_OPTIONS,
    ONEWAY_WIRE_PROFILE_MANUFACTURERS,
    device_features,
    device_type_name,
    kind_is_cover,
    kind_is_light,
    validate_device_id,
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


def _no_duplicate_driven_devices(value):
    """Reject a repeated device in `drives:` -- the follow-up would run twice for each command."""
    seen = set()
    for device_id in value:
        if device_id in seen:
            raise cv.Invalid(
                f"drives has '{device_id}' more than once; list each device a single time"
            )
        seen.add(device_id)
    return value


def _no_duplicate_followed_remotes(value):
    """Reject a repeated sender in `follows:` -- the cover would apply each of its presses twice."""
    seen = set()
    for sender in value:
        if sender in seen:
            raise cv.Invalid(f"follows has '{sender}' more than once; list each remote a single time")
        seen.add(sender)
    return value


ONEWAY_CONTROLLER_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.string_strict,
        # Optional and derived when omitted — see derive_oneway_node_id() for why asking the user
        # for one is an unanswerable question.
        cv.Optional(CONF_NODE_ID): validate_node_id,
        # Optional and inherits the hub's key when omitted. cv.sensitive matches the hub's own
        # system_key handling (the hub schema in __init__.py) so the value is redacted from
        # ESPHome's config dump; without it a per-identity key would leak into logs verbatim.
        cv.Optional(CONF_SYSTEM_KEY): cv.sensitive(validate_system_key),
        # No default here -- see validate_oneway_controllers() for why: it must become required,
        # not silently 0, whenever enrollment: true actually puts this byte on air. Named or raw
        # hex, same "known name, else escape hatch" shape as io_device_type below.
        cv.Optional(CONF_MANUFACTURER): validate_manufacturer,
        cv.Required(CONF_IO_DEVICE_TYPE): validate_device_type,
        # Seeds this identity's rolling counter on first use. This is the escape hatch for a
        # device that has stopped accepting commands because its stored counter ran ahead of
        # ours: bump this and reflash. A Somfy Izymo obeys any higher number (measured up to +32000)
        # but never a step across the 16-bit wrap, so a value that is too low fails silently, and
        # other receivers are not measured: prefer small steps. It only ever moves the counter
        # forward.
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
        # Removed option: a clear error instead of ESPHome's generic "not a valid option" (ADR 0124).
        cv.Optional(CONF_ENROLLMENT_WITH_MAC): cv.invalid(
            "'enrollment_with_mac' was removed (ADR 0124): the enrollment frame is always sent without "
            "the MAC trailer, like a real remote. Delete this line."
        ),
        # The device classes a VELUX enrollment 0x30 sweep targets. Unset -> the manufacturer
        # profile default ({roller_shutter, vertical_exterior_awning, dual_roller_shutter} for velux).
        # Set it to narrow the sweep, e.g. [vertical_exterior_awning], once you know which class
        # your actuator listens on. Ignored by the somfy gesture (which sends to the all-devices
        # address).
        # Max 3 -- the C++ side is a fixed array.
        # `unknown` (0x00) is rejected: it is the C++ "not overridden" sentinel
        # (effective_enrollment_classes()), so [unknown] would silently expand back to the full
        # three-class sweep -- the opposite of narrowing it. Duplicates are rejected too: each just
        # retransmits the same frame and costs ~1s of the blocking gesture for nothing.
        cv.Optional(CONF_ENROLLMENT_CLASSES): cv.All(
            cv.ensure_list(cv.All(validate_device_type, cv.int_range(min=1, max=DEVICE_TYPE_MAX))),
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
        # io_device_type. Not a vendor axis -- see ADR 0031.
        cv.Optional(CONF_EXECUTE_BROADCAST, default="typed"): cv.one_of(
            "typed", "all", lower=True
        ),
        # Unset and false are the same: four short copies, every copy on the live `start_preamble_ms`
        # tuning value with CTRL1 0x00, for every manufacturer (ADR 0122). true = copy 1 gets a
        # `low_power_wake_ms` wake-up (ADR 0115) + CTRL1_LOW_POWER, copies 2-4 the normal preamble --
        # the wake-up copy a solar/battery receiver needs. Applies to every 1W TX of the identity:
        # commands, positions, both enrollment gestures, un-enrollment. See ADR 0038.
        cv.Optional(CONF_LOW_POWER): cv.boolean,
        # Tri-state: unset = the device kind decides (an inverted kind such as a
        # horizontal awning mirrors OPEN/CLOSE/position on the wire, as the 2W cover does);
        # true/false force it. Resolved in C++ by effective_inverted().
        cv.Optional(CONF_INVERT_POSITION): cv.boolean,
        # The devices (their io_device_id) this identity also drives: after each command the hub
        # transmits, they follow it like a press of a linked remote -- the commanded state at once,
        # then a status read (ADR 0116). Which ids exist and whether their kind can follow is
        # checked at final validation, which sees the device entries.
        cv.Optional(CONF_DRIVES, default=[]): cv.All(
            cv.ensure_list(validate_device_id), _no_duplicate_driven_devices
        ),
        # An assumed-state cover entity for this identity, named by the value (ADR 0117): open, close and stop
        # as the identity's own commands, and a state that follows them. Only for a cover kind
        # (_cover_problem()). The ID follows `<identity_id>_cover`.
        cv.Optional(CONF_COVER): cv.string_strict,
        # The sender addresses of the real remotes whose presses the cover or light follows, as `linked_remotes:` lists
        # them for a two-way cover. Never every remote of the class: a neighbour's would move the state. Needs
        # `cover:` or `light:`.
        cv.Optional(CONF_FOLLOWS, default=[]): cv.All(
            cv.ensure_list(validate_node_id), _no_duplicate_followed_remotes
        ),
        # How long the cover takes for a full run each way (ADR 0127). With both, the cover estimates a position from
        # the commands and presses it follows; with neither it shows open or closed only. Both or neither, and only
        # with `cover:` (_cover_problem()). Typed in: nothing tells the hub when a one-way cover reaches its end.
        cv.Optional(CONF_OPEN_DURATION): cv.positive_not_null_time_period,
        cv.Optional(CONF_CLOSE_DURATION): cv.positive_not_null_time_period,
        # An assumed-state light entity for this identity, named by the value (ADR 0128): on, off and, with
        # `dimmable:`, a brightness slider, over the identity's own commands. Only for a light kind (_light_problem()),
        # and not together with `cover:`. The ID follows `<identity_id>_light`.
        cv.Optional(CONF_LIGHT): cv.string_strict,
        # The light gets a brightness slider, as `dimmable:` does on the two-way light. Needs `light:`.
        cv.Optional(CONF_DIMMABLE): cv.boolean,
        # How long the light fades from off to full and back (ADR 0128); both or neither, and only with `dimmable:`.
        cv.Optional(CONF_ON_DURATION): cv.positive_not_null_time_period,
        cv.Optional(CONF_OFF_DURATION): cv.positive_not_null_time_period,
        # How the light's slider value reaches the device (ADR 0128), only with its fade times: "timed" (default) sends
        # on or off and a stop when the estimate reaches the value; "frame" sends one position frame.
        cv.Optional(CONF_SET_POSITION): cv.one_of("timed", "frame", lower=True),
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


def _power_class_expression(identity):
    """Map `low_power:` to the C++ `OneWayPowerClass` the identity stores.

    `true` -> LOW_POWER (one wake-up copy with the low-power flag, then short copies); unset and
    `false` -> ALWAYS_ALIVE (four short copies), for every manufacturer. See ADR 0122 for the
    default and ADR 0038 for the shapes.
    """
    name = "LOW_POWER" if identity.get(CONF_LOW_POWER, False) else "ALWAYS_ALIVE"
    return f"esphome::home_io_control::OneWayPowerClass::{name}"


def _invert_override_expression(identity):
    """Map `invert_position:` to the C++ `std::optional<bool>` the identity stores.

    Unset -> `{}`: the kind's INVERTED capability is looked up in C++ by
    `effective_inverted()`, so the Python side never carries a copy of the type table.
    """
    if CONF_INVERT_POSITION not in identity:
        return "{}"
    return "true" if identity[CONF_INVERT_POSITION] else "false"


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
            f".execute_acei = 0x{identity.get(CONF_EXECUTE_ACEI, 0):02X}",
            f".execute_broadcast_all = "
            f"{'true' if identity[CONF_EXECUTE_BROADCAST] == 'all' else 'false'}",
            f".enrollment_classes = {_enrollment_classes_initialiser(identity)}",
            f".power_class = {_power_class_expression(identity)}",
            f".invert_override = {_invert_override_expression(identity)}",
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

    Shared between validate_oneway_controllers() (collisions against the hub's own node_id and
    other oneway_controllers entries) and final_validate_oneway_controller_addresses()
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
    if owner.endswith("linked_remotes"):
        # The hub does not hear its own transmissions; the link it needs is the identity's own key.
        hint += (
            ". To have the hub follow its own commands on a device, list the device under the "
            "identity's drives: instead"
        )
    raise cv.Invalid(
        f"oneway_controllers id '{identity_id}' uses node_id {node_id}, which collides with "
        f"{owner}{hint}. Two transmitters sharing an address share a rolling sequence "
        f"counter, which silently desyncs both."
    )


@dataclass(frozen=True)
class _ResolvedIdentity:
    """A `oneway_controllers:` entry after its defaults are filled in.

    `identity` is the (mutated) config entry; the other fields are facts the rules need that the
    entry no longer shows once defaults are in.
    """

    identity: dict
    radio_type: str
    # True only when the user wrote `manufacturer:`; an omitted value is defaulted to 0 (Somfy).
    manufacturer_explicit: bool

    @property
    def identity_id(self):
        return self.identity[CONF_ID]

    @property
    def can_transmit(self):
        """True when the identity can emit an EXECUTE: it has commands or an enroll button."""
        return bool(self.identity[CONF_COMMANDS]) or self.identity[CONF_ENROLLMENT]


def _resolve_identity_defaults(identity, hub_config):
    """Fill in an identity's derived address, key and manufacturer, and return it resolved."""
    hub_node_id = hub_config[CONF_NODE_ID]
    if CONF_NODE_ID not in identity:
        identity[CONF_NODE_ID] = derive_oneway_node_id(hub_node_id, identity[CONF_ID])
        identity[CONF_NODE_ID_DERIVED] = True

    # A per-identity key is optional so that identities on the hub's own network need not
    # repeat it; an adopted foreign network's key is what makes the override necessary.
    if CONF_SYSTEM_KEY not in identity:
        identity[CONF_SYSTEM_KEY] = hub_config[CONF_SYSTEM_KEY]

    # Recorded before the default, so the wire-profile warning can tell an explicit unprofiled
    # vendor from an omitted manufacturer (the common back-compat case, which maps silently to
    # Somfy).
    manufacturer_explicit = CONF_MANUFACTURER in identity
    if not manufacturer_explicit:
        identity[CONF_MANUFACTURER] = 0
    return _ResolvedIdentity(identity, hub_config[CONF_RADIO_TYPE], manufacturer_explicit)


def _reject_collisions(resolved, seen_node_ids):
    """Raise if the identity's address is already claimed; otherwise claim it in `seen_node_ids`.

    `seen_node_ids` maps a resolved address to its human-readable owner, so a collision message
    can name both sides rather than just reporting that "an" address is taken.
    """
    identity = resolved.identity
    _reject_node_id_collision(
        resolved.identity_id, identity[CONF_NODE_ID], seen_node_ids, identity.get(CONF_NODE_ID_DERIVED)
    )
    seen_node_ids[identity[CONF_NODE_ID]] = f"oneway_controllers id '{resolved.identity_id}'"


def _enrollment_manufacturer_problem(resolved):
    """Error text when `enrollment: true` has no explicit `manufacturer:`, else None.

    manufacturer becomes required, not silently 0, whenever enrollment: true actually puts this
    byte on the air in a CMD_ONEWAY_ADD_CONTROLLER frame -- 1W gives no error a wrong value would
    ever surface as, so the mistake has to be caught here instead. Without it the manufacturer
    only selects the wire profile (0 selects the Somfy-shaped one), so defaulting to 0 is harmless.
    """
    if resolved.identity[CONF_ENROLLMENT] and not resolved.manufacturer_explicit:
        return (
            f"oneway_controllers id '{resolved.identity_id}' has enrollment: true but no manufacturer: "
            "set. Find the value from a key-adoption report for this network (the 'Recover "
            "1W Controller Key' switch prints it), or from the device's own documentation."
        )
    return None


def _cover_problem(resolved):
    """Error text when `cover:` cannot work on this identity, else None.

    The cover shows the state of a cover, so the identity's kind must be one (a named position is the capability
    every cover row has; kind_is_cover()). The travel times are both or neither, and need the cover.
    """
    identity = resolved.identity
    if CONF_COVER in identity and not kind_is_cover(identity[CONF_IO_DEVICE_TYPE]):
        kind = device_type_name(identity[CONF_IO_DEVICE_TYPE]) or hex(identity[CONF_IO_DEVICE_TYPE])
        return (
            f"oneway_controllers id '{resolved.identity_id}' has cover: but its io_device_type '{kind}' is not a "
            "cover kind, so open, close and stop mean nothing to it. Remove cover:, or set the io_device_type "
            "of the shutter, awning, blind, window or gate it controls."
        )
    has_open = CONF_OPEN_DURATION in identity
    if has_open != (CONF_CLOSE_DURATION in identity):
        missing = CONF_CLOSE_DURATION if has_open else CONF_OPEN_DURATION
        return (
            f"oneway_controllers id '{resolved.identity_id}' sets only one of open_duration: and close_duration:, "
            f"{missing}: is missing. The cover estimates its position from the time a full run takes each way, so "
            "it needs both. Set both, or neither."
        )
    if has_open and CONF_COVER not in identity:
        return (
            f"oneway_controllers id '{resolved.identity_id}' has open_duration: and close_duration: but no cover:. "
            "They give the cover its travel times; without cover: there is nothing to estimate a position for."
        )
    return None


def _light_problem(resolved):
    """Error text when `light:` or one of its keys cannot work on this identity, else None.

    The light shows the state of a light, so the identity's kind must be one (kind_is_light()). The fade times and
    `dimmable:` belong to the light; the fade times are both or neither and give the slider something to estimate.
    """
    identity = resolved.identity
    handle = f"oneway_controllers id '{resolved.identity_id}'"
    if CONF_LIGHT in identity and not kind_is_light(identity[CONF_IO_DEVICE_TYPE]):
        kind = device_type_name(identity[CONF_IO_DEVICE_TYPE]) or hex(identity[CONF_IO_DEVICE_TYPE])
        return (
            f"{handle} has light: but its io_device_type '{kind}' is not a light kind, so on and off mean nothing "
            "to it. Remove light:, or set the io_device_type of the light it controls."
        )
    if CONF_LIGHT not in identity:
        for key in (CONF_DIMMABLE, CONF_ON_DURATION, CONF_OFF_DURATION):
            if key in identity:
                return f"{handle} has {key}: but no light:. It is a key of the light; without light: there is nothing to apply it to."
        return None
    has_on = CONF_ON_DURATION in identity
    if has_on != (CONF_OFF_DURATION in identity):
        missing = CONF_OFF_DURATION if has_on else CONF_ON_DURATION
        return (
            f"{handle} sets only one of on_duration: and off_duration:, {missing}: is missing. The light estimates "
            "its level from the time a full fade takes each way, so it needs both. Set both, or neither."
        )
    if has_on and not identity.get(CONF_DIMMABLE):
        return (
            f"{handle} has on_duration: and off_duration: but is not dimmable. A fade time estimates a brightness "
            "level, which a light without a slider does not show. Set dimmable: true, or remove them."
        )
    return None


def _entity_problem(resolved):
    """Error text when the identity's `cover:` or `light:` entity cannot work, else None.

    The two entities exclude each other (an identity is one device). `follows:` lists the remotes whose presses move
    the entity's state, so it needs one. `set_position:` says how the light's slider value reaches the device, which
    needs the fade times that make a value possible (ADR 0128).
    """
    identity = resolved.identity
    handle = f"oneway_controllers id '{resolved.identity_id}'"
    if CONF_COVER in identity and CONF_LIGHT in identity:
        return f"{handle} has both cover: and light:. An identity is one device: keep the entity it is."
    problem = _cover_problem(resolved) or _light_problem(resolved)
    if problem:
        return problem
    if identity.get(CONF_FOLLOWS) and CONF_COVER not in identity and CONF_LIGHT not in identity:
        return (
            f"{handle} has follows: but neither cover: nor light:. follows: lists the remotes whose presses move "
            "the entity's shown state; without one there is nothing to move."
        )
    if CONF_SET_POSITION in identity and CONF_ON_DURATION not in identity:
        return (
            f"{handle} has set_position: but no fade times on a light. It says how the light's slider value reaches "
            "the device, which needs the time a full fade takes: set light:, dimmable: true, on_duration: and "
            "off_duration:."
        )
    return None


# === Warning rules ===
# Each rule takes a _ResolvedIdentity and returns the warning text or None. A warning, not
# cv.Invalid, because each flags a legal config that still transmits.


def _warn_unprofiled_manufacturer(resolved):
    """manufacturer: also drives the 1W CMD_EXECUTE ACEI byte (ADR 0031). Only Somfy and Velux
    have a verified wire profile; any other explicitly-named vendor falls back to the Somfy-shaped
    default, which is a guess. Only an identity that can emit an EXECUTE is flagged; an inert
    identity's wire shape does not matter yet."""
    identity = resolved.identity
    if (
        resolved.manufacturer_explicit
        and identity[CONF_MANUFACTURER] not in ONEWAY_WIRE_PROFILE_MANUFACTURERS
        and resolved.can_transmit
    ):
        return (
            f"home_io_control: oneway_controllers '{resolved.identity_id}' has manufacturer "
            f"0x{identity[CONF_MANUFACTURER]:02X}, which has no "
            "1W wire profile -- using the Somfy-shaped defaults (ACEI 0x43). Set execute_acei: "
            "explicitly if that is wrong for your device."
        )
    return None


def _warn_velux_interior_blind_default_sweep(resolved):
    """The velux profile's default 0x30 sweep is the exterior-shading set a KLI 310/313 uses
    ({roller_shutter, vertical_exterior_awning, dual_roller_shutter}). An interior-blind remote
    (KLI 312) sweeps vertical_interior_blind/interior_venetian_blind instead, so a blind identity
    that enrolls with the default sweep almost certainly misses the actuator. Enrollment ignores
    io_device_type, so point the user at enrollment_classes: and at the KLI's own 0x2E lines that
    name the classes."""
    identity = resolved.identity
    if (
        identity[CONF_MANUFACTURER] == MANUFACTURER_OPTIONS["velux"]
        and identity[CONF_ENROLLMENT]
        and CONF_ENROLLMENT_CLASSES not in identity
        and identity[CONF_IO_DEVICE_TYPE]
        in (
            DEVICE_TYPE_OPTIONS["vertical_interior_blind"],
            DEVICE_TYPE_OPTIONS["interior_venetian_blind"],
        )
    ):
        kind_name = next(
            name for name, value in DEVICE_TYPE_OPTIONS.items() if value == identity[CONF_IO_DEVICE_TYPE]
        )
        return (
            f"home_io_control: oneway_controllers '{resolved.identity_id}' is a VELUX {kind_name} with "
            "enrollment: true, but "
            "enrollment_classes: is unset, so the enrollment 0x30 sweep uses the exterior-shading "
            "default roller_shutter/vertical_exterior_awning/dual_roller_shutter and ignores "
            "io_device_type. This device may enroll on other classes: set enrollment_classes: (a KLI "
            "312 interior blind uses [vertical_interior_blind, interior_venetian_blind]). To find "
            "yours, press Gear on the existing remote and use the classes named in its 'rx 1W "
            "remote ... (0x2E)' DEBUG log lines -- see 'Finding your enrollment classes' in the "
            "1W transmit docs."
        )
    return None


def _warn_inverted_kind_broadcast_all(resolved):
    """An inverted kind mirrors open/close on the wire. A broadcast-to-all frame also reaches
    receivers of other kinds, so the mirror applies to them too -- legal, but worth a flag."""
    identity = resolved.identity
    if (
        resolved.can_transmit
        and CONF_INVERT_POSITION not in identity
        and identity[CONF_EXECUTE_BROADCAST] == "all"
        and "INVERTED" in device_features(identity[CONF_IO_DEVICE_TYPE])
    ):
        return (
            f"home_io_control: oneway_controllers '{resolved.identity_id}' is an inverted device kind with "
            "execute_broadcast: all -- open/close are mirrored for every receiver of this "
            "broadcast; set invert_position explicitly if this identity also drives "
            "non-awning devices."
        )
    return None


def _warn_inert_enrollment_classes(resolved):
    """enrollment_classes: only feeds the VELUX enrollment gesture. On a somfy/unprofiled
    identity, or one with no enroll button at all, it is silently inert."""
    identity = resolved.identity
    if CONF_ENROLLMENT_CLASSES in identity and (
        identity[CONF_MANUFACTURER] != MANUFACTURER_OPTIONS["velux"] or not identity[CONF_ENROLLMENT]
    ):
        return (
            f"home_io_control: oneway_controllers '{resolved.identity_id}' sets enrollment_classes: but it only "
            "affects the VELUX enrollment gesture (needs manufacturer: velux AND "
            "enrollment: true) -- it is ignored here."
        )
    return None


# In the order the warnings are logged.
_WARNING_RULES = (
    _warn_unprofiled_manufacturer,
    _warn_velux_interior_blind_default_sweep,
    _warn_inverted_kind_broadcast_all,
    _warn_inert_enrollment_classes,
)


def _inject_identity_entity_ids(identity):
    """Declare the identity's entity IDs at validation time, not in to_code(): an ID created
    late is silently dropped at runtime (ADR 0009). The `<identity_id>_<command>` shape is a
    documented contract, not an implementation detail -- users compose `time_based` covers
    against these IDs and cannot do that against IDs they can't predict.
    """
    identity_id = identity[CONF_ID]
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
    if CONF_COVER in identity:
        identity[CONF_COVER_ENTITY_ID] = ID(
            f"{identity_id}_cover",
            is_declaration=True,
            type=IOHomeOneWayCover,
        )
    if CONF_LIGHT in identity:
        identity[CONF_LIGHT_ENTITY_ID] = ID(
            f"{identity_id}_light",
            is_declaration=True,
            type=IOHomeOneWayLight,
        )
        identity[CONF_LIGHT_STATE_ID] = ID(
            f"{identity_id}_light_state",
            is_declaration=True,
            type=light_component.LightState,
        )
        if identity.get(CONF_DIMMABLE):
            identity[CONF_LIGHT_GAMMA_TABLE_ID] = ID(
                f"{identity_id}_light_gamma_table",
                is_declaration=True,
                type=light_component.GammaTable,
            )


def validate_oneway_controllers(config):
    """Resolve per-identity defaults and reject address/handle collisions at compile time.

    Runs as a post-validator on the whole hub config because every rule here needs the hub's own
    `node_id`/`system_key`, which a per-entry validator cannot see.

    Only checks addresses visible within `home_io_control:` itself (its own `node_id` and every
    configured `oneway_controllers` entry) — see final_validate_oneway_controller_addresses()
    below for the matching check against `linked_remotes:`/`io_device_id:` declared elsewhere in
    the same YAML, which needs the full cross-component config and so cannot run here. Even
    together the two cannot see a real remote the user has never mentioned to this config at all;
    see derive_oneway_node_id()'s own docstring for why that residual risk cannot be validated
    away.
    """
    identities = config.get(CONF_ONEWAY_CONTROLLERS, [])
    if not identities:
        return config

    seen_ids = set()
    seen_node_ids = {config[CONF_NODE_ID]: "the hub's own node_id"}

    for identity in identities:
        identity_id = identity[CONF_ID]
        if identity_id in seen_ids:
            raise cv.Invalid(
                f"Duplicate oneway_controllers id '{identity_id}' — each identity needs its own handle"
            )
        seen_ids.add(identity_id)

        resolved = _resolve_identity_defaults(identity, config)
        _reject_collisions(resolved, seen_node_ids)
        problem = _enrollment_manufacturer_problem(resolved) or _entity_problem(resolved)
        if problem:
            raise cv.Invalid(problem)
        for rule in _WARNING_RULES:
            message = rule(resolved)
            if message:
                _LOGGER.warning("%s", message)
        _inject_identity_entity_ids(identity)

    return config


def _collect_declared_device_addresses(full_config):
    """Map every node ID declared as an `io_device_id:` or a bare `linked_remotes:` entry, across
    every `home_io_control` entity in `full_config`, to a human-readable owner string.

    Only entries with `platform: home_io_control` are considered (declared_device_entries()).
    `class:` linked_remotes entries name a device *type*, not a node, so they carry no address to
    collide with and are skipped.
    """
    addresses = {}
    for domain, entry, owner_name in declared_device_entries(full_config):
        device_id = entry.get(CONF_IO_DEVICE_ID)
        if device_id:
            addresses[device_id] = f"{domain} '{owner_name}' io_device_id"
        for remote in entry.get(CONF_LINKED_REMOTES, []):
            if remote.startswith("class:"):
                continue
            addresses[remote] = f"{domain} '{owner_name}' linked_remotes"
    return addresses


def final_validate_oneway_controller_addresses(config):
    """Extend the oneway_controllers address-collision check to addresses declared outside
    `home_io_control:` — a `linked_remotes:` entry or an `io_device_id:` on some other entity in
    this same YAML.

    Runs as FINAL_VALIDATE_SCHEMA rather than inside validate_oneway_controllers() because only
    final validation has access to the full cross-component config (`fv.full_config`) —
    entries of the DEVICE_BOUND_DOMAINS platforms are validated independently of
    `home_io_control:`'s own CONFIG_SCHEMA and are not visible to it. By this point
    validate_oneway_controllers() has already run, so every identity's `node_id` (derived or
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


# The platforms whose entities show the state a one-way command leaves (ADR 0116): a cover's position and a
# light's level. Nothing else is built or tested to follow one.
_FOLLOWING_DOMAINS = ("cover", "light")


def final_validate_oneway_drives(config):
    """Check every `drives:` entry against the devices declared in this YAML.

    Final validation, because only it sees the device entries next to `home_io_control:`. An id that
    no entry declares would never follow anything, silently; a device that is not a cover or a light
    (a climate, a water heater, a sensor, ...) has no position or level for a one-way command to move.
    """
    identities = config.get(CONF_ONEWAY_CONTROLLERS, [])
    if not any(identity[CONF_DRIVES] for identity in identities):
        return config

    declared = {
        entry[CONF_IO_DEVICE_ID]: (domain, entry)
        for domain, entry, _ in declared_device_entries(fv.full_config.get())
        if entry.get(CONF_IO_DEVICE_ID)
    }
    for identity in identities:
        for device_id in identity[CONF_DRIVES]:
            if device_id not in declared:
                raise cv.Invalid(
                    f"oneway_controllers id '{identity[CONF_ID]}' drives '{device_id}', which is not "
                    "an io_device_id declared in this configuration"
                )
            domain, entry = declared[device_id]
            if domain not in _FOLLOWING_DOMAINS:
                raise cv.Invalid(
                    f"oneway_controllers id '{identity[CONF_ID]}' drives {domain} "
                    f"'{entry.get(CONF_NAME) or device_id}', which cannot follow its commands: only a "
                    "cover or a light shows the state of a one-way command"
                )
    return config


async def create_oneway_controller_entities(identity, var):
    """Create one identity's command buttons and its "Last 1W Command" diagnostic sensor.

    Same normalization as create_hub_arming_switch() (hub_entities.py): run a bare {id, name}
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

    if CONF_COVER in identity:
        cover_config = cover_component.cover_schema(IOHomeOneWayCover).extend(cv.COMPONENT_SCHEMA)(
            {
                CONF_ID: identity[CONF_COVER_ENTITY_ID],
                CONF_NAME: identity[CONF_COVER],
            }
        )
        cover_entity = await cover_component.new_cover(cover_config)
        await cg.register_component(cover_entity, cover_config)
        cg.add(cover_entity.set_parent(var))
        cg.add(cover_entity.set_controller_id(identity[CONF_ID]))
        for sender in identity[CONF_FOLLOWS]:
            cg.add(cover_entity.add_follow(sender))
        if CONF_OPEN_DURATION in identity:
            cg.add(
                cover_entity.set_travel_durations(
                    identity[CONF_OPEN_DURATION].total_milliseconds,
                    identity[CONF_CLOSE_DURATION].total_milliseconds,
                )
            )

    if CONF_LIGHT in identity:
        light_type = light_component.LightType.BRIGHTNESS_ONLY if identity.get(CONF_DIMMABLE) else light_component.LightType.BINARY
        light_values = {
            CONF_ID: identity[CONF_LIGHT_STATE_ID],
            CONF_OUTPUT_ID: identity[CONF_LIGHT_ENTITY_ID],
            CONF_NAME: identity[CONF_LIGHT],
        }
        light_schema = light_component.light_schema(IOHomeOneWayLight, light_type).extend(cv.COMPONENT_SCHEMA)
        if identity.get(CONF_DIMMABLE):
            # The device fades by itself; an ESPHome transition would send a stream of commands, so the default is
            # the two-way light's.
            light_schema = light_schema.extend(INSTANT_LIGHT_TRANSITION)
            # A brightness light gets a gamma table whose ID the pipeline would generate for a `light:` entry; this
            # one is built here, so it passes the ID declared at validation. The output ignores gamma (it sends the
            # linear value).
            light_values[light_component.CONF_GAMMA_TABLE_ID] = identity[CONF_LIGHT_GAMMA_TABLE_ID]
        light_config = light_schema(light_values)
        light_entity = cg.new_Pvariable(light_config[CONF_OUTPUT_ID])
        await cg.register_component(light_entity, light_config)
        await light_component.register_light(light_entity, light_config)
        cg.add(light_entity.set_parent(var))
        cg.add(light_entity.set_controller_id(identity[CONF_ID]))
        cg.add(light_entity.set_dimmable(bool(identity.get(CONF_DIMMABLE))))
        for sender in identity[CONF_FOLLOWS]:
            cg.add(light_entity.add_follow(sender))
        if CONF_ON_DURATION in identity:
            cg.add(
                light_entity.set_fade_durations(
                    identity[CONF_ON_DURATION].total_milliseconds,
                    identity[CONF_OFF_DURATION].total_milliseconds,
                )
            )
        if CONF_SET_POSITION in identity:
            cg.add(light_entity.set_slider_way(getattr(OneWaySliderWay, identity[CONF_SET_POSITION].upper())))

    if identity[CONF_ENROLLMENT]:
        # entity_category: config, not a switch behind an arming flag: the receiver's own 2s PROG
        # hold is the real interlock -- a hub cannot enroll into a device nobody has walked up to
        # (ADR 0026). ADR 0021's bootloader precedent doesn't apply here (that ADR is for an
        # irreversible write; un-enrollment exists here as a rollback path).
        #
        # No "(May Replace Existing Remotes)" caveat: enrolling a new identity onto a device is
        # additive, not destructive -- an existing remote keeps working alongside a newly enrolled
        # hub. Un-enrollment (`0x39`, the `oneway_remove_controller` action) reverses it only while
        # the receiver is in its programming window, confirmed on one receiver so far -- see that
        # action's own doxygen (management_actions.h).
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
