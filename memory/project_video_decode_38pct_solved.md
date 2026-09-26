---
name: 🎯 Video decode 38% — diagnostic complet
description: 2026-05-09 ~11:00 — A partial 38% picture on the native shadow-client. Full diagnosis through a bitstream dump + ffmpeg analysis. The bug = the server is multi-slice and we only receive the top slice.
type: project
---

## TL;DR

The shadow-client Linux GUI (in SHADOW_NATIVE=1 mode) shows the top 38% of the Windows desktop correctly + the remaining 62% = error concealment "smeared downwards". 

Diagnostic via `/tmp/shadow_dump.bin` + ffmpeg :
- SPS lit `1920x1080 yuv420p 25 fps High@52` ✓
- The IDR slice ends at **MB(0, 26), bytestream -5** = exactly 38% of the height
- Across 84 captured slices, **83 have first_mb_in_slice=0** (= top) and **only 1 has first_mb=4080** (= the bottom half, at row 34)

**Conclusion**: the Shadow server sends the picture **multi-slice**, where each displayed frame = 2 vertical slices (top then bottom). We receive the TOP slice a lot and the BOTTOM slice almost never. The bottom slice must be on another UDP channel / subchannel / chunk type.

## Wire format SUFP nonce — CONFIRMÉ

Pour chunks `byte10==1` (= data) :
```
nonce = [u8 idx + 0x10] || [11 bytes session-static]
       ↑
   a counter that increments per chunk
```

Exemple frame max=24 :
```
idx=0 nonce=10 84c405775dc36e9d41b740
idx=1 nonce=11 84c405775dc36e9d41b740
...
idx=9 nonce=19 84c405775dc36e9d41b740
```

Frame suivante max=10 :
```
idx=0 nonce=20 84c405775dc36e9d41b740   ← counter continue +0x10
```

Pour chunks `byte10==0` (= parity) :
- A **random** nonce per chunk (= not a counter, not derived)
- Decrypting with our chacha20-poly1305 key = **tag verify fails**
- Either a different key, or an extra AAD, or unencrypted Reed-Solomon parity (= it just looks random)

## Pattern multi-slice serveur

Pour 1080p, MB grid = 120×68 = 8160 MBs total.

Slice top : `first_mb_in_slice=0` couvre MB 0..4079 (= 34 lignes du haut)
Slice bottom : `first_mb_in_slice=4080` couvre MB 4080..8159 (= 34 lignes du bas)

Over 30 s of capture:
- 1 IDR keyframe: 12410 bytes, the top slice only
- 82 P-slices of 1241 bytes each: all top slices, first_mb=0
- 1 P-slice at 4080 = a bottom slice (= rare, a single occurrence!)

## Hypotheses for recovering the bottom slice

1. ~~A different UDP subchannel~~ — TESTED: we have `subchan=0` for keyframes + `subchan=1..7` for P-frames. Our code already handles every subchannel (= through `g_vid_reasm[subchan]`). But EVERY parsed NAL has `first_mb=0` except one isolated occurrence at 4080. So the server **does not send** the bottom slice regularly.
2. **The real pattern**: `byte0=0x13` (ver 1, type 3) = keyframes only. `byte0=0x23` (ver 2, type 3) = P-frames. The nonce counter `[u8 ctr+0x10][11B static]` continues across subchans (= 0x10..0x19 on subchan 0 keyframe, then 0x20 on subchan 1 P).

## Cause racine

The server does multi-slice top/bottom H.264 + inter-frame prediction. The initial IDR contains ONLY the top slice (= 12 KB). The bottom slice is never sent as long as the lower area does not change (= a bandwidth optimisation). The decoder stays in perpetual error concealment for the bottom.

## Solutions possibles

1. **Ask for a complete IDR** through the SslCtrlChanV2 control channel. Probably an RTCP-like message or JSON over TCP.
2. **Force the server** to send the bottom slice by simulating input that modifies the lower area.
3. **RE the desktop app** to understand how it handles this case (= it does not seem affected → either it receives the complete IDR, or it has logic for requesting a slice resend).

## Failed attempts (= what is not enough)

