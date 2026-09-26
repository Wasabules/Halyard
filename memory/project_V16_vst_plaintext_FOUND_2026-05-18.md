---
name: project-V16-vst-plaintext-FOUND-2026-05-18
description: "DECISIVE V16 2026-05-18 — A plaintext capture of VST :base+20 through the extended tls_hook (BIO + EVP). The byte-exact format = 25 B `41 01 00 14 00 [hash20]` + 3× a 0x64 trigger + a 0x70 heartbeat every 7 s. ShadowPCDisplay uses OpenSSL (= not wolfSSL, contrary to the earlier hypothesis). Variant 5 in our code was ALREADY correct, it was only missing the trigger + heartbeat."
metadata:
  node_type: memory
  type: project
---

# The VST `:base+20` connect_msg identified byte-exact — variant 5 + the 0x64 trigger

## The finding (the V16 plaintext capture, 2026-05-18)

### A myth broken: there is no wolfSSL on the desktop
The SESSION_RECAP_2026-05-16 hypothesis said "a wolfSSL_read/write hook is needed".
**FALSE**: the ShadowPCDisplay binary:
- `nm -D` → 16 OpenSSL `SSL_*` symbols; **zero** `wolfSSL_*`
- `ldd` → `libssl.so.3` + `libcrypto.so.3` dynamically linked

### Why the pre-V16 OpenSSL hook could not see VST
fd=302 (= :15020 in that session) had TCP_WRITE TLS records (= ciphertext)
but **NO** SSL_WRITE attached. The cause: ShadowPCDisplay uses
**SSL with BIO_s_mem** (= a memory BIO), not BIO_s_socket. The flow:
1. `SSL_write(ssl, plain, len)` — called with **fd=-1** (= our `SSL_get_fd` returns -1)
2. SSL interne chiffre dans BIO_mem
3. The app reads the ciphertext from the memory BIO and does `send(fd, encrypted)` — seen as TCP_WRITE

The SSL_write hook **IS triggered** but with fd=-1 peer=? → we had
misread that as "not attached to a real channel". In fact it is the ENTIRETY of
the application-level plaintext, and it has to be correlated by **the ssl object's
pointer**
+ timing CONNECT.

### Mapping ssl pointer → channel (= session V16 capture)

| ssl pointer | Channel | CONNECT timing | 1er SSL_WRITE |
|---|---|---|---|
| `0x37abe820` | `:15011` (SslCtrlChanV2) | t=527.192 | 91 B at t=527.279 |
| `0x37807740` | **`:15020` (VST)** | t=529.049 | 25 B at t=529.524 |
| `0x35c81ba0` | `:15015` (Clipboard?) | t=529.147 | 10 B at t=530.375 |
| `0x3781b750` | `:15014` (Hid/Gamepad?) | t=529.783 | 96 B at t=530.386 |

### The VST `:base+20` sequence, byte-exact

| # | Time post-handshake | Size | Bytes | Interpretation |
|---|---|---|---|---|
| 1 | t+0.5s | 25B | `41 01 00 14 00 <auth hash, 20 B>` | Auth/register (= identique UDP register) |
| 2 | t+0.87s | 1B | `64` ('d') | Trigger 1 — start downstream ? |
| 3 | t+2.03s | 1B | `64` ('d') | Trigger 2 |
| 4 | t+6.33s | 1B | `64` ('d') | Trigger 3 |
| 5+ | t+13.5s, +20.9s, +29.0s, +36.8s, ... | 1B | `70` ('p') | Heartbeat ~6-8s, espacement constant |

Total observed over a 90 s session: 1× 25 B + 3× `0x64` + 11× `0x70`.

### The bug in our pre-V16 code

`ctrl_video_tcp.c` :
- The default variant = 8 (= a streaming_token body, **wrong**) — 8 variants tested, all FIN
- Post-connect = 2× `0x70` direct (= heartbeat) — **manque le 3× 0x64 trigger**
- No periodic heartbeat in the RX thread — the server FINs after 25-30 s of idling

Variant 5 WAS already the right auth format (= 25 B `41 01 00 14 00 [hash20]`)
but we never sent the `0x64` trigger afterwards → the server timed out.

### Fix V16 (= ctrl_video_tcp.c)

1. Default `variant = 5`
2. Post-connect : 3× `wolfSSL_write(ssl, &0x64, 1)`
3. RX thread : heartbeat `wolfSSL_write(ssl, &0x70, 1)` toutes 7s
   (= configurable via `SHADOW_VST_HB_MS`, default 7000)

The Linux build is fine. What remains = a runtime test to confirm that VST becomes active
+ check the impact on the taskbar bug.

## Technical data for reproduction

- Capture : `/tmp/shadow_tls.log` 94 MB, session 2026-05-18 ~01:11-01:13
- VM port_base : **15000** (= IPv6 `<VM public IPv6>::`)
- The extended hook: `06-shadow-recon-linux/tls_hook/shadow_tls_hook.c` (V16)
  adds BIO_write/read + EVP_Encrypt/Decrypt_Update to the existing OpenSSL hooks

## Cross-refs

- [[project-vst-connect-msg-byte-exact]] — the TIER1 RE of 2026-05-16, the body hypothesis (= now invalidated: the body = the auth hash, not the streaming_token)
- [[project-video-ssl-tcp-channel-found]] — the discovery of the `:base+20` channel
- [[project-multinal-chunks-RE]] — the RegisterSession_Video booleans (= **no longer the suspect candidate** if the VST plaintext fix unblocks the bug)
- [[project-V12-taskbar-status-2026-05-15]] — the residual taskbar bug, to be re-validated after V16

## Status

- V16 step 1 (the extended hook): ✅ DONE
- V16 step 2 (plaintext capture) : ✅ DONE
- V16 step 3 (analyse + identification byte-exact) : ✅ DONE
- V16 step 4 (patch ctrl_video_tcp.c) : ✅ DONE (= build clean)
- V16 step 5 (runtime test — is the taskbar bug fixed?): ⏳ TO BE TESTED LIVE
