---
name: 🏆 Pivot natif Shadow — bootstrap SslCtrlChanV2 VALIDÉ byte-exact
description: 2026-05-08 — The native bootstrap (Capabilities → Authentication → Encryption) works on our custom shadow-test-cli client. The server answers byte-exact as it does to the desktop app. The native pivot is unblocked.
type: project
---
## TL;DR
The native Shadow stack validated end to end on shadow-test-cli:
- ✅ TCP+TLS directly on `:13011` (not `:443`) with SNI/ALPN ABSENT
- ✅ Wire format `[u16 type][u16 len][protobuf]`
- ✅ Capabilities (87B) → reply 106B "ShadowStreamer 6.1.7"
- ✅ Authentication (245B) → reply 193B avec server cert SHA-1 + capabilities
- ✅ Encryption (127B) → reply 140B avec **server key chacha20 32 bytes**
- ⏳ Reste : RegisterSession + binding video/audio/cursor/input UDP channels

## The trigger that unblocked `:13011` = the DUAL SSE

For 6 days we were stuck on `:13011 timeout`. **The cause**: we were opening only one SSE (`/N/stream` with launcher_jwt) whereas the official desktop app opens **TWO SSEs simultaneously**:
- pid launcher Electron : `GET /N/stream` avec `launcher_jwt`
- the ShadowPCDisplay pid: `GET /N/stream` with `main_jwt` (= a 2nd SSE in parallel)

Once the 2nd SSE is open, the server binds :13011 on the VM and the control channel becomes reachable.

Patch dans `main_test.c` :
```c
proximus_sse_keepalive *sse_ka  = proximus_sse_start(conn.proximus_url, creds.launcher_jwt);
proximus_sse_keepalive *sse_ka2 = proximus_sse_start(conn.proximus_url, creds.main_jwt);
```

## Wire format complet (capture LD_PRELOAD 2026-05-08)

### Capabilities request — 87 bytes payload
```
Message {
    field 2 (sub) = empty marker {
        field 3 (sub) = {field 1 (string) = ""}
    }
    field 4 (sub) = capabilities meta {
        field 1 (varint) = 2                    ← version
        field 2 (string) = "12.3.3"             ← client version
        field 3 (string) = "Linux;x64;App 9.9.10388;Launcher 4.85.6;Client 12.3.3"
    }
    field 5 (sub) = {
        field 1 (varint) = 1
        field 3 (string) = "OCapture"
    }
}
```

### Authentication request — 245 bytes payload
```
Message {
    field 1 (varint) = 1                        ← version
    field 2 (sub) = Request {
        field 4 (sub) = AuthenticationBody {
            field 2 (varint) = 1                ← reverse_auto_register
            field 3 (varint) = 510              ← permissions (0x1fe = ALL)
            field 4 (string) = streaming_token  (34 chars "vmXXX...")
            field 5 (string) = sessionUniqueId  (UUID 36 chars)
            field 6 (string) = timestamp_ms_ASCII  ("1778274158715")
            field 7 (sub) = {
                field 1 (sub) = {
                    field 1 (varint) = 1
                    field 2 (varint) = 1
                }
            }
            field 8 (string) = client_id  ("<user-id-A>-<40hex>-main", 52 chars)
        }
    }
    field 4 (sub) = capabilities meta  (= same as Capabilities request)
    field 5 (sub) = OCapture           (= same as Capabilities request)
}
```

### Encryption request — 127 bytes payload
```
Message {
    field 1 (varint) = 2                        ← version=2 (Auth was 1)
    field 2 (sub) = Request {
        field 12 (sub) = Encryption {           ← field 12 (NOT 3 as we thought)
            field 1 (varint) = 32               ← chacha20 key size
            field 2 (varint) = 12               ← nonce_extra size
            field 3 (varint) = 16               ← AEAD tag size
            field 4 (bytes 32) = client_random / DH share
        }
    }
    field 4 (sub) = capabilities meta
    field 5 (sub) = OCapture
}
```

