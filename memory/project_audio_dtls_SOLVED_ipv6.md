---
name: Audio DTLS :base+12 RESOLVED — wolfSSL built without WOLFSSL_IPV6 + an IPv6 peer
description: 2026-08-21 — The DTLS handshake failed in 100% of sessions because our first sendto returned -174 (NOT_COMPILED_IN)  EmbedSendTo refuses an IPv6 peer when the library lacks WOLFSSL_IPV6. Fixed with wolfSSL_set_dtls_fd_connected(). + A3, the fix revealed an rx_thread that never exited.
type: project
---
## Root cause

The audio DTLS handshake failed in **100 % of sessions** (`err=-308`) ever since
the channel was created. The I/O trace (A1) shows that **our very first `sendto`
returns `-174` = `NOT_COMPILED_IN`**: the ClientHello never left.

`wolfssl/src/wolfio.c::EmbedSendTo`:

```c
else if (!dtlsCtx->connected) {
    peer = dtlsCtx->peer.sa;
#ifndef WOLFSSL_IPV6
    if (PeerIsIpv6(peer, peerSz)) return NOT_COMPILED_IN;
#endif
}
```

The vendored wolfSSL is built **without `WOLFSSL_IPV6`** and the audio channel
resolves to IPv6 (`family=10`, `peer_len=28`).

## The fix (A2)

`wolfSSL_set_dtls_fd_connected()` instead of `wolfSSL_set_fd()`: our socket is
already `connect()`-ed, so `dtlsCtx.connected = 1` → `send()`/`recv()` → the whole
peer path is short-circuited, IPv6 guard included. No wolfSSL rebuild, IPv6 kept.
Toggle `SHADOW_AUDIO_DTLS_CONNECTED=0`.

A complete handshake, `TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384` / DTLSv1.2, with
sizes **identical to the desktop capture**: HelloVerifyRequest 60 B, Certificate
765 B, ServerKeyExchange 386 B, ServerHelloDone 25 B, Finished 14 B.

## A regression revealed (A3)

A successful handshake ⇒ the `rx_thread` finally existed, and **never** exited.
`SO_RCVTIMEO=100ms` is not enough: on the wolfSSL side a socket timeout in DTLS
triggers its internal retransmission and `wolfSSL_read` does not return. The
process blocked at shutdown (**151 s real for `--duration=30`**). A direct
violation of §7.3 (abort_flag within 100 ms — on Switch, leaked handles = a
reboot). Fix: `wolfSSL_dtls_set_using_nonblock(ssl, 1)`.

## What the previous RE got wrong

[[project-audio-channel-wire-RE]] (2026-05-23) concluded "the code is ready,
err=-308 = just a timeout, bump it to 10 s". The timeout had nothing to do with
it. The methodological error: the desktop capture had been analysed byte by byte,
but **nobody had ever traced OUR wire**. Instrumenting the I/O
(`SHADOW_AUDIO_DTLS_TRACE=1`) gave the answer in one run.

## Remaining

No app-data record (`0x17`) received — the log says `idle Ns (no audio packets)`.
Expected on a silent Windows desktop; to be confirmed with sound playing on the
VM, which requires being able to click — so it depends on the input bug.

Full detail: `KB.md §3.19`.
