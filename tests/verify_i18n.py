#!/usr/bin/env python3
"""Compares the language files against each other.

=== WHY THIS CHECK EXISTS (2026-08-29) ===

Two defects found on the same day, both invisible at compile time:

  - `menu/edit_pos` and `menu/reset_pos` had been written into the `action`
    section instead of `menu`. The UI displayed the RAW KEY.
  - The whole debug-menu block - eleven keys - existed only in French. In
    English, that entire submenu showed up as plain keys.

Nothing could have signalled it: a missing key returns the key itself, which
compiles, runs, and only shows on screen, in the language one does not use
oneself. It is the same shape as this repo's dead counters - a defect that only
manifests where nobody looks.

The reference is French: it is the language this repo is written and thought in.
A key present elsewhere but absent in French is reported too - it points at a
section renamed on one side only.
"""
import json, os, sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.join(here, "..", "resources", "i18n")

def flat_keys(path):
    d = json.load(open(path, encoding="utf-8"))
    out = set()
    for sec, v in d.items():
        if isinstance(v, dict):
            for k in v:
                out.add(sec + "/" + k)
        else:
            out.add(sec)
    return out

ref_name = "fr"
ref = flat_keys(os.path.join(root, ref_name, "shadow.json"))
failures = 0
checks = 0

print("== language files: the same keys everywhere ==")
for lang in sorted(os.listdir(root)):
    path = os.path.join(root, lang, "shadow.json")
    if not os.path.isfile(path):
        continue
    other = flat_keys(path)
    checks += 1
    if lang == ref_name:
        continue
    missing = sorted(ref - other)
    extra = sorted(other - ref)
    checks += 2
    if missing:
        failures += 1
        print("  FAIL: %s - %d missing key(s): %s%s"
              % (lang, len(missing), ", ".join(missing[:6]),
                 " ..." if len(missing) > 6 else ""))
    if extra:
        failures += 1
        print("  FAIL: %s - %d key(s) French does not have: %s%s"
              % (lang, len(extra), ", ".join(extra[:6]),
                 " ..." if len(extra) > 6 else ""))

# === 2026-09-02 - THE KEY THE CODE ASKS FOR, AND NOBODY HAS ===
#
# The comparison above is between LANGUAGES: it catches a key French has and
# English does not. It cannot catch a key that is missing from BOTH - and that
# is the one a new screen produces, because the code and the catalogue are
# written at different moments by the same hand.
#
# The symptom is the same as the two defects at the top of this file: the raw
# key is printed on screen. It just happens in every language at once, so
# switching language does not reveal it either.
#
# A key built at run time by CONCATENATION is still not seen here. That is a
# limit and not a hole: this repo writes its keys as literals, and a computed one
# would be worth a comment where it is written.
#
# TWO SHAPES ARE COLLECTED, and the second was added on 2026-09-12 because the
# first had just been narrowed by a refactor. `ui::tr("literal")` is the obvious
# one. But a key can also travel as a plain string before reaching `tr` -- the
# fourteen entries of `pad_map.cpp`'s GENERIC_KEYS table are returned by
# `targetGenericKey()` and translated by the caller. Moving the translation out
# of the core (so that `core/` stopped reaching up into the Borealis client)
# made those fourteen invisible here, which is exactly the kind of silent
# shrinkage of a net that this file exists to prevent.
#
# So any string literal that LOOKS like a key -- `section/name`, where `section`
# is one the catalogue actually has -- counts as a use. It cannot produce a
# false failure: an unknown section is simply not collected, and a literal that
# matches a real key is, in this repo, always one.
import re

print("== every key the code asks for exists ==")
# 2026-09-14 - THE NET HAD BEEN SCANNING AN EMPTY DIRECTORY.
#
# This read `here/../demo/src`, which was correct until the 2026-09-12 tree lift
# moved the sources to `/core/` and `/clients/`. `os.walk` on a path that does
# not exist yields NOTHING and raises nothing, so the check went on reporting
# "0 keys used in the code, all present" - a pass, every run, scanning zero
# files. Precisely the silent shrinkage of a net that the comment above this
# line was written to prevent, and it happened to that very net.
#
# The roots are now a LIST and each one is asserted to exist: a directory that
# moves again fails loudly instead of quietly measuring nothing.
src_roots = [os.path.join(here, "..", d) for d in ("core", "clients", "cli")]
for r in src_roots:
    if not os.path.isdir(r):
        print("  FAIL: source root absent: %s" % r)
        sys.exit(1)
sections = {k.split("/")[0] for k in ref if "/" in k}
asked = set()
KEYISH = re.compile(r'"((?:%s)/[A-Za-z0-9_]+)"' % "|".join(re.escape(s) for s in sorted(sections)))
for src_root in src_roots:
  for dirpath, _dirs, files in os.walk(src_root):
     for name in files:
         if not name.endswith((".cpp", ".hpp", ".c", ".h")):
             continue
         with open(os.path.join(dirpath, name), encoding="utf-8", errors="replace") as f:
             text = f.read()
         for key in re.findall(r'ui::tr\(\s*"([^"]+)"', text):
             asked.add(key)
         for key in KEYISH.findall(text):
             asked.add(key)

checks += 1
unknown = sorted(k for k in asked if k not in ref)
if unknown:
    failures += 1
    print("  FAIL: %d key(s) used in the code and absent from every catalogue: %s%s"
          % (len(unknown), ", ".join(unknown[:8]),
             " ..." if len(unknown) > 8 else ""))
else:
    print("  %d keys used in the code, all present" % len(asked))

print("%d checks, %d failure(s)" % (checks, failures))
sys.exit(1 if failures else 0)
