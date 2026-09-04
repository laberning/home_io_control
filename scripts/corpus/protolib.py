"""Shared read-side protocol helpers for the golden-frame corpus toolchain.

Ported, read-only mirrors of small pieces of components/home_io_control/proto_frame.{h,cpp},
proto_constants.h, and proto_crypto.cpp — just enough for validate.py and ingest.py to
check/derive frame shape and replay the proprietary HMAC/key-transfer scheme without a C++
toolchain in the loop. The AES-128-ECB primitive itself is NOT reimplemented here — it is a
standard algorithm, so this uses the `cryptography` package; only the proprietary
checksum/IV/truncation wrapper around it (proto_crypto.cpp) is ported by hand. Cross-language
agreement is pinned by scripts/corpus/tests/data/crypto_kat.yaml against hardcoded vectors in
tests/corpus_crypto_test.cpp — both generated from the real C++ implementation
(tests/corpus_bootstrap_dump_test.cpp :: DISABLED_PrintCryptoKatVectors).
"""

import re
from collections import namedtuple

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

# --- Mirrors components/home_io_control/proto_sizes.h ---------------------------------------
FRAME_MIN_SIZE = 9  # CTRL0 + CTRL1 + DST(3) + SRC(3) + CMD(1)
FRAME_MAX_SIZE = 32  # 9 header + 23 data

# --- Mirrors components/home_io_control/proto_frame.h :: CTRL0/CTRL1 bit layout -------------
CTRL0_END = 0x80
CTRL0_START = 0x40
CTRL0_PROTOCOL_1W = 0x20
CTRL0_LENGTH_MASK = 0x1F

# --- Mirrors components/home_io_control/proto_frame.cpp :: crc_ccitt() ----------------------
CRC_POLYNOMIAL_REVERSED = 0x8408


def parse_hex(hex_str: str) -> bytes:
    """Canonical hex-string -> bytes conversion, tolerant of internal whitespace.

    The one place this logic lives; build.py and validate.py's own parse_hex (which adds
    error-message context) both delegate here instead of reimplementing it.
    """
    return bytes.fromhex("".join(hex_str.split()))


def crc_ccitt(data: bytes) -> int:
    """Port of crc_ccitt() in proto_frame.cpp: poly 0x8408 (reversed), init 0x0000."""
    crc = 0x0000
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ CRC_POLYNOMIAL_REVERSED if (crc & 0x0001) else (crc >> 1)
    return crc & 0xFFFF


def ctrl0_implied_length(ctrl0: int) -> int:
    """Total non-CRC frame length implied by CTRL0 bits [4:0] (proto_frame.cpp :: frame_length())."""
    return (ctrl0 & CTRL0_LENGTH_MASK) + 1


def is_start(ctrl0: int) -> bool:
    return bool(ctrl0 & CTRL0_START)


def is_end(ctrl0: int) -> bool:
    return bool(ctrl0 & CTRL0_END)


def is_oneway(ctrl0: int) -> bool:
    return bool(ctrl0 & CTRL0_PROTOCOL_1W)


# --- Mirrors components/home_io_control/proto_constants.h :: CMD_* values needed by the
# --rekey pipeline (ingest.py) to locate challenge/response and key-init/key-transfer pairs.
CMD_ONEWAY_ADD_CONTROLLER = 0x30
CMD_ONEWAY_REMOVE = 0x39
CMD_KEY_INIT = 0x31
CMD_KEY_TRANSFER = 0x32
CMD_CHALLENGE_REQ = 0x3C
CMD_CHALLENGE_RESP = 0x3D
CMD_DISCOVER_SPE_REQ = 0x2A

# Commands that authenticate themselves in a single frame — payload = [challenge(6) | HMAC(6)],
# the HMAC taken over just the command byte, so one broadcast frame carries both halves of a
# challenge-response instead of a 0x3C/0x3D round trip. Only 0x2A is listed: it is the only shape
# this project has real evidence for (velux_kux100_pairing_full.yaml). Adding a command here
# without a capture that verifies under a known key would make --rekey rewrite bytes it does not
# understand.
SELF_AUTHENTICATED_COMMANDS = {CMD_DISCOVER_SPE_REQ}

