# FFmpeg for the Switch: 7.1.5 + the `nvtegra` hardware decoder

`nvtegra-7.1.5.patch` turns the official FFmpeg 7.1.5 release into the FFmpeg the
Switch `.nro` is built with. `tools/build-ffmpeg-switch.sh` applies it and runs
the exact `configure` line; `tools/pins.env` pins the tarball by SHA-256.

## Where it comes from

The patch was not written here. It is the set Switchfin applies to FFmpeg 7.1.5
for its own Switch build (`scripts/switch/ffmpeg/` in that project), EXTRACTED
from the tree that produced our working `.nro` (built 2026-08-22..25) by
diffing it against the pristine tarball. So it is exactly what was compiled,
not a re-derivation.

Proven on 2026-09-26: the official tarball (SHA-256 `de668509…d558f`) + this
patch + the recorded `configure` line give `libavcodec.a`, `libavutil.a`,
`libavformat.a`, `libswscale.a` and `libswresample.a` **identical byte for byte**
to the ones the `.nro` was linked against.

What it contains:

| Part | Author / licence | Compiled here? |
|---|---|---|
| the `nvtegra` hwaccel: `libavutil/hwcontext_nvtegra.*`, `nvtegra.*`, the NVDEC/VIC/host1x headers, `libavcodec/nvtegra_*.c`, `libavfilter/*nvtegra*` | averne, GPL-2.0-or-later | yes (the reason FFmpeg is built `--enable-gpl`) |
| hooks in `h264_slice.c`, `hevcdec.c`, `hwaccels.h`, `pixfmt.h`, `configure`, … | averne / Switchfin, under FFmpeg's LGPL-2.1-or-later | yes |
| `libavformat/libsmb2.c`, `libssh2.c`, `getnameinfo` shim, `avio.h` export | Switchfin contributors ("libsmb2 support by proconsule", derived from FFmpeg's LGPL `libsmbclient.c`) | **no** - `--disable-network` |

## Why not Averne's own FFmpeg + Envideo

Averne's newer generation moved the hardware access into a separate library,
Envideo, and renamed the hwaccel `envideo`. On this console it was unusable:
every GPU->CPU transfer took ~5.2 s (the nvgpu watchdog) and returned an EMPTY
surface. The older `nvtegra` generation goes through the VIC instead: 1-2 ms.
KB.md §9, 2026-08-22. Until 2026-09-26 the CI still built the `envideo`
generation, so a CI-built `.nro` would have fallen back to software decoding.
