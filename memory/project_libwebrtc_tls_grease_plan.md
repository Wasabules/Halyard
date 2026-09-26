---
name: libwebrtc port + TLS GREASE plan (3 agents 2026-05-05)
description: A 5-level prioritised plan to fix the t=120 s freeze. Levels 1-2 (wolfSSL + GREASE) are recommended. Level 5 (a libwebrtc port) is not recommended after the audit — the ROI is catastrophic.
type: project
---
**A synthesis from 3 agents, 2026-05-05** (the libwebrtc port + TLS GREASE + the crypto gap).

## The prioritised plan

### N1 — Quick wins wolfSSL (1 jour, low risk)
Reconfigure CMake : `-DWOLFSSL_PSK=ON -DWOLFSSL_OCSP=ON -DWOLFSSL_CERTIFICATE_STATUS_REQUEST=ON -DWOLFSSL_ECH=ON`. Active session resume, status_request, encrypted SNI.

### N2 — Patch wolfSSL TLS GREASE (1-2 jours)
The target JA4 for Chrome 147: `t13d1516h2_8daaf6152771_d8a2da3f94cd`. wolfSSL has no native GREASE. A ~150-line patch in `internal.c::SendClientHello()` behind `#ifdef WOLFSSL_CLIENT_GREASE`:
- A GREASE cipher `0xXaXa` at the head
- 3 GREASE TLSX extensions (head/middle/tail)
- GREASE dans supported_versions, supported_groups, key_share
- Custom ext compress_certificate (27) + application_settings (17613)

### N3 — TWCC v2 + a simplified GoogCC (2-3 weeks)
**Probablement le vrai gap** : server stream 14× moins (BWE estimator confused). ~800 LoC C, RFC rmcat-gcc-02.
- TWCC v2 sender-side feedback enrichi
- GoogCC delay-based BWE Kalman + loss-based AIMD
- NACK scheduling Chrome-compatible
- RTX padding probes

### N4 — Migration libdatachannel (1-3 semaines)
**A decisive precedent**: `libnxbox` (an Xbox xCloud Switch homebrew) uses EXACTLY libdatachannel+libjuice **with no t=120 s freeze**. That suggests a complete Chrome fingerprint is not the condition. An aarch64 devkitA64 port = 1-3 weeks.

### N5 — libwebrtc port complet (NON RECOMMANDÉ)
- 6.4 GB source, 3-4M LoC C++
- The `gn+ninja` build system is not portable to devkitA64 (a CMake rewrite: 3-6 months)
- Blocking dependencies: `epoll`, `eventfd`, `prctl`, `/proc` (libnx does not provide them)
- ~80 MB RAM, .nro 80-150 MB
- No Switch/Vita/3DS precedent
- 6-12 mois effort, ROI catastrophique

## Why

The audit reveals that:
1. The wolfSSL build already has 90% of Chrome's features active (HAVE_AESGCM, CHACHA, TLS13, MLKEM hybrid, SRTP, ALPN, SNI, EXTENDED_MASTER, SESSION_TICKET, RSA_PSS, ECDHE)
2. TLS GREASE is the last major difference on the TLS-fingerprint side
3. The **real gap is still BWE/CC** on the RTP side (the server adapts quality against our simplistic client TWCC)
4. libnxbox proves libdatachannel works on Switch homebrew for cloud gaming without a freeze

## How to apply

**Recommendation**: N1+N2 in parallel (~3 days), then test → if the freeze persists, N3 (~3 weeks) or N4 (~2 weeks, if the whole stack is bypassed).

**DO NOT** do N5 unless it is a long-term milestone with a team.

**Sources** :
- The decrypted Chrome pcap `captures/shadow-chrome-20260505_003141.pcap`
- options.h wolfSSL build switch
- https://webrtc.googlesource.com/src/
- https://github.com/paullouisageneau/libdatachannel
- https://github.com/ursusworks/libnxbox (preuve concept Xbox xCloud sur Switch)
- RFC 8701 TLS GREASE
