---
name: project-image-real-ceiling
description: "STATE 2026-05-14 18:00 — After N53/N54/N55/N57 (= EC=259, Switch NVDEC, Linux VAAPI, a Ghidra RE of HandleVideoPacket), it is confirmed that the software and hardware decoders give the same incomplete picture. The user confirms the official shadow-prod desktop shows 100%. What is missing is in the DATA received / decrypted, not in the decoder. A strong hypothesis: multi-stream encryption with a custom key derivation."
metadata: 
  node_type: memory
  type: project
---

## What we confirmed empirically

| Test | Result |
|------|----------|
| libavcodec software EC=3 | 50% top + 50% vert |
| libavcodec software EC=259 (FAVOR_INTER) | 30% top + 70% stretched |
| The VAAPI hardware decoder on Linux (NVDEC through nvidia-vaapi) | 30% top + 70% stretched (= identical) |
| CUDA hwaccel on Linux | OOM at init, falls back to CPU |
| The official shadow-prod desktop | a 100% complete picture (confirmed by the user) |
| The N54 Switch NRO built with Averne's FFmpeg + envideo | Compiled, not tested live |

**Critical conclusion**: the hardware decoder PRODUCES THE SAME result as the software one. The bottom slice really IS missing on the DATA side.

## The strong hypothesis that remains

The Encryption reply contains only ONE 32 B chacha20 key. But the decrypt OK/FAIL distribution:

| byte10 | meaning | counts |
|--------|---------|--------|
| 0x01   | "data" — decrypt OK | 1156/6126 (= 19%) |
| 0x00   | "parity" — decrypt FAIL | 4970/6126 (= 81%) |

The FAILing nonces have "random" bytes ≠ the counter+static structure of the OK ones. That suggests:
1. SEPARATE encryption for the bottom slices with a derived key
2. OR the FAILures really are Reed-Solomon parity (= XOR redundancy, not unique data)

## Why NVDEC does not solve it

The hardware decoder can handle multi-slice top/bottom CORRECTLY but it needs the bottom slice's DATA. If we only decrypt 19 % of the packets, we miss 81 % of the stream → it is impossible to reconstruct the bottom even with NVDEC.

## Pourquoi shadow-prod marche

Soit :
- (A) It has an additional key for decrypting the missing 81 % (= the bottom data)
- (B) It does Reed-Solomon recovery on the parity chunks to reconstruct the bottom data of the lost packets
- (C) Our LD_PRELOAD hook does not capture every packet the desktop receives (= UDP_RECVMSG sampling/throttling)

(C) is testable: redo the MASTER capture with a packet counter / check against tcpdump.

## Position actuelle des recherches

- N56 (RE of a second chacha20 key): the Encryption reply contains only 1 key. If it is derived, that is custom logic inside the binary = it needs deep RE
- N57 (RE of OnSufpChunkReceived): the decompilation was obtained but the code is stripped, methods appear as FUN_XX, hard to read without investing days
- Deeper still = RE the NVDEC class with precise H.264 slice_header parsing on the desktop, to understand how they interpret the chunks

## Cross-references

- [[project-image-ceiling-reached]] — Plafond libavcodec software
- [[project-sufp-fec-parity-found]] — Format wire chacha20 + flag byte10
- [[project-nvdec-hardware-path]] — why we tried NVDEC
- [[project-N54-ffmpeg-nvdec-built]] — the Switch FFmpeg envideo build is ready
