---
name: project-gamepad-native-base13
description: "🏆 DECISIVE 2026-08-21 — The gamepad goes through :base+13 over UDP chacha20 (238/238 decrypted), 14 B payloads. No shadowusb, no extra channel: the Joy-Cons route on Switch is open again."
metadata:
  type: project
---

Guided capture `captures_gamepad_20260821_183535` (a Bluetooth DualShock 4,
777 timestamped evdev events).

**The gamepad goes through `:base+13`**, over UDP chacha20-poly1305
(`[ct 14][nonce 12][tag 16]`, with the uplink key = the one from OUR `Encryption`
request, cf. [[project-shadow-dual-key-crypto]]). It is the same channel as the
keepalive — which was therefore only one of its messages.

**It does NOT go through the `:base+12` input channel**: throughout the session,
the 218 messages of 144 B there carry the mouse-movement signature. And **no
additional connection** is opened.

**No `shadowusb` daemon**: the DualShock 4 is recognised as soon as the stream
opens and forwarded natively. The Spice/usbredir lead had been abandoned; it is no
longer needed. **The Joy-Cons route on Switch is open again.**

A 14-byte payload, `04 [b1] [type] …`:

| type | meaning | encoding |
|---|---|---|
| `00` | button | `@11` = the id, `@13` = 1/0. Cross → `@11=02`, Circle → `@11=03` |
| `01` | axis | `@11` = the axis index (00–04), `@12`/`@13` = the value |
| `02` | d-pad | the value at `@3` |
| `05` | keepalive | a zeroed body, ~7 s |

The very first message of the session is `04 01 03 02 …` = the **plug
announcement**, the native equivalent of the web path's
`GamepadPluggedInputV4Model`.

**Axes fully decoded** (capture `captures_gamepad-axes_20260821_192400`, with a
tool providing a visual target and automatic calibration):

| `@11` | axis | | `@11` | axis |
|---|---|---|---|---|
| 0 | RX | | 3 | RY |
| 1 | LY | | 4 | L2 |
| 2 | LX | | 5 | R2 |

All in the direct sense. `@12` = the value on 0-255. And `@13` is not a checksum:
`@13 − @12` is constant per axis and equals 128 — the two bytes carry the **same
value in two representations**, one centred on zero, the other on 128.

**Design trap**: holding a stick still produces no message, the client only emits
on change. It is the movements that carry the information — correlate over the
time series, not over the holds.

The implementation is within reach: one more UDP socket, the encryption already in
place, 14-byte payloads. No FlatBuffers.

`KB.md` §3.25.
