#!/usr/bin/env python3
"""brand-assets.py - derive every packaged image from the branding masters.

The masters live in clients/borealis/branding/ (the logo as SVG and as PNG
exports, the background as a large JPEG) and are NOT packaged: resources/ is
copied whole into the .nro and the .vpk, so only what this script writes there
ships. Run it after changing a master, then commit its outputs:

    python3 tools/brand-assets.py

Needs Pillow. Every output is rebuilt from scratch, so the result depends only
on the masters and on this file.

What each target demands, and why the format is what it is:
  - Switch   resources/img/icon.jpg            256x256 JPEG (elf2nro --icon)
  - desktop  resources/icon/icon.png           256x256 PNG  (the window icon)
  - PS Vita  psv/sce_sys/icon0.png             128x128 8-bit indexed PNG
             psv/sce_sys/livearea/contents/bg.png       840x500 8-bit indexed
             psv/sce_sys/livearea/contents/startup.png  280x158 8-bit indexed
             The Vita's packager refuses true-colour PNGs in sce_sys.
  - PS4      ps4/sce_sys/icon0.png             512x512 8-bit indexed PNG
  - Windows  packaging/halyard.ico             16-256 px, the .exe's icon
  - macOS    packaging/halyard.icns            the app bundle's icon
  - Linux    resources/icon/icon.png again: tools/linux-desktop-entry.sh
             installs it with the .desktop entry the dock needs
  - in-app   resources/img/background.jpg      1280x720 JPEG, drawn by
             ui::paint::shadowBg behind every menu screen
             resources/img/background_vita.jpg  960x544 JPEG, the PS Vita's
             own variant (psvita_background.jpeg: brighter, more contrast),
             also the source of the Vita's LiveArea images
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "clients", "borealis", "branding")
BG_DARK = (26, 27, 33)          # the logo's own background, #1A1B21
TEXT = (232, 234, 244)          # the logo's light bar, #E8EAF4


def src(*p):
    return os.path.join(SRC, *p)


def out(*p):
    path = os.path.join(REPO, *p)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return path


def cover(im, w, h):
    """Scale to cover w x h, then crop the centre - no stretching, no bars."""
    scale = max(w / im.width, h / im.height)
    im = im.resize((round(im.width * scale), round(im.height * scale)), Image.LANCZOS)
    left, top = (im.width - w) // 2, (im.height - h) // 2
    return im.crop((left, top, left + w, top + h))


def indexed(im):
    """8-bit palette PNG, as the Vita and PS4 packagers require."""
    return im.convert("RGB").quantize(colors=256, method=Image.MEDIANCUT,
                                      dither=Image.FLOYDSTEINBERG)


def logo(size):
    return Image.open(src("png", f"logo-{size}x{size}.png")).convert("RGB")


def main():
    written = []

    def save(im, rel, **kw):
        path = out(*rel.split("/"))
        im.save(path, **kw)
        written.append((rel, im.size, im.mode, os.path.getsize(path)))

    background = Image.open(src("background.jpeg")).convert("RGB")
    vita_bg = Image.open(src("psvita_background.jpeg")).convert("RGB")

    save(cover(background, 1280, 720), "resources/img/background.jpg",
         quality=85, optimize=True, progressive=True)
    save(cover(vita_bg, 960, 544), "resources/img/background_vita.jpg",
         quality=85, optimize=True, progressive=True)
    save(logo(256), "resources/img/icon.jpg", quality=92, optimize=True)
    save(logo(256), "resources/icon/icon.png", optimize=True)
    save(indexed(logo(128)), "clients/borealis/psv/sce_sys/icon0.png", optimize=True)
    save(indexed(logo(512)), "clients/borealis/ps4/sce_sys/icon0.png", optimize=True)
    big = Image.open(src("png", "logo-512x512.png")).convert("RGBA")
    save(big, "clients/borealis/packaging/halyard.ico",
         sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    save(big, "clients/borealis/packaging/halyard.icns")
    save(indexed(cover(vita_bg, 840, 500)),
         "clients/borealis/psv/sce_sys/livearea/contents/bg.png", optimize=True)

    # The start gate: the mark on the left, the name beside it, on the
    # background's own darkness.
    gate = cover(vita_bg, 280, 158)
    mark = logo(256).resize((118, 118), Image.LANCZOS)
    gate.paste(mark, (16, (158 - 118) // 2))
    draw = ImageDraw.Draw(gate)
    font = ImageFont.truetype(os.path.join(REPO, "resources", "font", "font.ttf"), 34)
    name = "Halyard"
    box = draw.textbbox((0, 0), name, font=font)
    tx = 16 + 118 + 12
    ty = (158 - (box[3] - box[1])) // 2 - box[1]
    draw.text((tx, ty), name, font=font, fill=TEXT)
    save(indexed(gate), "clients/borealis/psv/sce_sys/livearea/contents/startup.png",
         optimize=True)

    for rel, size, mode, nbytes in written:
        print(f"  {rel:58s} {size[0]}x{size[1]} {mode:4s} {nbytes // 1024} KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
