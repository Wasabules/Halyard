---
name: project-evp-hook-definitive-finding
description: An LD_PRELOAD hook on the desktop client's EVP_DecryptUpdate proves its crypto path is IDENTICAL to ours - and that parity chunks are never decrypted at all.
metadata:
  node_type: memory
  type: project
---

# The EVP hook - what the desktop client actually decrypts

DECISIVE 2026-05-23. Hooking chacha20-poly1305 on the official desktop client
showed its decrypt path is **identical to ours**. Whatever made its picture
clean, it was not the crypto.

## What ~60 s of a desktop session looked like

- **3539 decrypt calls** in total
  - **2003 at 1241 bytes** - video data chunks, exactly our size
  - 709 at 5 bytes - small control and header messages
  - 281 at 8 bytes
  - the rest: Opus audio frames (~141 B), TLS records
- **2978 finalisations, all OK** - 100 % of tags verified
- **zero calls at 1269 bytes**, which is the parity-chunk size. The official
  client **never decrypts them** - the finding that mattered.
- **ten distinct cipher contexts**, one per channel

## The pattern, per message

```
set tag (16 B)
decrypt update  inl = N, outl = N     <- one call, in place
finalise        rc = 1, OK
```

For video, N is always 1241. For audio, 141 or 147 - Opus frames.

## A decrypted video chunk

```
02 b2 b6 44 25 01 11 0a 00 80 07 38 04 00 00 10   Shadow VideoFrame header
43 01 00 00 00 00 01 67 64 00 34 ac 2b 40 3c 01   43 01, then the SPS NAL (7)
13 f2 e0 22 00 00 07 d0 00 08 ca 01 1e 38 55 40   SPS continues
00 00 00 01 68 ee 3c b0                           PPS NAL (8)
00 00 00 01 65 b8 04 5f ...                       IDR slice NAL (5)
```

## Why this mattered

It removed crypto from the list of suspects for the picture bug, and the "zero
calls at the parity size" line pointed straight at what F31 later established:
those chunks are **not encrypted parity**, they are plaintext continuations. The
real defect was elsewhere and was an off-by-one
([[project_image_bug_SOLVED_offbyone_G4]]).

Related: [[project_F30_fec_brute_force_EXHAUSTED]] - superseded by F31.
