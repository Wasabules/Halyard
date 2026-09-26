#!/usr/bin/env python3
"""Cleans up the interface sounds a generator produced.

=== WHY THIS TOOL EXISTS (S88b, 2026-08-29) ===

The fourteen sounds come from ElevenLabs, which returns files CALIBRATED TO A
FIXED LENGTH - 600 or 1200 ms - whatever sound was asked for. A 47 ms navigation
tick therefore arrives inside a 600 ms file, 336 ms of which is silence BEFORE
the sound.

Three defects follow, and all three are audible:

  1. THE DELAY. The sound leaves 336 ms after the press. At that delay the ear no
     longer ties the sound to the gesture: the interface feels soft, which is
     exactly what the audio feedback was meant to cure.
  2. THE OPENING ARTEFACT. Eleven files out of fourteen carry a click or a hiss
     at t=0, between -20 and -35 dB, separated from the real sound by 100 to
     550 ms of silence. It is a generation leftover, and it sounds like a fault.
  3. THE LEVELS. Peaks run from -34.4 dB to -0.1 dB: a 34 dB spread, a factor of
     fifty. Played one after another in an interface, that gives sounds you
     cannot hear and sounds that make you jump.

This tool does not "touch up" by ear: it MEASURES, it cuts what is measurable,
and it WRITES DOWN what it did. The originals are kept - see --backup.

=== WHAT IT DOES, IN ORDER ===

  1. a peak envelope, per millisecond;
  2. locating the MAIN EVENT: the active region containing the peak, regions
     separated by less than `--bridge` ms counting as a single event (two strokes
     of one sound must not be split apart);
  3. everything before that event is dropped - that is the artefact;
  4. a short fade in and a fade out, so no cut clicks;
  5. peak normalisation to -6 dBFS;
  6. conversion to mono IF the two channels are near identical.

=== THE TRAP THAT DICTATED THE MONO RULE ===

`alerte.wav` has an inter-channel correlation of **-0.13**: its two sides are
almost in ANTIPHASE. Summing them into a mono cancels them - the file would
become nearly silent, and no peak measurement would say so before someone
listened. So we only go mono when the two channels are already near identical
(correlation > 0.995), in which case the operation is exactly lossless and
halves the size.

Usage:
    tools/normalise_sfx.py --dry-run    # measure and print, write nothing
    tools/normalise_sfx.py              # apply
    tools/normalise_sfx.py --variants   # also build nav_2 / nav_3
"""

import argparse
import math
import os
import shutil
import struct
import sys
import wave

RACINE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      '..', 'resources', 'sfx')

# === THE MIX, AND WHY THIS IS NOT NORMALISATION ===
#
# The first version brought EVERY file to the same peak. That is what a
# normaliser does, and it is wrong here: the brief asks for a HIERARCHY, and
# uniform normalisation destroys precisely that.
#
#   - `ouvrir` and `valider` play on THE SAME press of A. The first must sit
#     underneath; at equal level you hear a mush instead of a confirmation. The
#     delivered file was at -34 dB: lifting it to -6 like the others would have
#     made it as present as the confirmation.
#   - `nav` fires eight times a second when a direction is held. At the level of
#     an event sound, it becomes a machine gun.
#   - `connecte` is the reward after seven steps of waiting. It has earned the
#     right to be the loudest of the set.
#
# So we aim at a LOUDNESS per sound, not a peak. The difference matters: a forty
# millisecond tick and a nine hundred millisecond pad at the same peak have
# nothing like the same presence to the ear. The measure used is RMS energy over
# the loudest 50 ms window - a rough approximation of short-term loudness, but
# ample for ranking fourteen short sounds, and needing no dependency.
#
# The peak stays bounded at -3 dBFS afterwards: the loudness target must never
# clip a transient.
SONIE_VISEE_DB = {
    'nav_1': -26.0, 'nav_2': -26.0, 'nav_3': -26.0,
    'nav_bord': -29.0,       # quieter still: it is an edge, not an event
    'valeur': -26.0,
    'rubrique': -24.0,
    'valider': -20.0,        # the main gesture: it has body
    'retour': -22.0,
    'bascule_on': -23.0, 'bascule_off': -23.0,
    'ouvrir': -32.0,         # sits UNDER `valider`, which it overlaps
    'fermer': -33.0,         # … and below `retour`
    'connecte': -18.0,       # the reward: the most present of the set
    'echec': -22.0,          # marks the end of the wait, does not alarm
    'alerte': -20.0,         # must cut through the game's own sound
    'demarrage': -20.0,
}
SONIE_DEFAUT_DB = -24.0