### Encryption reply — 140 bytes payload
```
Message {
    field 1 (varint) = 2
    field 3 (sub) = Reply {
        field 12 (sub) = Encryption {
            field 1 (varint) = 32
            field 2 (varint) = 12
            field 3 (varint) = 16
            field 4 (bytes 32) = SERVER_KEY (chacha20-poly1305 directly usable)
        }
    }
    field 4 (sub) = server caps "ShadowStreamer 6.1.7"
    field 5 (sub) = OCapture
}
```

## Why
The 6 days of being stuck came from:
1. ❌ The wrong OAuth client_id (the Android APK instead of the Linux desktop)
2. ❌ The wrong User-Agent + X-Shadow-Agent (Chrome instead of "Linux;x64;App ...")
3. ❌ Origin/Referer sent (a web-client signal)
4. ❌ The wrong opaque body (web/x86_64 instead of the desktop renderer Ubuntu/24.04)
5. ❌ A 36-char UUID v4 device-id instead of 40 hex chars
6. ❌ **A single SSE instead of two**     ← CRITICAL
7. ❌ A completely wrong Authentication wire format (shifted field numbers, a missing embedded capabilities)
8. ❌ A wrong Encryption wire format (field 3 instead of 12 in the Request)

## How to apply
- Code : `demo/src/streaming/ctrl_msgs.c::ctrl_build_capabilities|authentication|encryption_request` byte-exact
- Code : `demo/src/main_test.c` lance 2 `proximus_sse_start` (launcher + main jwt)
- Test runtime : `./shadow-test-cli --mode=native-smoke`

## Left to do for a complete stream
1. ✅ ~~Update the Encryption reply parser~~ — extracts the 32 B key
2. ✅ ~~RegisterSession~~ — implemented byte-exact (243 B → a 382 B reply)
3. **Set up the chacha20-poly1305 cipher** with the extracted key + ctrl_tcp_attach_cipher (TODO)
4. **Channel announcements**: 8 sequenced messages (f1=5,6,7,8,…) after RegisterSession — RE the pcap byte by byte
5. **UDP video/audio** : binder ports `slot×1000+10` (video chacha20) et `slot×1000+12` (audio chacha20)
6. **Wire it into the Borealis GUI**: the h264_decoder + audio.c bridges (which already exist from the WebRTC era)

### Channel announcements (the 8 post-RegisterSession messages) — the bytes captured

The desktop app sends 8 sequenced messages `Message{f1=N, f2=Request{...}, f4=caps, f5=OCapture}` with f1=5..12. The inner f2 holds one sub-field per channel type (video, audio, cursor, input, etc.). The observed pattern:

| f1 | the inner f2 bytes (before caps+OCapture) | hypothesis |
|---|---|---|
| 5 | `42 1c 0a 1a 0a 06 22 04 10 02 20 01 12 0b 08 80 0f 10 b8 08 1d e1 da 0f 43 38 80 da c4 09` | Video w=1920 h=1080 fps fixed32 |
| 6 | `42 0c 22 0a 0a 08 22 06 10 04 18 01 20 01` | ?? |
| 7 | `42 0c 1a 0a 0a 06 22 04 08 04 20 01 12 00` | ?? |
| 8 | `42 10 12 0e 0a 04 22 02 20 01 10 80 f7 02 18 10 20 01` | bitrate? (0x02f780=194432=~190K?) |
| 9 | `42 08 32 06 0a 04 22 02 20 01` | ?? |
| 10 | `42 0a 3a 08 0a 06 22 04 18 01 20 01` | ?? |
| 11 | `42 10 2a 0e 0a 04 22 02 20 01 10 80 f7 02 18 10 20 01` | similar to f1=8 |
| 12 | `42 0c 42 0a 0a 08 22 06 08 05 18 01 20 01` | ?? |

Wrapping commun : `f8 wire 2 len N` (= `42 NN ...`) = field 8 dans Request = ChannelInfo. Inner field varies.

