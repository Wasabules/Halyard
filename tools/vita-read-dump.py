#!/usr/bin/env python3
"""Read a PS Vita core dump (.psp2dmp) and say where the application died.

    tools/vita-read-dump.py <dump.psp2dmp> [--elf build_psv/halyard]

A crash on this console leaves `ux0:data/<title>/psp2core-*.psp2dmp`
(`tools/vita-push.sh dumps` pulls them). The file is gzip around an ARM ELF
core whose PT_NOTE segment carries Sony's own notes. Nothing in vitasdk reads
it, so before this script a dump was a 400 KB binary nobody opened.

=== THE THREE THINGS THAT MAKE IT WRONG, EACH PAID FOR ==================

1. THE MODULE DOES NOT LOAD AT ITS LINK ADDRESS. `halyard.velf` is linked at
   0x81000000 and the loader put it at 0x81079000 - so every address is
   0x79000 off. `addr2line` does not fail on a wrong address: it returns the
   nearest preceding symbol, which in a 22 MB binary is a literal pool
   hundreds of kilobytes away. It answers with plausible, false names. The
   base is read from MODULE_INFO here and every address is rebased through it.

2. THE RECORD COUNT IS THE SECOND WORD of a note, not the first. Reading the
   first gave 17 threads where there are 5, and the extra "threads" were
   whatever followed in the buffer.

3. WITHOUT DWARF THERE IS NO LINE, AND BARELY A NAME. The release build links
   with `.symtab` only. Build with `-g` (`cmake -DCMAKE_C_FLAGS=-g
   -DCMAKE_CXX_FLAGS=-g`) BEFORE reproducing, or accept function names that
   are only approximately right.

A Thumb address has bit 0 set; it is cleared before resolving.

=== WHAT IT PRINTS ======================================================

The TTY capture (the console's own log ring, which holds what was printed in
the seconds before the dump - often the whole answer), the modules with their
real load addresses, each thread's registers resolved to functions, and the
main thread's stack walked for return addresses. The stack walk is a SCAN, not
a frame-pointer unwind: it prints every word that could be a return address
into our text, in stack order. Some are stale. It is a list of candidates that
reads like a backtrace, not a proof of one - the real call chain is the
subsequence that makes sense, and that is a judgement the reader makes.
"""
import argparse, gzip, os, re, struct, subprocess, sys

TEXT_GUESS = 0x81000000          # what the .velf is linked at


def read_core(path):
    """Returns the raw ELF bytes: the dump is gzip, sometimes already expanded."""
    with open(path, "rb") as f:
        head = f.read(2)
    if head == b"\x1f\x8b":
        with gzip.open(path, "rb") as f:
            return f.read()
    return open(path, "rb").read()


def parse(buf):
    """Notes by name (largest wins - a name can repeat, once empty) and the
    PT_LOAD segments, which is where the stacks live."""
    phoff = struct.unpack_from("<I", buf, 28)[0]
    phes = struct.unpack_from("<H", buf, 42)[0]
    phn = struct.unpack_from("<H", buf, 44)[0]
    notes, loads = {}, []
    for i in range(phn):
        t, off, va, _pa, fsz = struct.unpack_from("<IIIII", buf, phoff + i * phes)
        if t == 1 and fsz:
            loads.append((va, off, fsz))
        if t != 4:
            continue
        pos, end = off, off + fsz
        while pos + 12 <= end:
            nsz, dsz, _nt = struct.unpack_from("<III", buf, pos)
            name = buf[pos + 12: pos + 12 + nsz].split(b"\0")[0].decode("latin1")
            dpos = pos + 12 + ((nsz + 3) & ~3)
            data = buf[dpos: dpos + dsz]
            if len(data) > len(notes.get(name, b"")):
                notes[name] = data
            pos = dpos + ((dsz + 3) & ~3)
    return notes, loads


def records(data):
    """[?][COUNT][record size] then fixed-size records. The count is the
    SECOND word; taking the first is how this reader first invented threads."""
    if len(data) < 12:
        return []
    _u, count, rsz = struct.unpack_from("<III", data, 0)
    if not rsz:
        return []
    return [data[12 + i * rsz: 12 + (i + 1) * rsz]
            for i in range(count) if 12 + (i + 1) * rsz <= len(data)]


def modules(notes):
    """MODULE_INFO has variable-shaped records; the stride is fixed at 0x90 and
    the name sits at +0x20. Returns (name, [(addr, size)]) per module."""
    d = notes.get("MODULE_INFO", b"")
    if len(d) < 12:
        return []
    count = struct.unpack_from("<I", d, 4)[0]
    out, stride = [], 0x90
    for i in range(count):
        o = 12 + i * stride
        if o + stride > len(d):
            break
        name = d[o + 0x20: o + 0x40].split(b"\0")[0].decode("latin1", "replace")
        segs = []
        for k in range(0x44, stride - 4, 4):
            a = struct.unpack_from("<I", d, o + k)[0]
            sz = struct.unpack_from("<I", d, o + k + 4)[0]
            if 0x81000000 <= a < 0x90000000 and 0 < sz < 0x2000000:
                segs.append((a, sz))
        out.append((name, segs))
    return out


def elf_text_size(elf):
    """The size of the executable's first PT_LOAD, read from the ELF itself.
    This is the key the module list is matched against - guessing "the biggest
    segment" picks the 25 MB DATA segment and rebases everything into the
    heap, which resolves to nothing at all."""
    try:
        buf = open(elf, "rb").read(4096)
    except OSError:
        return None
    if buf[:4] != b"\x7fELF":
        return None
    phoff = struct.unpack_from("<I", buf, 28)[0]
    phes = struct.unpack_from("<H", buf, 42)[0]
    phn = struct.unpack_from("<H", buf, 44)[0]
    for i in range(phn):
        t, _off, _va, _pa, fsz, _msz, flg, _al = struct.unpack_from(
            "<IIIIIIII", buf, phoff + i * phes)
        if t == 1 and (flg & 1):            # PT_LOAD, executable
            return fsz
    return None


