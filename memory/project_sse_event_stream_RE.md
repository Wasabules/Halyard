---
name: project-sse-event-stream-RE
description: TIER 8 U1 — SSE event stream RE exhaustif. Aucun trigger taskbar — 1 event session-wide.
metadata:
  type: project
---

# TIER 8 U1 — SSE event stream deep parsing

**Date** : 2026-05-23
**Capture** : `06-shadow-recon-linux/captures/MASTER-20260514-153936/tls_plain.log` (1M lines, 235s session)
**Outil** : `tools/ida/tier8_sse_extract.py`

## TL;DR

**C95: the SSE is silent during the session**. The whole session = **1 SSE event
runtime** (= `shadow-manager.encoding_is_ready` at bootstrap). No event
gates multi-NAL, nor affects the stream. The "a cross-channel SSE→ctrl trigger" hypothesis is **REFUTED**.

## Taxonomie SSE binaire (= 7 types + 9 sub-events)

`ShadowPCDisplay` rodata parse :

**SSE data types** (level 1) :
- `event` → `handleSSEDataTypeEvent` (strings 32821)
- `forward` → `handleSSEDataTypeForward` (strings 32826)
- `error` → `handleSSEDataTypeError` (strings 32804)
- `session` → body parser
- `main` → main JWT subscription

**SSE event sub-types** (level 2, dispatched from `handleSSEDataTypeEvent`) :
- `l2tp` → `handleSSEDataEventL2TP`
- `vm-reachable` → `handleSSEDataEventVmReachable`
- `status-changed` → `handleSSEDataEventStatusChanged`
- `bsod` → `handleSSEDataEventBsod`
- `shadow-manager` → `handleSSEDataEventShadowManager`
- `shadow-manager.display-is-ready` → `handleSSEDataEventShadowManagerDisplayIsReady`
- `shadow-manager.install-status` → `handleSSEDataEventShadowManagerInstallStatus`
- `get-out` → `handleSSEDataEventGetOut`

Connect via `VmProxyHttpChannel::connectStream` (= mangled `*ZN18VmProxyHttpChannel13connectStream...`).
The `text/event-stream` type confirmed (strings 34235).

## Capture analysis

```
Total blocks parsed   : 8116 SSL_READ/WRITE
SSE-bearing blocks    : 15
Distinct peers        : 3
JSON type distribution :
  _NOTYPE  x13   (h2 framing / sentry / partial)
  _NON_JSON x9   (JWT-only)
  event    x2    🎯
  session  x1    ({"type":"session","length":245} = HTTP session info)
  main     x1    ({"type":"main","opaque":"<JSON arch/os/version>"})
```

**The only SSE event detected**:
```json
L25078 peer=<VM public IPv6>:
{"type": "event",
 "event": "shadow-manager",
 "data": {"target": "VMP",
          "sender": "ShadowManager",
          "type": "encoding_is_ready",
          "value": {"encoding_type": "hardware", "is_banner": false}}}
```

= notification one-shot bootstrap "VM encoder NVDEC ready".

## The DUAL SSE confirmed

The desktop opens **2 SSE streams** (= launcher_jwt + main_jwt), consistent with
`project_native_bootstrap_VALIDATED.md`. Our client does the same.

## Cross-channel triggers ?

Hypothesis: an SSE event → a ctrl-msg on `:base+11`. Checking around
L25078 → no correlated SSL_WRITE on ssl `0x2a3d75c0`. **No cross-channel
trigger observed**.

## Verdict

**C95: the SSE is not a multi-NAL / taskbar trigger**. Our client reads /
correctly ignores the events on the parse side. No code to write.

## Refs

- Strings : `06-shadow-recon-linux/dumps/strings_display.txt:32564-32894`
- Vmproxy dump : `06-shadow-recon-linux/dumps/ghidra_vmproxy.txt`
- Capture event @L25078
- Doc : `tools/ida/out/TIER8_RE_2026-05-16.md` §U1