- ✗ **Bootstrap byte-exact** (Cap, Auth1, Enc2, HB3, Reg4, Chans5-12)
- ✗ **Heartbeat 1Hz** sur SslCtrlChanV2 (= len=89 message protobuf)
- ✗ **Ready/DisplayReady messages** (= seq=18 `12 02 3a 00`, seq=19 `12 0a 32 08 0a 02 08 01 12 00`)
- ✗ **iP UDP keepalive** sur :15010 (= 6 bytes magic 'iP' counter)
- ✗ **An AAD decrypt** of the byte10==0 chunks (= hdr_11, hdr_4, frameid and chunkmax tested) — no AAD works

## Desktop wire-format findings (= confirmed through strace)

### Sur UDP video :port_base+10

L'app desktop envoie 4 types de packets feedback :

**`gE`, 15 bytes** (= ~14 Hz, = the most frequent packet):
```
67 45 01 [byte3] [u16 v1 LE] 00 [s32 LE delta_us] [u32 LE last_frame_id]
```
- `byte3` ∈ {0x50, 0x74, 0x78, 0x7c, 0x84, 0x88, 0x8c, 0x90, 0xb4} = mult de 4, varies
- `v1` ∈ {0x0270, 0x039e, 0x03bd, 0x03e5, 0x340d, 0x3421, 0x3435, 0x3563} = varies par session
- `delta_us` = signed jitter (ex: +21, +8, -25, -8)
- **`last_frame_id` = bytes 6-9 of the LAST SUFP chunk received (= an ECHO ACK)**

Confirmation byte-exact via strace :
```
recvmsg fd=271 → "#\274\0\0\7\0\363g\251\23\1..." (= byte0=0x23 byte1=0xbc bytes 6-9 = f3 67 a9 13)
sendmsg fd=271 → "gE\1\3505\4\0\6\0\0\0\363g\251\23"  (= bytes 11-14 = f3 67 a9 13 ← MATCH)
```

**`PI 01` 3 bytes** : keepalive simple, ~5×/s
**`p` 1 byte** : ping single byte 0x70, ~5×/s
**`16 bytes encrypted`** : occasionnel (= 1 par session ~15s)

### Format SUFP ver 2 (nouveau)

byte 0 = **0x23** (= ver 2 type 3) instead of 0x13 (ver 1). The observed subchans go up to 0xbb (= 187), not just 0..7.

### The root cause identified

Without sending `gE` ACKs with a correct `last_frame_id` echo, the server does NOT progress in its streaming → a perpetually partial picture.

### Limitation actuelle

Our implementation sends `gE` byte-exact (= confirmed through strace), BUT the picture stays partial. A hypothesis not yet validated:
- `byte3` must be an **audio jitter measured** dynamically (= not a constant 0x88)
- `v1` must be a **value derived** from the audio sample rate / output_id (= not a constant)
- `delta_us` must be a **real delta** between packets (= not 0)

The desktop app computes those stats through `VideoNetworkStatsAnalyzer`, which we have not yet decompiled in depth.

## The next RE step (= a TODO to finish the picture)

Identified in Ghidra:

- `VideoNetworkStatsAnalyzer` typeinfo @ virt 0x12a96b0, _ZTI @ 0x12a9690, _ZTV @ 0x12a97c8
- Vtable methods : 0xc2f8f0 (dtor), 0xc2fa20 (delete), 0xc33180, 0xc32880, 0xc364e0, 0xc364f0, 0xc364c0, 0xc36520, 0xc36490
- `FUN_00923b10` (4365B, App.cpp section "Using iframe request proto v") = handler iframe request, dispatch sur 3 protocols version :
  - proto v1: `FUN_00c1cf00` (~277 B) — the `gE 01` init packet at `param_1+0x3b9`
  - proto v2: `FUN_00c13650` (~713 B) — the `gE 01` init packet at `param_1+0x44a1`
  - proto v3: `FUN_00c24ca0` (~1050 B) — not decompiled yet
- `output_id` is passed to those functions as `*(u16*)(param_1 + 0x88)`, and `byte3 = 0x88` is used as a referenced constant

**To finish the complete picture**:

1. Fully decompile `FUN_00c13650` (= the proto v2 builder, the most likely one in use)
2. Identify how `byte3`, `v1` and `delta_us` are computed (= probably from the jitter, output_id and last_chunk_arrival_ts)
3. Implement those computations in our code
4. Tester

Estimate: 3-5 h of extra RE. Implementation: ~1 h.

## RE session 2026-05-09 ~13:00 — failed attempts

Several approaches tested **without unblocking the bottom picture**:

