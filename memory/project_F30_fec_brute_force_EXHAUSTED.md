---
name: project-F30-fec-brute-force-EXHAUSTED
description: "⚠️ SUPERSEDED 2026-05-16 — F30 is right that no textbook FEC matches, but its conclusion (= a custom FEC) is wrong. The truth, confirmed at C95: Shadow HAS NO FEC. See [[project-raptorq-encoder-BYTE-EXACT]]."
metadata:
  type: project
---

> **⚠️ SUPERSEDED 2026-05-16**: F30 correctly eliminated every textbook FEC, but its final conclusion (= a proprietary custom FEC) is wrong.
> An exhaustive IDA Pro RE on 2026-05-16 demonstrates: **0 FEC symbols, 0 RTTI hits, 0 GF tables, 0 FEC library imports**.
> The `byte10 & 1` is a framing routing bit (= trim the v2+v3 trailer), not a parity flag.
> The EVP hook's "0 calls inl=1269" only proves that the parity chunks are not chacha20-ciphered, not that they are FEC.
> **V11 PARITY_RAW is the byte-exact correct implementation.**
> The original content is kept for the record.

---

# F30 — FEC brute force EXHAUSTED — Shadow FEC est custom

## Trouvaille (= 2026-05-23 git commit 75c5770)

Every textbook FEC code was tested against the captured frame max=26 (= 10 data + 16 parity). **NO match.**

## Tests exhaustifs

| FEC code tested | Configuration | Result |
|---|---|---|
| RS Vandermonde GF(2^8) | 28 polys × 5 gens × 8 row_offsets | 0 match |
| RS Cauchy GF(2^8) | 28 polys × 5 gens × 5 (x,y)_off | 0 match |
| Pure XOR | 2^10 subset combinations × 4 alignments | 0 match |
| Scalar GF mul | 28 polys × 256 multipliers | 0 match |
| Python raptorq defaults | T={1241, 1252, 1268, 1269, ...} | 0 match |

## EVP_DecryptUpdate hook validation

A hook installed on the desktop client's `EVP_DecryptUpdate` + a dump into `/tmp/shadow_evp.log`:
- **2003 calls with inl=1241** = data chunks decrypted with chacha20 ✓
- **0 calls with inl=1269** = parity chunks NEVER decrypted with chacha20

**A critical implication**: the FEC operates in the **CIPHERTEXT DOMAIN** — the server does XOR/linear combinations on the encrypted bytes, not the plaintext. So the Shadow decoder must:
1. Decode the FEC over the raw encrypted bytes
2. THEN decrypt chacha20 over the reconstructed result

Not the other way round (= decrypting then FEC would not work, because the parity values are linear combinations of ciphertexts, not of plaintexts).

## Conclusion

Shadow utilise FEC **custom** :
- Soit RaptorQ avec OTI custom (cf. [[project-F29-RaptorQ-lead]])
- Or another proprietary code derived from Raptor10/RaptorQ

**The only remaining route**: an IDA Pro RE decompilation of **vtable[+0x20]** of the `18VideoUdpFrame` (vtables `0x124ca78` and `0x124cb68`, already identified through Ghidra).

## Refs

- Commit `75c5770` (2026-05-23 13:40:30) — F30 commit message
- `05-shadow-client-borealis/tools/fec_bruteforce_ciphertext.py` (187 L) — brute force script
- `/tmp/shadow_evp.log` — EVP_DecryptUpdate captures
- [[project-F29-RaptorQ-lead]] — RaptorQ family confirmed
- [[project-PARITY-RAW-FIX-2026-05-15]] — the V11 workaround that works by luck

## Status

- Textbook FEC completely eliminated at C95
- Shadow FEC = proven custom
- RE binary RaptorQ encoder = next step obligatoire