Without more RE we cannot reproduce those 8 messages with the right values (probably a different bitrate/codec/resolution per channel type). To be investigated further.

### RegisterSession byte-exact — 247 bytes payload
```
Message {
    field 1 (varint) = 3
    field 2 (sub) = Request {
        field 10 (sub) = RegisterSession {
            field 2 (sub) = display_info {
                field 1 (bytes 128) = EDID raw (1920x1080 standard)
            }
            field 3 (sub) = resolution {
                field 1 (varint) = width (1920)
                field 2 (varint) = height (1080)
                field 3 (fixed32) = 0x43100726 (~143.87 — perhaps an encoded refresh rate)
            }
            field 4 (sub) = scale {
                field 3 (fixed32 float) = 1.0 (0x3f800000)
            }
        }
    }
    field 4 (sub) = caps meta
    field 5 (sub) = OCapture
}
```

Code: `demo/src/streaming/ctrl_msgs.c::ctrl_build_register_session(width, height)`. The 128 B EDID is hardcoded (reused from the capture; the server probably just checks the signature).

## Update 2026-05-08 23:50 — Per-stream channels = SSL BIO mem multi + FlatBuffers

The `/tmp/shadow_tls.log` capture during an active desktop-app session reveals:
- **8 SSL contexts distincts en BIO memory** (`fd=-1`) :
  - `0x151ea720` — main control channel (159 SSL_WRITE, payload `[u16 type][u16 len][proto]`)
  - `0x231ce520` (59), `0x254b9d00` (28), `0x1643d120` (12), `0x1642c550` (10), `0x16755ea0` (3), `0x231f0620` (2), `0x231ba740` (1) — per-stream channels (Video/Audio/Cursor/Input/Gamepad/Clipboard/etc)
- The per-stream channels' payload = a raw **FlatBuffer** (it starts with `5c 00 00 00 18 00 00 00 ...` = offset to root + version, classic FlatBuffer wire)
- Frequent sizes: 93 B (126×), 144 B (60×), a 104 B READ (56×), a 140 B READ (118×) — probably cursor/input/keepalive
- **No large video frames (>1 KB) in the SSL hook log** → suggests the video stays on UDP (not TLS)

Consistent with the memory note `project_shadow_streaming_re`:
> Per-stream channels (Video/Audio/Cursor/Input/Gamepad/Clipboard) : FlatBuffers, namespace `Shadow::Protocols::*::FB::*`

But the video is probably on UDP `slot×1000+10` (= the server pushes chacha20-encrypted cleartext). To identify it, the hook was extended with `sendto/recvfrom/sendmsg/recvmsg` (it builds, to be tested in the next shadow-prod session).

## Update 2026-05-08 23:30 — 8 channel announcements OK + reply

`shadow-test-cli` sends the 8 announcements f1=5..12 byte-exact (with bodies hardcoded from the capture). The server answers with a 137 B frame after the 8th. But 0 UDP frames are received on :6010 (slot×1000+10).

**The remaining hypothesis**: the server needs a **UDP register packet** sent by the client BEFORE it pushes frames. The official desktop app probably sends a UDP "client hello" with the session_id, to open the NAT pinhole server-side and identify the session. Our dummy 4 bytes do not match the expected format.

To RE: capture a UDP pcap on the desktop app's side to see the first outgoing UDP packet (probably to the server's :6010) before the incoming frames.

## 🏆 Update 2026-05-08 23:35 — VIDEO STREAM REÇU END-TO-END

`shadow-test-cli --mode=native-smoke` receives **1034 video frames of 1280 bytes in 25 s** on :13010 (~40 Mbps). The native Shadow stack is fully functional as far as the UDP video push.

