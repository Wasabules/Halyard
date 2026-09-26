---
name: Shadow streaming reverse-engineering findings
description: The QUIC/FlatBuffers/protobuf stack of Shadow streaming, extracted by Ghidra from libAwesomeClientProject.so — a reference for M7+ (porting streaming to the Switch)
type: project
---
The reverse engineering of Shadow streaming was completed on 2026-04-30. The main document: `03-shadow-recon/SOCKET_AND_BOOTSTRAP.md` (751 lines, 41 KB). Targeted Ghidra dumps in `03-shadow-recon/dumps/ghidra_socket_bootstrap.txt` (5421 lines). 30+ Ghidra addresses mapped in the document's Appendix A.

**Why:** this work saves any future session from having to relaunch Ghidra or redo the analysis — everything is documented, symbol and address.

**How to apply:** when tackling M7 (real streaming on port 6000), start from `SOCKET_AND_BOOTSTRAP.md §10` ("Recap of what is left to do"), which proposes a 5-phase plan.

### The stack confirmed
- **Transport** : msquic v2 (`MsQuicOpenVersion(2,&api)`) avec datagram extension RFC 9221, IdleTimeout 1000ms, KeepAlive 1000ms, StreamRecvWindow 8MiB.
- **TLS**: the NONE credential variant is the most likely (`MQApi` ctor @ 0x00c72c70). No client certificate, no server validation (to be confirmed from a pcap).
- **ALPN**: a strong hypothesis = `"shadow"` (an isolated string in the rodata).
- **The control channel**: the first bidirectional stream after the QUIC connect, in **protobuf** (a `ControlProto::Message` envelope; the full schema is in STREAMING_PROTOCOL.md + SOCKET_AND_BOOTSTRAP.md §3).
- **Per-stream channels** (Video/Audio/Cursor/Input/Gamepad/Clipboard): **FlatBuffers**, namespace `Shadow::Protocols::*::FB::*`. NOT protobuf — an important finding from SOCKET_AND_BOOTSTRAP.
- **Crypto applicatif** : ChaCha20-Poly1305 AEAD seulement. CryptoFactory FATAL_LOG si `type != 1`.

### ChannelType enum (UDU level)
Confirmed through the `EnableEncryption(ChannelType=N, ...)` dispatchers:
- 0 = Video
- 1 = Input  
- 2 = AudioIn
- 3 = AudioOut
- 4 = Cursor
- 5 = Gamepad
- 6 = Clipboard

### Bootstrap state machine
1. QUIC connect (UDP/443→6000)
2. Open 1st bidi stream → control channel
3. Send `Request_Authentication` (8 fields ; field4=string probable=streamingtoken)
4. Receive `Reply_Authentication` (4 fields ; field2=string ⇒ shared crypto secret)
5. `CtrlChanV2Manager::SetAuthenticated()` flips the byte at offset 0x74 to 1
6. Exchange `Encryption{key=32B,nonce_counter,nonce_extra,supported_algorithms=[0,2,4,1]}`
7. Send `RegisterSession` (8 sub-messages dont video, audio, input, cursor, …)
8. Per-stream channels actifs (FlatBuffers framing dessus)

### VideoFrame wire format (`VideoFrame::parse_frame_header` @ 0x00809528)
- byte0 = `version(4 hi-bits)|codec(4 lo-bits)` (version 1 ou 2 ; codec H.264/HEVC/AV1)
- bytes 1-4 = `uint32 timestamp` LE
- v2 ajoute byte 5 = N extensions, chacune `type(1) + subtype(1) + uint16 len + payload`

### supported_algorithms enum
- 0 = NONE
- 1 = CHACHA20_POLY1305 (the only valid one on the client side)
- 2 = AES-128-GCM (probable, to be confirmed)
- 4 = AES-256-GCM (probable, to be confirmed)

### Risque technique majeur
**msquic has no official Switch port**. The document's recommendation: use **ngtcp2 + wolfSSL/boringSSL** (BSD, lighter, already known in embedded work) rather than porting msquic (which depends on schannel/openssl). For H.264 decoding: **NVDEC is reachable through libnx** on Switch (good news).

### A real web-browser pcap (2026-05-01) — finding 1
A Firefox capture of shadow.tech: the browser uses **WebRTC** (DTLS+SRTP+STUN, UDP port 11045 towards an IPv6 relay).