# --- Mirrors components/home_io_control/proto_constants.h :: CMD_* (subset used for scaffold
# readability/notes only — never used to derive a committed `expect:` value, which must be
# human/issue-thread verified per tests/corpus/README.md).
CMD_NAMES = {
    0x00: "EXECUTE",
    0x01: "ACTIVATE_MODE",
    0x03: "PRIVATE",
    0x04: "PRIVATE_RESP",
    0x0C: "PRIVATE2",
    0x0D: "PRIVATE2_RESP",
    0x19: "SET_SENSOR",
    0x1A: "SET_SENSOR_ACK",
    0x1E: "IDENTIFY",
    0x20: "WRITE_PRIVATE",
    0x21: "WRITE_PRIVATE_ACK",
    0x28: "DISCOVER_REQ",
    0x29: "DISCOVER_RESP",
    0x2A: "DISCOVER_SPE_REQ",
    0x2B: "DISCOVER_SPE_RESP",
    0x2C: "DISCOVER_CONFIRM",
    0x2D: "DISCOVER_CONFIRM_ACK",
    0x2E: "DISCOVER_ALT_REQ",
    0x2F: "DISCOVER_ALT_RESP",
    0x30: "ONEWAY_ADD_CONTROLLER",
    0x31: "KEY_INIT",
    0x32: "KEY_TRANSFER",
    0x33: "KEY_CONFIRM",
    0x36: "ADDRESS_REQ",
    0x37: "ADDRESS_RESP",
    0x38: "LAUNCH_KEY_TRANSFER",
    0x39: "ONEWAY_REMOVE",
    0x3C: "CHALLENGE_REQ",
    0x3D: "CHALLENGE_RESP",
    0x4A: "UNKNOWN4A_REQ",
    0x4B: "UNKNOWN4A_RESP",
    0x50: "GET_NAME",
    0x51: "GET_NAME_RESP",
    0x52: "SET_NAME",
    0x53: "SET_NAME_RESP",
    0x54: "GET_INFO1",
    0x55: "GET_INFO1_RESP",
    0x56: "GET_INFO2",
    0x57: "GET_INFO2_RESP",
    0x58: "GET_GENERAL_INFO3",
    0x59: "GET_GENERAL_INFO3_RESP",
    0x6F: "SET_CONFIG1",
    0x70: "SET_CONFIG1_RESP",
    0x71: "STATUS_UPDATE",
    0x72: "STATUS_UPDATE_RESP",
    0xF0: "SEND_RAW_MESSAGE",
    0xF1: "READ_GROUPS",
    0xF2: "REBOOT",
    0xF3: "SERVICE_STATUS_ACK",
    0xFE: "ERROR_RESP",
}


def cmd_name(cmd: int) -> str:
    return CMD_NAMES.get(cmd, f"UNKNOWN_0x{cmd:02X}")


# ==============================================================================================
# Crypto — port of components/home_io_control/proto_crypto.cpp's proprietary IV/HMAC/key-transfer
# scheme (NOT the AES-128 primitive itself, see module docstring).
# ==============================================================================================

HMAC_SIZE = 6
AES_KEY_SIZE = 16
AES_BLOCK_SIZE = 16
IV_SIZE = 16
IV_PADDING = 0x55

# Hardcoded, public, identical across every IO-Homecontrol installation worldwide — obfuscates
# (not protects) the system key in transit during pairing. Mirrors proto_constants.h::TRANSFER_KEY.
TRANSFER_KEY = bytes([0x34, 0xC3, 0x46, 0x6E, 0xD8, 0x8F, 0x4E, 0x8E, 0x16, 0xAA, 0x47, 0x39, 0x49, 0x88, 0x43, 0x73])


def compute_checksum(byte: int, c1: int, c2: int) -> "tuple[int, int]":
    """Port of compute_checksum() (proto_crypto.cpp) — a proprietary running-checksum
    accumulator, NOT a standard CRC. Order of operations matters: `tmp` is computed from the
    *incoming* c2 before c2 is overwritten; the branch is taken on the *incoming* c1's high bit.
    """
    tmp = (byte ^ c2) & 0xFF
    new_c2 = ((c1 & 0x7F) << 1) & 0xFF
    if (c1 & 0x80) == 0:
        if tmp >= 128:
            new_c2 |= 1
        new_c1 = new_c2
        new_c2 = (tmp << 1) & 0xFF
        return new_c1, new_c2
    if tmp >= 128:
        new_c2 |= 1
    new_c1 = (new_c2 ^ 0x55) & 0xFF
    new_c2 = ((tmp << 1) ^ 0x5B) & 0xFF
    return new_c1, new_c2


def construct_iv(data: bytes, challenge: bytes) -> bytes:
    """Port of construct_iv() (proto_crypto.cpp): bytes 0-7 = first 8 data bytes (0x55-padded if
    shorter), bytes 8-9 = running checksum over *all* of `data`, bytes 10-15 = challenge.
    """
    if len(challenge) != HMAC_SIZE:
        raise ValueError(f"challenge must be {HMAC_SIZE} bytes, got {len(challenge)}")
    iv = bytearray(IV_SIZE)
    c1 = 0
    c2 = 0
    for i, byte in enumerate(data):
        c1, c2 = compute_checksum(byte, c1, c2)
        if i < 8:
            iv[i] = byte
    for i in range(len(data), 8):
        iv[i] = IV_PADDING
    iv[8] = c1
    iv[9] = c2
    iv[10:16] = challenge
    return bytes(iv)