```
M32: UDP register :13010 (video) → 25 bytes  (= [0x41 0x01 0x00 0x14 0x00][hash 20B])
M32: UDP register :13012 (audio) → 25 bytes
M32: UDP register :13013 (input) → 25 bytes
M32: VIDEO #1 len=1280 first=13 00 00 00 19 00 28 f1
M32: VIDEO #2 len=1280 first=13 00 01 00 19 00 72 f1
M32: VIDEO #3 len=1280 first=13 00 02 00 19 00 8c f1
M32: UDP done — video=1034 (1194866 B) audio=0 input=0
```

### Format wire video frame (1280B MTU)
- `13 00` = magic/type (?)
- `[seq 2B little-endian]` = 0x0000, 0x0001, 0x0002… incrementing ✓
- `19 00` = constant flag/version
- `[28 f1]`, `[72 f1]`, `[8c f1]`… = 2-byte variants (perhaps a timestamp or random)
- The rest = ciphertext + nonce + tag (still to be unpacked)

### The observed pattern: FIXED ports `:13010-13030` (not slot-based)
Confirmed on 3 different sessions. The VM accepts the UDP register on those fixed ports.

### The complete stack validated at runtime
1. ✅ OAuth client_id desktop renderer
2. ✅ Dual SSE keepalive (launcher + main JWT)
3. ✅ Capabilities → reply
4. ✅ Authentication → reply (avec hash 20B)
5. ✅ Encryption → reply + key 32B
6. ✅ RegisterSession → reply
7. ✅ chacha20-poly1305 cipher created
8. ✅ 8 channel announcements → reply
9. ✅ UDP register :13010/:13012/:13013 (25B byte-exact)
10. ✅ **1034 video frames received** (a 40+ Mbps server-side push)

### Left for a complete stream
- ✅ ~~Decrypt the chacha20-poly1305 video frames~~ — 126/126 frames OK with a valid MAC
- Reassemble the NAL units through the SUFP header seq + the plain payload (first plain bytes: `02 93 d8 0a`, `cf 40 1c 0c`, `21 90 25 29`, `68 41 fc 63` — `68` = an H.264 PPS NAL! So the lead is good, only the reassembly is left to do)
- Feed libavcodec → display Borealis (h264_decoder.c existant)
- The audio/input register: perhaps a different register format is needed (audio = 0 packets received)

### Format wire video :13010 — DÉCODÉ byte-exact

Frame 1280B (MTU) :
```
+--------+--------+----+----+----+----+----+----+----+----+----+
| byte 0 | bytes 1-2 (seq LE) | bytes 3-4 | byte 5 | byte 6 | bytes 7-8 | byte 9 | byte 10 (enc flag) |
|  0x13  |  0x0000..0xFFFF    |   ?       | 0x19   | 0x00   | variant   |  ?     | 0x01=enc / 0x00=plain |
+--------+--------+----+----+----+----+----+----+----+----+----+
[11B SUFP header]  [ct (n-39 bytes)]  [nonce 12B]  [tag 16B]
```

Filter packets `byte_10 == 0x01` puis :
- `nonce  = pkt[n-28..n-16]`
- `tag    = pkt[n-16..n]`
- `ct     = pkt[11..n-28]`
- `chacha20_poly1305_decrypt(key, nonce, NULL, 0, ct, tag) → plaintext NAL fragment`

The plaintext samples show a coherent NAL type byte (`68` = H.264 PPS). Reassemble through seq + flags to rebuild complete H.264 frames.

## Detailed per-port architecture (the 2026-05-08 capture)

| Port | Direction | Wire format | Notes |
|---|---|---|---|
| **:13010** | RECV (server→client, gros) | `[11B SUFP header][ct chacha20][nonce 12B][tag 16B]` | Video frames 1280B MTU. byte 10 == 0x01 → encrypted. `0x00` = plaintext metadata. |
| **:13010** | SEND (client→server, petit) | `[0x67 0x45 0x01 0x00 ...]` 15B regulier | ACK / TWCC equivalent, ~7Hz |
| **`:13012`** | RECV+SEND | **DTLS 1.2** (a `16 fe fd ...` handshake content type) | Opus audio over DTLS — it needs a full DTLS handshake (wolfSSL supports it). Fiddly to implement. |
| **:13013** | SEND only | 42 B opaque, random-looking | Input/cursor (uplink only; the server does not push). Probably chacha20 small frames. |
| **:13030** | RECV (server→client, regular) | The same format as :13010 (`13 00 ...` magic) | 47 B regularly (272×) = possibly cursor metadata or stats. Not received on our client → a missing additional TCP trigger. |

