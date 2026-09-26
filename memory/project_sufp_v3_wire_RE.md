---
name: 🔬 SUFP v3 wire format - RE Ghidra ShadowPCDisplay
description: 2026-05-09 ~03:30 — A complete RE of the chunk parser through radare2 on ShadowPCDisplay. The 11 B header is confirmed, the semantics partially. Multi-chunk decryption remains to be explored (probably Reed-Solomon FEC).
type: project
---
## TL;DR

A Ghidra/r2 RE of the binary `/usr/share/shadow-prod/resources/app.asar.unpacked/release/native/ShadowPCDisplay` (= an 18 MiB stripped ELF) confirms the SUFP v3 wire format:

- **Header = 11 bytes** (= confirmed by `mov [rdi+0x20], 0xb` for pkt_type=3)
- **Layout**: `[byte 0=version<<4|type][byte 1=subchan][bytes 2-3=chunk_idx u16 LE][bytes 4-5=max_chunks u16 LE][bytes 6-9=u32 LE id/seq][byte 10=flags]`
- **`byte 10 & 1`** = the data flag (= our working filter). `flag==1` chunks decrypt directly as a self-contained chacha20 wire `[ct|nonce12|tag16]`. `flag==0` chunks are something else (= probably FEC parity).

## 6 protocoles negociables

`FlatBuffers / SCP / SSP / SFTP / SSUFP / SUFP`

Our code uses SUFP. SSUFP = "Secure SUFP" (= perhaps stricter encryption). To be tested, to see whether SSUFP gives a better result.

## Fonction RE'd

- `fcn.00d85fa0` at `0x00d85fa0` = the chunk parser
- For the pkt_type==3 path (= our case, byte0=0x13): code at `0xd865c0`
- Strings xref :
  - `OnSufpChunkReceived` at `0x012a9c1f`
  - `Decrypt(): Invalid nonce` at `0x012c17b0`
  - `chunk wrong version {}, size {}` at `0x012baa58`
  - `chunk does not belong to this frame` at `0x012bab68`
  - Source : `/udu/deps/framework/src/common/protocols/src/sufp/udp_data_chunk.cpp`

## État image runs 22-30

| Run | Approche | Decoded | Image visuelle |
|---|---|---|---|
| 22 | Legacy `byte10==0x01` + assume hdr_len=6/19 | 30 fps | "1ère ligne propre + flou" |
| 26 | Idem + scan Annex-B `00 00 00 01` | 30 fps | "Artefacts visibles" ✓ |
| 27 | Concat tous chunks → decrypt | 1 frame | "Waiting for video" |
| 29 | Concat byte10==0x01 chunks → decrypt | 9 frames/20s | "Waiting for video" |
| 30 | Revert run 26 (= mode stable production) | 30 fps | "Artefacts visibles" |

## Unknown semantics

For a SHARP picture we need to understand:
1. How the `byte10==0x00` (= flag=0) chunks are decodable. Hypotheses:
   - Reed-Solomon FEC : N chunks data + (max-N) chunks parity → recovery
   - Intermediate chunks of a split chacha20 wire
   - Another encryption with a differently derived nonce
2. The role of bytes 6-9 (= a u32 LE). Seen varying per chunk even within the same frame, so not a stable frame_id.
3. The complex xrefs towards `OnDataReceived` and `udp_packet_receiver.cpp`, which assemble and decrypt.

## Functions RE'd so far (= through a Ghidra headless decompile)

| Function | Address | Role | Pseudo-C |
|---|---|---|---|
| `FUN_00d85fa0` | 0xd85fa0 | UdpDataChunk::parse — header 11B v3, 10B v2 | ✓ |
| `FUN_00d8cd50` | 0xd8cd50 | UdpFrame::add — store chunk au slot[chunk_idx] | ✓ |
| `FUN_00d941f0` | 0xd941f0 | UdpPacketReceiver::dispatch — queue+order chunks | ✓ |
| `FUN_00e20e20` | 0xe20e20 | CryptoCipherOpenSsl::Decrypt — wrapper EVP_chacha20_poly1305 | ✓ |

## Decrypt API (= signature byte-exact)

```c
undefined8 FUN_00e20e20(
    long this,         // CryptoCipher* (= cipher state)
    uchar *ct_inout,   // ciphertext buffer (decrypt in-place)
    int   ct_len,
    uchar *nonce,      // nonce buffer (size = this->nonce_size @ +0x78)
    void  *tag         // tag buffer (size = this->tag_size @ +0x7a)
);
// Internally :
//   1. EVP_CIPHER_CTX_new (lazy) → this+0x80
//   2. BN_lebin2bn(nonce, nonce_size, NULL) → BIGNUM b
//   3. Replay check : BN_cmp(this->last_nonce @ +0x88, b) — fails if nonce <= last
//   4. EVP_DecryptInit(ctx, this->cipher @ +0x28, this->key @ +0x60, nonce)
//   5. EVP_CIPHER_CTX_ctrl(ctx, 0x11 = AEAD_SET_TAG, tag_size, tag)
//   6. EVP_DecryptUpdate(ctx, ct_inout, &len_out, ct_inout, ct_len)
//   7. EVP_DecryptFinal(ctx, ct_inout + len_out, &len_out)
```

**Key insight**: the caller passes the `nonce` and the `tag` SEPARATELY — so the caller must extract those bytes from the reassembled buffer. Shadow's standard `[ct | nonce12 | tag16]` wire format holds for self-contained chunks, but for multi-chunk we have to work out how the caller assembles and extracts.

