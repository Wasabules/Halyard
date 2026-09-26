#!/usr/bin/env python3
"""Finds source files that were only HALF migrated to English.

A word-list grep does not find them: every comment block carries enough English
to pass one. What finds them is a block that holds TWO DISTINCT French markers
at once -- eighteen such blocks survived three manual passes during the
2026-09-12 migration, which is why this detector exists at all.

The rule is deliberately conservative. One marker is noise (a borrowed word, a
proper noun, an accented name); two distinct markers in the same comment block
is a sentence someone forgot to translate.

  tools/check_french.py                  # the default set of source trees
  tools/check_french.py path [path ...]  # explicit paths
  tools/check_french.py --list           # one line per hit, no context

Exit code 1 when anything is found, so CI fails on a regression.

A file that legitimately holds French -- this one, which lists the markers, or
anything quoting the `fr` catalogue -- opts out with `check-french: ignore-file`
anywhere in it.
"""
# check-french: ignore-file
import os
import re
import sys

# Markers chosen for PRECISION, not coverage: each one is a word that
# essentially never appears in English technical prose. The deliberately
# EXCLUDED near-misses are the English collisions -- "on", "en", "son", "la",
# "le", "les", "des", "du", "est", "a", "plus", "si", "ou", "ce", "se", "il" --
# which would fire on ordinary English comments and on identifiers.
WORDS = r"""
qui que dont parce puisque lorsque tandis afin malgre malgré depuis deja déjà
tres très trop beaucoup chaque toujours jamais faut etait était etaient étaient
cette celui celle ceux leur leurs notre votre cela ceci quelque quelques
plusieurs aucun aucune meme même memes mêmes autre autres avec pour dans sans
sous mais donc alors ainsi seulement surtout ensuite enfin peut peuvent doit
doivent nous vous elle elles sont sera serait avait avaient avons faire etre
être avoir moins ici entre vers chez selon pendant avant apres après contre
tous toutes rien quand comment pourquoi encore aussi comme cet notamment
lorsqu jusqu puis ceux-ci celle-ci deja-vu
""".split()

WORD_RE = re.compile(r"\b(" + "|".join(sorted(set(WORDS), key=len, reverse=True)) + r")\b",
                     re.IGNORECASE)
# A run of French-accented letters counts as ONE marker, whatever its length.
ACCENT_RE = re.compile(r"[àâäçéèêëîïôöùûüÿœÀÂÄÇÉÈÊËÎÏÔÖÙÛÜŸŒ]")

SOURCE_EXT = {".c", ".h", ".cpp", ".hpp", ".cc", ".m", ".mm"}
SCRIPT_EXT = {".py", ".sh", ".bash"}
DEFAULT_PATHS = [
    "core",
    "clients",
    "cli",
    "legacy",
    "tests",
    "tools",
    "recon/tls_hook",
]
SKIP_DIRS = {"third_party", "library", "build_linux", "build_switch", "build_windows",
             ".git", "node_modules", "__pycache__"}


def hash_comment_blocks(text):
    """The `#` equivalent, for .py and .sh -- plus Python docstrings, which are
    where a script's prose actually lives."""
    lines = text.split("\n")
    i, n = 0, len(lines)
    while i < n:
        stripped = lines[i].strip()
        if stripped.startswith("#"):
            start, buf = i + 1, []
            while i < n and lines[i].strip().startswith("#"):
                buf.append(lines[i])
                i += 1
            yield start, "\n".join(buf)
            continue
        m = re.search(r'("""|\'\'\')', lines[i])
        if m:
            quote, start, buf = m.group(1), i + 1, [lines[i]]
            if lines[i].count(quote) < 2:
                i += 1
                while i < n:
                    buf.append(lines[i])
                    if quote in lines[i]:
                        break
                    i += 1
            yield start, "\n".join(buf)
        i += 1


def comment_blocks(text):
    """Yields (first_line_number, block_text) for each run of comment lines.

    A run is contiguous // lines, or one /* ... */ span. Contiguity matters:
    the point is to see a whole paragraph at once, because that is the unit a
    human translated -- or forgot to.
    """
    lines = text.split("\n")
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        stripped = line.strip()
        if stripped.startswith("//"):
            start, buf = i + 1, []
            while i < n and lines[i].strip().startswith("//"):
                buf.append(lines[i])
                i += 1
            yield start, "\n".join(buf)
            continue
        if "/*" in line:
            start, buf = i + 1, []
            while i < n:
                buf.append(lines[i])
                if "*/" in lines[i] and not (i == start - 1 and lines[i].index("*/") < lines[i].index("/*")):
                    i += 1
                    break
                i += 1
            yield start, "\n".join(buf)
            continue
        i += 1


def markers(block):
    """The DISTINCT markers in one block. Accents collapse to a single marker
    so that one accented French name cannot, by itself, reach the threshold."""
    found = {m.group(1).lower() for m in WORD_RE.finditer(block)}
    if ACCENT_RE.search(block):
        found.add("<accents>")
    return found


def walk(paths):
    for path in paths:
        if os.path.isfile(path):
            yield path
            continue
        for root, dirs, files in os.walk(path):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            for name in sorted(files):
                if os.path.splitext(name)[1] in SOURCE_EXT | SCRIPT_EXT:
                    yield os.path.join(root, name)


def main(argv):
    listing = "--list" in argv
    paths = [a for a in argv[1:] if not a.startswith("--")] or DEFAULT_PATHS
    paths = [p for p in paths if os.path.exists(p)]
    if not paths:
        print("check_french: no path to scan", file=sys.stderr)
        return 2

    hits, scanned = [], 0
    for path in walk(paths):
        scanned += 1
        try:
            text = open(path, encoding="utf-8", errors="replace").read()
        except OSError as exc:
            print("check_french: %s: %s" % (path, exc), file=sys.stderr)
            continue
        if "check-french: ignore-file" in text:
            continue
        ext = os.path.splitext(path)[1]
        split = hash_comment_blocks if ext in SCRIPT_EXT else comment_blocks
        for line_no, block in split(text):
            found = markers(block)
            if len(found) >= 2:
                hits.append((path, line_no, sorted(found), block))

    for path, line_no, found, block in hits:
        print("%s:%d  [%s]" % (path, line_no, ", ".join(found)))
        if not listing:
            for line in block.split("\n")[:6]:
                print("    " + line.rstrip())
            print()

    print("check_french: %d file(s) scanned, %d half-migrated block(s)"
          % (scanned, len(hits)))
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
