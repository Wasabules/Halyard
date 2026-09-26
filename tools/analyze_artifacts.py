#!/usr/bin/env python3
"""analyze_artifacts.py - finds transient artefacts in an H.264 stream.

This client's artefacts do not break the bitstream: ffmpeg decodes our dumps
without a single error, and yet the screen shows incoherent slabs for a fraction
of a second. They are VALID pictures with WRONG content - a fragment from
elsewhere - so no decoding counter sees them.

The only usable signal is temporal: a brief artefact is a block that differs
strongly from the picture before AND the one after. Real motion, by contrast,
resembles at least one of the two.

This score gives an OBJECTIVE, reproducible measurement: record a stream before
a fix, one after, and compare the distributions. That is what
replaces "it looks like there are fewer of them to me".

Usage : analyze_artifacts.py <flux.h264> [--extract]
"""
import os, subprocess, sys, tempfile

W, H = 320, 180          # a preview: enough to spot a block, and 100x faster
BLOC = 20                # ~120 px en 1080p


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    flux = sys.argv[1]
    extraire = "--extract" in sys.argv
    if not os.path.exists(flux):
        print("fichier introuvable : %s" % flux)
        sys.exit(1)

    import numpy as np
    with tempfile.NamedTemporaryFile(suffix=".gray", delete=False) as tmp:
        brut = tmp.name
    try:
        subprocess.run(["ffmpeg", "-v", "error", "-i", flux,
                        "-vf", "scale=%d:%d" % (W, H), "-pix_fmt", "gray",
                        "-f", "rawvideo", brut, "-y"], check=True)
        d = np.fromfile(brut, dtype=np.uint8)
    finally:
        os.unlink(brut)

    n = len(d) // (W * H)
    if n < 3:
        print("flux trop court (%d images)" % n)
        sys.exit(1)
    f = d[:n * W * H].reshape(n, H, W).astype(np.int16)

    # A moving edge also differs from the previous picture AND from the one
    # after: the temporal difference alone therefore does not tell an artefact
    # from a movement, and counted 18 % of the stream on an animated video.
    #
    # What really separates the two is GEOMETRY. A transmission artefact is a
    # slab aligned on the macroblock grid, with edges
    # sharp and straight; movement follows the picture's contours and ignores
    # that grid. So we weight the transient difference by how concentrated it is
    # on block boundaries.
    score = np.zeros(n)
    for i in range(1, n - 1):
        trans = np.minimum(np.abs(f[i] - f[i - 1]), np.abs(f[i] - f[i + 1]))
        hh, ww = H // BLOC * BLOC, W // BLOC * BLOC
        t = trans[:hh, :ww].astype(np.float32)
        # energy of the change, per block
        par_bloc = t.reshape(hh // BLOC, BLOC, ww // BLOC, BLOC).mean(axis=(1, 3))
        # sharpness of the block's edges: a slab stands out from its neighbours
        dv = np.abs(np.diff(t, axis=1)); dh = np.abs(np.diff(t, axis=0))
        bordv = dv[:, BLOC - 1::BLOC].mean() if dv.size else 0.0
        bordh = dh[BLOC - 1::BLOC, :].mean() if dh.size else 0.0
        interv = np.delete(dv, np.s_[BLOC - 1::BLOC], axis=1).mean() if dv.size else 1.0
        interh = np.delete(dh, np.s_[BLOC - 1::BLOC], axis=0).mean() if dh.size else 1.0
        alignement = ((bordv / max(interv, 0.01)) + (bordh / max(interh, 0.01))) / 2.0
        # a diffuse change (movement) has an alignment of ~1; a block, far more
        score[i] = par_bloc.max() * max(0.0, alignement - 1.0)

    med = float(np.median(score))
    seuil = max(3.0, med * 10)
    hit = int((score > seuil).sum())
    print("%d images   mediane %.1f   seuil %.1f" % (n, med, seuil))
    print("pictures with an artefact: %d  (%.2f %% of the stream)" % (hit, 100.0 * hit / n))
    print("pire score : %.1f" % score.max())
    ordre = np.argsort(score)[::-1]
    print("\nles plus atteintes :")
    for i in ordre[:8]:
        if score[i] <= seuil:
            break
        print("   image %5d   score %6.1f" % (i, score[i]))

    # Drift: does the SPATIAL blockiness grow over the session? A transient
    # artefact disappears; a drift accumulates because our
    # the reference picture drifts from the encoder's, and only corrects itself at
    # the next key frame. The two are cured differently, and the temporal score
    # above is blind to the second.
    def blocosite(im):
        dv, dh = np.abs(np.diff(im, axis=1)), np.abs(np.diff(im, axis=0))
        bv = dv[:, BLOC-1::BLOC].mean(); iv = np.delete(dv, np.s_[BLOC-1::BLOC], axis=1).mean()
        bh = dh[BLOC-1::BLOC, :].mean(); ih = np.delete(dh, np.s_[BLOC-1::BLOC], axis=0).mean()
        return ((bv / max(iv, .01)) + (bh / max(ih, .01))) / 2.0

    tiers = max(1, n // 3)
    debut = np.mean([blocosite(f[i]) for i in range(0, tiers, max(1, tiers // 20))])
    fin = np.mean([blocosite(f[i]) for i in range(n - tiers, n, max(1, tiers // 20))])
    print("\ndrift: blockiness %.3f at the start -> %.3f at the end  (%+.1f %%)"
          % (debut, fin, 100.0 * (fin / max(debut, .001) - 1)))

    if extraire:
        pires = [int(i) for i in ordre[:4] if score[i] > seuil]
        if pires:
            sel = "+".join("eq(n\\,%d)" % i for i in pires)
            sortie = os.path.join(os.path.dirname(flux) or ".", "artefact_%02d.png")
            subprocess.run(["ffmpeg", "-v", "error", "-i", flux,
                            "-vf", "select='%s'" % sel, "-vsync", "0",
                            "-q:v", "2", sortie, "-y"], check=True)
            print("\nimages extraites : %s (%d)" % (sortie, len(pires)))


if __name__ == "__main__":
    main()
