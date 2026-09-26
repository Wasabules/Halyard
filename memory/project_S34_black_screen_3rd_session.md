---
name: project_S34_black_screen_3rd_session
description: The server only sends a key frame on the 1st session; two function statics blocked every request afterwards (18/20 sessions with no picture)
metadata:
  type: project
---

An automated series of 20 sessions on console (2026-08-25, campaign S34): **18 attempts
out of 20 without a single picture**, with a perfectly normal inbound bitrate (22 MB,
`perdus=0`). The symptom reported: "I no longer have video decoding but I can see a
debit video entrant ».

**A protocol fact**: the Shadow server only emits SPS+PPS+IDR spontaneously on the
FIRST session. On reconnection it carries on with its GOP (NAL type 1 exclusively).
The decoder discards everything until its first key picture → a black screen if we do not
does not ask for one.

**Two locks, the same bug class** — function `static`s that outlived
the session while they compare themselves against counters reset to zero:
1. `g_last_evt_idr_tick` against `ip_counter` (a local of `ctrl_session_run`): after a
   24 s session, `0 - 500 = -500` → no more requests at all, for the whole process.
2. G13 testait `++calees == 15` (egalite stricte, `calees` statique) → un seul tir
   par processus.

The log counters were static too: mute from the 2nd session on.
**That is what masked the failure across several campaigns.**

The fix: the state grouped into `video_feedback_t` / `idr_stall_t`, reset by
session ; cadence en arithmetique modulaire 16 bits ; demande repetee ; demande
emitted right at the start of the session. The rules are isolated in the pure module
`streaming/idr_policy.h`, couvertes par `tests/test_idr_policy.c` (17 verifications,
6 echouent sur l'ancienne logique).

**VALIDATED** (a series of 10 sessions after the fix, 2026-08-25): **10/10 with a picture**,
1073 frames on average per 24 s session. The gate opens ~1 s after the start,
against 15 s on attempt 2, and never afterwards.

**A reflex worth keeping**: in the session path, a function `static` is suspect by
default — especially if it compares itself against a session counter. See
KB.md §3.28. Same series: the cursor/audio channel active 13/20 (65 %), so not a
defaut Switch-specifique — voir [[project_audio_FOUND_cursor_channel]].
