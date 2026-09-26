---
name: project-raptorq-encoder-BYTE-EXACT
description: "BREAKTHROUGH F31 — Shadow desktop binary has NO FEC encoder/decoder of any kind. RaptorQ family lead from F29 was a false positive. F30's 'custom FEC' was wrong too. Parity flag = 'framing trailer' bit, NOT FEC. V11 PARITY_RAW is the correct model. Mission pivots from 'decode RaptorQ' to 'no decoder needed'."
metadata:
  type: project
---

# F31 — Shadow has ZERO FEC : RaptorQ hunt was chasing a ghost

## The decisive find (= 2026-05-23, a full-pipeline RE in IDA Pro)

A full decompilation of the chunk → frame pipeline in `ShadowPCDisplay.i64`:

```
SufpUdpIOChannel::OnDataReceived  sub_D7F1E0
  └─> UdpDataChunk::init          sub_D85FA0    (parse SUFP header)
  └─> UdpPacketReceiver::feed     sub_D941F0    ← NO FEC
      ├─> UdpFrame::add_chunk     sub_D8CD50    ← just insertion
      ├─> sub_D89F40              ← just `*(WORD*)(this+24)==0`
      ├─> sub_D93DD0              ← list move
      └─> sub_D911D0              ← 300ms timeout
  └─> VideoUdpFrame::write_chunks sub_D8A000    ← dual-callback dispatch
```

**Zero maths beyond pointer arithmetic.** No XOR loop, no Galois, no matrix, no reconstruction.

## Exhaustive negative evidence

| Recherche | Hits |
|---|---|
| Function names `raptor`/`fec_`/`reed`/`solomon`/`ldpc`/`gf256`/`vandermonde`/`cauchy` | 0 (= seuls false positives = `aFec` display manufacturer) |
| RTTI type_info | 0 |
| The strings `RaptorQ`/`OTI`/`scheme_specific`/`K_padded`/`L_prim` etc. | 0 real ones |
| GF(256) exp tables (= prefix `01 02 04 08 10 20 40 80 1D 3A ...`) | 0 |
| Imports `raptorq`/`isa-l`/`leopard`/`wirehair`/`openfec` | 0 |

## The real semantics of flag10 (= byte10 & 1)

flag10's bit 0 is **not a parity bit**. It is a **routing bit**:
- `byte10 & 1 == 0` → an indexed chunk → `vtable[+80](data_ptr, len, flag=0)` → the reassembly's `chunk_idx` slot
- `byte10 & 1 == 1` → chunk "framing" → `sub_D808D0` → trim `v2+v3` trailing bytes via `vtable[+16]`/`vtable[+24]` metadata → `vtable[+56](payload_window)`

For video, `v2 = v3 = 0` (= empirically: V11's raw concatenation works). So **V11 PARITY_RAW is byte-exact** for the video stream.

## vtable[+0x20] = a chunk_size getter (= F30's target was wrong)

`sub_D89FC0` / `sub_D86DB0` (35/28 bytes) = just `return chunk[+16] - first_chunk->ptr` — a size GETTER, not an encoder.

## Implication finale

1. **There is NO RaptorQ decoder to port.**
2. **V11 PARITY_RAW remains the right fix** (= what ships today).
3. **Trailer trimming** (= the `v2+v3` bytes) is **possibly useful** for the residual ~5 % of artefacts — but v2=v3=0 is likely for video → V11 is already byte-exact.
4. **F29 and F30 are SUPERSEDED**:
   - F29's "RaptorQ size match" = a coincidence (= MTU 1269 = the SUFP v3 frame size, not a RaptorQ symbol size)
   - F29's "pkt[0] byte-exact" = a coincidence (= the first 16 bytes of chunk 0 are just the SUFP header, which matches any aligned view)
   - F30's "custom FEC" = the premise "FEC exists" was wrong

## Code-ready integration

No new code is needed. V11 PARITY_RAW (= already in `ctrl_session.c:909-951`) is byte-exact for video. If artefacts remain:

```c
#define SHADOW_PARITY_TRAILER_LEN  0    // F31: v2=v3=0 for video, probably confirmed
// Trim _TRAILER_LEN bytes off the end of the parity payload if > 0.
```

## Validation possible

An LD_PRELOAD hook on the desktop's vtables to confirm `v2 == v3 == 0` at runtime. 15 minutes of work, and it locks the trailer length at C95.

## Refs

- IDA EAs : `0xD7F1E0` (OnDataReceived), `0xD85FA0` (UdpDataChunk::init), `0xD941F0` (UdpPacketReceiver::feed), `0xD8A000` (write_chunks), `0xD8CD50` (add_chunk), `0xD8DC00` (UdpFrame::write), `0xD89F40` (is_complete), `0xD808D0` (framing handler)
- Vtables `18VideoUdpFrame` : `0x124CA78` (primary) et `0x124CB68` (secondary), type_info `0x124BA70` = `'13VideoUdpFrame'`
- IDA scripts : `tools/ida/raptorq_re{,2,3,4}.py`
- Dumps : `tools/ida/out/raptorq/*.c`
- Doc RE complet : `tools/ida/out/RAPTORQ_ENCODER_RE.md`
- Sanity check : `tools/ida/out/raptorq/SANITY_CHECK.md`
- [[project-PARITY-RAW-FIX-2026-05-15]] — V11 confirmed byte-exact
- [[project-F29-RaptorQ-lead]] — **SUPERSEDED**
- [[project-F30-fec-brute-force-EXHAUSTED]] — **SUPERSEDED** (exhaustion correcte, conclusion fausse)

## Status

- **The RaptorQ mission: CANCELLED — there is no target.**
- V11 PARITY_RAW confirmed as the right byte-exact model.
- C95 on the absence of FEC.
- Next steps : LD_PRELOAD pour confirmer trailer len = 0 (= conviction C95→C99).
