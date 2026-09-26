---
name: project_S36_sound_one_session_only
description: Sound was only audible in the 1st session — the audio de-duplication window lived in a static and discarded 100% of frames afterwards
metadata:
  type: project
---

Symptome utilisateur : « l'audio je l'ai entendu une seule fois ».

The server emits every Opus frame **twice** on `:base+30`. A window
sliding window of 64 sequence numbers plays only one of them (AUD8). That state lived in
`static` inside the receive function (`seq_hi`, `seq_seen`, `seq_init`) and
therefore outlived the session. And the server **renumbers on every stream**: the
next session sent much lower numbers, the computed difference came to
~-1,000,000, hence "far too late", and **every** frame was discarded.

Mesure decisive (`audio_dup_skipped` vs `udp_audio_pkts`) :
- the first session with sound: 3998 frames, **1999** discarded — one in two, correct
- every session after it: 3997 frames, **3997 discarded — 100 %**

Fix: an `audio_dedup_t` in the session context, with the rule isolated in the pure
module `streaming/audio_dedup.h` + `tests/test_audio_dedup.c` (14 checks,
including one that demonstrates that reusing the previous window does not
joue aucune trame).

**The third occurrence of the same family** after [[project_S34_black_screen_3rd_session]]
(the picture) and [[project_S35_udp_buffer_hos]]: session state kept inside
a function `static`. In the session path, a `static` is suspect
par defaut.

**A measurement trap worth remembering**: the `:base+30` channel only lights up
when the VM emits sound (3998 audio packets out of 4003 when it is active). So an
autotest on a silent desktop does NOT measure audio — the absence of sound there
proves
nothing. See KB.md §3.29 and §3.31.
