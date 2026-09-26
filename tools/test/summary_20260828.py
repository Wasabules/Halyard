#!/usr/bin/env python3
"""A summary of the autonomous campaigns of 2026-08-28.

It aggregates the session JSONs into a readable report. Two reading rules, learned
on this repo:
  - an average over fewer than 3 samples means nothing (the variance from one
    session to the next is enormous); so we ALWAYS show the raw values next to
    the average;
  - a counter at zero is a result, not the absence of one: we write it down.
"""
import json, glob, os, statistics, sys

OUT = os.path.join(os.path.dirname(__file__), "../../captures/tests_autonomes_20260828")

def charge(motif):
    r = []
    for f in sorted(glob.glob(os.path.join(OUT, motif + ".json"))):
        try:
            d = json.load(open(f))
        except Exception:
            continue
        d["_nom"] = os.path.basename(f)[:-5]
        r.append(d)
    return r

def val(d, chemin, defaut=0):
    cur = d
    for k in chemin.split("."):
        cur = cur.get(k, {}) if isinstance(cur, dict) else {}
    return cur if isinstance(cur, (int, float)) else defaut

CHAMPS = [
    ("pps",        lambda d: val(d, "video.total_packets") / max(d.get("session_seconds", 1), 1)),
    ("kbps",       lambda d: val(d, "video.avg_kbps")),
    ("fps",        lambda d: val(d, "video.fps")),
    ("images",     lambda d: val(d, "video.frames_decoded")),
    ("manques",    lambda d: val(d, "perte.chunks_missing")),
    ("perte_pct",  lambda d: 100.0 * val(d, "perte.chunks_missing") / max(val(d, "perte.chunks_expected"), 1)),
    ("nack",       lambda d: val(d, "perte.nack_sent")),
    ("redondants", lambda d: val(d, "perte.chunks_redundant")),
    ("queues_ok",  lambda d: val(d, "perte.chunks_tail_recovered")),
    ("tardifs",    lambda d: val(d, "perte.chunks_late")),
    ("orphelins",  lambda d: val(d, "perte.chunks_orphan")),
    ("tronquees",  lambda d: val(d, "perte.frames_dropped_trunc")),
    ("courts+10",  lambda d: val(d, "perte.paquets_courts_base10")),
]

def bloc(titre, sessions):
    if not sessions:
        return
    print("\n" + titre)
    print("  " + "-" * (len(titre) - 2))
    for nom, f in CHAMPS:
        vals = [f(d) for d in sessions]
        if len(vals) == 1:
            print("    %-11s %s" % (nom, ("%.2f" % vals[0]).rstrip("0").rstrip(".")))
        else:
            m = statistics.mean(vals)
            brut = " ".join(("%.1f" % v).rstrip("0").rstrip(".") for v in vals)
            print("    %-11s avg=%-9.2f  brut= %s" % (nom, m, brut))

groupes = [
    ("Calibrating the loss (the N56 lever, 45 s)", "cal_rcvbuf_*"),
    ("An INVALID A/B (campaign 2) — the toggle cut the NACK instead of putting it back on the heartbeat", "v10b_a*"),
    ("V10 BEFORE — drained on the 50 ms heartbeat, requested at the drain (90 s)", "v10[cd]_avant_*"),
    ("V10 AFTER — requested at the hole, drained continuously (90 s)", "v10[cd]_apres_*"),
    ("Temoin — retransmission entierement coupee (SHADOW_NACK=0)", "v10b_sans_nack"),
    ("Manette + G53 sans enregistrement UDP", "g53_sans_reg"),
    ("Manette + G53 AVEC enregistrement UDP", "g53_avec_reg"),
    ("Manette (campagne 1)", "g52_g53_manette"),
    ("Temoin sans manette", "g52_temoin_sans_manette"),
]
for titre, motif in groupes:
    bloc(titre, charge(motif))

# --- verdict A/B ---------------------------------------------------------
av, ap = charge("v10[cd]_avant_*"), charge("v10[cd]_apres_*")
if len(av) >= 2 and len(ap) >= 2:
    print("\n\nVERDICT A/B V10")
    print("  ---------------")
    for nom, f in CHAMPS:
        a = statistics.mean([f(d) for d in av])
        b = statistics.mean([f(d) for d in ap])
        if a == 0 and b == 0:
            print("    %-11s zero on both sides — nothing to tell apart" % nom)
        else:
            delta = (b - a) / a * 100 if a else float("inf")
            print("    %-11s before=%-9.2f after=%-9.2f  %s"
                  % (nom, a, b, ("%+.1f %%" % delta) if a else "(avant=0)"))
