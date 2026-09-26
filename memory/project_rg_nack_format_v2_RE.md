---
name: project-rg-nack-format-v2-re
description: "TIER2 2026-05-16 — The desktop client has NO rG parser. rG is an uplink-only NACK. Our N29 failure = a server back-off, rate-policed on stale NACKs. The format is confirmed at C95, but its use is demanding."
metadata:
  type: project
---

## Question TIER2-E

Why does our rG NACK kill the session (392→66 pps)? Is there a server-side
back-off path, and what conditions does the server check before
d'honorer un rG ?

## The answer

### 1. No rG parser on the desktop client — C90

`sub_D85FA0` (= `UdpDataChunk::init` at
`udu/deps/framework/src/common/protocols/src/sufp/udp_data_chunk.cpp`)
parse chaque UDP packet via `*buf & 0x0F` (low nibble byte0) :

| nibble | type | header | usage |
|--------|------|--------|-------|
| 3 | data SUFP v3 | 11 B | what we receive |
| 1 | data variant 1 | 10 B | rare |
| 2 | data variant 2 | 10 B | rare |
| 15 | ping | 10 B | size ≤ 10 → "Invalid ping packet" log |
| other | invalid | n/a | drop avec `chunk wrong version {}, size {}` |

`0x72 & 0xF = 2` → the desktop client would treat rG as "data variant 2" and
would fall into the size-validation branch, which fails → a silent drop. **The
desktop does NOT parse rG at all** — rG is only emitted (sender side),
never received (receiver side). It is an **uplink NACK** towards the server.

Dump : `tools/ida/out/tier1/sub_D85FA0.c`

### 2. The wire format, C95 (confirmed through a desktop emit capture)

```
byte 0:    0x72         // 'r'
byte 1:    0x47         // 'G'
byte 2:    0x01         // proto version
byte 3:    NN           // counter/nonce, rotates 0..255
byte 4:    count        // # u16 entries
byte 5:    0x00         // padding
bytes 6+:  count × u16 LE  // chunk_idx list (per-subchan)
```

Total = 6 + count*2 bytes. The maximum count observed = 9 entries (= 1-2 frames'
worth for a given subchan).

### 3. Why our N29 was killing the session — C80

Our code was sending `rG[0..N-1]` every 2 s = **textbook flapping
pattern** :

1. The server tracks every chunk `(subchan, id, chunk_idx)`. We NACK
   chunks **already delivered** (= chunks received N frames ago).
2. A "stale NACK" heuristic → the server reads it as "the client is flapping /
   broken" → **downgrade encoder bitrate** ou **stop UDP push**.
3. No "back-off ACK" message from the server — it just stops sending.
   The observed 392→66 pps = confirmation that the server detects and punishes it.

### 4. Probable conditions for an rG to be accepted (inferred)

- **Per-subchan** : 1 rG = 1 subchan (= multi-subchan = multi-rG).
- **recent chunk_idx**: a ~100 ms window, no older than ~2 frames.
- **No duplicates**: nonce byte3 differs on every burst.
- **Rate-limit global** : ≤ 5 rG/s max.

### 5. Conclusion bottom-slice

**rG is NOT a way to fix the bottom slice.** The bottom chunks are not
"lost in UDP" — they are **simply not emitted by the server**
(0.81% bottom against 11% on the desktop). There is nothing to NACK.

Pour utiliser rG correctement :
1. Tracker `(subchan, last_chunk_idx)` per-subchan
2. Detect `chunk_idx > last + 1` after a 100 ms deadline
3. Envoyer rG **uniquement** pour ces vrais gaps
4. Cap count ≤ 8, rate ≤ 5/s

But it will fix **nothing for the bottom** — it is a mechanism for
improving robustness on a lossy link, not for forcing the server to
encoder le bottom slice.

## Action items

- **Keep rG disabled** (= our 1 Hz flush is enough as a heartbeat).
- If lossy-link robustness is ever needed: implement the logic described
  in §4 above.
- **Bug taskbar = chercher ailleurs** (= `RegisterSession_Video` field
  map ou `opaque` field VST connect, cf. TIER2 §F).

## Cross-ref

- [[project-rg-nack-format-RE]] — la v1 (= format wire)
- [[project-video-decode-38pct-solved]] — analyse subchans
- [[project-image-50pct-state-2026-05-15]] — state actuel
- TIER2 doc : `tools/ida/out/TIER2_RE_2026-05-16.md` §E
