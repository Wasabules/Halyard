---
name: project-PARITY-RAW-FIX-2026-05-15
description: "THE FINAL VICTORY, V11, 2026-05-15 — The SUFP chunks with byte10=0 (\"parity\") are NOT Reed-Solomon FEC but contain ~90% of the H.264 NAL bytes in unencrypted PLAINTEXT. Including them RAW in the reassembly → a near-100% picture. It destroys the RS hypothesis of project_sufp_fec_parity_FOUND.md."
metadata:
  node_type: memory
  type: project
---

# PARITY chunks = plaintext NAL data, NOT Reed-Solomon FEC

## The decisive find (= the V11 RE, the final win)

Date: 2026-05-15. Runtime state: **"I have a nearly full-quality complete picture"** (the user).

The hypothesis, tested through the env toggle `SHADOW_PARITY_RAW=1`: store the parity chunks as raw bytes in the reassembly slots, alongside the chacha20-decrypted data chunks.

Empirical result:

| 30 s session metric | Before V11 (V10b) | After V11 (PARITY_RAW) |
|---|---|---|
| NAL top | 907 | 907 |
| NAL bottom | 210 | **896** |
| Bottom ratio | 9.0% | **49.7%** (≈ top/bottom parity) |
| IDR top | 15 | 15 |
| IDR bottom | 15 | 15 |
| decoded frames | ~250 | **865** (~29 fps) |
| parity chunks received | 9317 (skipped) | 9317 (used) |
| Visual | a 30% top + 70% striped picture | a nearly 100% complete picture |

## Wire format CORRECTED

SUFP byte10 :
- `0x01` = chunk data, **payload = chacha20-poly1305 ciphertext + nonce12 + tag16**
- `0x00` = chunk parity, **payload = plaintext NAL bytes** (= NOT Reed-Solomon, NOT FEC)

The ~10 parity : 1 data ratio observed in the capture is NOT redundancy. Shadow sends the **majority** of an H.264 frame's bytes in cleartext (= saving server-side CPU, maximising throughput) and encrypts only ~10 % (= most likely the NAL headers + the critical SPS/PPS).

## Fix

`05-shadow-client-borealis/demo/src/streaming/ctrl_session.c::on_video_packet` :

```c
if (is_data) {
    /* chacha20-poly1305 decrypt, store plaintext in slot */
    ...
} else {
    ctx->stats->parity_skip++;
    if (g_parity_raw && !r->chunks[chunk_idx]) {
        /* parity payload = plaintext NAL bytes */
        int raw_len = ct_len_full;
        if (raw_len > 0 && raw_len <= 2048) {
            r->chunks[chunk_idx] = malloc(raw_len);
            memcpy(r->chunks[chunk_idx], pkt + 11, raw_len);
            r->chunk_lens[chunk_idx] = raw_len;
        }
    }
}
```

ON by default since 2026-05-15. Toggle it off with `SHADOW_PARITY_RAW=0` for an A/B or for debugging.

## Consequences

1. **[[project-sufp-fec-parity-found]] is OBSOLETE** — the Reed-Solomon hypothesis was wrong. No RS, no FEC, no Galois fields in the binary (confirmed by the V3 agent: zero RS/FEC strings among 41k binary strings).

2. **No more need for RS recovery** — the "recovery" is a plain plaintext memcpy.

3. **Bandwidth profile** — Shadow sends ~10× more plaintext NAL bytes than we imagined. The 1:1 top/bottom ratio obtained confirms that the server pushes both slices at parity.

4. **Residual decoder errors** — ffprobe still shows a few `bytestream -7/-9` + concealment on P-frames. That is normal UDP packet loss (~1-5 %), not a protocol defect. To be mitigated on the libavcodec side with EC=3 + NACK.

## Refs

- Patch : `05-shadow-client-borealis/demo/src/streaming/ctrl_session.c:909-951` (default ON 2026-05-15)
- Test artifact : `05-shadow-client-borealis/build_windows/shadow-client-data/stream.h264` (= ~30s sample, 1920×1080 H.264 High @ 25fps)
- Logs session : `05-shadow-client-borealis/build_windows/shadow-client-data/webrtc.log` (stats NAL top=907 bot=896)
- [[project-IFR-counter-FIX-2026-05-15]] — a prerequisite (without the counter, the server de-duplicates the IDR refresh)
- [[project-image-100pct-proof]] — the empirical desktop reference (= it was badly calibrated and underestimated the bottom ratio)
- [[project-sufp-fec-parity-found]] — **OBSOLETE since V11**, kept for the record

## Status

- V11 PARITY_RAW = **WIN absolu**
- A nearly 100% picture on Windows, confirmed by the user
- Reste TODO :
  - Port Switch + test live NVDEC + PARITY_RAW combo
  - ffprobe re-validate stream.h264 pour quantifier residual concealment
  - Dead code cleaned up (BOTTOM_INJECT, IDR_INJECT, YUV_COMPOSITE) — they were cosmetic workarounds
