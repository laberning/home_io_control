"""Golden-frame corpus id/directory naming convention — the single source of truth.

`id = <subject>_<phase>[_<scenario>][_<chip>]`, `filename == id`, and the capture lives in
`tests/corpus/captures/<phase>/`. This module is deliberately dependency-free (no PyYAML) so
`ingest.py` and `validate.py` can import it without pulling in the generator's deps.
Kept in sync with `tests/corpus/README.md` "Naming convention"; `validate.py` enforces it.
"""

# Closed protocol-phase vocabulary. The phase token is also the capture's subdirectory.
#   exchange   — any 2W authenticated command (0x00 EXECUTE, 0x20 WRITE_PRIVATE, 0x50/0x52
#                name get/set) + 0x3C/0x3D + ack. Also a bare 0x3C/0x3D loopback.
#   statuspoll — a 0x03 -> 0x04 position poll (plain form), challenged or not.
#   probe      — a diagnostic read: 0x03 at a non-default function id / extended form,
#                0x0C (private2), or 0x54-0x58.
#   pairing    — 0x31/0x32/0x33 key exchange, either role (key extraction included).
#   discovery  — 0x28/0x29, 0x2A/0x2B roll-call, 0x2E/0x2F alt-discovery.
#   oneway     — 1W transmit / overheard traffic (0x00/0x20 with no 2W challenge).
#   enrollment — 1W 0x30 add-controller / 0x39 remove-controller.
#   unsolicited— 0x71 STATUS_UPDATE with no preceding request.
#   identify   — 0x1E identify / jog.
# A passive multi-phase sniff takes the phase of its dominant / most-interesting traffic.
PHASES = ("exchange", "probe", "oneway", "statuspoll", "pairing", "discovery",
          "enrollment", "unsolicited", "identify")

# Open subject vocabulary — the capture source (for 1W captures: the *transmitting* device,
# not the receiver, unless the transmitter is our own hub). A genuinely new device/source is a
# one-line addition here (rare). Keeping it closed is what stops `somfy_dimmer` drifting back in
# next to `somfy_izymo_dimmer`, or a `velux_windwo` typo, from ever reaching main.
SUBJECTS = frozenset({
    "atlantic_thermor", "multi_somfy", "reference_1w", "selfpair", "wind_sensor",
    "somfy_awning", "somfy_connectivity_kit", "somfy_izymo_dimmer", "somfy_j406",
    "somfy_oximo40", "somfy_rs100", "somfy_smoove", "somfy_sunilus", "somfy_tahoma",
    "synthetic", "tilt_cover", "unidentified_1w_remote",
    "velux_kig300", "velux_kli310", "velux_kli313", "velux_klr200", "velux_kux100", "velux_window",
})


def phase_of_id(cid: str) -> str:
    """The phase token of a capture id — also the subdirectory it lives in.

    Raises ValueError (not a bare StopIteration) for an id with no phase token; callers that
    accept arbitrary strings should run id_naming_problem() first.
    """
    phase = next((t for t in cid.split("_") if t in PHASES), None)
    if phase is None:
        raise ValueError(f"capture id {cid!r} has no phase token from {sorted(PHASES)}")
    return phase


def id_naming_problem(cid: str):
    """Return a human-readable reason `cid` violates <subject>_<phase>[_<scenario>][_<chip>], or None.

    `phase` is the first phase-vocab token; everything before it is `subject` (must be
    registered). A later phase word inside `scenario` (e.g.
    `..._pairing_key_exchange_retry_success`) is fine — only the leading `<subject>_<phase>`
    shape is checked.
    """
    toks = cid.split("_")
    phase_at = next((i for i, t in enumerate(toks) if t in PHASES), None)
    if phase_at is None:
        return f"contains no phase token — the token after the subject must be one of {sorted(PHASES)}"
    subject = "_".join(toks[:phase_at])
    if not subject:
        return "has a phase token but no subject before it"
    if subject not in SUBJECTS:
        return f"subject '{subject}' is not in scripts/corpus/naming.py SUBJECTS — add it there if it is a real new source"
    return None
