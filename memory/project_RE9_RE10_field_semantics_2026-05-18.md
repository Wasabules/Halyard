---
name: project-RE9-RE10-field-semantics-2026-05-18
description: "RE9+RE10 2026-05-18 — A deep semantic RE of the 5 RegisterSession_Video boolean fields (4/5/6/8/10). Individual A/B tests: NONE enables a multi-NAL trigger. Fields 4 and 8 = no effect. Fields 5+6 and 10 = a bad effect (the server changes mode but 0 frames). Conclusion: the V14 stack's 50% bottom NAL baseline IS the optimum, and the 2% taskbar bug is NOT server-side through these booleans."
metadata:
  node_type: memory
  type: project
---

# RE9+RE10 — Bool fields RegisterSession_Video : tests A/B verdict

## Mission

Find the **multi-NAL trigger** among the 5 wire-confirmed RE8 boolean fields (= fields 4/5/6/8/10) to fix the 2 % Windows taskbar bug.

## Findings RE9 (= field 10 deep-RE)

### Setter caller
`sub_853770.c:472` copie `*(_BYTE *)(a2+56) → v30+52` lors du RegisterStreaming call.
A notable shift: a2+56 → v30+52 (= a -4 byte shift).

### Probable semantics
Log desktop dit "vr: {}" pour a2+56 → field 10 = **`vr_enabled` probable** (C70).

### Wire change observable
- Field 10 = false → V14 baseline (= 50% bottom NAL stable)
- Field 10 = true → **bootstrap OK + 0 frames** → server change packet format
  - Likely: it sends multi-NAL packets / stereo / a VR format our client cannot decode
  - Needs a multi-NAL parser on the client side (~200 LoC)

### Recommendation
**Keeping field 10 = false** is the stable strategy. Enabling it would require
implement a multi-NAL parser + test it with an interactive capture.

## Findings RE10 (= fields 4/5/6/8)

### Ranking initial Agent
| Field | Offset | Probable semantics | A/B risk |
|---|---|---|---|
| 4 | +44 | `enable_low_latency` | LOW |
| 5 | +45 | `enable_reference_reorder` | LOW |
| 6 | +46 | `enable_intra_refresh` | LOW |
| **8** | +47 | `enable_chunked_nal` (= BEST multi-NAL candidate C65) | MEDIUM |

### Tests A/B runtime

| Test config | session | frames | fps | bottom_pct | Verdict |
|---|---|---|---|---|---|
| Baseline (= all false) | 28s | 854 | 30.5 | 50.0% | OK V14 |
| Field 4 alone | 28 s | 874 | 31.2 | **49.95%** | NO effect |
| Fields 5+6 combined | 28 s | 0 | 0 | **0%** | The server changes mode, 0 frames |
| Field 8 alone | 6 s | 221 | 36.83 | **49.79%** | NO effect |
| Field 10 alone | (tested in RE9) | 0 | 0 | **0%** | The server changes mode, 0 frames |

### Definitive verdict

**NONE of the 5 RE8 bools activates the multi-NAL trigger.**

- Fields 4 / 8 → no effect (= the server ignores them, or they flag basic encoding with no wire impact)
- Field 5+6 / 10 → bad effect (= server change format → client drop frames)

The **V14 stack's 50 % bottom NAL baseline IS the optimum** attainable without
an interactive desktop capture. The 2 % taskbar bug is NOT server-side through
ces bools.

## Remaining hypotheses for the 2% bug

1. **Client-side libavcodec EC** on P-frames with a partial bottom (= likely)
2. **The Capabilities_Streaming sub-message** not sent (= see RE4)
3. **Auth f7 channels map** 1 entry vs 9 desktop (= cf RE4)
4. **The VST :base+20 IDR retransmit** not wired to the h264_decoder (= cf. RE6)

VST branching = **the next concrete impactful action** (= already identified: 4142 B = valid H.264 NAL, just needs wiring to the decoder).

## Code state post-RE9+RE10

- Defaults all false → baseline V14 byte-exact preserved
- Env vars SHADOW_REG_F4/F5/F6/F8/F10 disponibles pour future A/B test
- No wire patch enabled by default
- The findings documented in this note + the commit message

## Cross-refs

- [[project-RE1-RE4-static-RE-findings-2026-05-18]] — the previous REs
- [[project-multinal-chunks-RE]] — the original multi-NAL hypothesis
- `tools/ida/out/tier1/reg_sub_10E60A0.c` — serializer desktop
- `tools/ida/out/sub_853770.c` — setter caller

## Conclusion

The RE8 booleans are a **DEAD END for the multi-NAL trigger**. The 2 % taskbar bug needs
either (a) a client-side multi-NAL parser implementation for field 10/VR, or (b)
an interactive desktop capture to identify another cause (= Capabilities_Streaming
ou Auth f7).

The remaining concrete action without an interactive capture: **wire the VST callback to the
h264_decoder** (= RE6 finding C95 = 4142B sont H.264 IDR retransmits).
It could improve resilience to UDP loss (= the 2 % bug may be related).
