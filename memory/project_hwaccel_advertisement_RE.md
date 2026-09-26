---
name: project-hwaccel-advertisement-RE
description: TIER5-C2 — the server NEVER receives our hwaccel over the wire (= the D3D11/Vaapi/Drm enum strings live in the sub_69B070 JSON stats, not in the ctrl Capabilities). Not a multi-NAL discriminator.
metadata:
  type: project
---

# TIER5-C2 — GPU/HWAccel capability advertisement RE

**Date** : 2026-05-23.  
**Source** : `tools/ida/tier5_re.py` + IDA 9.3 sur `06-shadow-recon-linux/ShadowPCDisplay.i64`.  
**Doc complet** : `tools/ida/out/TIER5_RE_2026-05-16.md` §C2.

## TL;DR

The desktop has an HW-backend enum (= Default/AvFoundation/OpenGL/VaX11/Drm/D3D11/Egl
serialised as strings in `sub_69B070`). **Those values DO NOT ENTER
the ctrl-Capabilities wire**; they live in a JSON stats blob sent
elsewhere (= probably an HTTPS telemetry endpoint). **The server NEVER receives
our hwaccel capability through the ctrl wire**. So the multi-NAL trigger is not
NOT gated on "the client supports hardware decoding".

## Findings

### 1. HW backend enum dans desktop

`sub_69B070` lines 750-800 + 1080-1110 contient un switch :
```c
switch ( a2[28].m128i_i32[0] ) {
    case 1: "default";       // = sw decode
    case 2: "AvFoundation";  // = macOS
    case 3: "OpenGL";        // = generic GL
    case 4: "VaX11";         // = Linux Vaapi X11
    case 5: "Drm";           // = Linux Vaapi DRM
    case 6: "D3D11";         // = Windows
    case 7: <unk_1229088>;   // = "Egl" probable
    default: "undefined";
}
```

### 2. Vraie destination = JSON stats, pas wire-ctrl

`sub_69B070` is the **JSON stats serialiser** (= a destination struct ≥ 1024 B,
string writes at offsets +944/+960/+976/+992/+1008…). No
`pb_write_*` call. No ctrl-chan SSL_write in the path.

Probable consommateur = HTTPS POST sur telemetry endpoint Shadow (= cf
TIER B2 unexplored "telemetry endpoints"). Distinct du ctrl chan
:base+11.

### 3. Strings absents du binary

- `NVDEC`, `CUDA`, `cuvid`, `nvtegra` : **0 hit** dans ShadowPCDisplay
  Linux. Consistent (= NVDEC is the server-side encoder, not a client-side
  decoder for the official desktop).
- `D3D11`, `DXVA2`: present in the rodata, probably for the Windows path.

### 4. Our Switch hwaccel (= envideo NVDEC) is not advertised on the wire

Our `ctrl_build_capabilities` has no "hwaccel_backend" field. The
the f5.f2 sub-message `Capabilities_Streaming` (= tested in RE12) does not include
non plus.

### 5. Refs

- HW serializer : `tools/ida/out/tier5/c2/c2_hw_sub_69B070.c` (3692 lines)
- Backend enum switch : lines 750-800 + 1080-1110
- The Vaapi decoder ctor (`VaapiVideoDecoder`): sub_C5BC30 (already in TIER1)
- Imports Vaapi : `vaInitialize`, `vaCreateConfig`, `vaCreateSurfaces`, etc.

## Confidence

- C95: the HW-backend enum in `sub_69B070` mapped byte-exact.
- C90: `sub_69B070` is JSON stats, not a wire-ctrl proto serialiser.
- C85: the server never receives our hwaccel through the ctrl wire.

## Implication bug taskbar

**None.** The server does NOT discriminate on the client's hwaccel capability through
the wire. If multi-NAL were gated on it, the desktop would have to send it
explicitly — and it does not. So the "the client advertises
NVDEC → server pousse multi-NAL" est RÉFUTÉE.

## Cross-refs

- `[[project-N54-ffmpeg-nvdec-BUILT]]`
- `[[project-nvdec-hardware-path]]`
- `[[project-ctrl-oneof-enum-VERIFIED]]`
- `[[project-channel-announcements-8msgs-RE]]`

## Status

C85 — closed. No wire-side action is possible. NVDEC remains the solution
the client's decoder, but it is purely local to the client.
