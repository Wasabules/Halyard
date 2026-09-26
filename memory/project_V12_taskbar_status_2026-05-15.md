---
name: project-V12-taskbar-status-2026-05-15
description: "The picture's state at the end of 2026-05-15 — V11 PARITY_RAW + V12 IFR ×2 gives a ~97-98% correct picture. The residue = 27 MB errors per 30 s (= 1/s), all concentrated on MB rows 58-67 (= the Windows taskbar zone). Hypothesised causes + what has been eliminated."
metadata:
  node_type: memory
  type: project
---

# Final picture state V11+V12 — the Windows taskbar residue

## Stack actuelle (= 2026-05-15 ~16h)

| Composant | Setting |
|---|---|
| PARITY_RAW | **ON by default** (byte10=0 chunks stored as plaintext bytes) |
| PARITY_TRIM | **0** (tested at 28 → it breaks the picture, proving there is no nonce+tag trailer to remove) |
| PARITY_DECRYPT | **OFF** (0 and AAD=SUFP both tested, 0/100 successes = the parity is not chacha20-encrypted) |
| IFR_PERIOD | **14** (= 2× more frequent than the default 28, reducing the drift cycle) |
| AU_DELIM | **ON** (tested OFF, no difference on the taskbar) |
| ERR_CONCEAL | **259** (= GUESS_MVS + DEBLOCK + FAVOR_INTER). Tested at 0 → identical artefacts with no concealment, so it is real corruption and not concealment) |

## Runtime metrics (session 2026-05-15 12:39, 110 s)

- video=40991 pkt, decoded=2928, ok=2877/2877, parity=21079
- NAL top=3057, bot=3013 (= ratio ~1:1) ✓
- IDR top=45, bot=45 (= perfectly symmetric) ✓
- aband=0, incompl=0 (= aucun chunk perdu mesurable) ✓
- decoded ~29 fps @ 1920×1080
- ffprobe stream.h264 : **27 erreurs MB total** sur 30s
  - The distribution: MB rows 58, 60, 61, 63, 65, 67 (= all within the taskbar zone = pixel rows 928-1087)
  - Pattern : `error while decoding MB X Y, bytestream -7/-9 → concealing N DC/AC/MV errors in P frame`

## The user-visible visual symptom

> "the Windows bar at the bottom is full of artefacts but sometimes I see it cleanly. A cycle of blurry → clear → blurry → stripes"

The cycle matches the IDR period. CABAC errors in the bottom slice propagate through the motion vectors until the next IDR refresh, then reset.

## Hypotheses eliminated

| # | Hypothesis | Test | Result |
|---|---|---|---|
| 1 | The parity chunks are chacha20-encrypted with the same key as the data | `wc_ChaCha20Poly1305_Decrypt` on 100 chunks | **0/100 OK** — not chacha20-poly1305 |
| 2 | The parity chunks are chacha20 + the SUFP header as AAD | `decrypt_aad_unsafe(nonce, tag, aad=SUFP_hdr, 11)` on 100 | **0/100 OK** — not the SUFP header as AAD |
| 3 | The parity payload is `[NAL][nonce 12][tag 16]` (= trim 28 off the tail) | `raw_len -= 28` | The picture breaks → the tail bytes ARE useful NAL |
| 4 | The AU_DELIM injection corrupts the slice parse | `SHADOW_AU_DELIM=0` | No difference |
| 5 | The artefacts are libavcodec concealment going wrong | `SHADOW_ERR_CONCEAL=0` | Identical artefacts → real corruption, not EC |
| 6 | Packet loss on the bottom slice's last chunks | the stats `aband=0 incompl=0` | No lost chunks measured |
| 7 | Reed-Solomon FEC to be applied | RE V3: 0 RS/FEC/Galois strings among 41k in the binary | No RS in the desktop binary |

## Remaining hypotheses (= untested)

| # | Hypothesis | Estimated effort |
|---|---|---|
| A | **VideoSslTcpChannel `:base+20`** sends IDR retransmits + bottom-slice corrections | High (= open the TCP channel + parse a new wire format) |
| B | **rG NACK** packets to retransmit lost chunks (= the format is misunderstood, disabled after the throughput crash) | Medium (= RE the full rG format) |
| C | The bottom slice's CABAC byte count is insufficient — perhaps `cabac_zero_word` padding is missing | A cheap test, unlikely |
| D | The chunk concat order must be interleaved data/parity rather than sequential | A cheap test, unlikely |
| E | The server caps the bottom slice's quality for our client (= a UA/SDP fingerprint) | Very expensive to validate |

## Concat structure validated

The bottom slice of our concatenation is ONE valid NAL unit (= type 5 IDR with `first_mb_in_slice=4080`, or type 1 P likewise). ffprobe parses the NALs correctly. The decoder finds the right boundaries. The problem is in the **CABAC content** of the bottom slice's last MBs.

## Refs

- [[project-IFR-counter-FIX-2026-05-15]] — V9 F2 = root cause bottom slice rare
- [[project-PARITY-RAW-FIX-2026-05-15]] — V11 = the bottom slice is present nearly 100% of the time
- [[project-sufp-fec-parity-found]] — OBSOLETE, a false RS hypothesis  
- [[project-video-ssl-tcp-channel-found]] — VideoSslTcpChannel :base+20 piste future
- `tools/ida/out/H1_PARITY_DEEP_analysis.md` — preuve absence de RS

## Status

- V11+V12 = WIN substantiel (= 30% → 98% image visible)
- The taskbar residue = an acceptable level for a Switch validation
- Nothing blocking the port of the client to the Switch + testing the NVDEC hardware path