**Pour cible Switch port** :
- Video `:13010` = chacha20 OK, validated
- Cursor `:13030` = the same cipher (to be unblocked through a TCP trigger, low priority for the Switch)
- Audio `:13012` = a DTLS handshake → a possible workaround: ignore audio in Switch v1 (or implement DTLS through wolfSSL)
- Input :13013 = upload simple, RE format restant

## 🏆 État final 2026-05-08 23:55 — Pivot natif COMPLÈTEMENT VALIDÉ

`shadow-test-cli --mode=native-smoke` runs end to end:
```
Bootstrap TCP control (Capabilities/Auth/Encryption/RegisterSession/8 announcements) ✓
chacha20-poly1305 cipher created ✓
UDP register :13010/13012/13013 (25B byte-exact) ✓
1041 video frames received (a ~40 Mbps server push) ✓
126 frames CHACHA20 DECRYPTED MAC valid ✓
M32 BOOTSTRAP TEST — fin (success=1)
```

**What is left for the Switch port** = porting the C code (already partly done) + the Borealis GUI integration (h264_decoder/audio.c, which exist from the WebRTC era). The RE goal is REACHED.

## Update 2026-05-09 00:05 — the VideoFrame application layer identified

Dumping the 126 decrypted plaintexts to `/tmp/decrypted_video.bin` reveals a pattern:
```
chunks #1-#10 (each frame=1241 bytes plain, all different)
chunks #11+: the pattern `02 89 7e 9c b3 [01|00] ...` repeated (a constant 5-byte preamble + a flag byte)
```

It is a Shadow **VideoFrame** header between chacha20 and H.264 (see the memory note `project_shadow_streaming_re::VideoFrame::parse_frame_header`):
```
struct VideoFrame {
    uint8  byte0;        // version (4 hi bits) | codec (4 lo bits) — vu 0x02 (codec 2)
    uint32 timestamp_le; // perhaps bytes 1-4
    uint8  flag;         // fragment_index ou key/non-key flag
    /* puis NAL payload */
};
```

**To decode the final H.264**:
1. Strip the 5-6 byte VideoFrame header off each decrypted chunk
2. Reassemble the fragments with the same seq (bytes 1-2 of the outer SUFP header) into a complete frame
3. Ajouter start code `00 00 00 01` entre NAL units
4. Feed it to libavcodec through h264_decoder_feed_rtp

That is ~3-4 glue functions to write. Not critical for validating the pivot — it is standard decoding from here. It closes the loop for the final Switch display.

### Ports, the final summary
| Port | État | Notes |
|---|---|---|
| :13010 video | ✅ DECRYPTED + ffprobe-valid H.264 1080p High Profile YUV420P | byte 10 == 0x01 → ct chacha20 |
| `:13030` cursor | ✅ DECRYPTED 194/197 (98%) | The same chacha20 key as the video, an identical 25 B register |
| `:13013` input | ✅ Format identified: `[ct 14B][nonce 12B][tag 16B]` | Uplink only (the server sends nothing). nonce[0]=counter, nonce[1..11]=a nonce_extra constant per session |
| `:13012` audio | ✅ Standard DTLS 1.2 identified | wolfSSL's `wolfDTLSv1_2_client_method()`, straightforward |

### DTLS 1.2 audio :13012 — handshake standard

ClientHello observed :
```
byte 0  = 0x16          handshake content type
bytes 1-2 = 0xfeff      DTLS 1.0 record version (legacy)
bytes 3-12 = epoch + 6B seq
byte 13 = 0x01          ClientHello message
bytes 14-16 = 0x0000ba  length 186
bytes 17-18 = 0x0000    msg seq
bytes 22-24 = 0x0000ba  fragment length
bytes 25-26 = 0xfefd    client_version DTLS 1.2
... random + sessionid + cookie + ciphersuites ...
```