def aes128_ecb_encrypt_block(block: bytes, key: bytes) -> bytes:
    """Single AES-128-ECB block encrypt (no padding — caller always supplies exactly 16 bytes)."""
    if len(block) != AES_BLOCK_SIZE or len(key) != AES_KEY_SIZE:
        raise ValueError("aes128_ecb_encrypt_block requires a 16-byte block and a 16-byte key")
    encryptor = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    return encryptor.update(block) + encryptor.finalize()


def create_hmac(data: bytes, challenge: bytes, key: bytes) -> bytes:
    """Port of create_hmac() (proto_crypto.cpp): IV from data+challenge, AES-128-ECB with `key`,
    truncate to HMAC_SIZE bytes. NOT a standard HMAC construction — proprietary to this protocol.
    """
    iv = construct_iv(data, challenge)
    encrypted = aes128_ecb_encrypt_block(iv, key)
    return encrypted[:HMAC_SIZE]


def verify_hmac(data: bytes, hmac: bytes, challenge: bytes, key: bytes) -> bool:
    return create_hmac(data, challenge, key) == hmac


def crypt_key(data: bytes, challenge: bytes, in_key: bytes, transfer_key: bytes = TRANSFER_KEY) -> bytes:
    """Port of crypt_key() (proto_crypto.cpp): XOR `in_key` with AES-128-ECB(IV, transfer_key).
    Symmetric — the same call encrypts (in_key=system key) or decrypts (in_key=received payload).
    """
    if len(in_key) != AES_KEY_SIZE:
        raise ValueError(f"in_key must be {AES_KEY_SIZE} bytes, got {len(in_key)}")
    iv = construct_iv(data, challenge)
    enc_iv = aes128_ecb_encrypt_block(iv, transfer_key)
    return bytes(a ^ b for a, b in zip(in_key, enc_iv))


def construct_iv_1w_node(node: bytes) -> bytes:
    """Port of construct_iv_1w_node() (proto_crypto.cpp): the 1W key-wrap IV -- the sender's
    3-byte node address repeated to fill 16 bytes (node[i % 3]), naturally landing iv[15] on
    node[0]. Used only by crypt_1w_key(); a 1W broadcast has no per-exchange challenge to bind
    the IV to, so the public, already-plaintext node address is the only per-sender input.
    """
    if len(node) != 3:
        raise ValueError("node must be 3 bytes")
    return bytes(node[i % 3] for i in range(IV_SIZE))


def crypt_1w_key(node: bytes, in_key: bytes, transfer_key: bytes = TRANSFER_KEY) -> bytes:
    """Port of crypt_1w_key() (proto_crypto.cpp): crypt_key()'s 1W sibling -- same
    XOR-with-AES-128-ECB(IV, transfer_key) core, node-derived IV instead of a
    challenge/frame-data-derived one (construct_iv_1w_node()). Self-inverse like crypt_key(): the
    same call wraps a plaintext key for transmission (CMD_ONEWAY_ADD_CONTROLLER, 0x30) or unwraps
    an overheard one.
    """
    if len(in_key) != AES_KEY_SIZE:
        raise ValueError(f"in_key must be {AES_KEY_SIZE} bytes, got {len(in_key)}")
    iv = construct_iv_1w_node(node)
    enc_iv = aes128_ecb_encrypt_block(iv, transfer_key)
    return bytes(a ^ b for a, b in zip(in_key, enc_iv))


def construct_iv_1w_sequence(data: bytes, sequence: int) -> bytes:
    """Port of construct_iv_1w_sequence() (proto_crypto.cpp): shares construct_iv()'s
    checksum/pad core over `data` (bytes 0-7 = span data or 0x55 padding, bytes 8-9 = running
    checksum), but bytes 10-11 are the 2-byte sequence (big-endian) and 12-15 are 0x55 padding,
    in place of construct_iv()'s 6-byte challenge tail -- a 1W frame has no per-exchange
    challenge, only the sequence being transmitted.
    """
    iv = bytearray(IV_SIZE)
    c1 = 0
    c2 = 0
    for i, byte in enumerate(data):
        c1, c2 = compute_checksum(byte, c1, c2)
        if i < 8:
            iv[i] = byte
    for i in range(len(data), 8):
        iv[i] = IV_PADDING
    iv[8] = c1
    iv[9] = c2
    iv[10] = (sequence >> 8) & 0xFF
    iv[11] = sequence & 0xFF
    iv[12] = iv[13] = iv[14] = iv[15] = IV_PADDING
    return bytes(iv)


