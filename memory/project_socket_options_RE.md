---
name: project_socket_options_RE
description: TIER10-Y2 — the setsockopt inventory is complete. No IP_TOS/IPV6_TCLASS/DSCP. Just libuv's defaults (SO_KEEPALIVE + TCP_KEEPIDLE=60 + TCP_NODELAY + IPV6_V6ONLY) + reuse_addr.
metadata:
  type: project
  confidence: C90
  date: 2026-05-23
  tier: 10
---

# TIER 10 Y2 — UDP/TCP socket options inventory

## TL;DR
**33 `setsockopt@plt` sites** in `ShadowPCDisplay`. They all correspond to
**libuv-internal paths** (= repetitive UDP, TCP and IPv6 init chunks). The only
Shadow-specific one = `SetupKeepAlive(sock, edx=initial?, 60)`, which calls
`uv_tcp_keepalive(sock, 1, 60)`. **NO `IP_TOS` / `IPV6_TCLASS` / DSCP**.
**NO Shadow-specific `SO_PRIORITY` on the video sockets**.
The "the server routes on the DSCP class" hypothesis is **REFUTED at C90**.

## Inventaire complet setsockopt calls

| EA | level | optname | optlen | Decoded |
|----|-------|---------|--------|---------|
| 0xa500a2 | 0 (IP) | 2 | 4 | IP_INCLUDE_HDR ou similaire (libuv init) |
| 0xa743d1 | 1 (SOL) | dyn | 4 | SetSocketOption generic |
| 0xa7b660 | 1 (SOL) | 2 | 4 | SO_REUSEADDR =1 |
| 0xa7b72b | 41 (IPV6) | 0x1a=26 | 4 | IPV6_V6ONLY |
| 0xa7ba98 | 1 (SOL) | 0xd=13 | 8 | SO_PRIORITY (or libuv buf) |
| 0xa7bc81 | 6 (TCP) | 1 | 4 | **TCP_NODELAY** (uv_tcp_nodelay) |
| 0xa7bcd8 | 1 (SOL) | 9 | 4 | **SO_KEEPALIVE** =1 |
| 0xa7bd2f | 6 (TCP) | 4 | 4 | **TCP_KEEPIDLE** =1 |
| 0xa7bd5f | 6 (TCP) | 5 | 4 | **TCP_KEEPINTVL** =1 |
| 0xa7bd7f | 6 (TCP) | 6 | 4 | **TCP_KEEPCNT** =10 |
| 0xa7be21 | 6 (TCP) | 1 | 4 | TCP_NODELAY (IPv6 variant) |
| 0xa7deee | 41 (IPV6) | 0x1a=26 | 4 | IPV6_V6ONLY |
| 0xa7dfc1 | 41 (IPV6) | 0x19=25 | 4 | IPV6_MULTICAST_LOOP |
| 0xa7e010 | 1 (SOL) | 2 | 4 | SO_REUSEADDR |
| 0xa7e095 | n/a | 0xb | 4 | UDP option (libuv) |
| 0xa7e737 | 1 (SOL) | 2 | 4 | SO_REUSEADDR (UDP) |
| 0xa7e93d | 41 (IPV6) | 0x14=20 | 20 | IPV6_JOIN_GROUP |
| 0xa7e978 | 0 (IP) | 0x23=35 | 8 | IP_MTU_DISCOVER |
| 0xa7eb57+ | … | … | … | libuv interface enum / multicast / etc. |
| 0xdb91cf+ | … | … | … | second libuv build (HTTP/curl thread) |

(33 sites enumerated in total; the details beyond the table = pure libuv UDP/IPv6
boilerplate that is not Shadow-specific.)

## SetupKeepAlive (= the only Shadow-specific code)

```
a7ab70:  mov  $0x3c,%edx       ; idle_timeout_sec = 60
a7ab75:  mov  $0x1,%esi        ; on = 1
a7ab7c:  call a7bca0           ; SetupKeepAlive(sock, 1, 60)
```

→ Under the hood (a7bca0..a7bda0):
- `setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &1, 4)`
- `setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &edx_caller, 4)` — = 60s
- `setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &1, 4)`
- `setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &10, 4)`

C'est **`uv_tcp_keepalive(handle, 1, 60)`** + custom KEEPINTVL=1s + KEEPCNT=10.
Configuration libuv standard pour 1-2 minutes connection death detection.

Strings matchent : `setsockopt(SO_KEEPALIVE/TCP_KEEPIDLE/TCP_KEEPCNT/TCP_KEEPINTVL/TCP_USER_TIMEOUT/TCP_RXT_CONNDROPTIME) failed` (= EA 34097-34101).

## ABSENT (= critique cross-check)

Recherche exhaustive sur :
- **`IP_TOS` (opt=1, level=0)** : strings ZERO, calls ZERO
- **`IPV6_TCLASS` (opt=67=0x43, level=41)** : strings ZERO, calls ZERO
- **A Shadow-specific `SO_PRIORITY` on the video socket**: the only site = libuv (opt=13, optlen=8 → suspicious, more likely an SO_SNDBUF lookalike)
- **`SO_BROADCAST`** : ZERO
- **`TCP_CORK` (opt=3)** : ZERO
- **`SO_DSCP`** : ZERO
- **DSCP class manual via `IP_TOS`** : ZERO

## Cross-ref Switch implementation

Notre `streaming/ctrl_tcp.c` + `streaming/udp_register.c` :
- does **NOT** set `SO_KEEPALIVE` / `TCP_KEEPIDLE` (= a gap compared with the desktop!)
- does **NOT** set `TCP_NODELAY`
- set seulement default socket options libnx

**On the wire**: none of these options is visible on the wire (= a DSCP byte of 0
by default). So the server **cannot** distinguish the desktop from the Switch
via socket options.

**On connection health**: the desktop has a 60 s KEEPALIVE → so if the network dies during
the server resets the connection at 60 s. A Switch without KEEPALIVE → the connection can linger
a silent TCP RST. **NOT a taskbar trigger but stability hygiene.**

## Verdict Y2
**REFUTED at C90**: no indication that the server distinguishes clients by
DSCP/TOS/priority class. The only setsockopt worth porting = `TCP_KEEPALIVE`
for long-run resilience, but that is cosmetic, not a taskbar fix.

## Refs Y2
| Artefact | Path |
|----------|------|
| Strings setsockopt errors | `dumps/strings_display.txt:34097-34106` |
| SetupKeepAlive func | `ShadowPCDisplay@0xa7bca0` |
| TCP_KEEPALIVE callers | `ShadowPCDisplay@0xa7ab70 + 0xa7be90` |
| KEEPIDLE value (60s) | `0xa7ab70: mov $0x3c,%edx` |
| KEEPCNT value (10) | `0xa7bd27: movl $0xa,0x14(%rsp)` |
| Switch impl manquant | `05-shadow-client-borealis/demo/src/streaming/ctrl_tcp.c` |
