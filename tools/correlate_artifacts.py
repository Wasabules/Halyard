#!/usr/bin/env python3
"""correlate_artifacts.py - ties artefact pictures to the state of their assembly.

The question is simple: are the pictures carrying an artefact the ones that
notre client a mal assemblees ? Y repondre demande d'aligner deux numerotations
which do not start from the same point:

  - the `[EMIT] n=...` log numbers EVERY picture emitted;
  - ffmpeg numbers only those it could decode, ignoring everything that
    precede la premiere image cle.

A first attempt groped for that offset over a few values and wrongly concluded
there was no correlation. The offset CAN BE COMPUTED: it is the rank of the first
key frame in the stream. We read it by parsing the dump.

Usage : correlate_artifacts.py <flux.h264> <journal.log>
"""
import os, re, subprocess, sys, tempfile


def unites_acces(chemin):
    """Returns the list of access units: True if the unit contains a key frame.

    A unit starts at the first slice (NAL 1 or 5) following another slice - that
    is the split our client makes, one emission per picture.
    """
    data = open(chemin, "rb").read()
    unites, courante, vue_tranche = [], False, False
    i, n = 0, len(data)
    while i + 3 < n:
        if data[i] == 0 and data[i+1] == 0 and (
                (data[i+2] == 1) or (data[i+2] == 0 and data[i+3] == 1)):
            saut = 3 if data[i+2] == 1 else 4
            if i + saut < n:
                t = data[i + saut] & 0x1F
                if t in (1, 5):
                    if vue_tranche:
                        unites.append(courante)
                        courante = False
                    vue_tranche = True
                    if t == 5:
                        courante = True
                elif t in (7, 8, 6, 9) and vue_tranche:
                    unites.append(courante)
                    courante, vue_tranche = False, False
            i += saut
            continue
        i += 1
    if vue_tranche:
        unites.append(courante)
    return unites


def images_artefact(chemin):
    import numpy as np
    W, H, B = 320, 180, 20
    tmp = tempfile.NamedTemporaryFile(suffix=".gray", delete=False).name
    subprocess.run(["ffmpeg", "-v", "quiet", "-i", chemin, "-vf",
                    "scale=%d:%d" % (W, H), "-pix_fmt", "gray", "-f",
                    "rawvideo", tmp, "-y"], check=True)
    d = np.fromfile(tmp, dtype=np.uint8)
    os.unlink(tmp)
    n = len(d) // (W * H)
    f = d[:n*W*H].reshape(n, H, W).astype(np.int16)
    out = set()
    for i in range(1, n - 1):
        t = np.minimum(np.abs(f[i]-f[i-1]), np.abs(f[i]-f[i+1])).astype(np.float32)
        hh, ww = H//B*B, W//B*B
        t = t[:hh, :ww]
        pb = t.reshape(hh//B, B, ww//B, B).mean(axis=(1, 3))
        dv, dh = np.abs(np.diff(t, axis=1)), np.abs(np.diff(t, axis=0))
        al = (dv[:, B-1::B].mean()/max(np.delete(dv, np.s_[B-1::B], axis=1).mean(), .01)
              + dh[B-1::B, :].mean()/max(np.delete(dh, np.s_[B-1::B], axis=0).mean(), .01))/2
        if pb.max() * max(0.0, al - 1.0) > 3.0:
            out.add(i)
    return out, n


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    flux, journal = sys.argv[1], sys.argv[2]

    etats = {}
    for l in open(journal, errors="replace"):
        m = re.search(r"\[EMIT\] n=(\d+) morceaux=(\d+)/(\d+) (\w+)", l)
        if m:
            etats[int(m.group(1))] = (m.group(4) == "INCOMPLETE")
    if not etats:
        print("no [EMIT] line in the log - run again with SHADOW_DIAG_REASM=1")
        sys.exit(1)

    unites = unites_acces(flux)
    decalage = next((i for i, cle in enumerate(unites) if cle), 0)
    arte, n_dec = images_artefact(flux)

    print("%d images emises, %d unites d'acces, %d images decodees" %
          (len(etats), len(unites), n_dec))
    print("premiere image cle a l'unite %d — decalage applique : %d" % (decalage, decalage))

    incompletes = {n - decalage for n, inc in etats.items() if inc}
    inter = arte & incompletes
    print("\n%d images a artefact, %d images incompletes" % (len(arte), len(incompletes)))
    if incompletes:
        print("  %d images incompletes portent un artefact (%.0f %%)" %
              (len(inter), 100.0*len(inter)/len(incompletes)))
    if arte:
        print("  %d images a artefact sont assemblees normalement (%.0f %%)" %
              (len(arte - inter), 100.0*len(arte - inter)/len(arte)))
        print("\n=> %s" % ("the assembly explains some of the artefacts"
                           if len(inter) > 0.2*len(arte)
                           else "the assembly does NOT explain the artefacts"))


if __name__ == "__main__":
    main()