## Vtable CryptoCipherOpenSsl

À `0x12c18d0..0x12c1900` (= .rodata) :
| Slot | Address | Function |
|---|---|---|
| 0 | 0xe1b4d0 | (probable Encrypt) |
| 1 | 0xe1b4e0 | (probable EncryptUpdate) |
| 2 | 0xe1b4f0 | ? |
| 3 | 0xe1b590 | ? |
| 4 | **0xe20e20** | **Decrypt** ← notre cible |

## UdpFrame structure (= reverse)

| Offset | Field | Notes |
|---|---|---|
| +8 | byte = subchan | matches chunk->subchan |
| +0xa | short = max_chunks | matches chunk->max_chunks |
| +0x18 (= [3]) | short = remaining_count | starts at max_chunks, decremented on each add |
| +0x28 (= [5]) | ptr = chunks[] | array of UdpDataChunk* (= 8B each) |
| +0x30 (= [6]) | ptr = chunks_end | sentinel |
| +0x40 (= [8]) | u32 = ??? | set from chunk->FUN_00d80880 (= probable codec/flag) |

UdpDataChunk struct = **56 bytes** (= confirmed via `operator delete(pvVar1, 0x38)`).

## Hypothesis status for the multi-chunk decrypt

- **Hypothesis 1 (tested in run 27)**: concatenate every chunk → the wire = `[ct|nonce12|tag16]` → fail (0 frames decoded)
- **Hypothesis 2 (tested in run 29)**: concatenate only the byte10==0x01 chunks → fail (9 frames / 20 s)
- **Hypothesis 3 (legacy run 22)**: decrypt per packet (chacha20 self-contained) → 30 fps but a partial picture
- **Hypothesis 4 (to be tested)**: Reed-Solomon FEC: N data + K parity chunks, where decrypting requires the whole frame rebuilt with recovery
- **Hypothesis 5 (tested in run 31)**: decrypt EACH chunk individually → concatenate the plaintexts by chunk_idx → status to be confirmed

## RTTI classes identified

The ShadowPCDisplay binary reveals (= through un-stripped RTTI strings):

- **`VideoUdpFrame`** (= our case) — a video frame over UDP
- **`AudioUdpFrame`** — frame audio sur UDP
- **`CursorUdpFrame`** — frame cursor sur UDP
- **`CursorTcpFrame`** — frame cursor sur TCP (= variante)
- **`CursorPacketReceiver`** — packet receiver pour cursor
- **`UdpFrame`** — base class
- **`NetworkFrame`** — top base class
- **`SufpUdpIOChannel`** — I/O wrapper pour SUFP UDP
- **`UdpDataChunk`** — struct chunk
- Stats classes : `VideoStatsConsumer`, `GenericStatsConsumer<VideoUdpFrame::Stats, 4>`, etc.

Hierarchie : `VideoUdpFrame : public UdpFrame : public NetworkFrame`

## Vtable layout VideoUdpFrame (= partial)

| Slot | Method | Offset | Notes |
|---|---|---|---|
| 5 | virtual_40 | 0x28 | Getter `return *(qword*)(this + 0x20)` (= probably `getData()` returning frame buffer) |

To be identified: the other methods (`onAllChunksReceived`, `decrypt`, `flush`).

## The function that calls Decrypt — not yet identified

`call qword [reg + 0x20]` = 250 occurrences (= vtable[4] of various classes). Only one inside the sufp range, at `0xd93cff`, but it takes 2 arguments (= not Decrypt, which takes 5). Decrypt is probably called through a different class's vtable (= perhaps `DecryptionPipeline` or `SecuredFrame`).

To identify it: an interactive Ghidra GUI is needed (= dynamic cross-references).

The C++ sources identified:
- `/udu/deps/framework/src/common/protocols/src/sufp/udp_data_chunk.cpp`
- `/udu/deps/framework/src/common/protocols/src/sufp/udp_frame.cpp`
- `/udu/deps/framework/src/common/protocols/src/sufp/udp_packet_receiver.cpp`
- `/udu/deps/framework/src/common/protocols/src/sufp/sufp_udp_io_channel.cpp`
- `/udu/deps/framework/src/common/crypto/src/crypto_cipher_openssl.cpp`
- `/udu/deps/framework/src/common/video/src/ffmpeg/ffmpeg_video_decoder.cpp`

## Next step

To decrypt the multi-chunk frames:
1. **A complete RE of UdpFrame::add (fcn.00d8cd50)** — understand exactly where and how the chunks are stored in the buffer + how "frame complete" is detected.
2. **Identify the function that calls `Decrypt()`** — cross-reference `method.CryptoCipherOpenSsl.virtual_56` (= through a vtable, not a direct call) to find the client that prepares the buffer.
3. **Test SSUFP negotiation**: modify RegisterSession to announce SSUFP instead of SUFP (= if possible, to see whether the server returns a simpler format).
4. **An alternative: use a Python script** that takes the whole packet dump + the key and tries decrypting under various structural hypotheses (= concatenating chunks 0..N for various N, decrypting with various nonce-extraction strategies).

## Refs

- Binary : `/usr/share/shadow-prod/resources/app.asar.unpacked/release/native/ShadowPCDisplay`
- Source paths : `/udu/deps/framework/src/common/protocols/src/sufp/`
- Code legacy stable : `streaming/ctrl_session.c::on_video_packet`
- Cross-ref : `feedback_sufp_format_obsolete.md`, `project_native_GUI_streaming_STABLE.md`
