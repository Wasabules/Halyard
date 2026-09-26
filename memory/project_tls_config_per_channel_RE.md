---
name: project-tls-config-per-channel-re
description: TIER3 — the desktop's TLS config = TLS 1.1+ (any) without SNI/ALPN, VERIFY_PEER with a CA bundle, default ciphers. Our Switch forces TLS 1.2 only = a mismatch.
metadata:
  type: project
---

## TL;DR

TIER3 2026-05-16: a complete RE of the desktop's TLS configuration through `sub_E12600` (the SslContext ctor) + `sub_DEC150` (`SslIOChannel::doHandshake`). The desktop uses **OpenSSL**; the whole TLS stack is centralised in two files — `ssl_context.cpp` + `ssl_io_channel.cpp` — and shared by every channel (VST, CtrlChanV2, Cursor, Input, AudioOut).

**Verdict**: our Switch (wolfSSL, TLS 1.2 only) is **functionally fine** but **more restrictive** than the desktop. A potential mismatch on performance (= TLS 1.3 0-RTT) but probably not the cause of the taskbar bug.

## Config TLS desktop — bullet points (C95)

| Parameter | Desktop value | Source |
|-----------|---------------|--------|
| `TLS_method` | `TLS_client_method()` (= n'importe quelle version) | `sub_570FD0` |
| `DTLS_method` | `DTLS_client_method()` (= audio/cursor) | `sub_571010` |
| `min_proto_version` | **TLS 1.1 (= 0x0302 = 770)** | `SSL_CTX_ctrl(ctx, 123, 770, 0)` in `sub_E12600` |
| `max_proto_version` | **not set** (= TLS 1.3 allowed) | (zero hits) |
| TLS 1.2 cipher list | **OpenSSL DEFAULT** (per-channel optional through `a1[30]`) | `SSL_set_cipher_list` in `sub_DEC150` |
| Cipher list TLS 1.3 | **DEFAULT OpenSSL** (= AES256-GCM/CHACHA20/AES128-GCM) | aucun `SSL_CTX_set_ciphersuites` |
| SNI | **NOT sent** (no `SSL_set_tlsext_host_name`) | grep, zero hits |
| ALPN | **NOT offered** (no `SSL_CTX_set_alpn_protos` / `SSL_set_alpn_protos`) | grep, zero hits |
| Cert verify | `SSL_VERIFY_PEER` + callback `OnSslVerifyCert = sub_DEC520` | `sub_DEC150:90` |
| Verify callback behavior | retourne `preverify_ok` (= valide), log error 18 (= self-signed depth 0) | `sub_DEC520:186` |
| CA bundle | loaded through `SSL_CTX_load_verify_locations(ctx, cert_path)`; `cert_path` = a std::string passed to the SslContext | `sub_E14AE0:77` |
| Session reuse | **none** (aucun `SSL_CTX_sess_set_*` / `SSL_set_session`) | grep |
| PSK | conditionnel (`a1[3] != 0` → `SSL_set_psk_client_callback(ssl, sub_DEAA40 = OnPSKClientCallback)`) | `sub_DEC150:89` |
| Mode | `SSL_MODE_AUTO_RETRY \| SSL_MODE_ENABLE_PARTIAL_WRITE \| SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER \| SSL_MODE_RELEASE_BUFFERS` (= bits 1+2+4+16 = 23) | `SSL_CTX_ctrl(ctx, 33, 23, 0)` |

## Pseudo-code handshake (`sub_DEC150` = `SslIOChannel::doHandshake`)

```c
__int64 sub_DEC150(_QWORD *a1) {
    a1[23] = BIO_new(BIO_s_mem());
    a1[24] = BIO_new(BIO_s_mem());
    SSL *ssl = a1[37];
    SSL_set_bio(ssl, a1[23], a1[24]);
    if (a1[3])
        SSL_set_psk_client_callback(ssl, sub_DEAA40);
    SSL_set_verify();                              // VERIFY_PEER + callback
    SSL_set_info_callback(ssl, sub_DEBD50);
    SSL_set_connect_state(ssl);
    SSL_set_ex_data(ssl, 1, a1);
    if (a1[30] && !SSL_set_cipher_list())
        log("could not set cipher list");
    return SSL_do_handshake(ssl);
}
```

## Comparison with our Switch / Linux client

| Setting | Desktop | Notre code | Match |
|---------|---------|------------|-------|
| Min proto | TLS 1.1 | TLS 1.2 (wolfSSL `wolfTLSv1_2_client_method`) | ⚠ mismatch (= TLS 1.3 lost) |
| Max proto | (any) | TLS 1.2 | ⚠ mismatch |
| SNI | (none) | (none) | ✓ |
| ALPN | (none) | (none) | ✓ |
| Cipher list | OpenSSL default | wolfSSL default | ~ |
| Cert verify | VERIFY_PEER + CA bundle | `SSL_VERIFY_NONE` | OK (= server side n'enforce pas) |
| Session reuse | none | none | ✓ |
| PSK | conditional | absent | ✓ (= a1[3] is always 0 on our side) |

## Action items potentiels

1. **Try `wolfTLS_client_method()` instead of `wolfTLSv1_2_client_method()`** — it allows TLS 1.3. Not urgent (= TLS 1.3 0-RTT is useless for the control channel), but good hygiene.
2. The cipher list — the desktop does not override it, and neither do we. ✓
3. Certificate verification — the desktop validates (= if we enabled VERIFY_PEER with the Shadow CA, we would be byte-exact). Not critical on Switch.
4. The **PSK callback** = `sub_DEAA40` (= `OnPSKClientCallback`) — reserved for channels with a pre-shared key (= probably the legacy WSS route). Not relevant to the native path.

## Refs

- Dumps IDA : `tools/ida/out/tier3/I2_*.c`, `I3_*.c`, `I4_*.c`
- Doc : `tools/ida/out/TIER3_RE_2026-05-16.md` §I
- The ssl_io_channel.cpp source: `udu/deps/framework/src/common/network/src/ssl_io_channel.cpp` (= an internal path, dumped from the strings)
- Memory cross-ref :
  - [[project-sslctrlchanv2-wire-format-FOUND]] — ctrl wire layer
  - [[project-libwebrtc-tls-grease-plan]] — TLS fingerprint considerations
  - [[project-shadow-real-protocol-confirmed]] — the underlying protocol
- The confidence ladder is C95 except for "no max_proto", which could be an undecoded option flag bit (C90).