def create_1w_hmac(data: bytes, sequence: int, controller_key: bytes) -> bytes:
    """Port of create_1w_hmac() (proto_crypto.cpp): create_hmac()'s 1W sibling -- IV from
    construct_iv_1w_sequence(), AES-128-ECB with `controller_key`, truncate to HMAC_SIZE bytes.
    `data` is a command-specific span with NO default; the caller must supply exactly what the
    target command authenticates (see proto_crypto.h's `@warning` -- spans do not generalise
    across commands, e.g. CMD_ONEWAY_ADD_CONTROLLER signs only `cmd + enc_key`, while
    CMD_ONEWAY_REMOVE signs `cmd + data` -- verified against real hardware captures, not merely
    assumed; see tests/corpus/captures/enrollment/somfy_smoove_enrollment_add_and_remove_controller_sx1276.yaml).
    """
    iv = construct_iv_1w_sequence(data, sequence)
    encrypted = aes128_ecb_encrypt_block(iv, controller_key)
    return encrypted[:HMAC_SIZE]


# --- 1W frame slicing (CMD_ONEWAY_ADD_CONTROLLER / CMD_ONEWAY_REMOVE) -----------------------
# Declared-payload sizes mirror proto_codecs.cpp's ONEWAY_ADD_CONTROLLER_PAYLOAD_SIZE and
# proto_commands.cpp's ONEWAY_REMOVE_PAYLOAD_SIZE.
ONEWAY_ADD_CONTROLLER_DECLARED_PAYLOAD_SIZE = AES_KEY_SIZE + 4  # enc_key(16) + man(1) + data(1) + seq(2)
ONEWAY_REMOVE_DECLARED_PAYLOAD_SIZE = 9  # data(1) + seq(2) + mac(6)

OneWayAddControllerParts = namedtuple("OneWayAddControllerParts", ["src", "enc_key", "rest", "sequence", "trailer"])
OneWayRemoveControllerParts = namedtuple("OneWayRemoveControllerParts", ["data", "span", "sequence", "mac"])


def find_oneway_add_controller_frames(frames):
    """Locate CMD_ONEWAY_ADD_CONTROLLER (0x30) frames. No surrounding context needed -- the
    key-wrap and optional MAC trailer live entirely in one frame."""
    return [f for f in frames if _frame_cmd(f) == CMD_ONEWAY_ADD_CONTROLLER]


def find_oneway_remove_controller_frames(frames):
    """Locate CMD_ONEWAY_REMOVE (0x39) frames."""
    return [f for f in frames if _frame_cmd(f) == CMD_ONEWAY_REMOVE]


def oneway_add_controller_parts(frame) -> OneWayAddControllerParts:
    """Slice a CMD_ONEWAY_ADD_CONTROLLER frame into its key-wrap, echoed man/data/sequence bytes,
    and optional out-of-length MAC trailer (IoFrame::has_mac, proto_frame.h).

    `trailer` is `b""` when the frame carries no MAC -- real Somfy hardware sends 29 bytes with no
    trailer, even though the published linklayer.md vector is 35 bytes with a genuine MAC. Both
    shapes are real; a rewriter must handle either.
    `frame.raw()` (via non_crc_bytes()) is expected to be exactly the declared 20 bytes, or that
    plus a 6-byte trailer -- anything else is a malformed capture.
    """
    raw = non_crc_bytes(frame)
    payload = raw[FRAME_MIN_SIZE:]
    if len(payload) < ONEWAY_ADD_CONTROLLER_DECLARED_PAYLOAD_SIZE:
        raise ValueError(f"0x30 payload is {len(payload)} bytes, shorter than the declared "
                         f"{ONEWAY_ADD_CONTROLLER_DECLARED_PAYLOAD_SIZE}")
    src = raw[5:8]
    enc_key = payload[:AES_KEY_SIZE]
    rest = payload[AES_KEY_SIZE:ONEWAY_ADD_CONTROLLER_DECLARED_PAYLOAD_SIZE]  # man(1) + data(1) + seq(2)
    trailer = payload[ONEWAY_ADD_CONTROLLER_DECLARED_PAYLOAD_SIZE:]
    sequence = (rest[2] << 8) | rest[3]
    return OneWayAddControllerParts(src, enc_key, rest, sequence, trailer)


def oneway_remove_controller_parts(frame) -> OneWayRemoveControllerParts:
    """Slice a CMD_ONEWAY_REMOVE frame into the signed span (`cmd + data` -- see
    create_1w_hmac()'s doxygen for why this span and not another), sequence, and MAC.
    """
    raw = non_crc_bytes(frame)
    payload = raw[FRAME_MIN_SIZE:]
    if len(payload) != ONEWAY_REMOVE_DECLARED_PAYLOAD_SIZE:
        raise ValueError(f"0x39 payload is {len(payload)} bytes, expected {ONEWAY_REMOVE_DECLARED_PAYLOAD_SIZE}")
    data_byte = payload[0:1]
    sequence = (payload[1] << 8) | payload[2]
    mac = payload[3:9]
    span = bytes([CMD_ONEWAY_REMOVE]) + data_byte
    return OneWayRemoveControllerParts(data_byte, span, sequence, mac)


