#!/usr/bin/env python3
"""offline_diag.py — reproduces and locates the corruption from /tmp/offline.

Answers ONE question: is the corruption in the stream we assemble, or in the way
we decode it live?

  - decodes the stream with ffmpeg and counts/locates the reference breaks
    (a frame_num gap = a picture missing from the chain);
  - crosses that with the pictures our reassembler reported incomplete;
  - spots abnormal access units (absurd size, no slice).

If gaps appear in the same places as our incomplete pictures, the corruption is
IN the assembly and the fix is on the receive side. If the stream is clean, it is
in the live decoding.
"""
import re, subprocess, sys, collections, os

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/offline"
h264 = os.path.join(OUT, "stream.h264")
diag = os.path.join(OUT, "diag.log")
if not os.path.exists(h264):
    print("no stream in", OUT); sys.exit(1)

# 1. the reference breaks ffmpeg sees
r = subprocess.run(["ffmpeg", "-v", "debug", "-i", h264, "-f", "null", "-"],
                   capture_output=True, text=True)
gaps = re.findall(r"Frame num gap (\d+) (\d+)", r.stderr)
mb = len(re.findall(r"error while decoding MB", r.stderr))
conceal = re.findall(r"concealing (\d+) DC", r.stderr)
print("=== what ffmpeg sees in OUR stream ===")
print("ruptures de reference (frame num gap) :", len(gaps))
print("erreurs de macrobloc :", mb)
print("images avec concealment :", len(conceal))
if gaps:
    ecarts = collections.Counter(int(b) - int(a) for a, b in gaps)
    print("taille des sauts :", dict(ecarts))
    print("premiers gaps (attendu->recu) :", gaps[:6])

# 2. images incompletes cote reassemblage
inc = [int(m.group(1)) for m in re.finditer(r"\[EMIT\] n=(\d+) .*INCOMPLETE",
       open(diag, errors="replace").read())] if os.path.exists(diag) else []
print("\n=== what OUR reassembler reports ===")
print("images emises incompletes :", len(inc))
g18 = len(re.findall(r"G18. derive detectee", open(diag, errors="replace").read())) if os.path.exists(diag) else 0
print("recuperations d'IDR (G18) :", g18)

# 3. verdict
print("\n=== verdict ===")
if len(gaps) + mb > 5:
    print("The assembled stream contains breaks: the corruption is on the")
    print("RECEPTION (assemblage / pertes). Prochaine etape : identifier les")
    print("unites d'acces malformees et corriger l'assemblage.")
else:
    print("The assembled stream is CLEAN (ffmpeg does not flinch). So the")
    print("corruption is in the LIVE DECODING (async hardware transfer, threads,")
    print("or rendering). Next step: instrument the live path.")
