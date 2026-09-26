---
name: project-4byte-server-pushes-RE
description: TIER 8 U3 — 4B server pushes ctrl :8011 = 100% wire framing header, pas standalone msg.
metadata:
  type: project
---

# TIER 8 U3 — 4-byte server pushes deep analysis

**Date** : 2026-05-23
**Capture** : `06-shadow-recon-linux/captures/MASTER-20260514-153936/tls_plain.log`
**Outil** : `tools/ida/tier8_4byte_pushes.py`

## TL;DR

**C95: the 500 4 B SSL_READs on ctrl :8011 are 100 % of the wire framing
header `[u16_be type=0x0001][u16_be len=NN]`**. The body follows in the READ
the next one <20 ms later. No standalone 4 B message. TIER 7 §T2.1 said "48 4B
pushes" but that was a partial window; the real total = 500.

## Distribution (type, len)

```
500 SSL_READs of 4 B on ssl=0x2a3d75c0 (= ctrl `:8011`), ALL type=0x0001:
  len=0x0094(148) -> x241   🔥 NotifyVideoCommand stats variant
  len=0x0093(147) -> x94
  len=0x0092(146) -> x60
  len=0x008b(139) -> x36    (= TIER6 M1.4 periodic 139B stats push)
  len=0x0065(101) -> x16
  len=0x006d(109) -> x13    🎯 kRequestFlush push (TIER7 T2)
  len=0x0090(144) -> x11
  ... 17 autres tailles
```

**Type toujours `0x0001`** = wire response/notification class.

## Standalone vs paired

```
4B avec follow-up READ <20ms : 498 / 500 (= 99.6%)
4B standalone               :   2
```

The 2 standalones = partial TCP flushes at the end of the session, not real messages.

## Cadence

```
min=1ms  p50=467ms  p90=901ms  max=1404ms
```

p50 = 467ms ≈ heartbeat 2Hz desktop. Mapping 1:1 avec heartbeat replies +
NotifyVideoCommand pushes.

## Receiver loop

```c
SSL_read(ssl, hdr, 4);
uint16_t type = ntohs(*(uint16_t*)&hdr[0]);
uint16_t len  = ntohs(*(uint16_t*)&hdr[2]);
SSL_read(ssl, body, len);
dispatch_reply(type, body, len);
```

There is no "len==0 → standalone" branch. Our Switch's `ctrl_tcp_recv_cleartext`
identique = byte-exact compatible.

## Verdict

**C95: not a new yield**. The only actionable value in this area
has already been extracted by TIER 7 §T2 (= the V16 kRequestFlush handler).

## Refs

- Script : `tools/ida/tier8_4byte_pushes.py`
- Output : `tool-results/bnf16tagn.txt` (~232KB)
- Parser : `streaming/ctrl_tcp.c::ctrl_tcp_recv_cleartext`
- TIER 7 T2 : `tools/ida/out/TIER7_RE_2026-05-16.md` §T2
- Doc : `tools/ida/out/TIER8_RE_2026-05-16.md` §U3
