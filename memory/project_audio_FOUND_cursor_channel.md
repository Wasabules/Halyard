---
name: project-audio-found-cursor-channel
description: The audio is multiplexed onto :base+30, the channel our code calls the cursor's. Same SUFP framing, same chacha20; plaintext is [0x12][seq] then raw Opus at byte 5.
metadata:
  node_type: memory
  type: project
---

# AUDIO FOUND - it was on the cursor's channel

2026-08-21, zero decode errors. KB §3.26.

**Where**: `:base+30`. Found by a guided capture alternating silence and sound
(`tools/capture_audio_run.sh`): 19.8 -> 170.3 packets/s when the sound comes on.
`:base+12` is input (refuted by measurement) and `:base+15` is SSH/SFTP.

**Framing**: the same 11-byte SUFP header as video, the same chacha20. The
plaintext is `[0x12][seq u32 LE]` and then **raw Opus from byte 5**. A TOC of
`0xf4` is CELT fullband stereo, 10 ms - 480 samples at 48 kHz.

**Three conditions, all of them necessary**:

1. A `0x70` ping every **7.5 s**, the same cadence as the input keepalive.
2. `kUnregisterSession` (oneof f9) on close - without it the server keeps the
   subscription and the channel is SILENT on the next connection, which is why
   it worked one session in two.
3. **Deduplicate**: every frame is sent twice. A sliding window of 64 sequence
   numbers, not a comparison against the last one.

**Two traps that cost several attempts**:

- `opus_decode` accepts a badly framed packet and returns NOISE without an
  error. Validate with `opus_packet_get_nb_samples` first.
- Writing to ALSA from the receive thread blocks it. Ring buffer plus a
  dedicated thread.

**Why this took months**: no capture had any sound in it. A channel that is
active continuously - the cursor - hides perfectly a channel that only wakes up
when there is something to play.
