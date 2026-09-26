---
name: project-i3-audio-native-done
description: Audio (Opus + output) and mouse/keyboard wired up on the native path - both were arriving and being dropped, for two different reasons.
metadata:
  node_type: memory
  type: project
---

# I3 + I1 phase 3 - audio and input wired on the native path (2026-05-18)

## Audio

**Symptom**: the audio callback was `NULL` in the glue, and the DTLS channel was
opened with no callback at all. The receive thread got its Opus packets and
dropped them silently - total silence on the native path.

**Fix**: align the callback's signature with the decoder's feed (`uint32_t`
RTP timestamp rather than a `uint64_t` milliseconds value nothing used); have
the glue create an `audio_decoder` at startup; forward each Opus payload from
`on_audio()`; pass the callback when opening the channel; destroy the decoder on
cleanup.

## Input

**Symptom**: the input TCP+TLS channel opened correctly, but no mouse or
keyboard event ever arrived. The view was calling the posting functions, which
drained onto the **SCTP/WebRTC** path - not connected on the native path.

**Fix, and it is an architectural one**: a small `native_input` bridge - a
thread-safe singleton with an atomic active flag. The session sets it after
opening the input channel and clears it on cleanup; the existing drain thread
gains a branch that routes to it when active, instead of to SCTP. The GUI call
sites are unchanged, which is the whole point: the view keeps posting the same
way, and the transport underneath is swapped.

**Note on what this leaves**: the module keeps the name `ctrl_audio_dtls`, whose
premise was refuted in August - `:base+12` is the INPUT channel, and audio
arrives on `:base+30`. See [[project_audio_FOUND_cursor_channel]] and
[[project_channel_map_proven]].
