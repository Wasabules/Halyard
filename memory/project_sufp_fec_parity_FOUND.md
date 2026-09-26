---
name: project-sufp-fec-parity-found
description: "⚠️ OBSOLETE since 2026-05-15, V11 — the Reed-Solomon hypothesis was FALSE. The byte10=0 chunks are NOT FEC but unencrypted plaintext NAL bytes. See [[project-PARITY-RAW-FIX-2026-05-15]] for the resolution."
metadata: 
  node_type: memory
  type: project
---

> **⚠️ OBSOLETE — kept for the record only.**
> The "byte10=0 → Reed-Solomon parity" hypothesis set out below is **wrong**.
> The V11 RE (2026-05-15) proves that the byte10=0 chunks contain **raw plaintext H.264 NAL**, not FEC. Including those RAW bytes in the reassembly → a near-100 % picture.
> See [[project-PARITY-RAW-FIX-2026-05-15]] for the real resolution.
> No RS/FEC/Galois in the binary (confirmed by the V3 agent's strings analysis).
> The chacha20 wire format below remains correct **for byte10=1 chunks only**.

---

## Wire format chacha20 UDP video (CONFIRMÉ)

```
[SUFP hdr 11B][ciphertext N][nonce 12B][tag 16B]
```

- nonce = `[counter LE 16-bit][static 10B]`
- the static 10 B = `d17d4be5c3ab7fa1ab8b` (= derived from the Encryption key, byte-exact for the whole session)
- counter = u16 LE, incremented by 1 per data packet sent

## SUFP byte10 = data/parity flag

| byte10 | meaning |
|--------|---------|
| 0x01   | a data chunk (= encrypted with chacha20-poly1305) |
| 0x00   | FEC parity chunk (= XOR/Reed-Solomon redondance) |

**Our code decrypts the data chunks (= 1156 OK) and ignores the parity chunks (= 4970 FAIL).**

## SUFP byte0 (ver << 4 | type)

| byte0 | ver | type | meaning | OK/FAIL ratio |
|-------|-----|------|---------|---------------|
| 0x13  | 1   | 3    | SUFP v1 data/parity | 41% OK |
| 0x23  | 2   | 3    | SUFP v2 data/parity | 13% OK |

v2 = a much higher parity ratio (= encoding with more redundancy, probably for degraded network conditions).

## Implication pour bottom slice

The server sends 84 bottom NALs in 240 s (= 10.7 % of the visible slices). **But**: the missing bottom slices could be ENCODED INSIDE THE PARITY CHUNKS. Without Reed-Solomon recovery, we miss them.

If we implement RS recovery over the 4970 parity chunks, we could potentially rebuild up to 90%+ of the bottom slices.

## The Reed-Solomon hypothesis

- The observed pattern: chunks 0-9 are data (OK) and chunks 10+ are parity (FAIL)
- max_chunks = 22 dans header
- The likely ratio: RS(22, 10) → 10 data + 12 parity → it can recover up to 12 erasures

## Sources

- `/tmp/analyze_format_B.py`: decrypts the MASTER-20260514-153936 capture using format B
- `/tmp/nonce_pattern.py`: the nonce pattern revealed
- `/tmp/analyze_fail_pattern.py` : ratio OK vs FAIL par chunk_idx
- Capture : `$REPO/06-shadow-recon-linux/captures/MASTER-20260514-153936/tls_plain.log`
- The chacha20 key: `<session-key-removed>` <!-- The value used to live here in cleartext. It is a SESSION chacha20 key, taken from a capture of May 2026: the session died long ago and the captures it decrypts are gitignored, so it opens nothing for anyone who clones this repo. It still had no business being in a public repo. Removed on 2026-09-03; the value remains in the local captures, next to the hook log it came from. -->
