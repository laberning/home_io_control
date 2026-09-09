# FAQ
<!-- doxygen-label: faq -->

## Do I need a Somfy or VELUX hub for this to work?

No. This component *is* the hub — it talks to your devices directly over the radio. You only need
an existing hub if your devices are already paired to one and you want to keep them that way, in
which case [key extraction](key-extraction.md) borrows its key rather than replacing it.

## Will it work with my device?

Probably, if it is an actuator from an IO-Homecontrol alliance member. The protocol is
shared, so an unlisted device has a good chance of working with `io_device_type` set to
the closest match. [Supported devices](supported-devices.md) lists everything anyone has reported.

## Which board should I buy?

A Heltec WiFi LoRa 32 (V3). It is cheap, easy to find, has an OLED for status, and is the board
these configs are exercised on most. [Hardware](hardware.md) covers the alternatives.

## Discover & Pair finds nothing. What now?

Almost always the device already holds another hub's key, and a device in that state cannot answer
a discovery at all. Read [The device is never found](troubleshooting.md#the-device-is-never-found)
before changing any radio settings.

## Do I have to reset my devices to use this?

No, and you generally should not. If you own the hub that currently controls them,
[key extraction](key-extraction.md) recovers its key with every device left paired exactly as it
is.

## Why does my cover show no position when it boots?

Because nothing has told it one yet. The protocol has no command that reads a position from cold,
so the entity has no percentage until the device reports one — after the first move, or the first
status poll. See [Position is unknown or state is stale](troubleshooting.md#position-is-unknown-or-state-is-stale).

## Can it control a device that only works with a remote?

Yes, one-way. The hub can transmit 1W commands, but it has to be enrolled into the device first,
and 1W has no reply frames so nothing is confirmed. See
[Sending 1W commands](configuration/oneway-transmit.md).

## My wall remote moves the device but Home Assistant does not notice.

Add the remote to that entity's `linked_remotes:`. The hub then overhears the press and polls the
device for its new position. See [Linked remotes](configuration/remotes.md#linked-remotes).

## Is my system key safe in the frame logs?

Yes. Key-transfer payloads are always masked in frame logs (`[N bytes masked]`), and turning frame
logging on does not change that. The one place the key appears in clear text is the ready-to-paste
YAML block a successful key extraction prints. Leave that block out when you attach logs to an
issue — see [Contributing](contributing.md#reporting-unsupported-devices).

## How do I report a device that does not work?

Open an issue with the board, radio, YAML, and the log around the attempt. A failure report is as
useful as a success — several entries in the device matrix started as failures. The full checklist
is in [Contributing](contributing.md#reporting-unsupported-devices).
