---
name: project_S57_audio_solved_we_spoke_too_early
description: Intermittent audio RESOLVED — our ping and re-registration went out in the window where the 1st frame arrives; 10/10 against 30 %
metadata:
  type: project
---

The `:base+30` audio channel only started in ~30 % of sessions. Seven hypotheses
server-side hypotheses tested and refuted, two nights of measurement.

**The cause was on our side.** `t_last_cursor_ping_ms` and `t_last_cursor_reg_ms`
were 0, and `now_ms` is a monotonic clock in ms: `now_ms - 0 >= 7500` was true
from the very first iteration. So our `0x70` ping and our registration resend went
out ~200 ms after the channel was registered — exactly when the first audio frame
is in flight (201-227 ms measured). The official client emits NOTHING in that
window: its first ping goes out at +7.5 s and it
never re-registers.

**AUD5 had been added to rescue a channel that does not start. It is the thing
that was preventing it from starting.**

A correlation across 69 sessions: AUD5 fires -> 0 sessions with sound out of 21.
Validation: **10/10 with sound** against 21/69 before, and zero `CHANNEL_DOWN`.

**The server had been telling us all along.** The notification that correlated
32/32 means "AUDIO channel down" — decoded thanks to the table of channel
canaux du binaire officiel (`VIDEO, AUDIO, INPUT, CURSOR, MICRO, CONTROLLER,
CLIPBOARD, FILETRANSFER`, indices 0 to 7) and the format string
`ctrlchanv2: Channel down: {} (sessionId={})`. It had been sleeping in our logs,
journalisee en hexa, jamais lue.

**Lecons** :
- an undecoded protocol message is lost information;
- before looking for a server-side cause, check our own emission SCHEDULE
  emission — not only the content of what we send;
- an interval counter initialised to 0 against a monotonic clock fires
  IMMEDIATELY. Check every `now - t >= delay` where `t` starts at zero.

Voir KB.md §3.38. Lie a [[project_S46_server_announces_phantom_entity]] (dont
the "phantom entity" interpretation is refuted by this one).
