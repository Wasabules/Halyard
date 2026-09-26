---
name: project_measurement_campaign_20260828
description: The autonomous 25-session desktop campaign of 2026-08-28 — verdicts on G52, G53, V9 and V10, and the lesson about revert toggles
metadata:
  type: project
---

Full report: `captures/tests_autonomes_20260828/RESULTATS.md`.
Replayable scripts: `tools/test/campagne*.sh` + `tools/test/summary_20260828.py`.

## Verdicts

- **G52 CONFIRMED** — the gamepad announcement finally goes out on desktop,
  `branchements=1` (no phantoms), the control without a gamepad silent. It
  required a **uinput** gamepad: `tools/test/virtual_gamepad.c`.
- **G53 NOT REPRODUCED** — zero bytes received on `:base+13` in 60 s, three times,
  with and without the UDP registration. KB §3.40 demoted to C60. The remaining
  hypothesis: the socket is `connect()`-ed and would filter out a reply coming
  from another source port. To be settled by `tcpdump`, not by re-reading code.
- **V9 QUANTIFIED** — ~3.6 short packets/s on `:base+10` were poisoning the
  congestion-feedback clock. Real and permanent.
- **V10 MEASURED** — loss 0.21 % -> 0.13 %, **6 pairs out of 6** (p = 0.016),
  bitrate +20 %, no trace of N29's drop. The mechanism is **not established**.

## Two methodological lessons

**A revert toggle must RESTORE, not cut.** `SHADOW_NACK_TICK=1` removed the flush
instead of putting it back on the beat: the block had been moved, not duplicated.
So the first campaign was comparing V10 against "no retransmission at all". Found
by LOOKING at the counter, not by re-reading the code. See
[[feedback_static_etat_de_session]] for the neighbouring family.

**A counter does not always measure what you think.** `chunks_redundant` was
supposed to prove that the server answers our `rG`s: it only counts USELESS
retransmissions (a slot already filled). The one that REPAIRS fills a hole and
never appears there. So its zero says nothing about whether retransmissions exist
— only that none is wasted.

## An open anomaly

`chunks_orphan` ≈ **4 %** of all chunks (~4000 per 90 s session), with
`orphan_dup = orphan_lost = 0`: all of them from the "max_chunks differs from the
current frame" path. Never looked at.

See [[feedback_verifier_les_mesures_grep]] — and note that `grep` is aliased to
`ugrep` in this environment: it silently returns zero lines on some patterns that
work with `awk`. Analyse the logs with `awk`.