### An Android APK pcap (2026-05-01) — finding 2 (DECISIVE)
**The APK does NOT use QUIC** either, despite linking `libmsquic.so`. The effective protocol is:
- Port 13011 TCP : **TLS 1.3** (control channel CtrlChanV2 → ControlProto protobuf)
- Port 13020 TCP : TLS 1.3 secondary
- Port 13012 UDP : **DTLS 1.2** (probable input/cursor)
- Port 13010 UDP : **raw + ChaCha20-Poly1305 applicatif** (video stream, UdpFrame)
- Port 13030 UDP : idem (audio)

Consistent with `STREAMING_PROTOCOL.md`, which already distinguishes `TcpFrame` from `UdpFrame` in the binary. The msquic in the APK is most likely vestigial / a disabled mode.

### The M7 v2 plan (revised)
- ✅ wolfSSL kept (TLS 1.3 over TCP + DTLS 1.2 + ChaCha20-Poly1305 — all in one)
- ❌ ngtcp2 abandoned (M7.2 obsolete; the 490-line `quic.c` recycled into TLS 1.3 over TCP)
- A simple stack: libnx TCP+UDP sockets + wolfSSL for TLS 1.3 and DTLS 1.2 + manual chacha20 for the raw UDP

### Findings from the 2026-05-01 probing session (every port hypothesis invalidated)
**A device-side dump of every REST response:**
- `/vm/ip`: `{"port":"<slot×1000>", "slot_number":<N>, "offset":0, ...}` — no `ports` field
- `/clients launcher`: it returns just `id, type, remote=<the public IP the server sees>, opaque, version, spice_secret`. No `ports`.
- `/clients main`: the same but with a `streamingtoken` instead. No `ports`.
- The `/stream` SSE (on both the launcher and main connections): no event contains a port. Only `vm_reachable`, `status_changed`, `encoding_is_ready`, `acquisition_is_ready`, `display_is_ready`, `is_streamer_reachable`.

**A device-side multi-port probe test** (5 candidates): all time out (a TCP connect SYN with no answer):
- `13011` (universel hardcoded apk-style) : timeout
- `port_base` (= conn.port = 8000) : timeout
- `port_base + 11` (formule slot×1000+11) : timeout
- `port_base + 1` : timeout
- `13010` : timeout

→ **The `slot×1000+offset` hypothesis is invalidated**. The Shadow VM accepts HTTPS (port 443) but does NOT appear to accept streaming on the reasonable candidate ports.

