# Third-party notices

Halyard is distributed under **GPL-3.0-or-later** (see `LICENSE`). That is not a
preference: wolfSSL, which every TLS and DTLS channel goes through, is
GPL-3.0-or-later, and FFmpeg is built here with `--enable-gpl`. Both impose
copyleft on anything distributed with them, so the combined work can only be
GPLv3 or later.

Everything below is compatible with that, but **only in one direction**: an
Apache-2.0 or MPL-2.0 component may be taken into a GPLv3 work, never the
reverse. Replacing `LICENSE` with a permissive one would not make the binaries
distributable — it would make them undistributable.

Each component keeps its own licence and its own copyright. Nothing here is
relicensed.

## Linked into the shipped binaries

| Component | Licence | Where |
|---|---|---|
| [wolfSSL](https://github.com/wolfSSL/wolfssl) | GPL-3.0-or-later (or wolfSSL commercial) | `third_party/wolfssl/COPYING` |
| [FFmpeg](https://ffmpeg.org) 7.1.5 + the `nvtegra` hwaccel (averne, via Switchfin), `--enable-gpl` — Switch | GPL-2.0-or-later (the nvtegra code; FFmpeg itself LGPL-2.1-or-later) | release tarball pinned in `tools/pins.env` + `patches/ffmpeg/nvtegra-7.1.5.patch`, see `patches/ffmpeg/README.md` |
| [FFmpeg](https://ffmpeg.org) 7.1.5, FLAC decoder only — PS Vita | LGPL-2.1-or-later | the same pinned tarball, built by `tools/build-libs.sh vita ffmpeg` |
| [Borealis](https://github.com/xfangfang/borealis) | Apache-2.0 | `third_party/borealis/LICENSE`, text in `licenses/apache-2.0.txt` |
| [libopus](https://opus-codec.org) | BSD-3-Clause | `third_party/libopus/COPYING` |
| [Material Icons](https://github.com/google/material-design-icons) | Apache-2.0 | `resources/material/LICENSE.txt` |
| [Inter](https://rsms.me/inter/) | SIL OFL 1.1 | `resources/font/LICENSE-Inter.txt` (the app); `site/fonts/LICENSE-Inter.txt` (the website, Inter 4.1 variable) |
| [Mozilla CA bundle](https://curl.se/docs/caextract.html) | MPL-2.0 | `resources/cacert.pem` |
| The UI sound set (`resources/sfx/`, 17 files) | generated for this project | see below |
| [libcurl](https://curl.se) | curl (MIT-style) | Switch: devkitPro's, over the console's own TLS service (no TLS library linked). PS Vita: 8.17.0 rebuilt on Mbed TLS by `tools/build-libs.sh vita curl` |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) 3.6.5 — PS Vita, under libcurl | Apache-2.0 OR GPL-2.0-or-later | as shipped by vitasdk |
| zlib, [zstd](https://github.com/facebook/zstd) | Zlib / BSD-3-Clause (or GPL-2.0) | as shipped by devkitPro / vitasdk |
| [jansson](https://github.com/akheron/jansson) | MIT | devkitPro on Switch; rebuilt from v2.14 for the Vita (`tools/bootstrap-libs.sh`) |

**No OpenSSL in any shipped binary (2026-09-26).** Until that date the PS Vita
package linked the vitasdk's libcurl, built on OpenSSL 1.0.2 - whose licence's
advertising clause the GPL does not allow - and could not be redistributed.
libcurl is now rebuilt for the Vita on Mbed TLS (same curl version, the
vitasdk's own options), and the ELF was checked by symbol and by string: no
OpenSSL code remains. The Switch never linked it: its libcurl uses the
console's TLS service. The Vita's FFmpeg is likewise ours since that date - the
vitasdk's brought LAME and mpg123 with it, at versions nothing pinned - so
every LGPL component shipped has its exact source in the release bundle.
`build-psvita.yml` checks all of this on the ELF of every build.

## Inside the console binaries through Borealis and the SDKs

Found by symbol in the `.nro` and `.vpk` ELFs on 2026-09-26, not from the build
files. All permissive; their notices travel with every release, in the
corresponding-source bundle (`third_party/borealis/library/lib/extern/*/`).

| Component | Licence | In |
|---|---|---|
| [NanoVG](https://github.com/memononen/nanovg) — the UI's vector renderer | Zlib | both |
| [Yoga](https://github.com/facebook/yoga) — layout | MIT | both |
| [{fmt}](https://github.com/fmtlib/fmt) | MIT | both |
| [TinyXML-2](https://github.com/leethomason/tinyxml2) | Zlib | both |
| [stb_image](https://github.com/nothings/stb) | MIT or public domain | both |
| [libretro-common](https://github.com/libretro/libretro-common) | MIT | Vita |
| [libromfs](https://github.com/WerWolv/libromfs), [libpulsar](https://github.com/p-sam/switch-libpulsar) | MIT | Switch |
| glad (generated OpenGL loader) | public domain / MIT | Switch |
| [GLFW](https://www.glfw.org) (devkitPro's Switch port) | Zlib | Switch |
| [Mesa](https://mesa3d.org) — EGL, the GL dispatch and nouveau (devkitPro) | MIT | Switch |
| [libnx](https://github.com/switchbrew/libnx) | ISC | Switch |
| vitasdk runtime (newlib, the system-call stubs) | BSD / MIT | Vita |

## Linked only into the desktop build (never distributed)

| Component | Licence |
|---|---|
| [libqrencode](https://fukuchi.org/works/qrencode/) — the sign-in QR code | LGPL-2.1-or-later |
| ALSA, OpenSSL 3, GLFW (system packages) | LGPL-2.1 / Apache-2.0 / Zlib |

## The application's own artwork

The logo, the icons (`resources/icon/`, `resources/img/icon.jpg`, the PS Vita
and PS4 `sce_sys/icon0.png`, the Windows `.ico` and macOS `.icns` under
`clients/borealis/packaging/`), the PS Vita LiveArea art and the menu
backgrounds (`resources/img/background.jpg`, `background_vita.jpg`) are
**Halyard's own**, since 2026-09-26. The
masters are in `clients/borealis/branding/` (the logo as SVG and PNG exports,
the background as a JPEG); `tools/brand-assets.py` derives every packaged file
from them. They replace Borealis' demo artwork, which the builds shipped until
then as placeholders (the LiveArea start gate read "borealis").

The two background masters (`background.jpeg`, `psvita_background.jpeg`) were
made with Google's generative AI and keep the C2PA provenance manifest it
embedded ("Created by Google Generative AI", SynthID watermark) - it names the
tool, not a person, and is left in place on purpose. The derived images in
`resources/` carry no metadata.

**Still Borealis' demo artwork**: the Android and iOS launcher icons under
`clients/borealis/android-project/` and `clients/borealis/ios/`. Neither target
is built or released.

## Assets made for this project

**The UI sounds** (`resources/sfx/`, and the unprocessed originals they were
normalised from in `clients/borealis/sfx_original/`) were **generated with ElevenLabs under a
paid subscription**, whose terms grant the subscriber commercial rights to the
output. They are not sampled from any console or existing product.
`docs/UI_SOUNDS.md` holds the design specification they were produced from — one
instrument across all fourteen, because consistency of timbre is what separates
a designed set from an assembled one.

Recording this matters more than it looks: "where did these seventeen WAV files
come from" is the first question a reviewer asks about shipped audio, and an
answer that exists nowhere is what makes an honest asset look risky.

**What used to be here and is not any more (2026-09-13)**: eighteen Pokémon
sprites, a tile sheet and thirteen system icons, 1.7 MB in total, inherited from
Borealis' demo application. Nothing in Halyard referenced them - only Borealis'
own demo, which this project does not build - yet they shipped inside both the
`.nro` and the `.vpk`. They are Nintendo and Game Freak artwork; removing them
cost nothing and took 1.7 MB off both packages.

## Not redistributed, and deliberately so

- **`recon/ShadowPCDisplay`** — the official Shadow desktop client. It is the
  primary reverse-engineering instrument and it is **proprietary**: it is not in
  this repository and must never be. Bring your own copy from Shadow.
- **[vitacompanion](https://github.com/devnoname120/vitacompanion)** — the
  PS Vita FTP/command plugin `tools/vita-push.sh` drives. Install it from
  upstream; a prebuilt `.skprx` is not ours to hand out.
- **Captures, packet traces and session logs.** They carry live credentials.
  See `SECURITY.md`.

## Trademarks

Shadow is a trademark of its owner. Nintendo Switch is a trademark of Nintendo.
PlayStation Vita is a trademark of Sony Interactive Entertainment. Those names
appear in this project only to state which service the client speaks to and
which hardware it runs on — a descriptive, nominative use. **This project is not
affiliated with, endorsed by, or sponsored by any of them.**
