---
name: The Shadow streaming protocol — the REAL one, confirmed through a decrypted pcap
description: The Linux ShadowPCDisplay client uses raw TCP+UDP (NOT QUIC). The msquic in the binary is dormant. Confirmed by a decrypted capture of 2026-05-02.
type: project
---
A decrypted pcap capture of the Linux Shadow client (`captures/session-20260502-214916/`):

## The protocol architecture confirmed (corrected 2026-05-02 through the Ghidra ExtractCtrlChan)

| Canal | Protocole | Port | Notes |
|---|---|---|---|
| **REST `/3/*` API** (vmproxy) | TCP/443 + TLS + **HTTP/2 via libcurl** | 443 | `POST /3/clients`, `/3/forward` SSE, etc. |
| **SslCtrlChanV2** | TCP + **direct TLS** (libuv + OpenSSL, **not h2**, not libcurl, **an empty ALPN**) | **443** (the same hostname as REST) | `[uint32_be len][protobuf]` frames; cleartext up to `EncryptionReply`, then chacha20-poly1305. Several concurrent streams (3 seen in the pcap: ~10K/273K/4K). |
| **Video** | UDP raw + chacha20-poly1305 | slot×1000+10 (ex 10010) | 5.1 MB / 8.7s ≈ ~5 Mbps |
| **Audio** | UDP raw + chacha20 | slot×1000+12 (ex 10012) | 245 KB |
| **Input/cursor** | UDP raw | slot×1000+13 (ex 10013) | 23 KB |
| Reserved | UDP/TCP | 10011, 10015, 10030 | <30 KB |

⚠ **A clarification**: the client multiplexes **several TLS connections onto the same hostname:443**, differentiated by ALPN:
- ALPN `h2,http/1.1` → REST API (libcurl + nghttp2)
- ALPN `http/1.1` → SSE long-poll (`/stream`, etc.)
- ALPN **vide** → SslCtrlChanV2 binaire (libuv + OpenSSL, frames length-prefixed protobuf)
Source code : `udu/deps/framework/src/common/ctrlchanv2/src/ssl_ctrl_chan_v2.cpp` (offset `0xea0340`).
Confirmed in the pcap: `06-shadow-recon-linux/captures/session-20260502-214916/shadow.pcap` streams 122/123/199 towards `ipv6-gpu-<instance>.frsbg01.compute.shadow.tech:443`, ALPN=ø.

⚠ **False hopes to avoid**:
- The `{id, port}` JSON schema in the binary is **NOT** for SslCtrlChanV2 — it is the reply to `POST /devices` (the shadowusb USB redirection for the gamepad).
- There is no need to consume the SSE `/3/forward` to discover a port (the first Ghidra RE agent was mistaken).

⚠ **M32 attempt abandoned on 2026-05-02**: 3 VMs tested (dusty-cabin, bless-truss, quaint-fern), direct TCP+TLS on 443 with no ALPN and `[uint32_be len][protobuf Authentication]` frames → **nginx answers HTTP/1.1 400 Bad Request every time**. SNI tested with and without an `ipv4-/ipv6-` prefix → identical. The Linux client's pcap (session-20260502-214916, gpu-revel) shows the binary works under certain conditions, but the server-side trigger was not identified (perhaps: an account flag, the ordering of a connection concurrent with the REST h2 stream, an initial magic byte, or an undocumented server parameter).
**The delivered modules (`streaming/{ctrl_tcp,ctrl_msgs,ctrl_rest,encryption,sufp,proto,smoke_test}.c`) are kept in the tree for future reuse** but disabled in connecting_activity. Refocusing on M15 WebRTC input, which is the real blocker.

**Server hostname pattern** : `gpu-<NAME>.frsbg01.compute.shadow.tech`
- The `ipv6-` prefix = AAAA only, `ipv4-` = A only, no prefix = dual-stack
- The OS preference (IPv6 on a modern stack) makes the client use IPv6
- **For Switch homebrew = force IPv4**: `getaddrinfo(AF_INET)` + `CURLOPT_IPRESOLVE_V4`. The same VM's IPv4 works.

## REST endpoints CtrlChanV2 (HTTP/2 sur TCP/443)
- `POST /3/clients` — creating the launcher client
- `GET /3/clients/<sessionId>` — info session
- `POST /3/forward` — forward des commandes (auth, register session, etc.)
- `GET /3/status` — SSE long-poll pour status updates
- Note: it is `/3/` (against `/2/` in the old capture) — an updated protocol

## QUIC = DORMANT
The `ShadowPCDisplay` binary LINKS msquic v2 (libmsquic.so.2.4.5) and has all the `Shadow::NetworkProtocol::{MQApi,MQClient,MQConnection,MQStream}` symbols, but the QUIC code is **never called** on the user accounts tested (the Android APK + Linux x86_64 user@<user>). msquic is probably:
- Code mort de migration en cours
- Or a backend for specific accounts/datacentres (pro, certain countries)
- Or a fallback that is never triggered

**Don't pivot to ngtcp2/QUIC** on the Switch. Reuse wolfSSL (TLS 1.3 over TCP + chacha20) + libnx UDP sockets.

**Why:** the static RE nearly spent 2 weeks porting ngtcp2 for nothing. A pcap taken in 5 minutes revealed the real answer.

**How to apply:**
- Architecture cible Switch =
  - libcurl + wolfSSL for the **REST `/3/*` API** (HTTP/2 over TCP/443)
  - **a libnx TCP socket + direct wolfSSL TLS** for SslCtrlChanV2 (a dynamic port obtained through the SSE `/3/forward`, length-prefixed protobuf frames)
  - sockets UDP libnx pour video/audio/inputs
  - wolfSSL chacha20-poly1305 to decrypt every UDP frame
  - The existing h264_decoder (NVDEC) unchanged
  - The existing audio (Opus + audout) unchanged
- The M17→M26 plan in `06-shadow-recon-linux/PORT_PLAN.md` needs revising: skip every QUIC step
- Reuse the existing `shadow/http.c` code (curl over wolfSSL), delete the whole WebRTC stack (sdp/ice/dtls/srtp/sctp/wss)
- Re-RE the gamepad format on UDP 10013 (the old APK notes on DTLS 1.2 cursor/input may apply)
