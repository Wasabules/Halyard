---
name: project-setstreamingprofile-re
description: H3 closed — SetStreamingProfile is NOT a ctrl message, it is a local client toggle; the residual lead is advertising the capabilities
metadata:
  type: project
---

# H3 — `SetStreamingProfile` ctrl-msg ? — **NON, fausse piste**

**Session** : 2026-05-16 IDA Pro 9.3 + Hex-Rays. Doc : `tools/ida/out/H2_H3_RE_2026-05-16.md` §H3.

## TL;DR

The string literal `"SetStreamingProfile"` **does not exist** in the binary
desktop. Les strings voisines (`streaming_profile`, `streaming-profile`,
`streaming_profile_changed`, `NotifyStreamingProfileChanged`, etc.) toutes
resolve to **local client-side paths**: the CLI parser, the capability list,
notify event observer, sentry telemetry, config persister. **Aucune
serialisation protobuf, aucun SSL_write.**

**H3 closed at C90.** There is no new `ctrl_build_set_streaming_profile` to write.

## Identifying the 6 functions concerned (xrefs)

| EA | Role | Type | Dump |
|----|------|------|------|
| 0x6368D0 | CLI parser `--streaming-profile speed/reliability` | local | `tools/ida/out/h2h3/h3_sub_6368D0.c` |
| 0x697050 | Capability vector builder (`std::vector<const char*>`) | local | `tools/ida/out/h2h3/h3_sub_697050.c` |
| 0x7B05D0 | `Client::SetStreamingProfile(int profile)` (the C++ method) | local | `tools/ida/out/h2h3/h3_sub_7B05D0.c` |
| 0x7C9FB0 | Sister fn (variantes profil) | local | `tools/ida/out/h2h3/h3_sub_7C9FB0.c` |
| 0x7835B0 | Config persister (key `streaming_profile_startup`) | local | `tools/ida/out/h2h3/h3_sub_7835B0.c` |
| 0x9D3BA0 | Sentry tag (`sentry_value_set_by_key`) | telemetry | `tools/ida/out/h2h3/h3_sub_9D3BA0.c` |

`sub_7B05D0` is *the* `Client::SetStreamingProfile` method:
- prend `a2 ∈ {1=speed, 2=reliability}`
- writes 6 dwords into the client state (`a1+1552`, `a1+1556`, `a1+564`,
  `a1+1560`, `a1+1564`, `a1+1568`)
- construit un nlohmann::json `{"renderer":"<x>","streaming_profile":"<x>"}`
- appelle un vtable[+16] observer notify
- **NEVER** a write to a socket or to a protobuf serialiser

## A residual lead: the capabilities advertisement (C60)

`sub_697050` emits the vector:
```
display_management, gamepads, network_notifications, dynamic_bitrate,
multiscreen, streaming_profile, audio_out_codec
```
inside a `std::vector<const char*>`. That vector is *probably* serialised into
a `repeated string` inside a `Capabilities` sub-message (= a field number
unknown without a plaintext capture). **Our client emits NOTHING from that vector
dans `ctrl_build_capabilities`.**

→ Action item (C60) : ajouter env `SHADOW_CAPS_STRINGS=1` qui pousse ces 7
strings dans `Capabilities` (= tester fields 6/7/8). Si server gate
multi-NAL on `client.advertises("streaming_profile")`, that unblocks it.

## Refs

- `tools/ida/out/H2_H3_RE_2026-05-16.md` §H3
- `tools/ida/out/H1_V8_unexplored.md` §C (= an earlier Ghidra analysis
  that prefigured this C55 conclusion, now confirmed at C90)
- `tools/ida/out/h2h3/H3_STRING_XREFS.md` (= liste xrefs brutes)

## Cross-links

- [[project-multinal-chunks-RE]] (= TIER1 : 5 bools du RegisterSession_Video)
- [[project-registersession-video-bools-RE]] (= H2-deep describes the byte-exact names)
- [[project-ctrl-oneof-enum-verified]] (= the 26 cases of the Request oneof, none of
  which maps to SetStreamingProfile)