1. ✗ **A byte-exact gE with a last_frame_id echo**: confirmed through strace that our app sends a correct `67 45 01 88 35 04 00 00 00 00 00 [u32 fid]`, but the server stays in degraded mode.
2. ✗ **Cross-subchannel reassembly**: modified on_video_packet to handle subchannels 1-7 (= confirmed through an OTHER dump that they contain H.264 P-slices), accumulating into a cross-subchannel display_buf per timestamp, flushing on a ts change. The result = "the picture collapses and washes out" → subchannels 1+ in fact contain REDUNDANT frames (= retransmits or other streams), not additional spatial slices. **Important: only the `subchan != 0` skip was restored, for stability.**
3. ✗ **VID_MAX_SUBCHANS raised to 256**: no subchan>7 chunks received in our short VM sessions. The desktop strace saw subchan=0xbb but our VM uses 0..7.
4. ✗ **Ready/DispReady delayed to 5 s post-bootstrap** (= mimicking the desktop's seq=18-19): no effect.
5. ✗ **iP RequestIFrame** (= `69 50 00 02 [output_id]`) : aucun effet.

## Future RE leads, unexplored

- **8 stat reports per channel**: at seq=79..86 (~30 s after the bootstrap) the desktop sends a burst of 8 `f9 sub { f1 sub { chan_id, stat_value } }` messages. A possible trigger.
- **The exact computation of the `gE`'s byte3, v1 and delta_us** through a full decompilation of FUN_00c13650 and FUN_00c24ca0.
- **Advertised capabilities** in Capabilities/ChannelAnnouncements: perhaps a "high_color_fidelity", "hdr" or "vr" flag that we do not set.
- **The 8 channels announced**: our bodies are byte-exact BUT perhaps the client has to declare `output_id` correctly.
- **A deep Frida hook** on the builders FUN_00c1cf00/c13650/c24ca0 to see the real runtime arguments.

## Hooks and tools used

- **strace** on ShadowPCDisplay: works very well after ptrace_scope=0
- **Frida 17**: works in `python3 frida_attach.py PID script.js` mode. Limitation: `Module.findExportByName(null, ...)` is obsolete → use `libc.findExportByName(name)`. send/sendto hook correctly. sendmsg requires custom iov parsing.
- **Ghidra headless** : `cd /tmp; /opt/ghidra/support/analyzeHeadless $REPO/06-shadow-recon-linux/ghidra-projects shadow-recon-linux -process ShadowPCDisplay -noanalysis -scriptPath /tmp -postScript MyScript.java`

## RE 2026-05-09 ~15:00 — transport architecture findings

### 3 video transports supported by Shadow

The desktop app has 3 video-channel implementations:
- `VideoUdpChannel` (= `video_udp_channel.cpp`) — what we use (= raw UDP encrypted with chacha20)
- `VideoTcpChannel` (= `video_tcp_channel.cpp`)
- `VideoSslTcpChannel` (= `video_ssl_tcp_channel.cpp`) — TCP+TLS

Confirmed by the 3 Ghidra builders: `FUN_00c1cf00` (UdpChannel), `FUN_00c13650` (TcpChannel = the mention `s_video_channel_tcVideoTcpChannel_a_012a7000` in the decompilation), `FUN_00c24ca0` (SslTcpChannel).

Pareil pour audio/cursor/input : `cursor_ssl_tcp_channel.cpp`, `audio_out_ssl_tcp_channel.cpp`, `input_flatbuffers_ssl_tcp_channel.cpp`, `stfp/stfp_ssl_tcp_io_channel.cpp`.

### QUIC supported

Strings : `Client {}: QUIC is supported for at least one chan, start client`, `QuicClientConnected`, `InputManager::SendMouseButton called but transport protocol is not QUIC`.

→ At least the **inputs** (= mouse, keyboard) require **QUIC** on the desktop app.

### The 8 channel announcements decoded

The wire format of each CI_BODY:
```
f<channel_type> (sub) :
  f1 (sub) :
    f4 (sub) :
      f2 = codec_id ou similar
      f3 = ? (= 1 souvent)
      f4 = output_id (= 1)
  f2 (sub) [optional] :
    f1 = width / sample_rate
    f2 = height / bit_depth
    f3 = refresh_hz_float
  f7 (varint) [optional] :
    max_bitrate
```

Channel types observed (= the top-level field number):
- **f1 = VIDEO** (= seq=5, 1920x1080@143Hz, 20Mbps)
- **f2 = AUDIO_OUT** (= seq=8, 48000Hz, 16bit)
- **f3, f4** = CURSOR/INPUT/?
- **f5 = AUDIO_IN** (= seq=11, 48000Hz, 16bit) ou MICRO
- **f6, f7, f8** = autres

### The "bottom slice IDR" hypothesis

The server only provides the FULL FRAME IDR when the client uses **VideoSslTcpChannel** or **QUIC**, not a raw **VideoUdpChannel**.

It is a protection to save UDP bandwidth (= a multi-slice IDR over UDP is often lost → forcing multiple resends that saturate the link).

### Pour finir

Implement `VideoSslTcpChannel` in our native stack. That is probably one day of work:
- Set up TLS through wolfSSL on `:port_base+11` (= the same port as ctrl)
- Define the TCP video wire format (= probably different from the UDP SUFP)
- Receive multi-slice IDR + decode

An alternative: implement QUIC through the msquic library (= more complex, ~1 week).

## RE 2026-05-09 ~15h45 — strace volumes par port

Confirmed through strace on ShadowPCDisplay (= a live session captured):

| Port | Type | Recv pkts | Send pkts | Role |
|---|---|---|---|---|
| :15010 | UDP | 5440 | 640 | **Video chunks SUFP** (= ce qu'on utilise) |
| :15011 | TCP | 81 | 89 | **CtrlChanV2** (= ce qu'on utilise) |
| :15012 | UDP | 7 | 3 | Audio mineur |
| :15013 | UDP | 0 | 4 | Input (= mouse/keyboard) |
| :15014 | TCP | 2 | 2 | Inconnu (faible volume) |
| :15015 | TCP | 8 | 11 | Inconnu (faible volume) — RECV/SEND pas READ/WRITE |
| :15020 | TCP | 2 | 6 | Inconnu (faible volume) |
| :15030 | UDP | 337 | 5 | **Cursor** (= ce qu'on utilise) |

**Conclusion on the ports**: the entire video VOLUME is on :15010 UDP (= 5440 packets / 30 s = 180 packets/s). The secondary TCP ports carry negligible volumes. **So the bottom slice does arrive through UDP :15010, not through another transport.**

Our app receives ~295 packets/s on :15010 (= MORE than the desktop) but only manages to decode 38 % of the picture. So the bug is in **our handling**, not in the packets' arrival.

## The final hypothesis — INVALIDATED

The hypothesis: "the bottom slice is in the byte10==0 chunks".
**Confirmed through offline tests** that it is NOT the case:

- ✗ Standard chacha20-poly1305 decrypt with the packet's key + nonce: InvalidTag
- ✗ Decrypting with derived keys (= reversed, XOR, sha256(key), shifted, halves): none work
- ✗ Decrypting with AAD = the 11 B hdr / 4 B / frame_id / chunk_idx / byte10 / subchan: none
- ✗ parity == XOR(data ciphertexts): a 0.6% match (= random)
- ✗ Parity raw == XOR(data plaintexts) : 0.4% match
- ✗ Raw ChaCha20 decrypt (= no Poly1305) of the parity: not valid H.264
- ✗ The parity nonces are completely random (= not derived from chunk_idx)

**Conclusion**: Reed-Solomon over Galois Field 256 with a coefficient matrix (= encrypted with AEAD?). Too complex to reverse without more RE.

## The definitive root cause

The **Shadow server really does send an IDR of only 12 KB** for our VM = covering the top 38 % of the picture. The `byte10==0` chunks are **Reed-Solomon parity** (= useful only if data chunks are lost, not for getting the bottom).

The bottom slice is **NOT sent over UDP**. Confirmed through:
- 5440 desktop video UDP packets / 30 s = the same volume as us (= ~180 packets/s)
- Bytes 11-14 of the `gE` feedback = an ECHO of the frame_id (= just an acknowledge mechanism)
- Negligible volumes on the other secondary TCP ports (= 8-11 packets on `:15015` per 30 s)

So the desktop app receives the bottom slice over **another transport**:
- Either through `VideoSslTcpChannel` (= TCP+TLS with a different wire format, a secondary port)
- Or through `QUIC` via libmsquic (= used for inputs at the very least)

## A roadmap to finish the complete picture

**Option A (RECOMMENDED, 1-2 days)**: implement `VideoSslTcpChannel`
1. Identify the port the desktop uses for that channel (= ports 15014, 15015, 15020 are candidates)
2. Run an strace on the desktop capturing the TCP_READ payloads to see the wire format
3. RE le wire format VideoTcpChannel via Ghidra (= `FUN_00c13650` already partially decompiled)
4. Implement the TLS setup + parse + decode function
5. Route the received NAL slices into the decoder

**Option B (3-5 days)**: implement QUIC through libmsquic
1. Link libmsquic.so.2 into our build
2. Setup QUIC client (= MsQuicOpen API)
3. Bind to whatever port server uses for QUIC channels
4. Parse the QUIC streams and route them to the decoder

**Option C (a workaround, NOT IDEAL)**: force the server to send a FULL IDR by spamming inputs
- Simulate mouse movements in the lower area of the picture
- The server then sends a delta P-frame covering that area
- Progressivement l'image se reconstruit
- But slow and unstable.

## RE 2026-05-13 — the SetVideoBitrate finding

**The server sends 1 Mbps by default (= ~415 packets/s), not the 20 Mbps announced.**

The pattern observed in stable streaming (= subchan=0 only):
- 414 packets/s received on `:port_base+10`
- 1 keyframe of max=24 chunks every 5 s (= 10 data + 14 parity)
- P-frames max=12 chunks chaque (= 1 data + 11 parity = 92% redondance !)
- Total bandwidth used: ~1 Mbps instead of the 20 Mbps requested in RegisterSession

→ The server stays in a very low bitrate mode. The trigger for high bitrate is probably a separate protobuf message.

### Critical strings identified

- `Requesting {}-{} video streaming session: {}x{}@{}fps, max bitrate: {}Mbps, codec: {}, cursor merged: {}, high color fidelity: {}, hdr: {}, vr: {}` @ virt 0x12598c8
- `Requesting to update {}-{} streaming session bitrate to {:.1f} Mbps` @ virt 0x12599c0
- `set_user_bitrate`, `dynamic_bitrate` (= proto field names)
- `UpdateBitrate`, `UpdateStreamingSessionState`, `UpdateStreamingSessionId` (= proto message types)

### Ghidra functions identified

- `FUN_00853770` (= 5848 bytes): sends the first "Requesting video streaming session" message with all the parameters (= width, height, fps, max_bitrate, codec, flags). Source: `CtrlChanV2Manager.cpp`.
- `FUN_00855c10` (= 2423 bytes): `SetVideoBitrate` at `CtrlChanV2Manager.cpp:0x509`. It sends "Requesting to update streaming bitrate". The error "There are no streaming session available" if called before a session exists.

### What is missing to finish

**A live desktop capture**: relaunch ShadowPCDisplay with the LD_PRELOAD hook + trigger 30 s of streaming. Identify the additional protobuf message or messages we do not send, which are the real trigger for high-bitrate streaming.

Without that capture we are coding blind. The gE byte-exact / Ready-DispReady / iP RequestIFrame hypotheses have all failed.

**An iterative workflow**, operational now:
- `SHADOW_AUTO_CONNECT=1 SHADOW_FRAME_DUMP=1 ./shadow-client` → auto-login + auto-connect + PPM dumps of frames 1, 10, 30, 60, 120, 300
- 1 iteration = ~30 s (= edit + rebuild + relaunch + capture a frame)

## RE 2026-05-13 ~midnight — a live desktop capture + seq=17 implemented

**A successful LD_PRELOAD capture** of the Shadow desktop. A UNIQUE seq=17 message identified (= sz=102, body=98):

```
00 01 00 62        wrapper [type=1][len=98]
08 11              f1 varint = 17 (seq)
12 0d              f2 sub len=13 (= Request)
  6a 0b            field 13 sub len=11 (= 0x6a = 13<<3|2)
    0a 09          field 1 sub len=9 (= EncodingParams)
      10 b1 c4 3e  field 2 varint = 1024561 (= ~1 Mbps bitrate)
      1d 00 00 0e 43  field 3 fixed32 = 142.0 float (= refresh rate Hz?)
22 41 [client_info 65B]
2a 0c [OCapture 14B]
```

Implemented `ctrl_build_video_encoding_config()` byte-exact. Inserted between the channels (seq=12) and Ready (seq=18). Test: **NO visual change** — the picture stays at 10% top.

**A crucial finding from the stats**: our VM receives 99 % small packets (< 39 bytes = server ACKs/metadata), only ~1 % are video data chunks. That is consistent with the partial picture.

The desktop app receives the **SAME volume** (= ~340 packets/s on :11010, against our 425 packets/s). But it displays a complete picture. Hypothesis: **a Reed-Solomon Galois Field** over the 14 parity chunks per keyframe is needed to reconstruct the full image. Our app rejects that parity → we only have the 10 initial data chunks, which cover only 10 % of the picture.

### Vraie roadmap finale (= seul moyen de finir)

**Implement Reed-Solomon GF(256)**: use a library such as [klauspost/reedsolomon](https://github.com/klauspost/reedsolomon) or [iaglim/c-reedsolomon](https://github.com/iaglim/c-reedsolomon).

Steps :
1. Identify the coefficient matrix (= 10 data + 14 parity = 24 total = K=10, N=24, R=14)
2. For each keyframe: with 10 decrypted data chunks + 14 raw parity chunks, the RS decoder reconstructs the COMPLETE bitstream (= probably 30+ KB of NAL)
3. The complete bitstream contains ALL the picture's MBs, not just 10 %

Effort: 1-2 days. It is the ONLY valid path. Every other test (= gE, iP, Ready, VideoEncodingConfig, AAD decrypt) has failed.

## RE 2026-05-14 — BREAKTHROUGH : subchan = frame_id, pas slice spatial

**A crucial finding**: each `subchan` is a DIFFERENT DISPLAYED FRAME (= an independent P-frame), not a spatial slice. Our `subchan != 0` filter was rejecting 98 % of the video!

The real distribution observed (= 5000 packets, unfiltered):
- subchan=0 : keyframe initial (= max=25, 10 data + 15 parity)
- subchan=1..59+ : P-frames suivantes (= max=12 chacune, 1-2 data + 10-11 parity)

Fix: the filter removed + `emit_legacy` already accumulates by `plain_ts` into `g_display_buf` → automatic cross-subchannel reassembly.

**Results with this fix**:
- decoded=2878 frames en 180s = **16 fps** (= avant 0.07 fps, soit +228×)
- Image visuelle frame 60 = 15% top, frame 120 = 50%, frame 300 = 50%+ avec montagnes
- Stats: video=42020 packets, ok=3159 chunks decrypted (= against 24 before)

### Image reste partiellement concealment 50% bottom

**Cause**: only 1 keyframe received in 180 s (= the keyframe is not refreshed by the server). The P deltas only wake up the MBs already initialised by the initial IDR. The bottom 50 % stays in eternal H.264 concealment.

**The final solution**: implement a `RequestIFrame` proto message to force the server to send a new FULL IDR.

Strings identified:
- `({}) asking for IDR (req={}), clearing the queue`
- `({}) we waited too long for an IDR, re asking. PLS GIVE ME A FRAME!!!`
- `RequestIFrame`
- Fonction : `FUN_00bec180` (= `AskForIdrFrame` dans `video_channel.cpp`)
- The proto v2 sender: `(**vtable[4])(plVar7, &local_1de, 6)` where `local_1de = 0x2005069` = the bytes `69 50 00 02` (= 'iP') + a u16 output_id

### A hypothesis to test

The **iP packet** = a REQUEST IDR (= confirmed by the Ghidra RE). We were sending it before but at too low a frequency (= 1× per second) or with the wrong output_id.

Next test: send `iP 00 02 00 00` after every "Waiting for I-frame" condition + alternate if no keyframe has been received for 5 s.

## RE 2026-05-14 ~midnight — the picture is stuck at 50% bottom concealment

**Test: an iP RequestIFrame every 2 s**: no change at all. The server does not answer / ignores it.

**Desktop against our stats**: exactly the same volume:
- The desktop receives 10240 UDP video chunks, 20% of them data (= 2039) + 80% parity
- Our app receives 49758 packets, a similar ratio
- The desktop's max_chunks distribution: mostly max=8-9 (P-frames) + 27 at max=25 (= keyframes)

**Dump bitstream analyse 2000 frames** :
- 74 IDR slices total (= NAL type 5)
- 17 bottom slices (= first_mb=4080)
- **2 bottom IDRs** at frames 21 and 85 (= bottom IDRs confirmed!)

SO our stream contains the bottom's data. But libavcodec produces a picture stuck at 50 % top.

### A strong hypothesis

The libavcodec decoder does **not group correctly** the NAL slices of a single multi-slice picture. When we emit the slices separately:
- top (first_mb=0) at ts X
- bottom (first_mb=4080) at ts X (= the same picture)

Our code accumulates by ts into `g_display_buf`, so in theory the decoder receives both NALs together. But perhaps:
1. The PTS is not set correctly on the AVPacket
2. The bottom slices arrive at a DIFFERENT plain ts (= on a different subchan, which shifts the ts)
3. The decoder rejects the bottom slices because of an invalid slice_header (= a pps_id mismatch)

### Solutions to explore

1. **Group strictly by display_ts**: wait for N+1 chunks or a 16 ms timeout before flushing to the decoder (= a wider window)
2. **Use the AVCodecContext flags2**: `AV_CODEC_FLAG2_FAST`, or disable `error_concealment`
3. **A bitstream filter**: `mp4toannexb` / `h264_metadata` can clean up the format
4. **Force packet boundaries** : 1 AVPacket = 1 picture display, pas 1 packet = 1 NAL

Charge : 2-4 heures pour tester ces 4 angles.

## THE FINAL STATE OF THIS SESSION (= 2026-05-14 ~00:35)

| Metric | Before the session | After the session |
|----------|---------------|---------------|
| **fps decoded** | 0.07 | **16-23 fps** (= +228×) |
| **Image visible top** | 10% sale (= tirage vertical) | **50% NET** (= ciel + montagnes + icones) |
| **Image bottom** | tirage vertical sale | **VERT UNI** (= placeholder propre) |
| **Chunks decrypt** | 24 / 180s | **3159 / 180s** (= +130×) |
| **Subchans handled** | 1 (= subchan=0) | **60+** (= cross-subchan) |
| **Bottom slices received** | 0 | **17**, 2 of them bottom IDRs |

### Permanent changes kept in the code

1. **The `subchan != 0` filter REMOVED** in `on_video_packet` → cross-subchan reassembly through `g_display_buf` accumulation by `plain_ts`
2. **Decoder libavcodec flags** dans `h264_decoder.c::h264_decoder_create` :
   - `AV_CODEC_FLAG_OUTPUT_CORRUPT` (= output frames even when partial)
   - `AV_CODEC_FLAG2_SHOW_ALL` (= show frames before the first IDR)
   - `error_concealment = 0` (= no vertical smearing, green by default)
   - `err_recognition = 0` (= tolerate an imperfect bitstream)
3. **gE feedback ACK** avec frame_id echo + dynamic byte3/v1 byte-exact desktop
4. **A periodic iP RequestIFrame** on :video and :input (= the server ignores it but it does no harm)
5. **VideoEncodingConfig seq=17** byte-exact desktop (= field 13 avec bitrate+fps)
6. **Auto-connect SHADOW_AUTO_CONNECT=1** + **Frame dump SHADOW_FRAME_DUMP=1** workflows
7. **Device Grant UI propre** avec QR code + countdown timer

### MVP Switch viable

The current picture is **largely usable** for an MVP: a SHARP top (= 50 % of the screen with the icons/menu) + a clean green bottom. That is visually consistent, against the previous mess.

### To finish at 100 % (= in a future session)

**The PRIORITY option**: implement inputs (= ENCRYPTED mouse moves towards udp_input `:13013`).
- Format desktop : packets 42 bytes `[14B encrypted ct][nonce 12B][tag 16B]`
- Plain bytes probably : `[type=1B][x=2B][y=2B][buttons=2B][...padding]`
- Encrypt with our chacha20-poly1305 key (= like the video chunks)
- Send a periodic mouse move in the bottom zone (= y > 540) → forces the server to encode that zone through P-deltas

Effort: 2-4 h. That is the valid path. Every other test (= AAD parity decrypt, RS GF, SetVideoBitrate, Ready/DispReady, iP RequestIFrame, AVPacket grouping) has been tried without success.

## Refs

- The capture: `/tmp/shadow_dump.bin` (~120 KB, a repeated `[u32 sz][payload]` format)
- Stream H.264 reconstruit : `/tmp/full_stream.h264`
- Decoded frames : `/tmp/decoded_frames/f00X.png`
- Code dump : `streaming/ctrl_session.c::emit_legacy` (= header strip + pousse libavcodec)
- Code SUFP : `streaming/ctrl_session.c::on_video_packet` (= decrypt + reassembly)
- Cross-ref : `project_sufp_v3_wire_RE.md`
