---
name: project-network-interface-RE
description: TIER9-W3 — the Shadow desktop has getifaddrs + udev_netlink but the result is consumed locally only, for socket binding. ZERO eth0/wlan0/MTU/link_speed is sent on the wire. A C95 refutation.
metadata:
  type: project
---

# TIER 9 W3 — Network interface enumeration + link details RE

## Verdict C95 — interface enum local-only, jamais wire

The Shadow desktop has the `getifaddrs` **primitive** + the udev/netlink **monitor**,
but no **interface type / MTU / link speed** information is
sent to any Shadow server endpoint.

## Strings binary

| Line | String | Category |
|------|--------|----------|
| 1266 | `getifaddrs` | **libc interface enum** |
| 37253 | `udev_monitor_new_from_netlink` | **udev hot-plug** |
| 37273 | `udev_monitor_new_from_netlink() failed` | error path |

**0 string** : `eth0`, `wlan0`, `enp*`, `ens*`, `if_mtu`, `link_speed`,
`ethtool` and `SIOCGIF*` found. The binary has the `getifaddrs` primitive (=
it returns a linked list of interfaces with their IPs + flags), but no string
shows a **specific use** of the interface name / MTU / link speed for
signaling wire.

The word `wireless` (53 occurrences) matches exclusively SDL2 gamepad names
(`Xbox 360 Wireless Controller`, `HORI Wireless Switch Pad`,
`Thrustmaster wireless 3-1`, …) — **NOT a network signal**.

## Capture analysis

`grep -n "eth0\|wlan0\|enp\|ens\|getifaddrs\|wireless\|link_speed" tls_plain.log`
→ **0 matches** across 1M lines of the MASTER capture, outside the gamepad context.

`grep -n "interface\|MTU\|mtu"` → 1 match at L1000538, but it is a **false positive**
(= a chacha20-encrypted UDP video body sequence that happens to contain
the bytes `0x6d 0x74 0x75` = "mtu" inside the ciphertext).

## A hypothesis on what calls `getifaddrs` — likely DTLS source-IP binding

`getifaddrs` is **probably** used to:
1. List the local IPs for DTLS socket binding / WebRTC ICE candidates
2. Choose the default route (= eth0 against wlan0) for outbound binding

It is **localhost-side** machinery, never signalled to the server.

The `udev_monitor_new_from_netlink` is probably there to detect hot-plugging
USB (= a gamepad / audio device being plugged or unplugged), not for the network.

## Verdict W3

**C95**: No **interface type / MTU / link speed** information is
sent to any Shadow server endpoint. The binary uses `getifaddrs`
for local socket binding but does not publish the result.

Our Switch client has no protobuf field to emit for W3.

**The "the server distinguishes a wired from a wireless client and adjusts the encoder"
REFUTED at C95** — there is no channel at all for transmitting that information.

## Refs

- Strings getifaddrs / netlink : `06-shadow-recon-linux/dumps/strings_display.txt:1266, 37253, 37273`
- Capture verification : `tls_plain.log` (= 0 matches sur interface keywords hors gamepad)
- TIER 9 doc : `tools/ida/out/TIER9_RE_2026-05-16.md` §W3
