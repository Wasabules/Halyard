---
name: project-switch-ffmpeg-nvtegra-pin
description: "The Switch .nro links FFmpeg 7.1.5 + the nvtegra patch, never Envideo; until 2026-09-26 the CI pinned the broken envideo generation"
metadata:
  type: project
---

The working Switch decoder is **FFmpeg 7.1.5 + the `nvtegra` hwaccel** (averne's
older generation, as patched by Switchfin), built `--disable-autodetect
--disable-network --enable-nvtegra`. The `.nro` contains no Envideo code.
`patches/ffmpeg/nvtegra-7.1.5.patch` + the official tarball + the `configure`
line in `tools/build-ffmpeg-switch.sh` reproduce its libraries byte for byte.

**Why:** Averne's newer FFmpeg + Envideo (`envideo` hwaccel) takes ~5.2 s per
GPU->CPU transfer on this console and returns empty surfaces (KB §9,
2026-08-22). Yet `docs/BUILD.md`, `tools/pins.env`, the CI and the release's
corresponding source all still pinned it until 2026-09-26 (KB §9, FFMPEG-1): a
CI-built `.nro` would have silently fallen back to software decoding.

**How to apply:** never re-pin the decoder from a README - read what the binary
links (`nm halyard.elf | grep -c nvtegra`, `ffversion.h`). A decoder change needs
a console session on the CI-built `.nro`, not a green build. Related:
[[project-switch-first-video-s7-s14]].
