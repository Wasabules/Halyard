#!/usr/bin/env python3
"""check-redistributable.py - refuse to ship what is not ours to ship.

This runs before a release is published. Its whole purpose is that automating
distribution must not automate an infringement: the moment a workflow uploads a
`.vpk`, whatever that package contains is being redistributed, at scale, by us.

What it looks for is content that came off a console or out of somebody else's
project. It reads the FILES, not a list of names - a font renamed `font.ttf`
would pass a name check and still be Nintendo's.
"""
import pathlib, re, struct, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def tracked() -> set:
    """The files git would actually publish.

    Scanning the FILESYSTEM was wrong and it showed: three of the six things
    this refused to publish were sitting in the working tree but gitignored -
    build artefacts of the RE hook, a plugin fetched locally. They would never
    have reached anyone. A gate that blocks a release for files nobody would
    receive is a gate someone eventually switches off.
    """
    out = subprocess.run(["git", "-C", str(ROOT), "ls-files", "-z"],
                         capture_output=True, text=True, check=True).stdout
    return {ROOT / p for p in out.split("\0") if p}

# Names inside a TTF/OTF `name` table that mean the file is not redistributable.
FONT_MARKERS = ("Nintendo", "Morisawa", "Sony", "PlayStation", "UD Shin Go")


def font_names(data: bytes) -> str:
    """The strings in a font's `name` table, or "" if it cannot be read."""
    try:
        n = struct.unpack(">H", data[4:6])[0]
        off = None
        for i in range(n):
            e = 12 + 16 * i
            if data[e:e + 4] == b"name":
                off = struct.unpack(">I", data[e + 8:e + 12])[0]
        if off is None:
            return ""
        cnt, so = struct.unpack(">HH", data[off + 2:off + 6])
        out = []
        for i in range(cnt):
            r = off + 6 + 12 * i
            pid, _eid, _lid, _nid, ln, o = struct.unpack(">HHHHHH", data[r:r + 12])
            raw = data[off + so + o:off + so + o + ln]
            try:
                out.append(raw.decode("utf-16-be") if pid == 3 else raw.decode("latin-1"))
            except Exception:
                pass
        return " | ".join(out)
    except Exception:
        return ""


def main() -> int:
    bad = []
    keep = tracked()

    # 1. Fonts, read from their own name table.
    for f in sorted(keep):
        if f.suffix != ".ttf":
            continue
        names = font_names(f.read_bytes())
        hit = [m for m in FONT_MARKERS if m.lower() in names.lower()]
        if hit:
            bad.append(f"{f.relative_to(ROOT)}: the font's own name table says "
                       f"{', '.join(hit)} -- console firmware content, not ours to ship")

    # 2. Compiled binaries of other people's projects.
    for f in sorted(keep):
        if f.suffix in (".skprx", ".suprx", ".nro", ".so", ".dll", ".prx", ".dylib"):
            rel = f.relative_to(ROOT)
            if "openorbis" in f.read_bytes()[:4096].lower().decode("latin-1"):
                continue          # OpenOrbis MIT stubs, listed in THIRD_PARTY_NOTICES
            if True:
                bad.append(f"{rel}: a compiled binary in the tree; ship source or "
                           f"point at upstream, do not redistribute someone else's build")

    # 3. The proprietary client, in any form.
    for f in sorted(keep):
        if f.name.startswith("ShadowPCDisplay"):
            bad.append(f"{f.relative_to(ROOT)}: Shadow's proprietary client. Never.")

    if bad:
        print("REFUSING to publish - these are not ours to redistribute:")
        for b in bad:
            print("  " + b)
        print("\nSee THIRD_PARTY_NOTICES.md. Replacing the fonts with OFL/CC0 ones is the fix;")
        print("do not simply delete them, the Vita and desktop builds load them by path.")
        return 1
    print("nothing unredistributable found in the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