# The public corpus key (must byte-for-byte match tests/support/test_helpers.h ::
# TEST_SYSTEM_KEY). Not independently pinned by a cross-language assert — the KAT vectors in
# scripts/corpus/tests/data/crypto_kat.yaml were generated *under this exact key* by the C++
# implementation, so a wrong value here would fail every KAT self-test immediately.
CORPUS_SYSTEM_KEY = bytes([0xDE, 0xCA, 0xFC, 0x0F, 0xFE, 0xE0, 0xFF, 0x1C, 0xEB, 0xAD, 0xBE, 0xEF, 0xF0, 0x0D, 0xBA, 0x11])


def _frame_cmd(frame) -> "int | None":
    raw = frame.raw()
    return raw[8] if len(raw) >= FRAME_MIN_SIZE else None


def find_challenge_response_triples(frames):
    """Locate (origin, challenge, response) triples for 0x3C/0x3D authenticated exchanges among
    a capture's frames, in the same shape corpus_crypto_test.cpp replays: transcript =
    [origin.cmd, *origin.data], authenticated against the 0x3C challenge.

    The roles are defined by direction *relative to the 0x3D*, not by tx/rx absolutely: whoever
    is challenged authenticates their own last frame. The common case is the controller being
    challenged (tx 0x3D — origin is the preceding tx command, e.g. 0x00 EXECUTE, challenged by
    an rx 0x3C), but the protocol is symmetric and the reverse occurs for real: in
    velux_kux100_pairing_full.yaml the *device* answers a controller-issued challenge
    (rx 0x3D over its own preceding rx 0x37 ADDRESS_RESP), confirmed by recomputation against
    that installation's recovered key before the capture was re-keyed. Matching only tx 0x3D
    would leave those HMACs unrewritten by ingest.py --rekey and unchecked by validate.py's
    `key: corpus` enforcement.

    `frames` items need only `.direction` ("tx"/"rx") and `.raw()` (non-CRC bytes) — RawFrame
    satisfies this directly; validate.py wraps its YAML frame dicts the same way.
    """
    triples = []
    for i, frame in enumerate(frames):
        if _frame_cmd(frame) != CMD_CHALLENGE_RESP:
            continue
        responder_dir = frame.direction
        challenger_dir = "rx" if responder_dir == "tx" else "tx"
        challenge = None
        origin = None
        for j in range(i - 1, -1, -1):
            candidate = frames[j]
            cmd = _frame_cmd(candidate)
            if cmd is None:
                continue
            if challenge is None and candidate.direction == challenger_dir and cmd == CMD_CHALLENGE_REQ:
                challenge = candidate
                continue
            if challenge is not None and candidate.direction == responder_dir:
                origin = candidate
                break
        if challenge is not None and origin is not None:
            triples.append((origin, challenge, frame))
    return triples


def find_key_transfer_triples(frames):
    """Locate (key_init_tx, challenge_rx, transfer_tx) triples for 0x31/0x3C/0x32 pairing
    key-exchange sequences — mirrors create_key_transfer()'s IV (proto_commands.cpp: data is
    just the KEY_INIT frame's single cmd byte, not a full transcript).
    """
    triples = []
    for i, frame in enumerate(frames):
        if frame.direction != "tx" or _frame_cmd(frame) != CMD_KEY_TRANSFER:
            continue
        challenge = None
        key_init = None
        for j in range(i - 1, -1, -1):
            candidate = frames[j]
            cmd = _frame_cmd(candidate)
            if cmd is None:
                continue
            if challenge is None and candidate.direction == "rx" and cmd == CMD_CHALLENGE_REQ:
                challenge = candidate
                continue
            if challenge is not None and candidate.direction == "tx" and cmd == CMD_KEY_INIT:
                key_init = candidate
                break
        if challenge is not None and key_init is not None:
            triples.append((key_init, challenge, frame))
    return triples


def non_crc_bytes(frame) -> bytes:
    """A frame's wire bytes with any trailing CRC stripped — the shape every crypto byte offset
    (transcript, HMAC, key-transfer payload) assumes. Every known on-air log format logs bytes
    without a trailing CRC already (frame.crc_present() is false for them — see RawFrame's
    docstring), so this is a no-op today; it exists so a future logging change that *does*
    include the CRC can't silently corrupt a "read to the end of the frame" slice like a HMAC
    transcript.
    """
    raw = frame.raw()
    return raw[:-2] if frame.crc_present() else raw


