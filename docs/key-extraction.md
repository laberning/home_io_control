# Key extraction
<!-- doxygen-label: key_extraction -->

**Key extraction is the route to take when you already own a working IO-Homecontrol hub** — a
Somfy TaHoma, Connexoon or Connectivity Kit, a VELUX KLF200, KLR200 or KIG300, and similar. The
component emulates an unpaired device so that hub pairs *to it* and hands over its
`node_id`/`system_key`. Your devices never leave their paired state, and once you hold the key,
[Scan Paired Devices](pairing.md#scan-paired-devices) lists every one of them at once.

It is also the answer when Discover & Pair finds nothing. A device that already holds a hub's key
has nothing left to respond to a discovery with, so tuning discovery parameters cannot help.

> **⚠️ Use only on a hub and network you own or are authorized to modify** — see the project
> [Disclaimer](../README.md#disclaimer--license).

## How it works

Moving an existing installation to this component means obtaining that installation's key. Without
this feature you would have to reset a device and sniff its re-pairing with a separate radio. With
it, the ESP32 poses as a new shutter: your existing hub runs its ordinary "add a device" wizard,
pairs to the ESP32 exactly as it would to a real actuator, and in doing so transmits its `node_id`
and `system_key`. The hub prints both as a ready-to-paste YAML block.

```yaml
home_io_control:
  # ... radio pins, node_id, system_key as usual ...
  accept_foreign_pairing: true
```

- `accept_foreign_pairing` (Optional, boolean, default `false`): When `true`, creates the
  **"Recover System Key"** switch entity on the hub.

The switch always boots off (`restore_mode: ALWAYS_OFF`), so a reboot can never leave it armed.

## Workflow

1. Flash with `accept_foreign_pairing: true` in your `home_io_control:` block.
2. Turn the **Recover System Key** switch on in Home Assistant. The hub arms for **10 minutes**
   and logs the throwaway node ID it will advertise.
3. Put your **existing** hub into its own "add device" mode, the same way you would pair a new
   shutter to it. On a Somfy TaHoma or Connectivity Kit the wizard's choice of control point
   matters: a "Smoove"-type control point makes the hub send a real discovery broadcast at once,
   while "remote control" makes it wait to *receive* a key instead, which this feature cannot
   answer. If the switch stays armed with nothing in the log, cancel and re-add the product with a
   different control-point choice before concluding the extraction failed (issue #27).
4. Watch the ESPHome log. Within a few seconds of the hub's discovery you get a clearly delimited
   block with your installation's real `node_id` and `system_key`.
5. Leave the switch alone until it turns itself off. Some hubs (the VELUX KLR200, confirmed) follow
   the key exchange with a verification round for the address they handed out, so the switch keeps
   listening for up to one more minute after the key is printed. A hub that sends nothing further
   leaves that minute unused.
6. If nothing happens within 10 minutes, the switch turns itself off and the log says whether any
   pairing attempt was seen at all and, if a partial one was, which phase it reached.
7. Copy the printed `node_id`/`system_key` into your YAML, replacing the values you made up, and
   reflash.
8. Press **Scan Paired Devices**, or call the `scan_paired_devices` action — see
   [Pairing](pairing.md#scan-paired-devices). Every device that trusts the recovered key answers
   with a ready-to-paste snippet, so nothing has to be paired.

## Known limitations

- **The recovered key is not verified for you.** Nothing reads it back, so control a device with
  it before you rely on it. If it turns out to be wrong, please report it: that points at an
  assumption in the decoding that needs fixing.
- **Expect the occasional retry.** The exchange sometimes needs a second attempt at the key step,
  and on SX1262 boards a slower transmit-to-receive turnaround makes that a little more likely.
  Give it a few more seconds before deciding it has failed. If the whole attempt times out, start
  it again; a repeat from the same hub works without toggling the switch off and on.
- **Your existing hub now lists a device that will never answer it.** After a successful run the
  hub believes it added a new actuator and may show one in its app. Nothing here can remove that
  entry from the hub's side; delete it through the hub's own app when you are done.

## See also

- [Pairing](pairing.md) — Discover & Pair, and Scan Paired Devices once you hold the key
- [Supported devices](supported-devices.md) — which hubs are confirmed as extraction sources
