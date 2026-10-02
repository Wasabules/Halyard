#!/usr/bin/env python3
"""check-z-formats.py - forbid %z outside the journal.

WHY. The vitasdk's newlib is built without `_WANT_IO_C99_FORMATS`, so it knows
neither %z nor %j nor %t. An unknown modifier prints the LETTERS and, far
worse, does NOT consume its argument: every conversion after it reads the wrong
slot of the va_list. On 2026-09-13 a `"... len=%zu hex=%s"` passed 0x500 to
strlen and took the console down -- on the first video packet, which is to say
at the exact moment the picture path started working.

There were 106 `%z` in this repo, 42 of them followed by another conversion.
The journal now neutralises them at runtime (journal.c, strip_z_modifier), but
a direct `snprintf` has no protection at all -- and nothing in the compiler
says a word: %zu is perfectly valid C99.

WHAT THIS SCRIPT DOES NOT DO. It does not understand multi-line calls. A format
string handed to snprintf three lines further down escapes it. It catches the
common case -- string and call on the same line -- and that is all it claims. A
partial guard that says so beats no guard.
"""
import re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
# The journal macros are safe: journal.c strips the modifier before vsnprintf
# on the libc that needs it.
# 2026-10-02: this used to be a hand-written list of twelve macro names. The
# repo defines FORTY of them (grep -rh "define [a-z]*log" core clients cli), so
# the list was 28 names out of date and quietly treated e.g. `slog` as a direct
# printf. Match the SHAPE instead: every one of them is <prefix>log, with a
# prefix of one to six lowercase letters, expanding to a JOURNAL_ macro.
# `log(` itself (math) is not matched: the prefix is required.
JOURNAL = re.compile(r'\b([a-z]{1,6}log|JOURNAL_[A-Z_]+_?)\s*\(')
DIRECT  = re.compile(r'\b(snprintf|sprintf|vsnprintf|fprintf|printf|vfprintf)\s*\(')
ZFMT    = re.compile(r'%[-+ #0-9.*]*z[diouxX]')

def main() -> int:
    bad = []
    for d in ("core", "clients", "cli"):
        for f in (ROOT / d).rglob("*"):
            if f.suffix not in (".c", ".cpp", ".h", ".hpp") or not f.is_file():
                continue
            # journal.c holds the countermeasure itself, and its examples.
            if f.name in ("journal.c", "check-z-formats.py"):
                continue
            for n, line in enumerate(f.read_text(errors="replace").splitlines(), 1):
                if not ZFMT.search(line):
                    continue
                if JOURNAL.search(line):
                    continue
                if DIRECT.search(line):
                    bad.append(f"{f.relative_to(ROOT)}:{n}: {line.strip()[:100]}")
    if bad:
        print("%z in a DIRECT printf call (the vitasdk's newlib does not know it,")
        print("and it does not consume its argument -- the next conversion reads astray):")
        for b in bad:
            print("  " + b)
        print(f"\n{len(bad)} site(s). Replace with an explicit cast: (unsigned long) + %lu.")
        return 1
    print("no %z in a direct printf call")
    return 0

if __name__ == "__main__":
    sys.exit(main())
