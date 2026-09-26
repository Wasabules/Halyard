---
name: project-nvdec-hardware-path
description: "DECISIVE 2026-05-14 ~17:30 — Shadow REQUIRES a hardware H.264 decoder (= the official documentation says so). Software libavcodec does not work. The solution is Averne's FFmpeg fork with the `nvtegra` hwaccel for the Switch's Tegra X1 NVDEC. Used in production by Moonlight-Switch + libnxbox."
metadata: 
  node_type: memory
  type: project
---

## The root-cause problem

Shadow.tech doc officielle (= [HEVC support article](https://support.shadow.tech/hc/en-us/articles/32731838839697-Using-High-Efficiency-Video-Coding-H-265)) :

> "The Shadow application requires your machine to support hardware decoding for H.264 or H.265, **it will not work with software decoding**."

Our stack uses **software** libavcodec on Linux and Switch. So we are in the blind spot by design: 30 % top + 70 % stretched = an inevitable ceiling with that stack.

## Solution = Averne's FFmpeg fork

**URL** : `https://github.com/averne/FFmpeg`

**Characteristics**:
- A userspace NVDEC driver for the Switch (= no need for the nvidia system libs)
- hwaccel name : `nvtegra`
- Supporte : H.264, HEVC, VP8, VP9, MPEG1/2/4, VC1, JPEG
- Dynamic frequency scaling
- Hardware-accelerated frame transfer
- Patches FFmpeg upstream submission : [mai 2024](https://patchwork.ffmpeg.org/project/ffmpeg/cover/cover.1717083799.git.averne381@gmail.com/)

**Config build pour Switch** :
```bash
source /opt/devkitpro/switchvars.sh
./configure --cross-prefix=aarch64-none-elf- \
            --enable-cross-compile \
            --arch=aarch64 \
            --cpu=cortex-a57 \
            --target-os=horizon \
            --enable-pic \
            --enable-gpl \
            --enable-nvtegra
make -j$(nproc)
```

## Used in production by

- **Moonlight-Switch** (= XITRIX/Moonlight-Switch) — NVIDIA GameStream client
- **libnxbox** (= ursusworks/libnxbox) — Xbox Cloud Streaming client

Both projects rely on the same Averne FFmpeg fork + the nvtegra hwaccel for H.264/HEVC hardware decoding. The source is available to consult for integration patterns.

## Implications pour shadow2switch

**Path forward** :
1. Cloner `github.com/averne/FFmpeg`
2. Build aarch64-none-elf target (= devkitpro switch cross-compile)
3. Replace our build_switch's FFmpeg libraries with that version
4. Modifier `h264_decoder.c` : utiliser `av_hwdevice_create(AV_HWDEVICE_TYPE_NVTEGRA)` + `av_hwframe_transfer_data`
5. Test: NVDEC handles the top/bottom multi-slice natively = the bottom is preserved through hardware reference-frame management

**Estimated effort**: 1-3 days of work (= build setup + integrating the FFmpeg hwaccel API + testing).

**Not applicable on a Linux desktop** (= no Tegra X1). On Linux the path would be VAAPI, but that is for local testing only, not the Switch target.

## Cross-references

- [[project-image-ceiling-reached]] — why we were stuck at 30%
- [[project-sufp-fec-parity-found]] — the chacha20 wire format validated
- [[project-ctrl-oneof-enum-verified]] — Toutes ctrl msgs byte-exact desktop
