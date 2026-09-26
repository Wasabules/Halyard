---
name: project-CUR1-cursor-format-2026-05-18
description: "CUR1 2026-05-18 — The cursor channel `:base+30` is unblocked: the SUFP cursor bug (= max_chunks=0 is self-contained, not a multi-chunk reassembly) is fixed. Frames decrypted with chacha20. 2 types identified: an initial 271 B bitmap + 8 B position updates."
metadata:
  node_type: memory
  type: project
---

# Cursor channel `:base+30` — unblocked + format identified

> **SUPERSEDED on 2026-09-11** — `:base+30` is AudioOut alone (KB §3.37, 2026-08-26). The "271-byte initial bitmap" `02 00 00 00 00 01 00 01 00 10 80 bb 02 …` is an Opus session's stream descriptor (16-bit, 48000 Hz, 2 channels, byte 7 = the codec; 13 B on FLAC), and the "8-byte position updates" `12 [seq] 00 00 00 f4 ff fe` are short Opus silence frames (TOC `f4`). See KB §3.26. The SUFP fix (`max_chunks=0` = a self-contained packet) remains correct.

## Bug identified + fix

**Before**: 210 cursor packets received in 25 s but **0 frame callbacks** fired. The cause:
- Cursor packets ont `max_chunks=0` (= self-contained mono-packet sentinel)
- Our `sufp_feed()` checks `chunk_idx >= max_chunks` → 0 >= 0 → everything is dropped
- Designed for video, which has max_chunks=24 (= multi-chunk reassembly)

**Fix** dans `ctrl_session.c::on_cursor_packet()` :
```c
if (type == 0x3 && max_chunks == 0 && flag10 == 0x01 && n > 11 + 28) {
    /* Self-contained chacha20-encrypted cursor frame — bypass sufp */
    int ct_len = n - 11 - 28;
    decrypt + callback direct
}
```

Falls back to `sufp_feed` if the format differs (= robustness).

## Wire format observed in V16

Packets cursor UDP :base+30 :
| byte0 | type | freq capture V16 |
|---|---|---|
| 0x13 | DATA3 v1 | 2 occurrences (= 310B initial) |
| 0x23 | DATA3 v2 | 882 occurrences (= 47B updates) |

They all have `max_chunks=0` → self-contained, decrypt + direct callback.

Header SUFP 11B :
- byte0 = version<<4 | type (v1=0x13, v2=0x23)
- byte1 = subchan
- bytes 2-5 = chunk_idx + max_chunks (= 0/0 pour cursor)
- bytes 6-9 = u32 frame_id (= an incrementing seq)
- byte10 = flag (0x01 = chacha20 encrypted)

Payload post-decrypt :
- ciphertext (= 8B ou 271B)
- nonce 12B
- tag 16B

## Format plaintext post-decrypt

### Type bitmap initial — 271 bytes
```
02 00 00 00 00 01 00 01 00 10 80 bb 02 01 01 00 01 00 00 00 ...
```
Hypotheses (to be RE'd byte-exact):
- byte 0 = 0x02 (= type cursor_bitmap)
- bytes 1-4 = ? (= reserved/flags)
- bytes 5-10 = ? (= probable width/height/hotspot/format)
- bytes 11+ = probable BGRA pixel data

Received **twice at startup** (= a cursor shape upload, server→client).

### Type position update — 8 bytes
```
12 01 00 00 00 f4 ff fe   ← #3
12 01 00 00 00 f4 ff fe   ← #4 (identique, stationary)
12 02 00 00 00 f4 ff fe   ← #5 (byte 1 increments)
```
Hypotheses:
- byte 0 = 0x12 (= the position_update type)
- byte 1 = an incrementing seq
- bytes 2-4 = 00 00 00 (= reserved)
- bytes 5-6 = `f4 ff` LE u16 = 0xFFF4 → signed -12 (= **delta X** ?)
- byte 7 = `fe` → signed -2 (= **delta Y** ?)

OR it could be absolute coords:
- bytes 5-6 = X u16 LE  
- byte 7 = Y u8 (= peu probable, max 255)

À RE en bougant souris desktop activement + comparer bytes.

## Status

- ✅ The RX pipeline decodes the plaintext bytes
- ✅ Callback wire vers ctrl_session_glue.c::on_cursor()
- ✅ Frames dumped into /tmp/cursor_NNN.bin through SHADOW_DUMP_CURSOR=1
- ✅ Log first 5 frames hex auto

### Phase 2 TODO

1. **RE the format byte-exact**: capture a desktop session with the mouse actively moving
   → identify the X/Y offsets within the 8 B + the bitmap format within the 271 B
2. **Rendu Borealis** : ajouter cursor overlay dans stream_view
   - Load the initial bitmap (= possibly converting BGRA → RGBA for NanoVG)
   - Track the position by accumulating deltas (or absolutely)
   - Render it onto the YUV frame before pushing it to the screen
3. **Hide system cursor** sur Switch (= touch primary input)

## Cross-refs

- [[project-I1-I2-input-audio-skeleton-2026-05-18]] — Pattern audio/input similaires
- [[project-shadow-screenshots-findings]] — UI Linux desktop cursor handling
- `ctrl_session.c::on_cursor_packet()` — Fix bypass SUFP
- `ctrl_session_glue.c::on_cursor()` — Parser + dump

## Status

✅ DONE Phase 1 — cursor frames decrypted + the callback wired + the format identified.
⏳ Phase 2 — decode it precisely + render it visually.