### Critical side findings
- **Shadow allows only 1 `main` client per VM** (HTTP 409 `Client threshold exceeded for this type` when the Android APK is connected in parallel). The server returns the occupying client's details in `data[0].opaque`: `device-id`, `os: android`, `os-name: Android 16`, etc.
- The public IP Shadow sees from the Switch and from the APK is identical (the ISP's NAT box) = `<home public IP>`. Yet the sessions are distinguished by `device-id`. Our Switch device id = a UUID v4 generated on the first run into `/switch/shadow-client/device.uuid`.
- Across 3 distinct APK pcaps (3 different VMs: 211.78, 212.225, 212.240), the streaming ports are **always 13010-13014/13020/13030**. But our Switch VM (slot=8 / port_base=8000) times out on those same ports too → the formula is not universal, it must be derived from another parameter.

### Blocked — the next RE step
Trace the **xrefs of `NetworkIOChannel::setPort(unsigned short)` @ 0x008362a7** in libAwesomeClientProject.so through Ghidra to identify where the port value comes from. Probably:
- A hardcoded constant (which we have not found in the strings)
- A computation using a field we have not observed in the REST responses
- A REST endpoint the APK calls but the browser does not (hence absent from the HAR)

### Pass 2 update (statically resolving the unknowns)
- ✅ **ALPN = `"shadow"`** confirmed (a NUL-bounded string at VA 0x52d9ec, the MQApi pipeline traced).
- 🔵 **Request_Authentication = 8 fields**: 3 bools + uint32 + 4 strings + a Capabilities sub-msg (extracted through `_InternalSerialize` @ 0x00c49028). A high-confidence hypothesis: field4=streamingtoken, field5=spice_secret, field6=JWT, field8=sessionId. The exact order of the 4 strings still has to be pinned at runtime (24 permutations to try).
- ⭐ **Audio/Video DO NOT USE FlatBuffers** — zero `Shadow::Protocols::Audio/Video::` symbols. Only **Cursor + Inputs** are FB. A huge simplification: for M7 we can implement Video/Audio without depending on flatbuffers at all.
- ✅ **VideoFrame v2 extensions**: only 2 types — `0=VrHMDTracking` (VR only, ignorable), `1=Influx` (a display reconfiguration IDR: width/height/freq/codec). PTS/keyframe/fragment_index are in the codec's NAL, not in the extensions.
- 🟡 **supported_algorithms**: the client's internal enum = {0=Invalid, 1=Chacha20Poly1305, 2=Tls} (3 values). The `[0,2,4,1]` list sent to the server has no mapping on the client side. A pragmatic decision for M7: mimic `[0,2,4,1]` (the server will choose 1=CHACHA, the only one CryptoFactory accepts).

Unknowns that can only be resolved at runtime:
- The exact mapping of Request_Authentication's 4 strings (24 brute-forceable permutations)
- Complete FlatBuffer schemas for Cursor/Inputs (partial extracts in `dumps/ghidra_pass2.txt`)
- QUIC_CREDENTIAL_FLAGS effectifs en runtime
- Ordre Encryption ↔ RegisterSession

### Pass 3 update — an RE of the Linux client (2026-05-02)
A dedicated document: `06-shadow-recon-linux/QUIC_STACK.md`. The dump: `dumps/ghidra_mqstack.txt`. Critical findings:

- **The Linux client DOES ship the QUIC stack** (`Shadow::NetworkProtocol::{MQApi,MQClient,MQConnection,MQStream}` — all found in the RTTI). `libmsquic.so.2.4.5` is linked. `MsQuicOpenVersion(2, ...)` → API v2 confirmed. ALPN="shadow" confirmed through the `negotiatedAlpn: {}` log.
- **3 transports coexist** in the binary: QUIC, TCP (Tcp/SslTcp/StfpTcp/StfpSslTcp), UDP (Udp/TimeoutUdp/SufpUdp). The string `QUICTCPUDP` = a packed enum. The client chooses **per channel** through `RegisterSession_StreamingProtocol_ProtoType` (the packed enum `FlatBuffersSCPSFTPSSPSSUFPSUFP`).
- **The TransportProtocol enum in InputManager::Start (FUN_008b0b80)**: `1=TCP, 2=UDP, 4=QUIC` at field offset 0x8c. SendMouseButton is QUIC-only; on TCP it logs `not QUIC` and drops.
- **Cert validation** : 2 modes — hash 20B (SHA-1 pinning, FUN_0112a640) ou cert file PEM (FUN_0112a850). Probable mode 1 en prod.
- **Crypto**: `EnableEncryption(channel, alg)` (FUN_00907c40) — alg=1 (Chacha20Poly1305) accepted; alg=2 (TLS) rejected with the client-side log `Streamer doesn't support TLS for streaming`. The env var `SHADOW_CLIENT_ENCRYPTED_CHANNELS` = a per-channel debug bitmask.
- **Datagrams RFC 9221 ON** (strings `Connection datagram received/state changed`).
- **MQConnection sizeof = 0xd0** (208 octets). Singleton MQApi avec mutex `DAT_01589b20`.
- **Dispatch QUIC vs fallback** : `Client::startQuicClient` @ 0x7d54d0 — log `QUIC is supported for at least one chan, start client` si ≥ 1 channel QUIC-capable. Sinon fallback total TCP+UDP.

**Key Ghidra addresses**: 0x1129f60 (MQApi::Init), 0x1123f50 (createConnection), 0x1127a10 (onConnected), 0x8b0b80 (the InputManager::Start dispatch), 0x849d40 (the ToControlProtoProtocolType enum map), 0x907c40 (EnableEncryption).

**Recommendation**: capture a real `ShadowPCDisplay` pcap with `SSLKEYLOGFILE=...` (msquic supports it) to resolve the 7 runtime unknowns listed in QUIC_STACK.md §8 (the real host:port, the concrete QUIC settings, the Auth field order, the stream ids, etc.) — the static RE has hit its limits.