def find_self_authenticated_frames(frames):
    """Locate frames that carry a whole challenge-response inside one payload — see
    SELF_AUTHENTICATED_COMMANDS. Returns a list of frames; use self_auth_parts() to slice one.

    Unlike the 0x3C/0x3D and 0x31/0x3C/0x32 finders this needs no surrounding context: the
    challenge is in the same payload as the HMAC. That is exactly what makes it easy to miss —
    such a frame looks like an opaque blob, so an un-re-keyed one sails through a corpus review
    while still carrying HMAC bytes derived from a real system key.
    """
    found = []
    for frame in frames:
        cmd = _frame_cmd(frame)
        if cmd not in SELF_AUTHENTICATED_COMMANDS:
            continue
        payload = non_crc_bytes(frame)[FRAME_MIN_SIZE:]
        if len(payload) == 2 * HMAC_SIZE:
            found.append(frame)
    return found


def self_auth_parts(frame) -> "tuple[bytes, bytes, bytes]":
    """Slice a self-authenticated frame into (transcript, challenge, hmac). The transcript is the
    command byte alone — the same single-byte convention create_key_transfer() uses for its IV.
    """
    raw = non_crc_bytes(frame)
    payload = raw[FRAME_MIN_SIZE:]
    return bytes([raw[8]]), payload[:HMAC_SIZE], payload[HMAC_SIZE:2 * HMAC_SIZE]


HmacParts = namedtuple("HmacParts", ["transcript", "challenge", "hmac"])
KeyTransferParts = namedtuple("KeyTransferParts", ["key_init_cmd", "challenge", "encrypted_payload"])


def hmac_parts(origin, challenge, response) -> HmacParts:
    """Slices a (origin, challenge, response) 0x3C/0x3D triple (as found by
    find_challenge_response_triples()) into the exact byte ranges create_challenge_resp()
    (proto_commands.cpp) builds/verifies against — the one place this offset knowledge lives,
    used by both validate.py's crypto enforcement and ingest.py's --rekey pipeline.
    """
    origin_raw = non_crc_bytes(origin)
    challenge_raw = non_crc_bytes(challenge)
    response_raw = non_crc_bytes(response)
    transcript = bytes([origin_raw[8]]) + origin_raw[FRAME_MIN_SIZE:]
    chal_bytes = challenge_raw[FRAME_MIN_SIZE:FRAME_MIN_SIZE + HMAC_SIZE]
    hmac = response_raw[FRAME_MIN_SIZE:FRAME_MIN_SIZE + HMAC_SIZE]
    return HmacParts(transcript, chal_bytes, hmac)


def key_transfer_parts(key_init, challenge, transfer) -> KeyTransferParts:
    """Slices a (key_init, challenge, transfer) 0x31/0x3C/0x32 triple (as found by
    find_key_transfer_triples()) into crypt_key()'s inputs — see hmac_parts() docstring.
    """
    key_init_raw = non_crc_bytes(key_init)
    challenge_raw = non_crc_bytes(challenge)
    transfer_raw = non_crc_bytes(transfer)
    key_init_cmd = key_init_raw[8:9]
    chal_bytes = challenge_raw[FRAME_MIN_SIZE:FRAME_MIN_SIZE + HMAC_SIZE]
    encrypted_payload = transfer_raw[FRAME_MIN_SIZE:FRAME_MIN_SIZE + AES_KEY_SIZE]
    return KeyTransferParts(key_init_cmd, chal_bytes, encrypted_payload)


class RawFrame:
    """One frame extracted from a pasted on-air log, before it becomes a capture YAML entry."""

    def __init__(self, direction: str, hex_bytes: str, freq: int = 0, t_ms: "int | None" = None,
                chip: "str | None" = None, unverified: bool = False, source_line: str = ""):
        self.direction = direction  # "tx" | "rx"
        self.hex_bytes = hex_bytes  # space-separated hex, as captured (no CRC — see crc_present)
        self.freq = freq
        self.t_ms = t_ms
        self.chip = chip
        self.unverified = unverified  # True = fallback-tier extraction, needs human review
        self.source_line = source_line

    def raw(self) -> bytes:
        return parse_hex(self.hex_bytes)

    def crc_present(self) -> bool:
        """CRC-detection rule (verified against RX call sites, see module docstring below):
        every on-air log line this parser ingests — both `io_capture` (tx_frame/parse_ok)
        and legacy `io_frame` (TX/RX) — logs bytes with CTRL0-implied length exactly, i.e.
        **without** the trailing 2-byte CRC. `log_component_capture()` (hub_internal.h) is
        called with the frame's own `buf/len` (post radio_sx1262.cpp's software CRC strip for
        RX; pre-hardware-CRC-append for TX), and `log_frame()` (log_frame.h, used for the
        legacy `io_frame` tag) receives the same already-stripped bytes. So `crc_present()` is
        false whenever the raw length already matches `ctrl0_implied_length()`; it is true only
        if the raw length is exactly 2 bytes longer (a shape no known capture in this repo has
        produced, but kept for forward compatibility with a future logging change).
        """
        raw = self.raw()
        if len(raw) < 1:
            return False
        implied = ctrl0_implied_length(raw[0])
        return len(raw) == implied + 2