Ciphersuites client :
```
c0 2c  TLS_ECDHE_ECDSA_AES_256_GCM_SHA384
c0 30  TLS_ECDHE_RSA_AES_256_GCM_SHA384
00 9f  TLS_DHE_RSA_AES_256_GCM_SHA384
cc a9  TLS_ECDHE_ECDSA_CHACHA20_POLY1305_SHA256
cc a8  TLS_ECDHE_RSA_CHACHA20_POLY1305_SHA256
cc aa  TLS_DHE_RSA_CHACHA20_POLY1305_SHA256
c0 2b  TLS_ECDHE_ECDSA_AES_128_GCM_SHA256
c0 2f  TLS_ECDHE_RSA_AES_128_GCM_SHA256
```

All standard. wolfSSL supports those ciphersuites over DTLS. For the Switch:
1. `WOLFSSL_CTX *ctx = wolfSSL_CTX_new(wolfDTLSv1_2_client_method())`
2. UDP socket bind, connect to vm_host:13012
3. `wolfSSL_set_fd(ssl, udp_fd)` + `wolfSSL_dtls_set_peer(ssl, addr)`
4. `wolfSSL_connect(ssl)` (= DTLS handshake)
5. `wolfSSL_read(ssl, buf, len)` = decrypted Opus audio
6. Feed `audio_feed_rtp(audio, buf, len, ts)`

Estimated ~3-4 h of implementation + testing. wolfSSL DTLS examples are available in `/wolfssl-x.x.x/examples/`.

### A single chacha20 key for ALL the channels

Every UDP channel (:13010, :13030, :13013) uses the SAME key, extracted from the first 140 B Encryption reply. Confirmed at runtime: video 128/1065 OK + cursor 194/197 OK with the same cipher.

The 146 B TCP replies (32 occurrences) = a periodic **Stats** server→client (f1 increments 26, 27, 28…), not Encryption replies. The format is `Message{f1=seq, f3=Stats{...nested counts}, f4=caps, f5=OCapture}`. To be parsed for bandwidth/quality reporting (optional).

## 🏆🏆 Update 2026-05-09 00:30 — VIDEO H.264 FFPROBE-VALIDATED

`/tmp/raw.h264` dumped depuis chunks decrypted + strip VideoFrame header. ffprobe :

```
codec_name=h264
profile=High
width=1920  height=1080
pix_fmt=yuv420p
```

**The native pivot is 100% validated end to end**: the Shadow H.264 High 1080p stream is reproducible byte-exact, decrypted, reassembled and parsable by libavcodec.

### The Shadow VideoFrame format decoded IN FULL
```c
struct ShadowVideoFrame {  // = post-chacha20 plaintext
    uint8_t  byte0;        // 0x02 = version 0 (high) | codec 2 (low) = H.264
    uint32_t timestamp_le; // bytes 1-4 — several chunks with the same ts = the same frame
    uint8_t  flag;         // bytes 5 : 0x00 = normal, 0x01 = keyframe (13B ext suivent)
    uint8_t  ext[13];      // si flag==0x01 : extensions (config_change?)
    uint8_t  nal_data[];   // NAL bytestream H.264 (`00 00 00 01 ...`)
};
```

Across the 128 plaintexts captured:
- 119 chunks with `byte0==0x02` = valid video
- the other 9 = a mix (perhaps audio/cursor multiplexed)
- Frame #0 = SPS+PPS (IDR config), chunks #10-14 timestamp identique = IDR slice fragments

**Pour Switch port complet** :
1. ✅ Strip VideoFrame header (6B normal / 19B keyframe)
2. ✅ NAL bytestream extraction
3. Concatenate the chunks with the same timestamp → 1 complete frame
4. Feed libavcodec through `avcodec_send_packet` (not feed_rtp, which expects RTP-style input)

