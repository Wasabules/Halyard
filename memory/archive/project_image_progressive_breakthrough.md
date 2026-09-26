---
name: project-image-progressive-breakthrough
description: "2026-05-14 — DECISIVE: NotifyResolution + Request_Flush + error_concealment=3 turn the 50% top + green into a picture with extrapolated blue streaks (= ~70% perceptually visible)."
metadata: 
  node_type: memory
  type: project
---

> **ARCHIVED 2026-09-13 — a state, not a finding.** This records superseded by G4 - the cause was an off-by-one.
> It was true when written. Nothing here should be acted on: read `KB.md`
> for what holds today. Kept because knowing what was tried, and when, is
> what stops it being tried again.


## Key findings (= 4 parallel agents)

### Wire format messages CTRL pour reconfig encoder server-side

| case | message | wire bytes (= seq=1, body) |
|---|---|---|
| 7 | `Request_Flush` (kFlush) | `08 01 12 02 3a 00 ...caps` |
| 13 | `Request.UpdateSession.Video` | (= notre seq=17 EncodingConfig actuel) |
| 14 | `Request.NotifyResolution.UpdateDisplayConfig` | `08 SEQ 12 0c 72 0a 0a 08 1a 06 08 W_varint 10 H_varint ...caps` |
| 15 | `Request.NotifyVideoCommand` | `08 SEQ 12 02 7a 00 ...caps` (= 3 scalar fields uint32/fixed32/uint32, the semantics are unclear) |

`kFlush` = case 7 of the Request oneof, **NOT** a sub-enum of NotifyVideoCommand (= contrary to the initial hypothesis).

### Decoder libavcodec error_concealment=3

It enables **EC_GUESS_MVS | EC_DEBLOCK** → the bottom MBs are predicted from their spatial neighbourhood (= the valid top scanline) instead of falling back to green YUV.

Visually: "blue/orange/brown vertical streaks" (= an extrapolation of the last scanlines) instead of a solid green band.

## The optimal baseline configuration (the binary after N40)

```c
/* Enabled by default (= no env vars needed):
 * - error_concealment = 3
 * - NotifyResolution 1× initially (= seq+1 after ready)
 * - Request_Flush disabled (env-toggleable)
 */
```

Toggles env vars :
- `SHADOW_NRES_PERIOD_MS=N`: a periodic NotifyResolution (default 0 = the initial one only)
- `SHADOW_FLUSH_PERIOD_MS=N`: a periodic Request_Flush (default 0 = disabled)
- `SHADOW_FLUSH_CMD=N`: (legacy, the NotifyVideoCommand command — use Flush instead)
- `SHADOW_ERR_CONCEAL=N` : libavcodec error_concealment (default 3)
- `SHADOW_DISPLAY_HEIGHT=N` : RegisterSession height
- `SHADOW_NOCROP=1`: see the raw frame (= without the height/2 clamp)
- `SHADOW_STRETCH=1` : remplit window, no letterbox

## Results observed

| config | frame visuel |
|---|---|
| baseline (= rien) | 50% top desktop + 50% vert solid |
| `SHADOW_ERR_CONCEAL=3` | 50% top + 50% extrapolated blue streaks (= aesthetically better) |
| `SHADOW_NRES_PERIOD_MS=1000` | image construit progressivement haut→bas (= user observation) |
| `SHADOW_FLUSH_PERIOD_MS=1000` | server reset constant, image varie |
| Combo NotifyResolution 1Hz + Flush 500ms | image ~50-60% + bottom partiel |

## Limite restante : 100% image

The Shadow server, systematically:
- envoie SPS=1920×1080 mais H.264 slices first_mb=0 only (= top half)
- bottom slices arrivent rarement (= 1.6% via N31 audit)
- with NotifyResolution + Flush, we FORCE transient bottom IDRs but they do not stabilise

For a stable 100 % picture, we need:
1. **Either** a permanent full-bottom IDR (= a different server config, out of the client's reach)
2. **Or** a Reed-Solomon decoder to rebuild the bottom from the parity chunks (= complex)
3. **Or** capture a desktop session with a different account / a different VM to compare whether the 50 % limit is universal

Right now the baseline (= the 540 clamp + stretch + EC=3) gives a usable picture.

## Cross-ref

- [[project-video-540-clamp-workaround]]
- [[project-image-50pct-root-cause]]
- [[project-rg-nack-format-re]]
- [[project-video-ssl-tcp-channel-found]]
