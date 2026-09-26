---
name: project-n54-ffmpeg-nvdec-built
description: "DECISIVE 2026-05-14 17:40 — Averne's FFmpeg fork (the envideo branch) + the Envideo library cross-compiled for Switch, DONE. shadow-client.nro rebuilt at 13 MB with the NVDEC hwaccel. The `envideo_device_create` symbol is present in the elf. What remains is a live Switch test."
metadata: 
  node_type: memory
  type: project
---

## Build accompli

### 1. Envideo lib (= NVDEC userspace driver)
```bash
cd $REPO/07-averne-ffmpeg/Envideo
meson setup build-switch --cross-file misc/switch-crossfile.txt \
    -Dnvgpu=enabled -Dtests=false -Dtegra-drm=false --default-library=static
meson compile -C build-switch
# Output: libenvideo.a (≈400KB static lib)
```
Installed: `/opt/devkitpro/portlibs/switch/lib/libenvideo.a` + `/opt/devkitpro/portlibs/switch/include/envideo/`

### 2. envideo.pc (custom prefix + C++ runtime)
```
prefix=/opt/devkitpro/portlibs/switch
Libs: -L${libdir} -lenvideo -lstdc++
Cflags: -I${includedir}
```

### 3. Averne FFmpeg (envideo branch, c8ff0ba)
```bash
cd $REPO/07-averne-ffmpeg/FFmpeg
./configure --cross-prefix=aarch64-none-elf- --enable-cross-compile \
    --arch=aarch64 --cpu=cortex-a57 --target-os=horizon \
    --enable-pic --enable-gpl --enable-envideo \
    --disable-programs --disable-doc --disable-everything \
    --enable-decoder=h264 --enable-decoder=hevc \
    --enable-parser=h264 --enable-parser=hevc \
    --enable-hwaccel=h264_envideo --enable-hwaccel=hevc_envideo \
    --extra-cflags="-D__SWITCH__ -fPIC -I/opt/devkitpro/portlibs/switch/include -isystem /opt/devkitpro/libnx/include" \
    --extra-ldflags="-fPIC -L/opt/devkitpro/portlibs/switch/lib -L/opt/devkitpro/libnx/lib -specs=/opt/devkitpro/libnx/switch.specs" \
    --extra-ldexeflags="-fPIC" \
    --prefix=/opt/devkitpro/portlibs/switch
make -j12
```

**Patch needed**: `libavcodec/envideo_decode.c:86` — `dev_info.is_tegra` → `dev_info.tegra_layout` (= a rename in a recent Envideo).

Installed (by a manual sudo cp; make install does not work):
- `/opt/devkitpro/portlibs/switch/lib/libavcodec.a` (19MB, contient 84 symboles envideo)
- `/opt/devkitpro/portlibs/switch/lib/libavformat.a`
- `/opt/devkitpro/portlibs/switch/lib/libavutil.a` (contient AV_HWDEVICE_TYPE_NVTEGRA)
- `/opt/devkitpro/portlibs/switch/lib/libswscale.a`
- `/opt/devkitpro/portlibs/switch/lib/libswresample.a`
- Headers `/opt/devkitpro/portlibs/switch/include/libavcodec/*.h`

Backups stock dans `*.stock-bak`.

### 4. shadow-client CMakeLists patch (= ligne 176-177)
```cmake
avformat avcodec swresample swscale avutil
envideo stdc++  # NVDEC hardware accel (Averne envideo lib + C++ runtime)
```

### 5. h264_decoder.c (= already prepared for NVTEGRA)
The code already exists under `#ifdef __SWITCH__` (= lines 282-295):
- `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_NVTEGRA, ...)`
- `get_hw_format` retourne `AV_PIX_FMT_NVTEGRA`
- Frame transfer through `av_hwframe_transfer_data` at on_frame

Before N54: missing symbols, the stock library had no NVTEGRA. Now it links fine.

## Verification linker

```bash
nm shadow-client.elf | grep envideo_device_create
# Output: 00000000000751a0 T envideo_device_create
```

## Next step

Push `shadow-client.nro` (13 MB, $REPO/05-shadow-client-borealis/build_switch/) to the Switch through `gio` (per [[reference-switch-dev-workflow]]) and test live. If NVDEC initialises correctly, the multi-slice top/bottom picture should be handled natively.

## Risques connus

1. The `nvgpu` driver may need a `pcvSetClockRate` boost to run NVDEC at full speed
2. `tegra-drm=false` disables the DRM path (= Linux4Tegra); we keep nvgpu only (= Switch HorizonOS)
3. Si init NVDEC fail → code fallback CPU decode (= retombera sur stack actuelle ~30% image)
4. devkitA64 gcc 15.2 + FFmpeg = untested in Averne's CI, may produce warnings

## Cross-references

- [[project-nvdec-hardware-path]] — Pourquoi NVDEC est THE solution
- [[project-image-100pct-proof]] — the bottom-slice ratio that NVDEC should handle
