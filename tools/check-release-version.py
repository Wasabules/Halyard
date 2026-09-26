#!/usr/bin/env python3
"""check-release-version.py - read the version INSIDE the packages, and refuse
a release whose binaries do not announce the tag.

    tools/check-release-version.py 0.1.0 --nro halyard.nro --vpk halyard.vpk

The version comes from the git tag at configure time (CMakeLists.txt, "THE
VERSION IS THE TAG"). A build that could not see the tag - a shallow checkout,
a stale build directory - stamps "0.0.0-dev" or "X.Y.Z-dev" instead, and
nothing else would notice: the workflow would publish, under the right tag, a
package that says otherwise on the console. So the check reads what a console
reads:

  - .nro  the NACP's display version (what the homebrew menu shows)
  - .vpk  param.sfo's APP_VER (what the Vita compares on install) and the
          LiveArea bubble's text (what the user sees)

Standard library only: it runs on a bare runner.
"""
import argparse
import io
import re
import struct
import sys
import zipfile


def nro_version(path):
    d = open(path, "rb").read()
    if d[0x10:0x14] != b"NRO0":
        raise ValueError("not an NRO")
    aset = struct.unpack_from("<I", d, 0x18)[0]
    if d[aset:aset + 4] != b"ASET":
        raise ValueError("no asset section")
    _, _, nacp_off, nacp_size, _, _ = struct.unpack_from("<QQQQQQ", d, aset + 8)
    nacp = d[aset + nacp_off:aset + nacp_off + nacp_size]
    # NACP: 16 language entries of 0x300 bytes, then... the display version
    # string sits at 0x3060, 16 bytes, NUL-padded.
    return nacp[0x3060:0x3070].split(b"\0")[0].decode("utf-8")


def sfo_fields(blob):
    magic, _ver, key_start, data_start, count = struct.unpack_from("<4sIIII", blob, 0)
    if magic != b"\0PSF":
        raise ValueError("not a param.sfo")
    out = {}
    for i in range(count):
        k_off, fmt, length, _max, d_off = struct.unpack_from("<HHIII", blob, 20 + 16 * i)
        key = blob[key_start + k_off:].split(b"\0")[0].decode()
        raw = blob[data_start + d_off:data_start + d_off + length]
        out[key] = raw.split(b"\0")[0].decode("utf-8") if fmt in (0x0004, 0x0204) \
            else struct.unpack("<I", raw)[0]
    return out


def vpk_versions(path):
    with zipfile.ZipFile(path) as z:
        sfo = sfo_fields(z.read("sce_sys/param.sfo"))
        live = z.read("sce_sys/livearea/contents/template.xml").decode("utf-8")
    return sfo.get("APP_VER"), live


def psn_version(v):
    """The CMakeLists.txt encoding of X.Y.Z into the Vita's "MM.mm"."""
    major, minor, patch = (int(x) for x in v.split("."))
    mm = f"{major:02d}"
    if minor < 10 and patch < 10:
        return f"{mm}.{minor}{patch}"
    return f"{mm}.{minor:02d}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("version", help="X.Y.Z, the tag without its v")
    ap.add_argument("--nro")
    ap.add_argument("--vpk")
    a = ap.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+", a.version):
        sys.exit(f"not a release version: {a.version!r} (want X.Y.Z)")
    if not (a.nro or a.vpk):
        sys.exit("nothing to check: give --nro and/or --vpk")

    bad = 0

    def report(what, got, want):
        nonlocal bad
        ok = got == want
        bad += not ok
        print(f"  {'OK  ' if ok else 'FAIL'} {what:28s} {got!r}" + ("" if ok else f"  (want {want!r})"))

    if a.nro:
        print(a.nro)
        report("NACP display version", nro_version(a.nro), a.version)
    if a.vpk:
        print(a.vpk)
        app_ver, live = vpk_versions(a.vpk)
        report("param.sfo APP_VER", app_ver, psn_version(a.version))
        shown = re.search(r"Halyard (\S+)</str>", live)
        report("LiveArea bubble", shown.group(1) if shown else None, a.version)

    if bad:
        sys.exit(f"{bad} package field(s) do not announce {a.version}")
    print(f"every package announces {a.version}")


if __name__ == "__main__":
    main()
