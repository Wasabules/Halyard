---
name: libnxbox RE — freeze app-level DC ack
description: libnxbox (closed source, libpeer not libdatachannel) had a freeze@10min caused by an un-acked "TransactionStart" DataChannel message. It suggests Shadow's freeze@120s is also a missing app-level ack.
type: project
---
**RE 2026-05-05**: an agent downloaded the libnxbox NRO and dumped its strings.

## The libnxbox stack confirmed

- **libpeer** (sepfy) — PAS libdatachannel. ICE custom, SCTP custom (~3000 LoC C).
- **mbedtls** vendored (pas wolfSSL)
- **usrsctp** vendored
- **the averne ffmpeg fork** (NVDEC nvtegra) — identical to our stack
- **SDL2** pour audio + UI
- **libcurl + libopus** static
- NO goog-cc / TWCC strings → the xCloud server accepts a client with no BWE
- AUCUN TLS GREASE / spoof → mbedtls default
- `CN=libpeer` dans DTLS cert

## Insight FREEZE

**Release 0.8.4**: "streams terminated at frame 18000, ~10 minutes" → the fix = answering the **`TransactionStart` data-channel messages** (e.g. `/streaming/title/constrain`). xCloud sends them periodically and the client must ack. Without an ack → the stream is killed.

**Direct application to Shadow**: our t=120 s freeze could be a server→client DC message that we ignore (any application-level "ping" on shadow-input/cursor/controller).

## Patches Switch HOS critiques (libnxbox strings)

- SCTP force cleanup wrapper sur reconnect (`ipi_count_asoc=0` + free port 5000)
- A fixed local UDP IP on port 9002 (manual port forwarding) — Shadow does not allow it (the server hands out candidates)
- The NRO **must run in title mode** (applet mode crashes — the heap is too small)
- libnx pthread errors tolerated (`pthread_mutex_init failed`, `pthread_once has failed`)

## Top 5 lessons applicables shadow2switch

1. **Audit the server→client DC traffic**: look for application messages we ignore (our webrtc.log shows `dc: stream=3 (?) ppid=53 len=1160` but we do not process it)
2. **HTTP keepalive too**: we have `/status` polling, check that we consume the body (not just the 200)
3. **A usrsctp forced cleanup** on reconnect (our custom sctp.c probably has the same problem)
4. **Single fixed UDP host candidate** : pas applicable Shadow (server-driven candidates)
5. **The TLS fingerprint**: libnxbox without spoofing is fine for xCloud; Shadow inspects harder (Cloudflare)

## Why

libnxbox proves that **a minimal WebRTC client on Switch can hold without freezing**. But they have no TWCC/GoogCC. Their key = **honouring the server's application messages**. Our t=120 s freeze may be of the same nature.

## How to apply

1. **An immediate (free) action**: audit our `webrtc.log` for server→client `dc: stream=N`, and see whether any of them require an application-level reply. If we find unprocessed messages → that is very probably the cause.
2. **Plan B** : TWCC v2 + GoogCC (~8 jours)
3. **Plan C** : migration libdatachannel (~19 jours, risques MbedTLS+usrsctp)
4. **Not planned**: copying libnxbox (closed source + libpeer ≠ libdatachannel, and their xCloud server ≠ Shadow)

**Sources** : agent RE 2026-05-05 sur https://github.com/ursusworks/libnxbox + binary `/tmp/nxbox_nro/libnxbox.nro`.
