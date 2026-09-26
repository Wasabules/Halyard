---
name: reference-captures-kb
description: The Captures Knowledge Base — ALWAYS consult it BEFORE capturing / RE'ing / re-decrypting. It avoids duplicate work.
metadata: 
  node_type: memory
  type: reference
---

# The Captures KB — the single entry point

**Fichier principal** : `$REPO/CAPTURES_KB.md`

**Outil query** : `$REPO/tools/query_captures.sh`

## The mandatory workflow before any capture or RE

```bash
# 1. A general listing
bash $REPO/tools/query_captures.sh

# 2. Search a specific topic
bash $REPO/tools/query_captures.sh <topic>
bash $REPO/tools/query_captures.sh wire <field>
bash $REPO/tools/query_captures.sh udp_video :11010
bash $REPO/tools/query_captures.sh ctrl_messages
```

## Key datasets available (= already captured)

| Data | Path | Content |
|---|---|---|
| **TLS plain hook desktop** | `/tmp/shadow_tls.log` (60MB) | Bootstrap byte-exact + UDP/TCP traffic plein |
| **30 pcaps Shadow** | `06-shadow-recon-linux/captures/*` + `captures/` | Chrome WebRTC + Linux desktop + sslkeys |
| **47 Ghidra dumps** | `06-shadow-recon-linux/dumps/` | Decompilations of the key functions |
| **Our UDP video chunks** | `/tmp/shadow_chunks.bin` | 3000 raw chunks (= data + parity) |
| **Our decoded NALs** | `/tmp/shadow_dump.bin` | 928 decrypted access units |
| **The compiled hook** | `06-shadow-recon-linux/tls_hook/shadow_tls_hook.so` | `LD_PRELOAD=`-ready |
| **Launcher hook** | `tls_hook/run_shadow_with_hook.sh` | `bash` directement |

## Key findings extracted (= available without re-REing)

- **The desktop session's chacha20 KEY**: `<session-key-removed>` <!-- The value used to live here in cleartext. It is a SESSION chacha20 key, taken from a capture of May 2026: the session died long ago and the captures it decrypts are gitignored, so it opens nothing for anyone who clones this repo. It still had no business being in a public repo. Removed on 2026-09-03; the value remains in the local captures, next to the hook log it came from. -->
- **Wire formats** : Capabilities/Auth/Encryption/RegisterSession/ChannelInfo/EncodingConfig/Flush/NotifyResolution/NotifyVideoCommand byte-exact dans `ctrl_msgs.c` + dumps
- **Request oneof** : case 1=Notify, 2=Ping, 4=Auth, 5=Restart, 6=Hid, **7=Flush**, 8=RegisterSession, 9=UnregSession, 10=DisplayConfig, 11=Update, 12=Encryption, 13=UpdateSession, **14=NotifyResolution**, **15=NotifyVideoCommand**
- **UpdateSession_Video fields** : f1=bool auto_framerate, f2=stream_id, f3=fps, f4=bitrate_bps, f5=redundancy
- **The desktop receives 2 NALs per packet**: top first_mb=0 + bottom first_mb=4080 in the SAME ciphertext (= confirmed by decrypting the pcap)
- **Ports** : +10 UDP video / +11 TCP ctrl / +12 UDP audio / +13 UDP input / +20 TCP STFP fallback / +30 UDP cursor

## Quand re-capturer ?

❌ DO NOT re-capture if:
- The function's wire format is already in a dump
- The session's EncryptionReply is already saved (the KEY is valid as long as we stay in that session)
- The byte-exact bootstrap is already documented

✅ Re-capturer SI :
- A new hypothesis needs a runtime observation that the static dumps do not give
- A different account/VM (= a JWT/tier change)
- A specific user activity (= movement at the bottom of the screen, etc.)
- A live Frida hook is needed for the dynamic state machine

## How to apply

Before ANY RE question:
1. `bash $REPO/tools/query_captures.sh <topic>`
2. If an answer exists → cite it
3. Otherwise → launch a Ghidra script or a new capture, and **ADD the findings to CAPTURES_KB.md**
