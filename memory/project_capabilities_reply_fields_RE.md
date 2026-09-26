---
name: project-capabilities-reply-fields-RE
description: The Capabilities reply = just version 6.1.7, but the 137 B channel reply carries the negotiated port/bitrate/fps, unparsed
metadata:
  type: project
---

# Capabilities reply parsing — TIER4 axe D

**Date** : 2026-05-23. **Confidence** : C80.

## TL;DR

The server's `Capabilities` reply (106 B) contains **no feature flag, no
session_token, no codec profile**. It carries just the server version
(= 6.1.7). Our `ctrl_parse_capabilities_reply`, which extracts only the version
est suffisant.

**On the other hand**, the **ChannelInfo replies** (137 B after seq=5, etc.) contain
information **not parsed by our client**: the negotiated port, a bitrate echo,
an fps echo, and an unknown identifier of 124720 (= probably a per-channel nonce_extra).

## Byte-decode Capabilities reply 106B (L18571)

```
1a 0a                       ← f3 (sub, len=10) = Reply.kCapabilities
   1a 08                    ← f3.f3 (sub, len=8)
      0a 06                 ← f3.f3.f1 (sub, len=6) = Capabilities_Version
         08 06              ← f1=6      (major)
         10 01              ← f2=1      (minor)
         18 07              ← f3=7      (patch)
                            → server version 6.1.7
22 19 ...                   ← f4 = client_meta echo {f1=1, f2="6.1.7", f3="ShadowStreamer"}
2a 41 ...                   ← f5 = OCapture echo
```

No other field. The Capabilities reply = **version only**.

## Byte-decode Video channel reply 137B (L22991, seq=5)

```
08 05                       ← f1=5 (echo seq)
1a 27                       ← f3 = Reply.kRegisterSession sub (len=39)
   42 25                    ← f8 = ChannelInfo Reply (len=37)
      0a 23                 ← f1 = inner Video reply (len=35)
         0a 0f              ← f1 sub (len=15) = codec_config wrap
            08 01           ← f1=1
            10 b0 a8 07     ← f2=124720      ← UNKNOWN (nonce_extra? stream_id?)
            22 07           ← f4 sub (len=7) = inner codec
               10 02        ← f2=2          (H.264)
               20 01        ← f4=1          (profile)
               28 e2 36     ← f5=6978       ← PROBABLY ALLOCATED PORT or stream_id
         12 0b              ← f2 sub (len=11) = resolution_fps echo
            08 80 0f        ← f1=1920
            10 b8 08        ← f2=1080
            1d a0 da 0f 43  ← f3 fixed32 = 143.85 (negotiated fps)
            38 80 da c4 09  ← f7 varint = 19999232 (~20Mbps echo)
```

## Unparsed fields that could still be useful

| Field | Value | Hypothesis | Action |
|-------|-------|-----------|--------|
| `f4.f5 = 6978` | 14050 (?) | Port allocated by server OR stream-id | C60 : parser pour bind socket si != base+10 |
| `f1.f2 = 124720` | 0x01E8B0 | A per-channel nonce_extra OR a session_id | C40: possibly useful for chacha20? To investigate |
| `f2.f3 = 143.85` | an fps echo | The negotiated FPS | C70: check that it matches our request |
| `f2.f7 = 19999232` | ~20Mbps | Negotiated max_bitrate | C70 : valider |

**INSIGHT C40**: if `f1.f2=124720` is a nonce_extra **different** from the one
sent in the Encryption reply (already parsed by `ctrl_parse_encryption_reply`),
perhaps **decryption fails silently** for some chunks and
that is what causes the taskbar artefacts? **To be tested**: extract that field
and use it as a nonce_extra fallback.

## Cross-references — quels fields server re-utilise ?

| Field reply | Re-used by client ? |
|-------------|---------------------|
| Capabilities version 6.1.7 | NON (juste log) |
| ChannelReply f4.f5 = 6978 | NON |
| ChannelReply f2.f7 bitrate | NON (on hardcode dans subsequent sends) |
| ChannelReply f1.f2 = 124720 | NON (potentially nonce gap) |
| Auth reply hash 20B | OUI (UDP register packet) |
| Encryption reply key 32B | OUI (chacha20 decrypt) |

## Action items

- ⚠️ C60: implement a parser for the channel-5 reply's `f4.f5`, validate it against
  port_base+10. Log warning si mismatch.
- ⚠️ C40, speculative: test `f1.f2 = 124720` as an alternative nonce_extra
  pour chacha20 si current decrypt fails. Probably no effect.
- ❌ No obvious taskbar fix in this layer.

## Refs

- Capture : `06-shadow-recon-linux/captures/MASTER-20260514-153936/tls_plain.log`
  L18571 (Capabilities reply 106B), L22991 (Channel-5 Video reply 137B)
- Code parser actuel : `05-shadow-client-borealis/demo/src/streaming/ctrl_msgs.c` (ctrl_parse_*)
- Full documentation: `tools/ida/out/TIER4_RE_2026-05-16.md §D`