# Plafond de peak after mise a niveau.
CRETE_MAX_DB = -3.0

# Fallback when `--uniform` is asked for: the old peak normalisation.
CRETE_VISEE_DB = -6.0

# An event is "active" above this value BELOW THE FILE'S PEAK. 45 dB catches a
# sound's tail without catching the noise floor.
SEUIL_RELATIF_DB = -45.0
# ... and never below this ABSOLUTE floor, or a very quiet file would see its own
# noise floor promoted to the rank of sound.
SEUIL_ABSOLU_DB = -66.0


def db(lineaire):
    return -120.0 if lineaire <= 0 else 20.0 * math.log10(lineaire / 32768.0)


def read_wav(path):
    w = wave.open(path, 'rb')
    n, channels, width, rate = (w.getnframes(), w.getnchannels(),
                                w.getsampwidth(), w.getframerate())
    brut = w.readframes(n)
    w.close()
    if width != 2:
        raise ValueError('%s: %d bits, only 16-bit is handled' % (path, width * 8))
    ech = struct.unpack('<%dh' % (len(brut) // 2), brut)
    if channels == 2:
        return list(ech[0::2]), list(ech[1::2]), rate
    return list(ech), list(ech), rate


def write_wav(path, left, right, rate, mono):
    w = wave.open(path, 'wb')
    w.setnchannels(1 if mono else 2)
    w.setsampwidth(2)
    w.setframerate(rate)
    if mono:
        data = struct.pack('<%dh' % len(left), *left)
    else:
        entrelace = []
        for i in range(len(left)):
            entrelace.append(left[i])
            entrelace.append(right[i])
        data = struct.pack('<%dh' % len(entrelace), *entrelace)
    w.writeframes(data)
    w.close()


def envelope_ms(left, right, rate):
    """Peak per millisecond. That is the right resolution: finer, and you follow the
    signal's oscillations instead of its envelope; coarser, and you miss the
    start of an attack."""
    step = max(1, rate // 1000)
    env = []
    for i in range(0, len(left), step):
        fin = min(i + step, len(left))
        c = 0
        for j in range(i, fin):
            a = abs(left[j])
            b = abs(right[j])
            if a > c:
                c = a
            if b > c:
                c = b
        env.append(c)
    return env, step


def main_event(env, peak, groupe_ms, pont_ms, gap_db):
    """Bounds (in ms) of the event that contains the peak.

    === WHY TWO CRITERIA AND NOT ONE ===

    The first version separated events by SILENCE alone, with a 120 ms bridge. It
    failed on three files - `alerte`, `echec`, `valider` - whose opening artefact
    is separated from the real sound by less than the bridge, or not separated at
    all: it decays slowly right into the sound.

    Measurement showed the real rule. With TIGHT grouping (30 ms), all fourteen
    files have exactly ONE main region, and the artefact is always a distinct
    region, between 17 and 44 dB below the peak. It is the LEVEL that separates
    them cleanly, not the length of the silence.

    So we keep the peak's region, and attach a neighbour only if it is both CLOSE
    (< bridge) and LOUD (within `gap_db` of the peak). That second criterion is a
    guard for the future: none of the fourteen current files needs it, but a
    sound in two clearly separated strokes would otherwise be cut in half, and
    nothing but the ear would report it."""
    threshold = max(peak * (10 ** (SEUIL_RELATIF_DB / 20.0)),
                32768.0 * (10 ** (SEUIL_ABSOLU_DB / 20.0)))

    actifs = [i for i, v in enumerate(env) if v >= threshold]
    if not actifs:
        return 0, len(env) - 1, []

    regions = []
    debut = prec = actifs[0]
    for i in actifs[1:]:
        if i - prec > groupe_ms:
            regions.append((debut, prec))
            debut = i
        prec = i
    regions.append((debut, prec))

    i_crete = max(range(len(env)), key=lambda k: env[k])
    principale = 0
    for k, (a, b) in enumerate(regions):
        if a <= i_crete <= b:
            principale = k
            break

    plancher = peak * (10 ** (-abs(gap_db) / 20.0))
    a, b = regions[principale]

    k = principale - 1
    while k >= 0:
        ra, rb = regions[k]
        if a - rb > pont_ms or max(env[ra:rb + 1]) < plancher:
            break
        a = ra
        k -= 1

    k = principale + 1
    while k < len(regions):
        ra, rb = regions[k]
        if ra - b > pont_ms or max(env[ra:rb + 1]) < plancher:
            break
        b = rb
        k += 1

    return a, b, regions


def loudness_max(g, d, rate):
    """RMS energy over the loudest 50 ms window.

    A rough approximation of short-term loudness. It is enough here and needs no
    dependency - a real LUFS measurement would want a weighting filter, to rank
    fourteen sounds shorter than a second. What matters is that it accounts for
    DURATION, which the peak ignores."""
    n = len(g)
    w = min(int(rate * 0.05), n)
    if w <= 0:
        return 0.0
    step = max(1, w // 4)
    meilleur = 0.0
    for begin in range(0, max(1, n - w + 1), step):
        s = 0.0
        for i in range(begin, begin + w):
            m = (g[i] + d[i]) * 0.5
            s += m * m
        r = math.sqrt(s / w)
        if r > meilleur:
            meilleur = r
    return meilleur


def noise_floor(g, d, rate):
    """RMS energy over the QUIETEST 50 ms window.

    It serves a check that would be easy to forget: lifting a file recorded very
    low lifts its noise by as much. `ouvrir.wav` arrives at -34 dBFS and takes
    +14 dB - so does its hiss. An interface sound that brings its own hiss is
    worse than no sound at all, because you hear it on every page you open.

    Measured AFTER the cut, so on the sound alone: measuring the original file
    would give the digital silence of the padding, that is, a reassuring and
    false answer.

    === AND IT MEANS NOTHING ON A SHORT SOUND ===

    First version: it took the quietest window of the whole file. On
    `valeur.wav`, which runs 73 ms once cut, there is no "quiet" window - the
    quietest one IS the sound, and the measurement announced "0 dB of headroom",
    that is, an alarm on a perfectly healthy file.

    A window of silence only exists if the file is long enough AND one looks
    AFTER the decay. Below that, the right answer is "not measurable", not a
    number. Returning None rather than a figure is what stops anyone reading an
    alarm where there is nothing to read - the same principle as
    `LinkType::Unknown` on the console side.

    WHAT IT CANNOT DO, and one must know it while reading it: it does not tell a
    steady HISS from a RESONANT TAIL still decaying. Both present as late signal.
    So the figure is returned without a verdict - the ear decides."""
    n = len(g)
    w = int(rate * 0.05)
    i_crete = max(range(n), key=lambda k: max(abs(g[k]), abs(d[k])))
    start = i_crete + int(rate * 0.15)     # 150 ms after the peak
    if w <= 0 or n - start < w * 2:
        return None

    step = max(1, w // 2)
    worst = None
    for begin in range(start, n - w + 1, step):
        s = 0.0
        for i in range(begin, begin + w):
            m = (g[i] + d[i]) * 0.5
            s += m * m
        r = math.sqrt(s / w)
        if worst is None or r < worst:
            worst = r
    return worst


def process(name, path, args):
    left, right, rate = read_wav(path)
    n = len(left)
    if n == 0:
        return None

    crete_avant = max(max(abs(v) for v in left), max(abs(v) for v in right))
    if crete_avant == 0:
        return {'name': name, 'note': 'FICHIER MUET — rien a faire'}

    env, step = envelope_ms(left, right, rate)
    a_ms, b_ms, regions = main_event(env, crete_avant, args.group,
                                              args.bridge, args.ecart)

    # A margin before the attack: cutting flush would shave the very start of
    # the transient, which is exactly what makes a percussive sound crisp.
    a = max(0, (a_ms - args.margin) * step)
    # === THE TAIL, AND WHY IT IS CUT HIGHER THAN THE REST ===
    #
    # The threshold used to FIND the event (-45 dB) is deliberately low: nothing
    # must be missed. But keeping everything above it leaves tails that are
    # inaudible and expensive. Measured on the delivered files: `retour` runs
    # 545 ms, 380 of them decaying below -30 dB; `nav_bord` 236, of which 150. On
    # a sound that fires on every press of B, those 380 ms hold the device open
    # and overlap the next sound.
    #
    # So the tail is cut at `--tail-db` below the peak, with a fade-out long
    # enough that a sharp decay does not click. It is the ordinary practice of an
    # audio editor, and it is inaudible at -32 dB.
    plancher_queue = crete_avant * (10 ** (-abs(args.queue_db) / 20.0))
    b_util = b_ms
    for i in range(b_ms, a_ms - 1, -1):
        if env[i] >= plancher_queue:
            b_util = i
            break
    b = min(n, (b_util + args.tail) * step)

    g = left[a:b]
    d = right[a:b]
    m = len(g)
    if m < step:            # less than a millisecond: touch nothing
        return {'name': name, 'note': 'trop court after analyse — INCHANGE'}

    # Fades. Short in (we do not want to soften the attack), longer out (a clean
    # cut on a non-zero tail clicks).
    ne = min(int(rate * args.fondu_entree / 1000.0), m // 4)
    ns = min(int(rate * args.fondu_sortie / 1000.0), m // 2)
    for i in range(ne):
        k = i / float(ne)
        g[i] = int(g[i] * k)
        d[i] = int(d[i] * k)
    for i in range(ns):
        k = i / float(ns)
        g[m - 1 - i] = int(g[m - 1 - i] * k)
        d[m - 1 - i] = int(d[m - 1 - i] * k)

    # Levelling. After the fades: they can only lower samples, so they can never
    # create the new peak.
    crete_apres = max(max(abs(v) for v in g), max(abs(v) for v in d)) or 1

    if args.uniform:
        gain = (32768.0 * (10 ** (CRETE_VISEE_DB / 20.0))) / crete_apres
        sonie_db = None
    else:
        loudness = loudness_max(g, d, rate)
        sonie_db = db(loudness)
        vise = SONIE_VISEE_DB.get(name, SONIE_DEFAUT_DB)
        gain = (32768.0 * (10 ** (vise / 20.0))) / max(loudness, 1.0)
        # The loudness target must never clip a transient: we bound the peak
        # AFTERWARDS, even if that leaves us a little under target.
        plafond = (32768.0 * (10 ** (CRETE_MAX_DB / 20.0))) / crete_apres
        if gain > plafond:
            gain = plafond
    for i in range(m):
        g[i] = max(-32768, min(32767, int(g[i] * gain)))
        d[i] = max(-32768, min(32767, int(d[i] * gain)))

    # Mono only when it is LOSSLESS: see the header - `alerte.wav` has its
    # channels in antiphase and a mono would empty it.
    sg = sum(float(v) * v for v in g)
    sd = sum(float(v) * v for v in d)
    sgd = sum(float(g[i]) * d[i] for i in range(m))
    corr = sgd / math.sqrt(sg * sd) if sg > 0 and sd > 0 else 0.0
    mono = corr > 0.995

    return {
        'name': name, 'rate': rate, 'g': g, 'd': d, 'mono': mono, 'corr': corr,
        'avant_ms': n * 1000.0 / rate, 'apres_ms': m * 1000.0 / rate,
        'coupe_tete_ms': a * 1000.0 / rate,
        'crete_avant_db': db(crete_avant), 'gain_db': 20 * math.log10(gain),
        'sonie_db': sonie_db,
        'bruit_db': (lambda b: db(b) if b is not None else None)(
            noise_floor(g, d, rate)),
        'sonie_apres_db': (sonie_db + 20 * math.log10(gain)) if sonie_db is not None else None,
        'regions': len(regions),
    }


def resample(g, d, facteur):
    """Changes the pitch by changing the speed. Linear interpolation.

    It is the simplest method, and here it is the RIGHT one: for a forty
    millisecond tick, a speed shift is indistinguishable from a pitch shift, and
    the length changing with it is a bonus - two variants that also differ in
    length are even harder to confuse."""
    m = int(len(g) / facteur)
    ng, nd = [], []
    for i in range(m):
        x = i * facteur
        j = int(x)
        f = x - j
        j2 = min(j + 1, len(g) - 1)
        ng.append(int(g[j] * (1 - f) + g[j2] * f))
        nd.append(int(d[j] * (1 - f) + d[j2] * f))
    return ng, nd


def tilt(g, d, rate, db, cutoff=2000.0):
    """Tilts the spectrum: a positive `db` brightens, a negative one darkens.

    === WHY EQUALISE AT ALL, WHEN THE PITCH ALREADY CHANGES ===

    Two variants differing only in pitch remain the SAME sound transposed, and
    the ear hears them that way - especially on a percussive tick, where pitch is
    barely salient. Varying the timbre as well turns the three variants from
    three versions of one sound into three neighbouring sounds, which is exactly
    the effect wanted.

    The filter is a plain one-pole low-pass, and the result is a BLEND of the
    signal and its filtered version. It is the cheapest shelving equaliser there
    is, and it needs no dependency. On a sixty millisecond sound, a
    higher-order filter would not be audible.

    The peak is restored afterwards: brightening adds energy in the treble and
    would clip a transient already close to the ceiling."""
    a = 1.0 - math.exp(-2.0 * math.pi * cutoff / rate)
    k = (10.0 ** (abs(db) / 20.0)) - 1.0
    out = []
    for canal in (g, d):
        lp = 0.0
        out = []
        for x in canal:
            lp += a * (x - lp)
            aigu = x - lp
            # db > 0 : on ajoute de l'aigu. db < 0 : on en retire.
            out.append(x + (k * aigu if db > 0 else -k * aigu))
        out.append(out)
    ng, nd = out

    before = max(max(abs(v) for v in g), max(abs(v) for v in d)) or 1
    after = max(max(abs(v) for v in ng), max(abs(v) for v in nd)) or 1
    r = float(before) / float(after)
    ng = [max(-32768, min(32767, int(v * r))) for v in ng]
    nd = [max(-32768, min(32767, int(v * r))) for v in nd]
    return ng, nd


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--dry-run', action='store_true', help="measure and print, write nothing")
    p.add_argument('--dir', default=RACINE)
    p.add_argument('--backup',
                   default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        '..', 'clients', 'borealis', 'sfx_original'),
                   help="where the originals are copied. OUTSIDE `resources/`: "
                        "that directory is packed into the console's romfs, and "
                        "leaving 1.2 MB of originals there would ship them on the "
                        "SD card for nobody ever to read")
    p.add_argument('--group', type=int, default=30,
                   help='ms of silence that separate two events')
    p.add_argument('--bridge', type=int, default=120,
                   help='ms beyond which a neighbouring region is never attached')
    p.add_argument('--gap-db', type=float, default=12.0,
                   help='dB below the peak under which a neighbouring region is '
                        'taken for an artefact and dropped')
    p.add_argument('--margin', type=int, default=3, help="ms gardees before l'attaque")
    p.add_argument('--tail', type=int, default=25, help='ms kept after the tail')
    p.add_argument('--tail-db', type=float, default=32.0,
                   help='dB below the peak under which the decay is cut (it is '
                        'inaudible and holds the device open)')
    p.add_argument('--fade-in', type=float, default=1.5)
    p.add_argument('--fade-out', type=float, default=25.0,
                   help='long enough that a sharp decay does not click')
    p.add_argument('--uniform', action='store_true',
                   help='normalise every peak to -6 dBFS instead of aiming at a '
                        'loudness per sound (destroys the intended hierarchy)')
    p.add_argument('--variants', action='store_true',
                   help='build nav_2 and nav_3 from nav_1 (+/- 2 semitones)')
    args = p.parse_args()

    folder = os.path.abspath(args.folder)
    if not os.path.isdir(folder):
        print('folder introuvable :', folder)
        return 1

    fichiers = sorted(f for f in os.listdir(folder) if f.endswith('.wav'))
    if not fichiers:
        print('no .wav in', folder)
        return 1

    sauve = args.backup if os.path.isabs(args.backup) \
            else os.path.join(folder, args.backup)
    if not args.dry_run:
        os.makedirs(sauve, exist_ok=True)

    print('%-16s %9s %9s %9s %8s %8s %9s %8s %6s' % (
        'file', 'before', 'after', 'head cut', 'peak', 'gain', 'loudness',
        'bruit', 'out'))
    print('-' * 98)

    total_avant = total_apres = 0
    for f in fichiers:
        path = os.path.join(folder, f)
        try:
            r = process(f[:-4], path, args)
        except Exception as e:
            print('%-16s  ERREUR : %s' % (f, e))
            continue
        if r is None or 'g' not in r:
            print('%-16s  %s' % (f, r.get('note', 'ignore') if r else 'vide'))
            continue

        total_avant += os.path.getsize(path)
        mesurable = r['bruit_db'] is not None and r['sonie_apres_db'] is not None
        snr = (r['sonie_apres_db'] - r['bruit_db']) if mesurable else None
        print('%-16s %7.0fms %7.0fms %8.0fms %6.1fdB %+6.1fdB %7s %8s %6s%s' % (
            f, r['avant_ms'], r['apres_ms'], r['coupe_tete_ms'],
            r['crete_avant_db'], r['gain_db'],
            ('%.1fdB' % r['sonie_apres_db']) if r['sonie_apres_db'] is not None else '-',
            ('%.0fdB' % r['bruit_db']) if mesurable else 'trop court',
            'mono' if r['mono'] else 'stereo',
            '  <- late floor %.0f dB below the sound' % snr
            if snr is not None and snr < 25 else ''))

        if not args.dry_run:
            if not os.path.exists(os.path.join(sauve, f)):
                shutil.copy2(path, os.path.join(sauve, f))
            write_wav(path, r['g'], r['d'], r['rate'], r['mono'])
            total_apres += os.path.getsize(path)

    if args.variants:
        src = os.path.join(folder, 'nav_1.wav')
        if os.path.exists(src):
            g, d, rate = read_wav(src)
            for name, demi_tons, teinte in (('nav_2', 2.0, 2.5), ('nav_3', -2.0, -2.5)):
                facteur = 2.0 ** (demi_tons / 12.0)
                ng, nd = resample(g, d, facteur)
                ng, nd = tilt(ng, nd, rate, teinte)
                mono = (g == d)
                print('%-16s  derive de nav_1 : %+.0f demi-tons, %+.1f dB d aigu, %.0f ms' % (
                    name + '.wav', demi_tons, teinte, len(ng) * 1000.0 / rate))
                if not args.dry_run:
                    write_wav(os.path.join(folder, name + '.wav'), ng, nd, rate, mono)
        else:
            print('nav_1.wav absent : step de variants')

    if not args.dry_run and total_avant:
        print('-' * 76)
        print('total : %.0f Ko -> %.0f Ko (%.0f %%)' % (
            total_avant / 1024.0, total_apres / 1024.0,
            100.0 * total_apres / total_avant))
        print('originaux conserves dans', sauve)
    elif args.dry_run:
        print('-' * 76)
        print("dry_run: no fname was changed.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
