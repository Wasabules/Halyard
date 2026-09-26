---
name: project-F29-RaptorQ-lead
description: "⚠️ SUPERSEDED 2026-05-16 — RaptorQ was ELIMINATED through an exhaustive IDA Pro RE. F29's 3 'converging proofs' were coincidences (aligned SUFP v3 header bytes). See [[project-raptorq-encoder-BYTE-EXACT]] for the C95 elimination."
metadata:
  type: project
---

> **⚠️ SUPERSEDED 2026-05-16**: RaptorQ was ELIMINATED through an exhaustive RE (= 30+ function decompiles + an exhaustive search).
> See `tools/ida/out/RAPTORQ_ENCODER_RE.md` + [[project-raptorq-encoder-BYTE-EXACT]].
> The binary contains **0 RaptorQ/FEC symbols**; vtable[+0x20] is a size getter.
> F29's "converging proofs" were coincidences:
> - "pkt[0] byte-exact" = SUFP v3 header bytes
> - "26 packets × 1268B" = juste la MTU UDP standard
> - "K=10 / N=26" = just the chunk structure observed empirically
>
> **V11 PARITY_RAW remains the BYTE-EXACT CORRECT implementation.**
> The residual taskbar bug has another cause (= not FEC).
> The original content is kept below for the record.

---

# F29 — RaptorQ confirmed as Shadow's FEC family

## The decisive find (= 2026-05-23 git commit c857d41)

Shadow uses a variant of **RaptorQ (RFC 6330)** for FEC on the :base+10 UDP video channel.

## Preuves convergentes

| # | Proof | Consistency |
|---|---|---|
| 1 | **Size match** : MTU=1269 → 26 RaptorQ packets de 1268B = matches Shadow 26×1269 wire frames | ✓ |
| 2 | **Structure match** : N=26 packets, K=10 source = standard RaptorQ ratio pour 12410B source object | ✓ |
| 3 | **pkt[0] byte-exact** : RaptorQ first packet data = `02011dde5801110a00800738` = data idx=0 plaintext FIRST 16 BYTES EXACT | ✓ |

## Mismatches (= OTI custom)

- pkt[1..9] Python raptorq != data[1..9] (= systematic indexing differs)
- pkt[10..25] repair symbols != Shadow parity bytes (= scheme_specific OTI custom)

**Implication**: Shadow uses RaptorQ with **custom OTI parameters** or a modified systematic mapping. Not the RFC 6330 defaults.

## Implication for our taskbar bug

V11 PARITY_RAW (= including the raw parity chunks in the NAL concatenation) works by LUCK:
- The 16 "parity" chunks contain **RaptorQ repair symbols**, NOT plaintext NAL
- Raw concatenation → the libavcodec decoder interprets ~98 % of it through emulation-prevention heuristics
- The residue = taskbar artefacts = real repair symbols that ought to be DECODED to reconstruct the missing data CTs

**The real fix** = implement a RaptorQ decoder with the OTI extracted from the binary → a byte-exact reconstruction of the 10 data CTs → the end of the artefacts.

## Next steps

1. **RE the RaptorQ encoder in the desktop binary** (priority 1):
   - The `18VideoUdpFrame` vtables at 0x124ca78 and 0x124cb68 (already identified in Ghidra)
   - The specific target: `vtable[+0x20]` (= probably the packet builder that calls the RaptorQ encoder)
   - Extraire OTI scheme_specific bytes
2. **OR brute-force the OTI parameter combinations** (3-7 days, but the combinatorics are large)
3. **OR a Wireshark capture + spec analysis** (= not very reliable, since the payload is chacha20-encrypted)

## Refs

- Commit `c857d41` (2026-05-23 13:30:51) — F29 commit message
- `05-shadow-client-borealis/tools/fec_raptorq_test.py` — Python raptorq encode + diff
- `05-shadow-client-borealis/tools/fec_raptorq_v2.py` — repair symbols compare
- [[project-F30-fec-brute-force-EXHAUSTED]] — the elimination of every textbook scheme
- [[project-PARITY-RAW-FIX-2026-05-15]] — the V11 fix that works by luck
- [[project-V12-taskbar-status-2026-05-15]] — the taskbar residue = real repair symbols to decode

## Status

- RaptorQ confirmed at C90+ (= 3 converging proofs)
- OTI extraction = vraie next step pour fix taskbar 100%
