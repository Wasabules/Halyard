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
# is one the catalogue actually has -- counts as a use. An unknown section is
# simply not collected.
#
# 2026-10-02: this used to add "and a literal that matches a real key is, in
# this repo, always one". That was REFUTED by a doc comment containing
# `"vm/start"` as a prose example. Comments are now stripped before scanning;
# see `strip_comments` below.
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


# === 2026-10-02 - COMMENTS ARE NOT CODE, AND THE CLAIM ABOVE WAS WRONG =======
#
# The comment above this block used to assert that the key-ish net "cannot
# produce a false failure: a literal that matches a real key is, in this repo,
# always one". It just did. `core/services/http.h` documents its label argument
# with the example `"vm/start"` - inside a C comment - and `vm` is a real
# catalogue section, so the net read a prose example as a missing key and
# FAILED the build.
#
# A guard that cries wolf gets switched off, which would cost far more than the
# false positive. So comments are stripped before scanning. Nothing is lost: a
# `ui::tr` call inside a comment is not a call.
#
# The stripper tracks STRING AND CHARACTER LITERALS, because this repo is full
# of URLs - `"https://api.eu.shadow.tech"` contains `//` and a naive stripper
# would eat the rest of the line, silently shrinking the very net this file
# exists to keep wide. Raw strings and line continuations inside a literal are
# not handled; neither appears in these sources, and both would only ever make
# the net wider, not narrower.
def strip_comments(text):
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                # newlines are kept so that line numbers stay usable
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)
for src_root in src_roots:
  for dirpath, _dirs, files in os.walk(src_root):
     for name in files:
         if not name.endswith((".cpp", ".hpp", ".c", ".h")):
             continue
         with open(os.path.join(dirpath, name), encoding="utf-8", errors="replace") as f:
             text = strip_comments(f.read())
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

# === UI11 2026-10-02 - HOW MANY ARGUMENTS THE FORMAT WANTS ===================
#
# `ui::tr` forwards to Borealis' fmt, so a catalogue entry can carry `{}`
# placeholders. Give it fewer arguments than it has placeholders and NOTHING is
# printed: fmt throws, Borealis catches, logs
#
#   ERROR Invalid format "{} machine(s) sur {}" from string "shadow/vm/count":
#         argument not found
#
# and returns an empty string. So the label silently disappears - which is how
# `vm/count` spent an unknown number of releases leaving the machine-list status
# line blank, with the explanation sitting in a log nobody reads during a build.
#
# The two checks above compare keys. Neither looks INSIDE a value, so neither
# could see this. This one counts the `{}` in every catalogue entry and the
# arguments at every `ui::tr` call site that names it.
#
# WHAT IT DELIBERATELY DOES NOT DO: it only judges a call it can read whole on
# one logical expression, and it only complains about TOO FEW arguments. Extra
# ones are harmless to fmt, and a call split across lines with a nested call in
# it is skipped rather than guessed at - a guard that cries wolf gets disabled,
# which is worse than a guard with a known edge.
print("== a format gets as many arguments as it has placeholders ==")

def placeholders(value):
    """`{}` and `{0}`-style slots, ignoring the `{{` escape."""
    return len(re.findall(r'(?<!\{)\{[^{}]*\}', value.replace("{{", "")))

# `ref` above is a set of KEY NAMES; the values are needed here, so the
# reference catalogue is read once more rather than changing what `flat_keys`
# returns and disturbing the two checks that already rely on it.
_refdoc = json.load(open(os.path.join(root, ref_name, "shadow.json"),
                         encoding="utf-8"))
_refvals = {}
for _sec, _v in _refdoc.items():
    if isinstance(_v, dict):
        for _k, _val in _v.items():
            if isinstance(_val, str): _refvals[_sec + "/" + _k] = _val
    elif isinstance(_v, str):
        _refvals[_sec] = _v

want = {k: placeholders(v) for k, v in _refvals.items() if placeholders(v) > 0}

def top_level_commas(s):
    """Arguments at depth 0 - so `tr("k", f(a, b))` counts as ONE."""
    depth = 0
    n = 0
    in_str = False
    esc = False
    for c in s:
        if in_str:
            if esc: esc = False
            elif c == '\\': esc = True
            elif c == '"': in_str = False
            continue
        if c == '"': in_str = True
        elif c in "([{": depth += 1
        elif c in ")]}":
            if depth == 0: return None    # unbalanced: we are not reading it right
            depth -= 1
        elif c == ',' and depth == 0: n += 1
    return None if depth != 0 or in_str else n

bad = []
# `ui::tr("key"` up to the matching close paren, on one line only.
CALL = re.compile(r'ui::tr\(\s*"([^"]+)"([^;]*)')
for src_root in src_roots:
    for dirpath, _dirs, files in os.walk(src_root):
        for name in files:
            if not name.endswith((".cpp", ".hpp", ".c", ".h")):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding="utf-8", errors="replace") as f:
                for lineno, line in enumerate(f, 1):
                    for key, rest in CALL.findall(line):
                        if key not in want:
                            continue
                        # The call's own arguments: up to the paren that closes
                        # `tr(`. Anything we cannot balance on this line is
                        # skipped, on purpose.
                        depth = 1
                        args = []
                        in_str = False
                        esc = False
                        for c in rest:
                            if in_str:
                                args.append(c)
                                if esc: esc = False
                                elif c == '\\': esc = True
                                elif c == '"': in_str = False
                                continue
                            if c == '"': in_str = True
                            elif c in "([{": depth += 1
                            elif c in ")]}":
                                depth -= 1
                                if depth == 0: break
                            args.append(c)
                        if depth != 0:
                            continue            # continued on the next line
                        got = top_level_commas("".join(args))
                        if got is None:
                            continue
                        if got < want[key]:
                            rel = os.path.relpath(path, os.path.join(here, ".."))
                            bad.append((rel.replace("\\", "/"), lineno, key,
                                        got, want[key]))

checks += 1
if bad:
    failures += 1
    print("  FAIL: %d call(s) pass fewer arguments than the format needs "
          "(the label renders EMPTY):" % len(bad))
    for f_, ln, key, got, exp in bad[:10]:
        print("    %s:%d  %s  %d given, %d placeholder(s)" % (f_, ln, key, got, exp))
else:
    print("  %d format(s) with placeholders, every readable call site agrees"
          % len(want))

print("%d checks, %d failure(s)" % (checks, failures))
sys.exit(1 if failures else 0)
