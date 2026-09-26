---
name: Shadow WebRTC data channel format
description: the 4 named channels + the length-prefixed FlatBuffers format of the input messages, for M15
type: project
---
Found through a `chrome://webrtc-internals` dump + a JS hook on RTCDataChannel.send/onmessage during a real Shadow session (captured 2026-05-02, in `dumpchrome/inputs/`).

**Channels** (created by the client, in this order, stream IDs 1/3/5/7 — the odd convention for a DTLS server):
- `shadow-input` ordered=true maxRetransmits=1 protocol="raw" — clavier+souris bidirectionnel
- `shadow-cursor` ordered=true protocol="raw" — Shadow → client (curseur, bitmap)
- `shadow-controller` ordered=false maxRetransmits=1 protocol="raw" — manettes
- `shadow-clipboard` ordered=true protocol="raw" — clipboard sync

**Format binaire** (PPID=53, raw) :
```
+--------+----------------------------------+
| size:4 | FlatBuffers payload              |
| LE     | (size = total_len - 4)           |
+--------+----------------------------------+
```
After the size come 12 bytes of constants (`18000000 00000000 00000000`) then the FlatBuffers vtable+data (the offsets `0e0020001f0014000c000b0004000e000000` are typical).

**Sizes observed**:
- a 96 B SEND (size=0x5c): the initial init/handshake
- a 144 B SEND (size=0x8c): a mouse-move event (~60-100/s while moving, with an incrementing sequence visible at offset ~0x4c)
- a 152 B SEND (size=0x94): keyboard / click / another input with an extra payload
- a 104 B RECV (size=0x64): the server's ack/echo (with a seq matching the SEND)
- 392b RECV cursor (size=0x184) : update curseur position
- 4232b RECV cursor (size=0x1084) : update curseur full (avec bitmap probable)
- ~5200b SEND clipboard

An 8-byte timestamp-looking field in every message (e.g. `e527 e69d 0100 0000`).

**Why:** without this information M15's inputs are impossible to implement. The format is confirmed as **FlatBuffers** (consistent with the Ghidra RE that said "per-stream FlatBuffers" — project_shadow_streaming_re.md).

**How to apply:** for M15 we need to:
1. Implement SCTP/DCEP to open the 4 channels (sctp.c stage A is already done)
2. Decode the bytes captured in `dumpchrome/inputs/raw_dc/` to extract the FlatBuffer schema (or use `flatc --binary --raw-binary` after extracting the schema from libavcodec/libcef or from a Ghidra pass on the Linux client)
3. The 96 B SEND init format is probably reusable as is (no relevant timestamp, just a setup), a good lead for validating the SCTP pipe before generating it dynamically
4. The mouse-move message = 144 B with a seq (offset 0x4c) + delta x/y + a timestamp
5. The web client also sends `Sending hid-sync {numlock,capslock,scrolllock}` at start-up, which looks like a message on shadow-input — look for the corresponding pattern
