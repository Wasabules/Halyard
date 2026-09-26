---
name: project_S46_server_announces_phantom_entity
description: SUPERSEDED — the correlation was right, its interpretation wrong. See project_S57_audio_solved_we_spoke_too_early
metadata:
  type: project
---

> **SUPERSEDED on 2026-08-27.** The correlation documented here is RIGHT; its
> interpretation is wrong. The notification means "**AUDIO channel down**" and the
> identifier it carries is a sessionId: these are OUR channels, not a "second
> entity". The real cause — we were killing the channel by emitting while the
> server was answering — is in
> [[project_S57_audio_solved_we_spoke_too_early]]. The note is kept because
> this correlation is what put us on the trail.

The `:base+30` audio channel starts in ~30 % of sessions, in a binary and
decided at bootstrap. Seven client-side hypotheses eliminated (KB §3.29), the bootstrap
made byte-identical to the official client's: no effect.

**The server had been announcing the cause all along.** Its small status reports
on the control channel (36/41/49 B), which we only logged by their
TAILLE, listent les entites enregistrees :

    12 0c 0a 0a 08 05 1a 06 08 <id> 10 <horodatage>

A correlation measured across 8 consecutive sessions, **without exception**:
- entite `05` SEULE          -> son present (3/3)
- entite `01` presente       -> son absent  (5/5)

So a second entity is registered server-side, and as long as it is, the audio
stream does not reach us. That fits condition 2 of the campaign
audio: without a clean release on close, the subscription stays attached to the
previous client. The official client, the only client on its VM, never encounters
le probleme.

**Piste** : `GET /clients` (K14) enumere les clients, `proximus_delete()` existe
already — identify entity `01` and delete it before starting.

**Lesson**: we were logging the SIZE of those messages, not their content. The
difference had been visible for hours (137 against 140 bytes) without being
readable. An undecoded protocol message is lost information. See
KB.md §3.35.
