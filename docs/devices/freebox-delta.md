# Freebox Delta
<!-- doxygen-label: dev_freebox_delta -->

The Freebox Delta is an internet box from Free with a built-in IO-Homecontrol home-automation hub.
It works as a key source: the hub obtains the installation's key from it, and the devices stay
shared between Home Assistant and the Freebox app. One installation is confirmed on an SX1276
board in issue #215.

## What works

| Capability | Status |
|---|---|
| Obtaining the installation key | ✅ Confirmed, with Discover & Pair and `pairing_key_transfer: pull` |
| Open, close, stop and status of the shutters behind it | ✅ Confirmed, alongside the Freebox |
| Open and close of a Somfy gate behind it | ✅ Confirmed |
| A move started from the Freebox app | ✅ Shows up in Home Assistant at the end of travel |
| Key extraction with the Recover System Key switch | ❌ The Freebox sends no key that way |

## Onboarding route that worked

This is Discover & Pair, not key extraction, and the Recover System Key switch stays off.

1. Set [`pairing_key_transfer`](../configuration/tuning.md#pairing_key_transfer) to `pull`:

```yaml
home_io_control:
  # ...
  tuning:
    pairing_key_transfer: pull
```

2. Open "Share your network" ("Partager votre réseau") in the Freebox app.
3. Press Discover & Pair.

The Freebox hands over its key but never answers the hub's challenge. The devices that hold the
key prove it instead by answering a roll-call, and the hub prints the key. Put it into your YAML as
`system_key`, reflash, and press [Scan Paired Devices](../scan-paired-devices.md) for the block of
each device.

## Known quirks

- **Arming key extraction does not work.** With the Recover System Key switch armed, the Freebox
  confirms the pretend device only with `key_extraction_multi_info: 0xDC`, and sends no key even
  then; see
  [key-extraction tuning](../configuration/tuning.md#key_extraction_type_word-and-key_extraction_multi_info).

## Evidence

Issue #215, on an SX1276 board: seven of the reporter's devices answered the roll-call under the
pulled key. Open, close, stop and status of the shutters work alongside the Freebox, and so do
open and close of a Somfy gate.

## See also

- [Pairing](../pairing.md)
- [Key extraction](../key-extraction.md) — the route for the other vendor hubs
