---
name: Shadow USB-over-WSS protocol
description: The USB tunnel (for arbitrary peripherals) uses WSS+Spice+usbredir. CAREFUL: this does NOT concern the gamepad, which has a native path on :base+13.
type: project
---
> **CORRECTION 2026-08-26.** This note describes GENERIC USB transfer
> ("Use my USB devices on my Shadow", which requires the `shadowusb` package). It
> does **NOT describe the gamepad**: "Controllers" and "USB" are two distinct
> functions in the Shadow settings, and the gamepad has a NATIVE path on
> `:base+13`, whose format is decoded (KB §3.36). A guided capture with a
> A DualShock 4 showed no `/devices`, no Spice, no trace of the
> peripheral: ×50 the traffic on `:base+13`, and nothing else.
> Voir [[project_channel_map_proven]].

# The Shadow USB tunnel: the stack confirmed

RE du daemon `ShadowUSB` (Mac `ShadowUSB.pkg`, version 4.4.5, x86_64 Mach-O).
Key strings: `SslWebsocketSpiceChannel<SpiceMainChannel<SslWebsocketClientChannel>>`,
`SpiceUsbRedirChannel`, `Sec-WebSocket-Version: 13`, `REDQ` magic, `SPICE_LINK_*`.

## Endpoint REST `/N/devices`

- **POST** `https://<vm-host>/<instance>/devices`
- **Auth** : `Authorization: Bearer <usb_token>` (= `tokens.usb` du proximus-credentials)
- **Body** : `{"protocol":"spice","type":"spice-html5"}`
  - NOT `{"protocol":"XHCI"}` (our initial mistake, which still returned a fake `port`)
- **Response 201** : `{"data":{"id":"<uuid>","spice_url":"wss://...","spice_secret":"<ticket>"}}`

## The tunnel's stack (after /devices)

```
TCP+TLS (port 443, SNI vm-host)
  └── WebSocket (Sec-WebSocket-Version: 13, Upgrade negotiation HTTP)
       └── SpiceLink handshake (magic "REDQ" + capabilities + auth ticket=spice_secret)
            └── SpiceMainChannel
                 └── SpiceUsbRedirChannel (sub-channel via SPICE main)
                      └── usbredir frames (HELLO, DEVICE_CONNECT, INTERRUPT_PACKET)
```

The `--ws` in the LaunchDaemon plist confirms that WebSocket mode is enabled.

## Why

- Our `shadowusb_register_device` sent `{"protocol":"XHCI"}` and the server
  apparently returned a `port` (e.g. 15104) that was UNREACHABLE over direct TCP.
  A misinterpretation: those ports are not external.
- The real fields are `spice_url` (a WSS URL ready to connect to) and `spice_secret` (auth).
- Implementing a complete SPICE client is required (several hundred lines).
  The Mac libs: `libusbredirhost.dylib`, `libusbredirparser.dylib`, with msquic present
  but it is not obvious that it is used (perhaps for an HTTP/3 fallback).

## How to apply

- `shadowusb_register_device` doit parser `id` + `spice_url` + `spice_secret`.
- For the tunnel: implement in this order:
  1. A WebSocket client (an HTTP/1.1 Upgrade + client-side frame masking)
  2. SpiceLink magic + cap negotiation + auth via spice_secret
  3. SpiceMainChannel ack + open SpiceUsbRedirChannel
  4. usbredir frames standard (HELLO/DEVICE_CONNECT/INTERRUPT)
- Daemon Linux : `apt install shadowusb` (paquet shadow.tech repo bullseye/main),
  installed under /usr/lib/shadowusb/ — useful for traces if a binary diff is ever needed.