# --- io_capture (structured) — hub_internal.h :: log_component_capture() --------------------
# With a decoded frame (cmd/src/dst present — tx_frame always has one; parse_ok always has one):
_IO_CAPTURE_WITH_FRAME_RE = re.compile(
    r"chip=(?P<chip>\S+)\s+phase=component\s+stage=(?P<stage>\S+)\s+freq=(?P<freq>\d+)\s+ts=(?P<ts>\d+)\s+"
    r"len=(?P<len>\d+)\s+cmd=0x(?P<cmd>[0-9A-Fa-f]+)\s+src=(?P<src>[0-9A-Fa-f]{6})\s+dst=(?P<dst>[0-9A-Fa-f]{6})\s+"
    r"payload=(?P<payload>[0-9A-Fa-f ]+)"
)
# Without a decoded frame (parse_fail — frame pointer was nullptr, so no cmd/src/dst fields):
_IO_CAPTURE_NO_FRAME_RE = re.compile(
    r"chip=(?P<chip>\S+)\s+phase=component\s+stage=(?P<stage>\S+)\s+freq=(?P<freq>\d+)\s+ts=(?P<ts>\d+)\s+"
    r"len=(?P<len>\d+)\s+payload=(?P<payload>[0-9A-Fa-f ]+)"
)

# --- io_frame (legacy) — log_frame.h :: log_frame() ------------------------------------------
# The `cmd=... flags` segment is optional: older firmware builds logged this tag without it
# (e.g. "TX [17 bytes] freq=868950000 preamble=1024: <hex>" with
# no `cmd=` field at all), while newer builds always include it.
_IO_FRAME_RE = re.compile(
    r"(?P<prefix>TX|RX)\s+\[(?P<len>\d+)\s+bytes\]\s+freq=(?P<freq>\d+)\s*"
    r"(?:preamble=(?P<preamble>\d+)\s*)?(?:cmd=(?P<cmd_name>\S+)\s*(?P<flags>(?:\[[A-Z]+\])*)\s*)?:\s*"
    r"(?P<hex>[0-9A-Fa-f]{2}(?:\s+[0-9A-Fa-f]{2})+)"
)

# ESPHome wall-clock line prefix, e.g. "[08:56:10.754][D][io_capture:190]: ..."
_TIMESTAMP_PREFIX_RE = re.compile(r"^\[(?P<h>\d{2}):(?P<m>\d{2}):(?P<s>\d{2})\.(?P<ms>\d{3})\]")

# Fallback tier: any line with a plausible run of >= FRAME_MIN_SIZE space-separated hex byte
# pairs, for logs that don't match either known tag format (mangled pastes are common in issue
# threads). Deliberately liberal; callers must treat `unverified=True` results as needing a
# human to confirm the extraction before the capture is committed.
_HEX_RUN_RE = re.compile(r"(?:\b[0-9A-Fa-f]{2}\b(?:\s+))" r"{" + str(FRAME_MIN_SIZE - 1) + r",}\b[0-9A-Fa-f]{2}\b")


def _io_capture_metadata_agrees(match: "re.Match") -> bool:
    """Cross-checks an `_IO_CAPTURE_WITH_FRAME_RE` match's cmd=/src=/dst=/len= fields against the
    parsed `payload=` bytes: the structured tag logs both independently (the
    real parser's decoded fields, and the raw wire bytes it decoded them from), so a mangled
    paste whose payload disagrees with its own logged cmd/src/dst is a corruption signal that
    CTRL0/CRC self-consistency alone would not catch — a self-consistent but wrongly-delimited
    payload still passes those checks.
    """
    payload = parse_hex(match.group("payload"))
    if len(payload) != int(match.group("len")):
        return False
    if len(payload) <= 8:
        return False
    if payload[8] != int(match.group("cmd"), 16):
        return False
    if payload[2:5] != bytes.fromhex(match.group("dst")):
        return False
    if payload[5:8] != bytes.fromhex(match.group("src")):
        return False
    return True


def _wall_clock_ms(line: str) -> "int | None":
    m = _TIMESTAMP_PREFIX_RE.match(line)
    if not m:
        return None
    h, mnt, s, ms = (int(m.group(g)) for g in ("h", "m", "s", "ms"))
    return ((h * 60 + mnt) * 60 + s) * 1000 + ms


