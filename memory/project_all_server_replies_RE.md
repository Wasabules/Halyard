---
name: project-all-server-replies-RE
description: TIER6-M1 — An exhaustive RE of 22 ctrl `:base+11` SSL_READs. Decoded the 137 B Channel-5 reply (= bitrate+fps+per-channel-id 124720); the 522 B Channel-12 one embeds an OpenSSH ED25519 private key; the periodic 139 B one = a NotifyVideoCommand runtime stats push. No missing field gating multi-NAL.
metadata:
  type: project
---

# Project — All server replies parsing exhaustif (TIER6-M1)

## Verdict

**C95**: no reply field gates multi-NAL on the server side. Our existing
parser correctly ignores the content of the non-critical replies. **3
findings KB-enrichments** :

1. Channel-5 reply 137B (post-seq=5) contient `f1.f2 = 124720` = **per-channel
   session identifier** (= unique for the video, +160 for the cursor=124880, +406
   pour FileTransfer=125126).
2. The 522 B Channel-12 reply (post-seq=12 FileTransfer) **embeds a private
   ED25519 SSH key** (= 391 B of `-----BEGIN OPENSSH PRIVATE KEY-----...`). That is
   the SFTP auth mechanism for file transfers to the Shadow VM.
3. Periodic 139B server push contient **stats runtime** (= timestamp_us,
   bandwidth, RTT, queue depth, total bytes). Our client extracts none of it.

## Inventaire 22 replies

| Line | seq | Len | Type |
|------|-----|-----|------|
| L18571 | — | 106B | Capabilities reply (version 6.1.7) |
| L18632 | — | 195B | Auth reply (hash 20B) |
| L19397 | — | 140B | Encryption reply (key 32B + nonce 12B) |
| L20664 | 3 | 343B | DisplayConfig reply (14 supported modes 1280×720 → 1920×1080 @ 144Hz) |
| L20734 | 4 | 112B | Heartbeat echo |
| **L22991** | **5** | **137B** | **Channel-5 Video reply** : bitrate=20Mbps, fps=143.85, codec=H264, profile=1, identifier=124720, port=6978 |
| L23020 | — | 36B | Async push NotifyVideoCommand bitrate cap |
| L24071 | 6 | 121B | Channel-6 Cursor reply (id=124880, port=6988) |
| L25053..L26913 | 7-11 | 119-129B | Input/Audio/Controller/Clipboard/Micro replies (per-channel ids + ports) |
| **L26924** | **12** | **522B** | **Channel-12 FileTransfer reply** = SSH ED25519 private key 391B + id=125126 |
| L26961 | 13 | 124B | Time sync reply (f6=29970, f7=600 = clock/drift) |
| L33093 | 14 | 100B | Flush echo (kFlush ack seq=14) |
| L33230 | — | 108B | NotifyVideoCommand push |
| L34543+ | 18+ | 139B × N | **Periodic runtime stats push** (= timing + network + queue) |

## Code recipe — optional FileTransfer parser

```c
// Extract embedded SSH key from channel-12 reply
int parse_filetransfer_reply(const uint8_t *buf, size_t len, char *ssh_key, size_t key_cap);
```

Not critical for the taskbar bug, but if we want SFTP to the VM (= GUI drag & drop),
ce serait l'entry-point.

## Aucun trigger taskbar

Refs : `tools/ida/out/TIER6_RE_2026-05-16.md §M1`.