def app_base(mods, want_text):
    """The application's TEXT segment, identified by its size.

    The loader does not place the module where it was linked, and the address
    is not the same from one launch to the next (0x81079000, then 0x81022000
    on the very next run) - so this must be read from every dump, never
    remembered. The segment is matched by size against the ELF, within a few
    kilobytes: the loader pads, so equality does not hold."""
    best = None
    for _name, segs in mods:
        for addr, sz in segs:
            if want_text and abs(sz - want_text) < 0x10000:
                return (addr, sz)
            if sz > 0x400000 and (best is None or sz < best[1]):
                best = (addr, sz)   # smallest of the large ones = text, not data
    return best


class Resolver:
    def __init__(self, elf, base):
        self.elf, self.base = elf, base
        self.off = (base - TEXT_GUESS) if base else 0
        self.tool = None
        for cand in ("arm-vita-eabi-addr2line",
                     os.path.expanduser("~/vitasdk/bin/arm-vita-eabi-addr2line")):
            if subprocess.run(["which", cand], capture_output=True).returncode == 0 \
               or os.path.exists(cand):
                self.tool = cand
                break

    def __call__(self, addr):
        if not (self.tool and self.elf and os.path.exists(self.elf)):
            return ""
        a = (addr & ~1) - self.off
        try:
            r = subprocess.run([self.tool, "-e", self.elf, "-f", "-C", f"{a:#x}"],
                               capture_output=True, text=True, timeout=20)
        except Exception:
            return ""
        parts = [p.strip() for p in r.stdout.splitlines() if p.strip()]
        if not parts or parts[0] == "??":
            return ""
        loc = parts[1] if len(parts) > 1 and parts[1] != "??:0" else ""
        loc = re.sub(r"^.*/(?=[^/]+$)", "", loc)
        return f"{parts[0]}" + (f"  [{loc}]" if loc else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--elf", default="build_psv/halyard",
                    help="the LINKED executable (not the .velf/.self), built with -g")
    ap.add_argument("--stack-words", type=int, default=1024)
    ap.add_argument("--no-tty", action="store_true")
    a = ap.parse_args()

    buf = read_core(a.dump)
    if buf[:4] != b"\x7fELF":
        sys.exit(f"{a.dump}: not a core (a .tmp dump is truncated and unreadable)")
    notes, loads = parse(buf)
    mods = modules(notes)
    want = elf_text_size(a.elf)
    base = app_base(mods, want)
    res = Resolver(a.elf, base[0] if base else 0)

    print("=== the console's own log, just before the dump ===")
    if not a.no_tty:
        for key in ("TTY_INFO2", "TTY_INFO"):
            d = notes.get(key, b"")
            if not d.strip(b"\0"):
                continue
            txt = "".join(c if (32 <= ord(c) < 127 or c == "\n") else "."
                          for c in d.decode("latin1", "replace"))
            lines = [l for l in re.sub(r"\.{3,}", " ", txt).splitlines() if l.strip()]
            for l in lines[-25:]:
                print("  " + l)
            break

    print("\n=== modules ===")
    for name, segs in mods:
        if segs:
            print(f"  {name or '(the application)':<24} "
                  + "  ".join(f"{x:#010x}+{s:#x}" for x, s in segs[:2]))
    if base:
        print(f"\n  application text at {base[0]:#010x} (linked {TEXT_GUESS:#010x}"
              f", so every address is rebased by {base[0]-TEXT_GUESS:#x})")
    else:
        print("\n  !! application base NOT found - addresses below are NOT rebased")

    names = {}
    for r in records(notes.get("THREAD_INFO", b"")):
        names[struct.unpack_from("<I", r, 0)[0]] = \
            r[4:36].split(b"\0")[0].decode("latin1", "replace")

    print("\n=== threads ===")
    main_sp = None
    for r in records(notes.get("THREAD_REG_INFO", b"")):
        tid = struct.unpack_from("<I", r, 0)[0]
        g = [struct.unpack_from("<I", r, 4 + 4 * i)[0] for i in range(16)]
        cpsr = struct.unpack_from("<I", r, 0x44)[0]
        nm = names.get(tid, "?")
        print(f"\n  {nm}  ({tid:#x})  cpsr={cpsr:#010x}")
        for label, v in (("pc", g[15]), ("lr", g[14]), ("sp", g[13]), ("r0", g[0])):
            s = res(v) if base and base[0] <= v < base[0] + base[1] else ""
            sysm = "  (a system module)" if 0xe0000000 <= v < 0xf0000000 else ""
            print(f"    {label} {v:#010x}  {s}{sysm}")
        if main_sp is None:
            main_sp = g[13]

    if main_sp is not None and base:
        print(f"\n=== the main thread's stack from {main_sp:#010x} ===")
        print("  (a scan, not an unwind: stale words appear too)")
        seg = next(((va, off, sz) for va, off, sz in loads
                    if va <= main_sp < va + sz), None)
        if not seg:
            print("  the stack is in no dumped segment")
            return
        va, off, sz = seg
        seen = set()
        for i in range(a.stack_words):
            addr = main_sp + i * 4
            if addr + 4 > va + sz:
                break
            w = struct.unpack_from("<I", buf, off + (addr - va))[0]
            if not (base[0] <= w < base[0] + base[1]) or not (w & 1) or w in seen:
                continue
            seen.add(w)
            s = res(w)
            if s:
                print(f"  {addr:#010x}  {w:#010x}  {s}")


if __name__ == "__main__":
    main()
