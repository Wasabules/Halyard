---
name: project-RE1-RE4-static-RE-findings-2026-05-18
description: "RE1-RE4 2026-05-18 — 4 Explore agents in parallel on the cursor bitmap / CI_BODY / Capabilities / the 343 B reply. Key findings: (1) the 271 B cursor layout decoded, (2) CI_BODY_5 is missing 4 booleans (but their field numbers are unknown), (3) Capabilities_Streaming is absent (3 fields), (4) the 343 B reply = a fallback menu of 18 resolutions, NOT a smoking gun. A test at fps=60 confirms the bottom NAL is identical at 50%."
metadata:
  node_type: memory
  type: project
---

# Static RE findings — 4 parallel agents

## RE1 — Test fps=60 = smoking gun hypothesis (Agent D rapport)

### Agent D's hypothesis
The 343 B post-M13 reply contains 18 fallback resolutions. The supposed cause:
- Notre advertise fps=144 (= V14 hardcoded)
- Server VM est natif 60Hz → mismatch → fallback offer + top-only mode
- Patch: `SHADOW_FPS=60 SHADOW_BITRATE_MBPS=20` unblocks the bottom NAL

### Test runtime A/B

| Config | session | frames | fps | bottom_pct | NAL top/bot |
|---|---|---|---|---|---|
| Baseline (fps=144) | 6s | 217 | 36.17 | 49.68% | 239/236 |
| Treatment (fps=60, bitrate=20M) | ~28s | 893 | 31.9 | 50.0% | 974/972 |

### Verdict: **❌ Hypothesis NOT CONFIRMED**

The bottom NAL is **identical at 50 %** in both cases. The 343 B reply with 18 resolutions is
probably NORMAL (= the server always offers alternatives), not a sign of
a degraded mode.

**Implication**: the cause of the 2 % taskbar bug is NOT an fps mismatch.
Probably client-side (= libavcodec's EC on P-frames with no complete bottom).

## RE2 — Cursor bitmap 271B layout (Agent A rapport)

> **SUPERSEDED on 2026-09-11** — those 271 bytes are an Opus session's stream descriptor on `:base+30` (AudioOut, KB §3.37), not a cursor bitmap: bytes 9-12 = 16-bit, 48000 Hz, 2 channels; the "height" at bytes 7-8 is the codec byte (1 Opus, 2 FLAC). See KB §3.26.

### Byte-exact layout identified (C95)
```c
struct cursor_bitmap {
    uint8_t  type;        // [0] = 0x02
    uint8_t  reserved[4]; // [1-4]
    uint16_t width;       // [5-6] LE
    uint16_t height;      // [7-8] LE
    uint16_t hotspot_x;   // [9-10] LE
    uint8_t  hotspot_y;   // [11]
    uint8_t  format;      // [12]
    uint8_t  flags;       // [13]
    uint16_t stride;      // [14-15] LE
    uint8_t  pixels[/* ... */]; // [16+]
};
```

### Implemented in cursor_state.{h,c}
- `cursor_state_get_bitmap_parsed(out)` → returns the parsed struct
- An off-by-1 detected: 271-16=255 (= instead of the 256 expected for a 16×16 mono image).
  Either the layout has a hotspot_y u16_LE (= a 1 B shift), or it is another encoding.
  À confirmer avec capture cursor non-blank.

### Status
- ✅ Parser code en place
- ⏳ The pixel format (BGRA against palette against mono) to be confirmed empirically
- ⏳ NanoVG rendering = phase 3, once the pixel format is confirmed

## RE3 — CI_BODY_5 4 bools manquants (Agent B rapport)

### Identified by Agent B (C75)
Le CI_BODY_5 (= video channel announce) manquerait 4 bool fields :
- `cursor_merged`
- `high_color_fidelity`
- `hdr_enabled`
- `vr_enabled`

### Implemented in ctrl_video_params_t + build_video_chan_body()
- Added through env vars: `SHADOW_CURSOR_MERGED`, `SHADOW_HIGH_COLOR_FIDELITY`,
  `SHADOW_HDR`, `SHADOW_VR`
- Default = false (= V14 byte-exact baseline)
- If enabled, encoded at field# 8/9/10/11 (a hypothesis)

### Test runtime
- Defaults (= tous false) : bootstrap OK, fps=30.46, 853 frames, bottom 50%
- Enabled (= all true): **bootstrap FAILS, frames=0 exit_code=2** ← the field#s are WRONG

### Verdict
**The infrastructure is ready but field# 8/9/10/11 are the wrong values**. The server rejects it.
To be RE'd more precisely: the correct field numbers, through a decompilation of the desktop's serialiser.
Pour l'instant garder defaults false (= safe baseline V14).

## RE4 — Capabilities_Streaming + Auth f7 channels map (Agent C rapport)

### Key findings

**Capabilities_Streaming sous-message** (C70) :
- Existe (`N12ControlProto22Capabilities_StreamingE` dans binary)
- 3 fields probable : f2=codec, f3=chroma, f4=hdr/profile
- **On envoie RIEN actuellement** — juste `{f1=1, f3="OCapture"}`

**Auth f7 channels map** (C70 — CRITIQUE) :
- Notre : `{Video→{UDP}}` (= 1 entry)
- Desktop : `{Video, Audio, Cursor, Input, Mic, Clipboard, Gamepad, AudioOut, FileTransfer}` (= 9 entries)
- **Hypothesis**: the server reads our minimal map as a "basic client" → a degraded mode

### Verdict
**Not implemented this session** — RE4 needs the proto map format decoded
+ risky A/B tests (= they can break the bootstrap). Documented for a future iteration.

### Action items (futures)
1. Decompile the desktop's `Auth::Serialize` for the exact map(StreamType→set<ChannelType>) layout
2. Encode the 9 entries into ctrl_build_authentication
3. A/B test: measure whether the bottom NAL goes above 50%

## Summary

| RE | Status | Impact |
|---|---|---|
| RE1 (fps=60 smoking gun) | ❌ Not confirmed | The V14 baseline of 50% bottom is the current ceiling |
| RE2 (cursor parser) | ✅ The parser is in place | Phase 3 (rendering) waits on a non-blank cursor |
| RE3 (4 bools in CI_BODY_5) | ⚠️ Infrastructure ready, field#s wrong | Keep the defaults, RE the field#s later |
| RE4 (Capabilities + Auth f7) | ⏳ Documented | High effort, risks breaking the bootstrap |

## Conclusion

The 4 hypotheses **not confirmed at runtime** for unblocking the 2 % taskbar bug.
The current V14 stack = the **probable optimum** without an interactive desktop capture
extra. The remaining 2% = probably client-side (= libavcodec EC on
P-frames avec bottom partiel).

Code propre maintenant :
- The cursor parser is ready for when we have a non-blank cursor
- The 4 CI_BODY_5 booleans are prepared but disabled by default
- Capabilities_Streaming + Auth f7 = TODO si on veut tenter

## Cross-refs

- `tools/ida/out/H1_V8_unexplored.md` §B (= CI_BODY mapping H1)
- `tools/ida/out/H1_V9_server_discrimination.md` §D (= Auth f7 map)
- `tools/ida/out/TIER1_RE_2026-05-16.md` §B (= RegisterSession bools)
- `/tmp/cursor_*.bin` (= samples 271B + 8B observed)
- [[project-CUR1-cursor-format-2026-05-18]] — the previous cursor state
