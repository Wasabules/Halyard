---
name: project-window-focus-events-RE
description: TIER 8 U4 — ⭐⭐⭐ ComChan sends a type=1/2/4 triplet per focus change. Type=2 was missing in M4.
metadata:
  type: project
---

# TIER 8 U4 — Window manager focus events RE ⭐⭐⭐

**Date** : 2026-05-23
**Capture** : `06-shadow-recon-linux/captures/MASTER-20260514-153936/tls_plain.log`
**Outil** : `tools/ida/tier8_comchan_events.py`

## TL;DR

**🎯 C85 NEW**: the capture reveals that **ComChan sends a TRIPLET
`type=1 → type=2 → type=4` on every window focus change**. The TIER 6
M4 ComChan RE had missed `type=2`. The V15 recipe must be updated
with the correct triplet, otherwise the server stays in "idle/passive" mode.

## Taxonomie window events binary

Strings `ShadowPCDisplay` (L29486-29504) → **17 SHADOW_WINDOWEVENT_*** :

```
FOCUS_GAINED, FOCUS_LOST           ← focus
FULLSCREEN, WINDOWED, TOGGLE_FULLSCREEN  ← fullscreen mode
MINIMIZED, MAXIMIZED, RESTORED     ← window state
HIDDEN, SHOWN, EXPOSED             ← visibility
LEAVE, ENTER                       ← mouse in/out
CHANGE_SCREEN, MOVED, SIZE_CHANGED ← topology
USER_ASK_TO_CLOSE, REMOVED, NOEVENT
```

App observer methods (`udu_classes.txt`) :
- `NotifyFullscreen(bool)`, `NotifyWindowHidden()`, `NotifyWindowVisible()`
- `NotifyStartingWindow(int)`, `NotifyTopologyChanged()`, `NotifyUserAction(UserAction)`
- ShadowAppToggleFullscreenEvent, ShadowAppGrabKeyboardEvent, ShadowAppGrabMouseEvent

## ComChan capture analysis — a TRIPLET discovered

ssl=0x283868a0 (= ComChan) : **22 WRITE, 0 READ** sur 235s. **7 triplets +
1 init** :

```
L28992 +0.00   type=0  len=0   (= init ComChan)
─────────────────────── 1st focus change ────────────────────
L33147 +0.48   type=1  len=0   ← FOCUS event
L33593 +0.56   type=2  len=0   🎯 WINDOW STATE event (NEW, M4 missed)
L33887 +0.61   type=4  len=56  ← APP NAME "/home/.../run_master_capture.sh"
─────────────────────── 2nd ────────────────────────────────
L662726 +80.61  type=1
L662764 +80.61  type=2  🎯
L662795 +80.62  type=4  len=56  same script
─────────────────────── 3rd (= switch to capturephoto) ─────
L683434 +175.20 type=1
L683456 +175.20 type=2  🎯
L683470 +175.21 type=4  len=12  "capturephoto"
─────────────────────── 4-7 (= same app, redundant) ────────
L691*, L700*, L702*, L1035* — 4 more triplets, "capturephoto"
```

7 triplets in 235 s ⇒ ~1 every 33 s (= a very low rate, real focus changes).

## Triplet meaning hypothesis (C75)

| Type | Body | Interpretation |
|------|------|----------------|
| `type=0` (1×) | empty | INIT ComChan post-handshake |
| `type=1` (7×) | empty | FOCUS_CHANGED (= NotifyFocus / NotifyUserAction) |
| **`type=2`** (7×) | **empty** | **WINDOW STATE event** (= NotifyWindowHidden/Visible/Fullscreen) ← **CRITIQUE** |
| `type=4` (7×) | string | ACTIVE_APP_NAME (= NotifyStartingWindow + _NET_ACTIVE_WINDOW lookup) |

**Why 3 separate messages**: 3 distinct App observers react in a
chain to the same focus change.

## ComChan = uplink-only

22 WRITEs, **0 READs**. The server never pushes on ComChan. It is a signal
of pure user-side presence/activity.

## Our Switch = no ComChan = a silent "idle" signal

```bash
grep "8014\|8015\|base+14\|base+15\|ComChan" demo/src/streaming/
  → 0 match
```

V15 was never implemented (task #60 pending). Without ComChan, the server receives
not the "the user is interactive" signal → suspected of throttling multi-NAL on the
bottom slice (= cause taskbar candidate).

## V15 recipe — refined avec triplet

```c
// ctrl_comchan.c
static void comchan_send_triplet(SSL *ssl, const char *app)
{
    uint8_t t1[10] = { 0,0,0,1, 0,0,0,0, 0,0 };   // FOCUS
    SSL_write(ssl, t1, 10);

    uint8_t t2[10] = { 0,0,0,2, 0,0,0,0, 0,0 };   // 🎯 WINDOW STATE (NEW)
    SSL_write(ssl, t2, 10);

    uint16_t alen = (uint16_t)strlen(app);
    uint8_t t4[10 + 256];
    memset(t4, 0, sizeof(t4));
    t4[3] = 4;
    t4[8] = (alen >> 8) & 0xff;
    t4[9] = alen & 0xff;
    memcpy(t4 + 10, app, alen);
    SSL_write(ssl, t4, 10 + alen);
}

int comchan_start(const char *vm_host, uint16_t port_base) {
    // open TCP+TLS :base+15 (= ComChan ; refuted by BASE15 SFTP confirm — see task #68)
    // Note port mapping conflict :8014 vs :8015 unresolved.
    int fd = tcp_connect(vm_host, port_base + 15);
    SSL *ssl = ssl_wrap(fd, /*no_alpn*/true, /*no_sni*/true);

    // init type=0
    uint8_t t0[10] = {0};
    SSL_write(ssl, t0, 10);

    // initial triplet + periodic
    comchan_send_triplet(ssl, "Shadow2Switch");
    while (!abort) {
        sleep_ms(5000 + jitter);
        comchan_send_triplet(ssl, "Shadow2Switch");
    }
}
```

## Hypothesis C70: type=2 is THE multi-NAL gate

Speculation: type=2 is the "window visible/active" signal without which the
the server treats the client as "minimized/hidden" → it suppresses bottom-slice
encoding (= the taskbar zone). If V15 sends ONLY type=1+type=4 (= the original M4
recipe), the server stays in conservative mode. With type=2 → it releases multi-NAL.

À tester live.

## Refs

- Script : `tools/ida/tier8_comchan_events.py`
- Capture triplet L33147/L33593/L33887 + 6 autres
- Strings SHADOW_WINDOWEVENT_* : `06-shadow-recon-linux/dumps/strings_display.txt:29486-29504`
- App observers : `06-shadow-recon-linux/dumps/udu_classes.txt:19,33,39,46,49`
- TIER 6 M4 baseline : `tools/ida/out/TIER6_RE_2026-05-16.md` §M4
- BASE15 SFTP : `tools/ida/out/BASE15_SILENT_CHANNEL_RE.md` (port conflict task #68)
- Doc : `tools/ida/out/TIER8_RE_2026-05-16.md` §U4

## Status

**C85 actionnable**, code recipe ready. Next : impl ctrl_comchan.c avec
triplet correct + auto-test runtime measure bottom NAL ratio delta.
