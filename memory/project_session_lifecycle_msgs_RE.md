---
name: project-session-lifecycle-msgs-RE
description: No lifecycle message is missing — the desktop sends only 7 oneof cases (kEncrypt/kAuthenticate/kRegister/kDisplayConfig/kFlush/kNotifyRes/kNotifyVideoCmd) + a 2 Hz heartbeat
metadata:
  type: project
---

# Session lifecycle messages — TIER4 axe C

**Date** : 2026-05-23. **Confidence** : C90.

## TL;DR

The complete inventory of the MASTER capture (ssl=0x2a3d75c0 = the ctrl `:base+11`)
shows that **the desktop sends no lifecycle message beyond what we
already send**. The cases `kReady`, `kPlaybackStarted`, `kFrameAck`,
`kDisplayReady`, `kSessionReady` and `kUpdateSession` of the Request oneof are
**all unused** on the desktop client. Our V14 stack (Capabilities,
Auth, Encryption, DisplayConfig+EDID, 8× ChannelAnnouncement, a 2 Hz Heartbeat,
an IFR every 14 frames, Flush at 1 Hz, NotifyResolution, NotifyVideoCommand) is complete.

## An inventory of the MASTER capture's ctrl messages (47 SSL_WRITE/READs analysed)

### Bootstrap initial (seq=1..12) :
- seq=1 Capabilities (91B)
- seq=1 Auth (249B) → reply 195B
- seq=2 Encryption (131B) → reply 140B
- seq=3 DisplayConfig + EDID (247B) → reply 343B = 12 video modes
- seq=4 Heartbeat (93B)
- seq=5..12 ChannelAnnouncements (119/103/107/99/101/107/103B) → 8 replies
  (112B/137B/36B/121B/129B/125B/119B/124B)

### Steady state (post-bootstrap) :
- **93 B Heartbeat** every ~500 ms (= 2 Hz) on ctrl
- **a 99 B periodic state** every ~1 s
- Aucun kReady / kPlaybackStarted / kFrameAck

## The oneof cases observed against the total (26 enum cases)

| Field# | Case | Sent by desktop ? | Notre code envoie ? |
|--------|------|-------------------|---------------------|
| f3 | kEncrypt | OUI seq=2 + heartbeats | OUI |
| f4 | kAuthenticate | OUI seq=1 | OUI |
| f6 | kDisplayReady | NO | YES (= perhaps superfluous, worth testing) |
| f7 | kFlush | YES (already tested in N40) | YES |
| f8 | kRegisterSession | OUI seq=5..12 + DisplayConfig | OUI |
| f10 | kDisplayConfig | OUI seq=3 | OUI |
| f14 | kNotifyResolution | YES (to be confirmed in steady state) | YES |
| f15 | kNotifyVideoCommand | OUI (Flush kFlushVideo) | OUI |
| f5, f9, f11-f13, f16-f27 | NONE | NON | NON |

**See [[project-ctrl-oneof-unused-RE]]** for the full confirmed enum mapping
via IDA RTTI.

## UDP heartbeats per channel

| Channel | Heartbeat | Notre code | Status |
|---------|-----------|------------|--------|
| `:base+10` video | `0x70` 1B + `0x50 0x49 0x01 ...` IFR every 14 frames | OUI (V12) | ✅ OK |
| `:base+12` audio | DTLS-encrypted, undecodable | a DTLS handshake + RX (I2) | ✅ The code is ready |
| `:base+13` input | a 1 B `0x70` post-connect | TBD (to be checked) | ⚠️ |
| `:base+30` cursor | a periodic 1 B `0x70` (observed at L28992+) | TBD | ⚠️ |

**Action item C50** : ajouter cursor `0x70` 1B heartbeat tick @ 1s pour
be symmetric with the desktop. **No strong confidence that it is related to the
taskbar**, but it is protocol hygiene.

## State machine messages — verdict

**No missing lifecycle message that could gate the server's emission**. The
desktop sends no explicit "I'm ready for high bitrate" signal. The
server upgrades the bitrate on **some other criterion** (= probably real input
detected, or the cumulative `gE` bandwidth feedback already emitted by V14).

## Refs

- Capture : `06-shadow-recon-linux/captures/MASTER-20260514-153936/tls_plain.log` L18534..L33093
- Full documentation: `tools/ida/out/TIER4_RE_2026-05-16.md §C`
- Cross-ref : [[project-ctrl-oneof-unused-RE]], [[project-V12-taskbar-status-2026-05-15]]

## Action items

- ⚠️ Optional : ajouter cursor 1B `0x70` heartbeat (C50, low risk)
- ⚠️ Check whether we send kDisplayReady (= our code appears to, but
  the desktop does NOT — a possible source of server-side desynchronisation)
- ❌ No new message to implement
