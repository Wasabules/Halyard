---
name: Shadow encryption + UDP framing — RE complet via Ghidra
description: Wire-level details of the chacha20-poly1305 encryption and the SUFP framing. Enough to implement the Switch side without a decrypted pcap.
type: project
---
Full documentation: `$REPO/06-shadow-recon-linux/ENCRYPTION_AND_FRAMING.md` (a deep Ghidra RE of `ShadowPCDisplay`, 2026-05-02).

## Encryption (CryptoCipherOpenSsl) — 98% certitude

**Wire format AEAD** : `[ciphertext_N | nonce_12B | tag_16B]` (overhead = 28B).
- ChaCha20-Poly1305 sans AAD
- Nonce = 12 B little-endian, initialised at random on the first encrypt, +1 per subsequent encrypt
- Replay protection : `BN_cmp(rx_last, new) >= 0` → drop
- Encrypt @ 0x00e1b590, Decrypt @ 0x00e20e20
- `nonce_len = 12`, `tag_len = 16` (cipher object @ +0x4a/+0x4c)
- The Tx key @ +0x30, the Rx key @ +0x60 (32 B each)

**Switch impl** : `wc_ChaCha20Poly1305_Encrypt(key32, iv12, NULL, 0, plain, len, out, tag16)` direct.

## UDP framing — SUFP (Shadow UDP Framed Protocol) — 90% certitude

**Chunk type 3** (modern, 11B header + payload) :
| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `pkt_type:4 \| version:4` (lo nibble = type=3) |
| 1 | 1 | `channel_subtype` |
| 2-3 | 2 | `chunk_idx` uint16 LE (1-indexed) |
| 4-5 | 2 | `max_chunks` uint16 LE |
| 6-9 | 4 | `frame_id` uint32 LE |
| 10 | 1 | `flags` (bit 0 = last) |
| 11+ | N | encrypted chunk payload |

Reassembly @ `SufpUdpIOChannel::OnDataReceived` (0x00d7f1e0). Sliding window de N frames (~16 ?).

**Type 0xf** = ping/RTT measurement. **Types 1/2** = legacy (10B header).

Once reassembled: `[ct | nonce12 | tag16]` → decrypt → a plaintext payload `VideoUdpFrame / AudioUdpFrame / CursorUdpFrame / Inputs FB`.

## CtrlChanV2 bootstrap — 85% certitude

```
TCP TLS 1.3 connect → ALPN/cert PKI standard
    ↓
[srv→cli] Request_State_Version
    ↓
[cli→srv] Request_Authentication { reverse_auto_register, permissions(bitmap),
                                    streamingtoken, client_id, sessionId, connId }
    ↓
[srv→cli] Reply_Authentication (no key)
    ↓
[cli→srv] Request_Encryption { supported_algorithms: [0,2,4,1] } per channel
    ↓
[srv→cli] Reply_Encryption { chosen=1(ChaCha20), key=32B, nonce_extra }
    ↓
EnableEncryption() → CryptoFactory::NewCipher(key) → 2 EVP_CTX (Tx+Rx)
    ↓
[cli→srv] Request_RegisterSession { Video=SUFP(5), Audio=SUFP(5), Cursor=SUFP, Input=SUFP, ... }
    ↓
[srv→cli] Reply_RegisterSession { ports=[10010,10012,10013,...], etc. }
    ↓
UDP sockets ouverts par channel, SUFP chunks circulent
```

**Permissions bitmap** : Video=0x02, AudioIn=0x04, AudioOut=0x08, Cursor=0x10, Input=0x20, Gamepad=0x40, Clipboard=0x80, FileTransfer=0x100.

**ProtoType enum** : `FlatBuffers=0, SCP=1, SFTP=2, SSP=3, SSUFP=4, SUFP=5`.

## Streaming token (≠ JWT OAuth) — 90% certitude

It comes from the `POST /3/clients` body reply: `{data: {id, streamingtoken, streamingtoken_expiry, remote}}`. That `streamingtoken` has to be re-injected into `Request_Authentication.field3` (a string), with the `client_id` (= `data.id`) in field4.

## Inconnues restantes

1. The exact semantics of the `subchan`(1) and `flags`(10) bytes in SUFP — clear from a pcap
2. The exact order of the 4 strings in Request_Authentication (24 brute-forceable permutations)
3. The details of the RegisterSession_Video/Audio/etc. sub-messages (the codec/bitrate fields)
4. The frame-window size for the SUFP reassembly (~16 assumed)

None of them blocks starting the Switch implementation. The unknowns will be resolved at runtime.

## Key Ghidra addresses
- `0x00e1b590` Encrypt, `0x00e20e20` Decrypt, `0x00e1b080` CryptoFactory::NewCipher
- `0x00d7f1e0` SufpUdpIOChannel::OnDataReceived, `0x00d85fa0` udp_data_chunk::init
- `0x00858480` CtrlChanV2Manager::Authenticate, `0x00907c40` EnableEncryption
- `0x00b6dab0` the REST `/3/clients` parser (= where we get the `streamingtoken`)

**Why:** without this RE we would have had to either guess (a blind implementation = very probably broken) or wait for a second, better-instrumented capture. With these documents we can start M27→M32 with confidence.

**How to apply:** when first writing a crypto/SUFP/CtrlChanV2 module on the Switch side, read the corresponding section of the full document. Reproduce the pseudo-C as is through wolfSSL+nanopb.
