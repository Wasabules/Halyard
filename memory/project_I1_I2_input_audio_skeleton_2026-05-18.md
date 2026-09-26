---
name: project-I1-I2-input-audio-skeleton-2026-05-18
description: "I1+I2 2026-05-18 — Skeletons for the native input channel (:base+14 TCP+SSL+FlatBuffer) and audio (:base+12 DTLS+Opus). I1 = the input channel is OPEN and accepted byte-exact against the desktop. I2 = the audio DTLS handshake FAILS for want of a PSK. The Phase 2/3 plan is documented."
metadata:
  node_type: memory
  type: project
---

# Native input + audio channels — the state after I1/I2 phase 1

## Findings from the V16 plaintext capture

| Port | Type | Format | RTTI desktop |
|---|---|---|---|
| :base+12 | UDP | DTLS 1.2 + Opus | `AudioUdpChannel` (probable) |
| :base+13 | UDP | 42B chacha20 + payload 14B | (= keep-alive only ?) |
| :base+14 | TCP+SSL | `[u32_LE size][FlatBuffer]` | `InputSslTcpFlatBuffersChannel` |
| :base+15 | TCP+SSL | `[10B header][string]` | `ClipboardSslChannel` (= shell commands visibles) |
| :base+20 | TCP+SSL | 25B auth + 0x64 + 0x70 hb | `VideoSslTcpChannel` (= VST V13/V16) |
| :base+30 | UDP | SUFP chacha20 | `CursorUdpChannel` |

## I1 — Input native channel :base+14 ✅ PHASE 1 DONE

### Implemented
- `ctrl_input_tcp.{c,h}` (= ~280 LOC, clone du pattern ctrl_video_tcp)
- Open TCP+TLS sans SNI/ALPN, cipher list = TLS 1.3 + 1.2 fallback
- The 96 B Connect blob copied byte-exact from the V16 desktop (= the MOUSE_TICK_TEMPLATE pattern)
- Spawn rx_thread pour echo logging
- API : send_mouse_move / send_mouse_button / send_scancode / send_mouse_wheel

### Runtime test (auto-test.sh 30 native-stream)
```
[input-tcp] TLS handshake OK cipher=TLS_AES_256_GCM_SHA384
[input-tcp] Connect 96B sent
[input-tcp] rx_thread started
[input-tcp] idle 5s (no echo from server)  ← stable
```
**Channel accepted** — no FIN, the server is waiting for our events.

### Limitation Phase 1
`send_mouse_move/etc` send a byte-exact 144 B MOUSE_TICK_TEMPLATE but X/Y/button/keycode are **NOT patched yet** in the payload. So right now it sends a heartbeat-like ping but **does not move the Shadow VM's cursor**.

### Phase 2 (TODO)
RE byte-exact du payload 144B :
- Method: capture the desktop's SSL_WRITE on `:15014` with the mouse REALLY moving (= open the desktop interactively and move the mouse during the capture)
- Diff the bytes between captures to locate the X/Y offsets
- Likely (based on the analogous WebRTC format):
  - X uint16 LE @134
  - Y uint16 LE @132
  - MouseDown : b124=0x14 (vs 0x10 mouseup)
  - Keyboard : keycode uint16 LE @142, press/release via bytes 114/116/128

### Phase 3 (TODO)
Wire the Borealis GUI events (stream_view input) → ctrl_session_glue → ctrl_input_tcp. For the Switch: touch screen + Joycon mouse mode.

## I2 — Audio DTLS channel :base+12 ⚠️ PHASE 1 BLOQUÉ

### Implemented
- `ctrl_audio_dtls.{c,h}` (= ~210 LOC)
- bind UDP :base+12, wolfSSL_DTLSv1_2_client_method()
- handshake 2s timeout, rx_thread 100ms timeout
- callback `on_audio(payload, len, ts, user)` pour Phase 2
- stats : handshake_ok, bytes_recv, frames_recv

### Runtime test
```
[audio-dtls] UDP connected (family=10)
[audio-dtls] DTLS handshake FAIL rc=-1 err=-308 (error state on socket)
```

### Root cause : besoin PSK (= Pre-Shared Key)
- err=-308 = SOCKET_ERROR_E wolfSSL → server rejette notre ClientHello
- The desktop binary has `SSL_set_psk_client_callback` listed in `nm -D`
- KB §1.4 mentions a `tlskey` (64 hex = 32 bytes) returned in `/N/clients` for the `usb` client type
- Hypothesis: the audio on :base+12 uses a PSK with a secret distributed through the REST API

### Phase 2 (TODO) — Identifier le PSK
1. Extend `tls_hook` to capture the `EVP_PKEY_psk_*` calls + the `SSL_set_psk_client_callback` arguments during the desktop's audio connection
2. Re-run the V16 capture with the extended hook
3. Identify the identity + key bytes used by the desktop
4. Cross-reference against the proximus_credentials response (= probably one of the JWTs or a binary blob)

### Phase 3 (TODO) — Audio playback
Once the PSK is identified + the handshake is OK:
1. Identifier wire format post-DTLS (= raw Opus ? RTP+Opus ?)
2. Wire the `on_audio` callback → `audio_feed_rtp()` (the existing audio.c works for WebRTC, to be validated for the native path)
3. Audout Switch / ALSA Linux playback automatique
4. Verify that gameplay sound is played

## Workflow d'usage maintenant

### Tester input channel ouvert
```bash
SHADOW_INPUT_TCP=1 tools/auto-test.sh 30 native-stream
grep "input-tcp" /tmp/shadow-client/webrtc.log
# Attendu : "TLS handshake OK" + "Connect 96B sent"
```

### Disabling channels (debug)
```bash
SHADOW_INPUT_TCP=0 SHADOW_AUDIO_DTLS=0 tools/auto-test.sh ...
```

### Toggles added
| Env var | Default | Comportement |
|---|---|---|
| SHADOW_INPUT_TCP | 1 | Ouvre :base+14 (= input chan) |
| SHADOW_AUDIO_DTLS | 1 | Ouvre :base+12 DTLS (= audio chan, handshake FAIL en attendant PSK) |

## Cross-refs

- [[project-V16-vst-plaintext-FOUND-2026-05-18]] — the BIO+EVP hook that revealed the `:15014` + `:15012` traffic
- [[project-shadow-input-protocol]] — WebRTC FlatBuffer wire format (= base pour Phase 2 byte-exact)
- [[project-shadow-real-protocol-confirmed]] — the map of UDP/TCP ports by function
- KB §3.11 — Input + audio wire format hypotheses
- `ctrl_video_tcp.{c,h}` — Pattern reference pour I1
- `webrtc/audio.c` — Opus decoder + audout existant pour I2 phase 3

## Status

- I1 phase 1 : ✅ DONE — channel ouvert + Connect accepted
- I1 phase 2 : ⏳ TODO — RE payload byte-exact (= besoin nouvelle capture desktop interactive)
- I1 phase 3 : ⏳ TODO — wire GUI events
- I2 phase 1 : ⚠️ PARTIEL — skeleton OK, handshake FAIL faute PSK
- I2 phase 2: ⏳ TODO — identify the PSK through the extended hook + a re-capture
- I2 phase 3 : ⏳ TODO — Opus decode + playback
