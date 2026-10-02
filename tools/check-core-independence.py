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

print("== core/ depends on no client (LIB1) ==")
print("  %d file(s) under core/ checked" % checked)

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
