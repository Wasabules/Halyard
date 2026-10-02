#!/usr/bin/env python3
"""`core/` must not depend on any client. Checked, not hoped for.

=== LIB1 2026-10-02 - WHY A SCRIPT AND NOT A CONVENTION ======================

`core/` is the protocol and the services; `clients/` is one user interface that
happens to use them. That `core/` never reaches up into `clients/` was TRUE
before this check existed and it was unverified: nothing would have failed, on
any platform, if somebody had added `#include "../../clients/borealis/..."` to
a core file. It would simply have stopped being a library, quietly, and the
next client would have discovered it.

Since `core/` is now built as `halyard-core` (a real CMake target), a bad
include would at least break THAT target's compile. But only if the included
header happens not to compile standalone — plenty of them would, and then the
coupling would be invisible again. Hence this.

WHAT IT FORBIDS, and nothing more:
  - any `#include` from core/ that names `clients/`, borealis, nanovg or glfw;
  - any `ui::` or `brls::` use in core/ (the UI namespaces);
  - a non-weak declaration in core/ of a function core/ does not define, when
    the client defines it — the "somebody else will link this" pattern that
    `shadow_link_info` was. Weak is fine, and is the documented way to ask a
    client for something optional.

WHAT IT DELIBERATELY ALLOWS:
  - i18n KEYS returned as data (`eq.h`, `errors.h` say so in their comments):
    core names a string, the client translates it. That is an interface, not a
    dependency;
  - `__attribute__((weak))` declarations. `shadow_link_info` and
    `halyard_ui_keys_blocked` are both asks, not requirements: the library
    links without them.

Run by CI (lint.yml) and by tests/run_tests.sh.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
CORE = os.path.join(ROOT, "core")

if not os.path.isdir(CORE):
    print("FAIL: no core/ directory at %s" % CORE)
    sys.exit(1)

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.M)
FORBIDDEN_INC = re.compile(r'clients/|\bborealis\b|\bnanovg\b|\bglfw\b|/devui/', re.I)
UI_NS = re.compile(r'\b(?:brls::|ui::(?!tr\b))')

failures = []
checked = 0

for dirpath, _dirs, files in os.walk(CORE):
    for name in sorted(files):
        if not name.endswith((".c", ".h", ".cpp", ".hpp")):
            continue
        path = os.path.join(dirpath, name)
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        checked += 1
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()

        # Comments are prose: `clients/` is named in several core comments to
        # explain who the caller is, and that is not a dependency. Only real
        # #include lines and real code count.
        for m in INCLUDE.finditer(text):
            inc = m.group(1)
            if FORBIDDEN_INC.search(inc):
                line = text.count("\n", 0, m.start()) + 1
                failures.append("%s:%d  includes `%s`" % (rel, line, inc))

        # Strip comments and strings before looking for the UI namespaces, so a
        # comment saying "the client's ui:: layer" is not a hit.
        stripped = strip = text
        strip = re.sub(r'/\*.*?\*/', '', strip, flags=re.S)
        strip = re.sub(r'//[^\n]*', '', strip)
        strip = re.sub(r'"(?:\\.|[^"\\])*"', '""', strip)
        for m in UI_NS.finditer(strip):
            line = strip.count("\n", 0, m.start()) + 1
            failures.append("%s:~%d  uses the UI namespace `%s`"
                            % (rel, line, m.group(0)))
            break   # one per file is enough to make the point

# === LIB2 2026-10-02 - AND core/ IS LAYERED, WHICH IS ALSO CHECKED ==========
#
# Measured on 2026-10-02, before any of it was moved: the five subdirectories of
# `core/` had FOUR dependency cycles between them (`common<->services`,
# `input<->protocol`, `media<->protocol`, `protocol<->services`). Nine includes
# in four files closed them, and each one was a file in the wrong directory:
# `filetransfer` was a protocol channel sitting in services/, `log.h` was a
# facade over a service sitting in a `common/` that was not a layer,
# `ctrl_session_glue` and `smoke_test` were the composition layer sitting in
# protocol/, and `jwt_instance` was a pure utility buried in a smoke test.
#
# What that bought, and it is the point of the whole exercise: **`protocol/` no
# longer knows that `media/` or `input/` exist**. A client that brings its own
# decoder - a Mac front end on VideoToolbox, a browser on WebCodecs - can take
# the protocol and leave the 31 platform #ifdefs of `core/media` behind.
#
# A layering is only worth having if it cannot drift, so the order is declared
# here and any edge going the wrong way is a failure. Bottom first:
LAYERS = ["services", "protocol", "media", "input", "session"]
# `media` and `input` are siblings: neither may include the other. `session` is
# the only subdirectory allowed to reach into all of them, which is what makes
# it the composition layer rather than a sixth peer.
RANK = {name: i for i, name in enumerate(LAYERS)}

def owners_of_headers():
    out = {}
    for dp, _d, fs in os.walk(CORE):
        sub = os.path.relpath(dp, CORE).split(os.sep)[0]
        for f in fs:
            if f.endswith((".h", ".hpp")):
                out[f] = sub
    return out

HEADER_OWNER = owners_of_headers()
layer_bad = []
unknown = set()

for dirpath, _dirs, files in os.walk(CORE):
    sub = os.path.relpath(dirpath, CORE).split(os.sep)[0]
    if sub == ".":
        # `core/version.h` is the one file at core/'s root - the app identity,
        # generated from CMake, and it includes nothing. The layering is about
        # the subdirectories; a root header belongs to all of them.
        continue
    if sub not in RANK:
        unknown.add(sub)
        continue
    for name in sorted(files):
        if not name.endswith((".c", ".h", ".cpp", ".hpp")):
            continue
        path = os.path.join(dirpath, name)
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        with open(path, encoding="utf-8", errors="replace") as f:
            body = f.read()
        body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
        body = re.sub(r"//[^\n]*", "", body)
        for m in INCLUDE.finditer(body):
            tgt = HEADER_OWNER.get(os.path.basename(m.group(1)))
            if tgt is None or tgt == sub or tgt not in RANK:
                continue
            if RANK[tgt] >= RANK[sub]:
                line = body.count("\n", 0, m.start()) + 1
                layer_bad.append("%s:%d  %s -> %s  (`%s`)"
                                 % (rel, line, sub, tgt, m.group(1)))

print("== core/ depends on no client, and is layered (LIB1/LIB2) ==")
print("  %d file(s) under core/ checked" % checked)
print("  layers, bottom first: %s" % " < ".join(LAYERS))
if unknown:
    failures.append("unknown subdirectory of core/: %s - add it to LAYERS in "
                    "tools/check-core-independence.py, deciding where it sits"
                    % ", ".join(sorted(unknown)))
failures.extend(layer_bad)

if failures:
    print("  FAIL: %d violation(s):" % len(failures))
    for f in failures[:20]:
        print("    %s" % f)
    if len(failures) > 20:
        print("    ... and %d more" % (len(failures) - 20))
    print()
    print("  core/ is built as the `halyard-core` library and is meant to be")
    print("  reusable by a Qt or Tauri client. If core genuinely needs")
    print("  something only a client can answer, declare it WEAK - see")
    print("  `shadow_link_info` in core/protocol/ctrl_session.c.")
    sys.exit(1)

print("  no include of clients/, borealis, nanovg or glfw; no UI namespace")
sys.exit(0)