def parse_log_line(line: str) -> "RawFrame | None":
    """Extract at most one RawFrame from a single log line, trying tiers in order:
    io_capture (structured, preferred) -> io_frame (legacy) -> freeform hex-run fallback.
    Returns None if the line carries no recognizable frame.

    `t_ms` always comes from the ESPHome `[HH:MM:SS.mmm]` wall-clock prefix that every log line
    carries, never from io_capture's `ts=` field — that field is the device's monotonic uptime
    counter (a different, unrelated time base per boot), so mixing it with wall-clock-derived
    timestamps from `io_frame`/fallback lines would produce meaningless relative offsets.
    """
    ts_ms = _wall_clock_ms(line)

    match = _IO_CAPTURE_WITH_FRAME_RE.search(line)
    if match:
        stage = match.group("stage")
        direction = "tx" if stage == "tx_frame" else "rx"
        # A metadata/payload disagreement routes the frame into the unverified (human-review)
        # tier rather than dropping it — see _io_capture_metadata_agrees for why.
        agrees = _io_capture_metadata_agrees(match)
        return RawFrame(direction, match.group("payload").strip(), freq=int(match.group("freq")), t_ms=ts_ms,
                        chip=match.group("chip"), unverified=not agrees, source_line=line)

    match = _IO_CAPTURE_NO_FRAME_RE.search(line)
    if match:
        # stage=parse_fail (no cmd/src/dst) — a frame the real parser rejected. Extracted for
        # completeness/statistics, but flagged unverified: it will not round-trip through
        # parse()/serialize() and should not be selected into a committed capture.
        return RawFrame("rx", match.group("payload").strip(), freq=int(match.group("freq")), t_ms=ts_ms,
                        chip=match.group("chip"), unverified=True, source_line=line)

    match = _IO_FRAME_RE.search(line)
    if match:
        direction = "tx" if match.group("prefix") == "TX" else "rx"
        return RawFrame(direction, match.group("hex").strip(), freq=int(match.group("freq")), t_ms=ts_ms, chip=None,
                        unverified=False, source_line=line)

    match = _HEX_RUN_RE.search(line)
    if match:
        return RawFrame("rx", match.group(0).strip(), freq=0, t_ms=ts_ms, chip=None, unverified=True,
                        source_line=line)

    return None


def _same_physical_frame(a: RawFrame, b: RawFrame) -> bool:
    """True when two adjacently-extracted RawFrames are the *same* on-air event logged twice —
    every physical TX/RX in this codebase is logged once via `io_capture` (hub_internal.h) and
    once via the legacy `io_frame` tag (log_frame.h), back-to-back within a few ms of each other.
    Same direction + identical bytes, adjacent in parse order, is
    the signal; genuine retransmissions (e.g. three DISCOVER_REQ retries) are NOT adjacent to
    each other in this sense because caller code always logs the io_capture/io_frame pair
    together before the next real event.

    That dual-logging invariant only holds when `io_capture` is actually emitted for the event.
    On an io_capture-disabled log (io_frame-only — e.g. a monitor config without the structured
    tag), every physical event is logged exactly once, so two adjacent identical-bytes-same-
    direction io_frame lines are never a duplicate-tag artifact — they are two genuinely
    distinct events (most commonly a retry retransmitting the same request payload byte-for-
    byte). `chip` is only ever populated by the io_capture side (`_IO_FRAME_RE`'s branch always
    sets it None), so requiring at least one side to carry it distinguishes a real io_capture/
    io_frame pair from two bare io_frame lines that merely happen to match.
    """
    if a.direction != b.direction:
        return False
    if "".join(a.hex_bytes.split()).upper() != "".join(b.hex_bytes.split()).upper():
        return False
    return a.chip is not None or b.chip is not None


def _merge_frames(a: RawFrame, b: RawFrame) -> RawFrame:
    """Merge a same-physical-frame pair, keeping the best field from either side.

    Observed firmware quirk (a retried DISCOVER_REQ): the io_capture
    tx_frame log entry on a retry carries freq=0 ts=0 (no capture context available at that call
    site), while the io_frame entry for the exact same transmission carries the real freq. Take
    whichever side has a non-zero freq/t_ms; prefer `a` when both are informative.
    """
    freq = a.freq if a.freq else b.freq
    if a.t_ms not in (None, 0):
        t_ms = a.t_ms
    elif b.t_ms not in (None, 0):
        t_ms = b.t_ms
    else:
        t_ms = a.t_ms if a.t_ms is not None else b.t_ms
    chip = a.chip or b.chip
    unverified = a.unverified and b.unverified
    hex_bytes = b.hex_bytes if a.unverified and not b.unverified else a.hex_bytes
    return RawFrame(a.direction, hex_bytes, freq=freq, t_ms=t_ms, chip=chip, unverified=unverified,
                    source_line=a.source_line + " | " + b.source_line)


def parse_log(text: str) -> "list[RawFrame]":
    """Extract every recognizable frame from a multi-line pasted log, in file order, merging
    adjacent io_capture/io_frame duplicates of the same physical frame (see _same_physical_frame).
    """
    frames: "list[RawFrame]" = []
    for line in text.splitlines():
        frame = parse_log_line(line)
        if frame is None:
            continue
        if frames and _same_physical_frame(frames[-1], frame):
            frames[-1] = _merge_frames(frames[-1], frame)
            continue
        frames.append(frame)
    return frames
