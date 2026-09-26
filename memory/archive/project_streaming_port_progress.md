---
name: Switch streaming port — progression
description: The current state of the Switch port of the streaming modules (after the WebRTC→raw TCP+UDP pivot). 4 base modules delivered, the rest documented.
type: project
---

> **ARCHIVED 2026-09-13 — a state, not a finding.** This records a progress count (4 of 11 modules) long overtaken.
> It was true when written. Nothing here should be acted on: read `KB.md`
> for what holds today. Kept because knowing what was tried, and when, is
> what stops it being tried again.

## The `/3/clients` smoke test validated at runtime (2026-05-02)

POST/DELETE `/3/clients` confirmed end to end from the Switch:
- `main_jwt` + n'importe quel type → response `{ id: "...-main", streamingtoken, streamingtoken_expiry, remote }` HTTP 201. Token = credential pour TCP control + UDP stream.
- `launcher_jwt` → the response `{ id: "...-launcher", spice_secret, ... }` HTTP 201. NO streamingtoken (probably for file-transfer/clipboard).
- The server **ignores the body's `type`**, it attaches it to the JWT. So there is no point trying a cross-check.
- `DELETE /3/clients/<id>` → 204 No Content.

For the streaming pipeline: use `creds.main_jwt`, POST /3/clients, extract `streamingtoken` + `id` → inject them into the `Request_Authentication` protobuf.

## Modules delivered (the build is OK, untested at runtime)

`demo/src/streaming/` created. It coexists with `webrtc/`, which will be removed at the end of the port.

| Module | Fichiers | Statut | Notes |
|---|---|---|---|
| Force IPv4 + DNS strip | `shadow/http.c` (+ `streaming/ctrl_rest.c::strip_ipv6_prefix`) | ✅ | `CURL_IPRESOLVE_V4` added to common_setopts. The "ipv6-" prefix strip lives in ctrl_rest.c. |
| A ChaCha20-Poly1305 wrapper | `streaming/encryption.{c,h}` | ✅ | wolfSSL's `wc_ChaCha20Poly1305_Encrypt/Decrypt`. An auto-incremented LE nonce, replay protection on Rx, 2 distinct keys (Tx+Rx) matching the Linux binary's layout. |
| SUFP fragmentation | `streaming/sufp.{c,h}` | ✅ | Type 1/2/3 chunk parsing, reassembly window 32 frames, builder pour outgoing chunks + ping. |
| REST `/3/*` endpoints | `streaming/ctrl_rest.{c,h}` | ✅ | register_client → streamingtoken, forward (commands JSON), unregister. JSON parse minimal sans dependency. |
| A mini protobuf encoder/decoder | `streaming/proto.{c,h}` | ✅ | The complete wire format (varint, length-delimited) with no external dependency. Helpers for write_uint/string/bytes/submsg + iter_fields. |
| ControlProto messages | `streaming/ctrl_msgs.{c,h}` | ✅ | Build Authentication (4 strings + permissions bitmap), Encryption Request (algos), Parse Authentication+Encryption Reply (extrait key 32B, nonce_extra). |

## Modules still to write

| # | Module | Effort | Dependencies |
|---|---|---|---|
| M31 | A protobuf encoder/decoder for ControlProto::{Authentication, Encryption, RegisterSession} | 6 h | nanopb cross-compile + a .proto reconstructed from `ENCRYPTION_AND_FRAMING.md` §3 |
| M32 | TCP/TLS control channel + state machine bootstrap | 4h | Existing wolfSSL TLS client + protobuf module |
| M33 | A UDP socket pump + per-port dispatch → SUFP → cipher → handler | 3 h | sufp + encryption (already OK) |
| M34 | A reassembled video frame → strip the header → the existing h264_decoder | 1 h | webrtc/h264_decoder.c recycled |
| M35 | A reassembled audio frame → the existing audio.c | 1 h | webrtc/audio.c recycled |
| M36 | Input/cursor templates → encrypt → a UDP send | 2 h | shadow_input.c recycled |
| M37 | Clean up the obsolete webrtc/ files (wss/sdp/ice/dtls/srtp/sctp, ~2500 LOC) | 1 h | — |

**Estimated remaining total**: ~18 h for a working stream on the Switch.

## Inconnues runtime restantes

All "resolvable at testing time", but worth keeping an eye on:
1. The order of the 4 strings in `Request_Authentication` — 24 brute-forceable permutations (try the order `[streamingtoken, client_id, sessionId, connectionId]` first, which is the most likely hypothesis)
2. The exact semantics of byte 1 (subchan) in SUFP — empirically observable
3. The SUFP frame-window size — currently 32 in `streaming/sufp.h::SUFP_WINDOW_SIZE`, to be raised if too small
4. Sub-messages `RegisterSession_*` fields exacts — defaults marchent souvent

## Tests Frida non concluants (4 essais)

Frida is unstable on this stripped binary: 2 of 9 hooks (`NewCipher`, `Encrypt`) at 0x121b080/0x121b590 point into string constants (typeinfo data) rather than code. The other 7 install but no event was captured across the 4 short sessions (ShadowPCDisplay exits too quickly, or the flow is not triggered).

The scripts are kept in `tools/frida/` in case we want to try again with a longer session (60 s+ between clicking Start and closing).

## The decision

**We continue on the basis of a static RE at 90 %+ confidence.** The unknowns will be resolved at runtime on the Switch side when we test against a real Shadow VM.