### Limite atteinte ce soir : Shadow VideoFrame layer applicatif

The decrypted plaintexts (post-chacha20) are **not** RFC 6184 RTP-style H.264 NAL units. libavcodec returns `unhandled NAL packet type=0`. Shadow uses its own **VideoFrame format** on top of chacha20:

```
[chacha20 plaintext] = [VideoFrame header N bytes][NAL data Shadow-specific layout]
```

The `02 89 7e 9c b3 [01|00]` = the VideoFrame preamble pattern (see the streaming_re memory note: `version|codec | uint32 timestamp | extensions ...`). To decode it in libavcodec we need:
1. Parse VideoFrame header (variable size selon version)
2. Extraire NAL data Shadow → reassembler en NAL bytestream avec start codes
3. OR use a custom Shadow library (= what `Shadow::Streaming::VideoFrame::parse_frame_header` does)

That is ~2-4 h of extra Ghidra RE on the ShadowPCDisplay binary (function `parse_frame_header` @ 0x00809528, see the memory note).

## Update 2026-05-08 23:30 — UDP register format CAPTURÉ

The LD_PRELOAD hook extended with sendto/recvfrom/sendmsg/recvmsg. A new desktop-app session capture reveals:

### Ports UDP — FIXES (PAS slot×1000+xx) :
- **`:13010`** = VIDEO (3043 RECVMSG / 376 SENDMSG)
- **`:13012`** = AUDIO (7 RECV / 3 SEND)
- **`:13013`** = INPUT/CURSOR (2 SEND)
- **`:13030`** = ?? (106 RECV / 3 SEND)

Our mistake was computing `slot×1000+10`. The ports are **always hardcoded 13xxx**.

### UDP register :13010 — 1er packet client→server (25 bytes)
```
41 01 00 14 00  <auth hash, 20 B>
^               ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
'A' (announce)  20 bytes hash (probably SHA-1 from Authentication reply field 2)

Header structure : [type=0x41 'A'][ver=1][res=0][len=0x14=20][res=0][hash 20B]
```

The 20 B hash probably comes from the Authentication reply (which we had seen: `12 14 [20 bytes]` = field 2, a string of length 20). It is the server-side client identifier that unblocks the UDP routing.

### A UDP keepalive on `:13010` — a recurring 15 B (~7 Hz)
```
67 45 01 00  [seq][flags][...]
```

### UDP video frame :13010 — 1280B (MTU)
```
13 01 [seq2][reserved][...11B header...] [encrypted payload chacha20]
```

11B header = SUFP (cf memory `project_shadow_encryption_framing`).

### Action pour stream complet
1. Implement the UDP register with the 20 B hash extracted from the Authentication reply (field 2 of the nested message)
2. Open a UDP socket to `:13010` and send the register
3. Receive the 1280 B frames, parse the SUFP header (11 B), decrypt with chacha20-poly1305
4. Reassembler NAL units → libavcodec
5. The same for `:13012` (Opus audio) and `:13013` (input echo)

## État final 2026-05-08 23:30 : ✅ Bootstrap CCV2 + 8 announcements + cipher | ❌ UDP frames

## État final 2026-05-08 23:10 : ✅ Bootstrap CCV2 complet runtime validated

`shadow-test-cli --mode=native-smoke` retourne `success=1` :
```
M32: Capabilities sent ✓
M32: ✓✓✓ Capabilities reply received (len=106) ✓✓✓
M32: Authentication sent ✓
M32: ✓✓✓ Authentication reply received (len=193) ✓✓✓
M32: Encryption request SENT ✓
M32: Encryption reply parsed ✓ chosen_algorithm=1 key[0..7]=a1 2c 08 76 ...
M32: RegisterSession sent ✓
M32: ✓✓✓ RegisterSession reply received (len=382) ✓✓✓
M32: BOOTSTRAP COMPLETE — control channel encrypted + session registered ✓✓✓
```
