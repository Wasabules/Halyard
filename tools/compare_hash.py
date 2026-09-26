#!/usr/bin/env python3
"""compare_hash.py - compares live against offline luma hashes, picture by picture.

The first divergence says where the live decoding parts company with the offline
one (same bytes). If everything agrees, the corruption is not in the decoding but
in the rendering/display. If it diverges early and stays diverged, the live
decoding corrupts.
"""
import re, sys, csv
livelog, offcsv = sys.argv[1], sys.argv[2]

live = {}
for l in open(livelog, errors="replace"):
    m = re.search(r"\[HASH\] n=(\d+) luma=([0-9a-f]+)", l)
    if m: live[int(m.group(1))] = m.group(2)

off = {}
for r in csv.DictReader(open(offcsv)):
    off[int(r["n"])] = r["luma_hash"]

common = sorted(set(live) & set(off))
if not common:
    print("aucune image commune (numerotations desalignees ?) — live:%d off:%d"
          % (len(live), len(off))); sys.exit(1)

d = [n for n in common if live[n] != off[n]]
print("images live=%d  offline=%d  communes=%d" % (len(live), len(off), len(common)))
print("images identiques   : %d" % (len(common) - len(d)))
print("images divergentes  : %d" % len(d))
if d:
    print("PREMIERE divergence a l'image %d" % d[0])
    print("  live   : %s" % live[d[0]])
    print("  offline: %s" % off[d[0]])
    # an ISOLATED divergence = a different picture; a CONTINUOUS divergence has
    # from one point on = the live decoder has drifted and does not come back.
    after = [n for n in common if n >= d[0]]
    div_after = sum(1 for n in after if live[n] != off[n])
    print("  from there on: %d/%d pictures diverge (%.0f %%)"
          % (div_after, len(after), 100.0*div_after/len(after)))
    print("\n=> %s" % ("live decoder DRIFT: it parts company and does not return - "
                        "the problem is in the live decoding/state"
                        if div_after > 0.5*len(after) else
                        "divergences isolees : images ponctuellement differentes, "
                        "not a continuous drift"))
else:
    print("\n=> IDENTICAL: the live decoding is bit-exact with the offline one. So")
    print("   the corruption seen on screen is in the RENDERING (shader/GL/pacing),")
    print("   not in the decoding.")
