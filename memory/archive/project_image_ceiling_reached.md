---
name: project-image-ceiling-reached
description: "STATE 2026-05-14 ~17:00 — The ceiling reached on the bottom slice with generic libavcodec. 30% top + 70% stretched = a stable baseline. Every hypothesis RE'd byte-exact against the desktop (Capabilities, RegisterSession_Video bitrate 20 Mbps, channel announcements, the chacha20 wire). The server sends ~10% bottom NAL — the desktop receives the same but uses a custom VAAPI hardware renderer."
metadata: 
  node_type: memory
  type: project
---

> **ARCHIVED 2026-09-13 — a state, not a finding.** This records a ceiling that G4 then removed entirely.
> It was true when written. Nothing here should be acted on: read `KB.md`
> for what holds today. Kept because knowing what was tried, and when, is
> what stops it being tried again.


## What we confirmed byte-exact against the desktop

| Aspect | Statut |
|--------|--------|
| Wire SslCtrlChanV2 `[u16 type][u16 len][proto]` | ✓ |
| The chacha20-poly1305 wire `[hdr11][ct][nonce12][tag16]` | ✓ (= our format B is correct) |
| Nonce structure `[counter LE 2B][static 10B]` | ✓ |
| Capabilities byte-exact (= 87B) | ✓ |
| RegisterSession_Video f7 = 20000000 (= 20Mbps) | ✓ CI_BODY_5 byte-exact |
| 8 channel announcements byte-exact | ✓ |
| Wire format ctrl messages | ✓ |

## Why it is 30% top + 70% stretched

The server objectively sends ~10 % bottom slices (= confirmed by decrypting the MASTER capture). The **bottom is missing** in 90 % of the P frames; it is not a client bug.

The desktop client has the **same input limitation** but uses a **VAAPI hardware decoder** that handles missing MBs better at MB level (= automatic temporal prediction through standard H.264 reference-frame management).

Generic libavcodec in software mode does its error concealment at SLICE level (= not MB). FF_EC_FAVOR_INTER (= 256) only applies to MB-level corruption, not to a whole missing slice.

## Leads eliminated

1. ❌ kUpdateSession (case 13) = never sent by the desktop
2. ❌ A periodic kUnregisterSession = cleanup only
3. ❌ A periodic RegisterSession = bootstrap only (8 channels)
4. ❌ The BOTTOM_INJECT cache (a stale frame_num) = it makes the picture worse
5. ❌ YUV composite 2-pass = pas testable visuellement utile
6. ❌ Reed-Solomon FEC parity recovery = the data chunks already contain everything, the parity is redundancy for packet loss
7. ❌ FF_EC_FAVOR_INTER 256 = it degrades anyway

## Pistes restantes (= chacune ≥1 jour)

1. **Implement a custom slice-aware decoder**: replace libavcodec with a decoder that explicitly handles missing slices through a reference-frame copy. Very complex, months of work.

2. **Reverse-engineer ShadowPCDisplay's VaapiVideoDecoder**: study how their decoder configures VAAPI for these multi-slice streams. Not applicable on Switch but useful as a baseline on Linux.

3. **Patch the bottom cache's frame_num**: implement bit-level patching of the slice_header to reuse the bottom cache with the current frame_num. It could work but the H.264 wire code is complex.

4. **A test on another Shadow VM**: perhaps some datacentres/encoders send more bottom slices.

## État actuel buildable

- N47 periodic 9 s DisplayReady: active
- N48 Heartbeat 2Hz : actif
- N53 error_concealment=259 : actif (FF_EC_FAVOR_INTER inclus)
- N51 BOTTOM_INJECT: OFF (= `SHADOW_BOTTOM_INJECT=1` re-enables it)
- N52 YUV composite: OFF (= `SHADOW_YUV_COMPOSITE=1` re-enables it)
- N49 periodic RegisterSession: OFF (= `SHADOW_REG_PERIOD_MS=N` re-enables it)
