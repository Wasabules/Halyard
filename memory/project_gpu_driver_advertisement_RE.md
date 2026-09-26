---
name: project-gpu-driver-advertisement-RE
description: TIER9-W2 — the Shadow desktop probes the GPU through vaQueryVendorString+epoxy_glGetString but publishes the result ONLY in the /1/stats PUBLIC telemetry, NEVER on the ctrl wire nor in the /1/clients opaque. A C95 refutation.
metadata:
  type: project
---

# TIER 9 W2 — GPU driver / video hardware advertisement RE

## Verdict C95 — GPU info reste local + ELK telemetry uniquement

The Shadow desktop actively **probes** the GPU/driver, **BUT** the result serves
only to:
1. Choisir le bon hwaccel local (= CUDA si NVIDIA, VAAPI sinon)
2. Pushed as PUBLIC telemetry through `/1/stats` for operator observability

**NEVER** sent to the streaming server. Cross-confirms TIER 5-B1 Capabilities
+ TIER 5-C2 HWaccel RE.

## The client-side GPU probing infrastructure (= ELABORATE)

**Strings** :
- `strings_display.txt:80` = `vaQueryVendorString` (= **VAAPI vendor probe**)
- `strings_display.txt:178` = `epoxy_glGetString` (= **OpenGL vendor probe** via libepoxy)
- `strings_display.txt:36125` = `glGetString` (= OpenGL function)
- `strings_display.txt:37036` = `glGetStringi` (= OpenGL extension function)
- `strings_display.txt:37656` = `GenuineIntel` (= **CPUID match string**)
- `strings_display.txt:37659` = `AuthenticAMD` (= **CPUID match string**)
- `strings_display.txt:33448-33464` = **31 lignes** `"intel i965 driver for intel(r) {model} - {ver}"`
  (sandybridge / haswell / broadwell / ivybridge / kaby lake / gm45)
  = matching base pour identifier Intel VAAPI driver version
- `strings_display.txt:35962` = `NVIDIA Corporation` (= hardcoded GL_VENDOR match)

**Note importante** : aucun string `nvml*`, `nvenc*`, `cuda*`, `cudaGetDevice*`
found in the ShadowPCDisplay desktop binary. The binary detects VAAPI (Intel/AMD
Mesa) and the OpenGL renderer string, **but not NVML** on the Linux client side. The
"CUDA" mode that appears in the `/1/stats` body probably comes from the ffmpeg backend
which auto-selects CUDA NVDEC when it detects an NVIDIA GPU through
GL_VENDOR string match.

## Where the GPU info appears in the wire capture

**Seul endroit** : `POST /1/stats` body, sous `"video-pipeline"` metadata
JSON-escaped (= `tls_plain.log:L18785-L18792`) :

```json
{"video-pipeline": "{
  \"selected\": {
    \"chroma\": \"VC420\",
    \"codec\": \"H264\",
    \"decoder\": \"FFmpeg\",
    \"mode\": \"CUDA\",          <-- GPU info ici
    \"painter\": \"OpenGL\",
    \"renderer\": \"OpenGL\"
  },
  \"wanted\": { ... }
}"}
```

`"privacy": "PUBLIC"` = ELK ingestion telemetry pour dashboard operator.
**Never read by the streaming server to gate encoder behaviour.**

## `/1/clients` opaque blob = ZERO GPU info

Cross-check capture L13499-L13598 (`POST /1/clients` body opaque) :
```json
{"opaque":"{
  \"arch\":\"x86_64\",
  \"device-id\":\"<device-id 40hex>\",
  \"os\":\"linux\",
  \"os-name\":\"Ubuntu\",
  \"os-version\":\"24.04\",
  \"platform-type\":\"desktop\",
  \"timestamp\":\"2026-05-14 15:39:55\",
  \"version\":\"1.0.0\"
}", "type":"main"}
```

**ZERO GPU info, ZERO hwaccel, ZERO driver-version.** The Shadow server **does
does not know** which GPU/driver the client runs on, **so it cannot use it
as a discriminator** for the encoding mode.

Le hostname `ipv6-gpu-<instance>.frsbg01.compute.shadow.tech`
includes `gpu` because the SERVER's VM was provisioned with an NVIDIA Quadro
GPU tier (= predetermined by the user's account, not negotiated by the client).

## Wire ctrl-msg `Capabilities` = ZERO GPU field

Cross-checked against TIER 5-B1 and TIER 4-D: `ctrl-Capabilities` (= 87 B
a byte-exact match with the desktop) contains NO GPU/vendor/hwaccel field. The
server receives `version=6.1.7` and that is all. See
`memory/project_capabilities_full_byte_exact_RE.md` (C95) et
`memory/project_hwaccel_advertisement_RE.md` (C85).

## Verdict W2

**C95**: the whole GPU-detection apparatus on the desktop side is used
client-side only. **NEVER** sent to the streaming server. The hypothesis
"server gate encoder behavior sur GPU vendor client" RÉFUTÉE C95.

Our Switch client must **NOT** try to send that field — there is no
protobuf field for it on the ctrl wire nor inside the `/1/clients` opaque.

## Refs

- Strings GPU probe : `strings_display.txt:80, 178, 33448-33464, 35962, 36125, 37036, 37656, 37659`
- Capture `/1/clients` opaque : `tls_plain.log:L13499-L13598`
- Capture `/1/stats` video-pipeline : `tls_plain.log:L18674-L18802`
- TIER 9 doc : `tools/ida/out/TIER9_RE_2026-05-16.md` §W2
- Cross-ref Capabilities : `memory/project_capabilities_full_byte_exact_RE.md` (C95)
- Cross-ref HWaccel : `memory/project_hwaccel_advertisement_RE.md` (C85)
