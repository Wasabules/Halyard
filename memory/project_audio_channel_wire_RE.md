---
name: project-audio-channel-wire-RE
description: Audio :base+12 wire format — byte-exact DTLS 1.2 ECDHE-ECDSA, a PSK is NOT required, and it does NOT gate the video
metadata:
  type: project
---

# Audio channel :base+12 wire RE — TIER4 axe B

**Date** : 2026-05-23. **Confidence** : C95.

## TL;DR

The audio channel is **standard DTLS 1.2 ECDHE-ECDSA** with no PSK. Our I2 RE
earlier one, which concluded there was a PSK failure (err=-308), was **incorrect**: the
the desktop uses no PSK cipher suite. The MASTER capture proves a
classic DTLS handshake sequence on :base+12 UDP with a HelloVerifyRequest +
cookie + cert chain ECDSA. **Notre code RE2 (`ctrl_audio_dtls.c`) match
desktop**.

## Byte-exact sequence capture (`:8012` UDP, fd=305)

| L | Direction | Size | Format |
|---|-----------|------|--------|
| L27176 | client→ | 211B | DTLS ClientHello sans cookie : `16 fe ff 00 00 ... fe fd` |
| L27672 | server→ | 60B | DTLS HelloVerifyRequest (cookie 32B) |
| L27777 | client→ | 243B | DTLS ClientHello AVEC cookie (32B inline) |
| L28249 | server→ | 106B | ServerHello + extensions |
| L28342 | server→ | 765B | Certificate chain (ECDSA) |
| L28472 | server→ | 386B | ServerKeyExchange + ServerHelloDone |
| L28581 | server→ | 25B | ChangeCipherSpec |
| L28771 | client→ | 198B | ClientKeyExchange + Finished |
| L29164 | server→ | 14B | Server Finished |
| L35683+ | server→ | 141B repeated | Opus audio frames encrypted |

## Ciphers offered by the desktop (the 8 from the 211 B ClientHello)

```
c0 2c  ECDHE-ECDSA-AES256-GCM-SHA384
c0 30  ECDHE-RSA-AES256-GCM-SHA384
00 9f  DHE-RSA-AES256-GCM-SHA384
cc a9  ECDHE-ECDSA-CHACHA20-POLY1305
cc a8  ECDHE-RSA-CHACHA20-POLY1305
cc aa  DHE-RSA-CHACHA20-POLY1305
c0 2b  ECDHE-ECDSA-AES128-GCM-SHA256
c0 2f  ECDHE-RSA-AES128-GCM-SHA256
+ AES-128/256-SHA et SCSV fallbacks
```

**No PSK cipher** (= no 0x00 0x8C..0x91, no 0xC0 0x35..0x38). Our
earlier PSK hypothesis = wrong.

## Notre code status

`ctrl_audio_dtls.c:139-150` (the RE2 patch of 2026-05-18) already matches that cipher
list. `ctrl_audio_dtls.c:165` raises `SO_RCVTIMEO` to 5 s for the double
a HelloVerifyRequest round trip. Our `wolfDTLSv1_2_client_method()` is correct.
**The code is ready, it just needs a runtime test to validate that the handshake passes.**

## Does the audio gate the video? NO

Cross-channel analysis capture MASTER :
- The `:8010` video chunks arrive **before** the audio DTLS finished (L27191 t=998.573
  vs L29164 t=998.698).
- The cursor channel `:8030` opened and registered with 25 B WITHOUT DTLS (= plain UDP with
  the same `41 01 00 14 00 [hash20]` format as the video register).
- No video bitrate change correlated with the audio DTLS finished.

**Verdict C90**: the audio is an independent channel. **Not related to the taskbar bug**.

## Action items

- ✅ The code is ready. A runtime test on Linux with SHADOW_AUDIO_DTLS=1 to validate
  handshake passe.
- ⚠️ Note : err=-308 = `SOCKET_ERROR_E` wolfSSL = simple timeout. Si encore
  observed, bump the timeout to 10 s (already 5 s after RE2). Not a missing PSK.

## Refs

- Capture : `06-shadow-recon-linux/captures/MASTER-20260514-153936/tls_plain.log` L27079..L29164
- Code : `05-shadow-client-borealis/demo/src/streaming/ctrl_audio_dtls.c`
- Full documentation: `tools/ida/out/TIER4_RE_2026-05-16.md §B`
- Superseded : [[project-I1-I2-input-audio-skeleton-2026-05-18]] (PSK claim incorrect)
