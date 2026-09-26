---
name: project-cursor-user-activity-RE
description: ⚠️ PARTIALLY REFUTED 2026-05-16 — ComChan class + wire format C95 confirmed, but port = `:base+14` NOT `:base+15` (previous agent mis-mapped ssl=0x283868a0 → fd=256 by temporal proximity ; byte-exact TLS record size proves fd=322/:8014). ROI/multi-NAL trigger hypothesis REFUTED — ComChan is lifecycle/keepalive bus (NotifyUserAction = idle-timer reset, not video encoder gate). Net C70 — channel real, port corrected, taskbar bug NOT solved by ComChan impl.
metadata:
  type: project
---

# Project — Cursor / User activity broadcasting RE (TIER6-M4) ⭐⭐

## ⚠️ REFUTED by cross-validation 2026-05-16 — RECONFIRMED CORRECTED 2026-05-23

**See `tools/ida/out/M4_CROSSCHECK_2026-05-16.md` for the full counter-analysis.**

**2026-05-23 independent byte-exact verification** : `tools/ida/out/PORT_8014_8015_RESOLUTION.md`
confirms **:base+14 = ComChan (C95)** and **:base+15 = SftpClientChannel SSH+SFTPv3 (C95)**.
M4 original port assignment was wrong ; TIER 7 (claiming `:base+15 = ComChan`) was also wrong
on this point. M4_CROSSCHECK + BASE15 stand.

Original claim broken into pieces :

| Claim | Status | Evidence |
|-------|--------|----------|
| Class name "ComChan" exists | ✅ C95 confirmed | `strings ShadowPCDisplay` → `ComChanInterface`, `ComChanManager`, `HeartbeatComChanMessage`, `InitComChan`, `ShadowComChanStartedEvent` |
| Wire format `[u32 type][u32 sub][u16 len][string]` BE | ✅ C95 confirmed | byte-exact L28992 / L33887 / L683470 |
| Port = `:base+15` | ❌ REFUTED | ssl=0x283868a0 → fd=322 → `:base+14` per TLS record size (22B plaintext + 22B overhead = 44B ciphertext = fd=322 TCP_WRITE len). `:base+15` IS opened but silent after handshake. |
| Server uses ComChan to gate multi-NAL ROI encoding | ❌ REFUTED | Zero "ROI"/"region_of_interest"/"active_zone" strings in binary. No xref between ComChan and video encoder symbols. ComChan symbols (`NotifyUserAction`, `NotifyWindowHidden/Visible`, `GrabKeyboard/Mouse`) point to lifecycle/keepalive bus, NOT ROI encoding. |
| Implementing ComChan will fix bottom NAL ratio | ❌ REFUTED | Most-plausible function = VM auto-suspend keepalive ; will not change per-frame H.264 slicing. |

**Net confidence M4 → C70** (channel exists, format right, port re-mapped, hypothesis demoted).

The taskbar/bottom-NAL bug investigation should **NOT** prioritize ComChan
implementation. Pursue server-side H.264 picture-segment splitting RE
(cross-ref `H1_*` dumps).

---

## ORIGINAL CONTENT (preserved for context — note port WAS WRONG)

## Verdict

🎯 **C80 BREAKTHROUGH** — premier candidat actionnable depuis TIER1 :
**`:base+15` ComChan = a bidirectional TCP+TLS channel** that **our client
the Switch NEVER opens**. The desktop uses it to broadcast
the user's activity to the server. Hypothesis C70 = that activity signal
gates the server-side decision whether to emit multi-NAL (= the taskbar bug).

## Capture plaintext

```
CONNECT fd=256 peer=:8015 (L27384)
SSL handshake (ssl=0x283868a0)

SSL_WRITE 10B (L28992) :  00 00 00 00 00 00 00 00 00 00       ← init type=0
SSL_WRITE 10B (L33147) :  00 00 00 01 00 00 00 00 00 00       ← focus type=1
SSL_WRITE 66B (L33887) :  00 00 00 04 00 00 00 00 00 38       ← type=4 + 56B path
                           "$REPO/tools/run_master_capture.sh"
SSL_WRITE 10B (L683434) : 00 00 00 01 00 00 00 00 00 00       ← focus type=1 (later)
SSL_WRITE 22B (L683470) : 00 00 00 04 00 00 00 00 00 0c       ← type=4 + 12B
                           "capturephoto"
```

## Wire format C95

```
[u32_be type][u32_be sub=0][u16_be string_len][string utf8]
  4 B          4 B           2 B                string_len B
```

Types observed:
- `type=0` (10B all zero) : init / register session
- `type=1` (10B) : focus event (window gained/lost focus)
- `type=4` (variable) : active app name / process path

## RTTI = ComChan

`udu_classes.txt` lignes 4, 12, 13 :
- `App11InitComChanE...` + `ComChanInterface` + `ShadowComChanStartedEvent`
- `App20NotifyStartingWindowE`
- `App16NotifyUserActionE`
- `App18NotifyWindowHiddenE` / `App19NotifyWindowVisibleE`
- `ShadowAppGrabMouseEvent` / `ShadowAppGrabKeyboardEvent`
- `App16NotifyAppStoppedE` + `QuitReason`

⇒ **ComChan = Command Channel bidirectionnel** pour user activity / app
lifecycle. Our Switch has no `streaming/ctrl_comchan.{c,h}` file at all.

## Hypothesis C70 — the taskbar trigger

The Shadow server is designed for active gaming:
1. Quand ComChan signale "user is interactive" (= focus + active app)
   → server priorise bottom slice + multi-NAL emission ON
2. Sans ComChan signal → mode encoding conservateur (= top slice only,
   a striped bottom)
3. Consistent with the bottom NAL ratio of 0.81% on the Switch against 11% on the desktop (= 13×
   fewer signals ↔ 13× less bottom NAL)

## Code recipe Switch

`05-shadow-client-borealis/demo/src/streaming/ctrl_comchan.c` :

```c
int comchan_start(const char *vm_host, uint16_t port_base) {
    int fd = tcp_connect(vm_host, port_base + 15);
    SSL *ssl = ssl_wrap(fd, /*no_alpn=*/true, /*no_sni=*/true);

    // 10B init
    uint8_t init[10] = {0};
    SSL_write(ssl, init, 10);

    pthread_create(&g_comchan_th, NULL, comchan_thread, ssl);
    return 0;
}

static void *comchan_thread(void *arg) {
    SSL *ssl = arg;
    // initial focus
    uint8_t focus[10] = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    SSL_write(ssl, focus, 10);
    // app-name
    const char *app = "Shadow2Switch";
    uint16_t len = strlen(app);
    uint8_t msg[64] = {0};
    msg[3] = 4; msg[8] = len >> 8; msg[9] = len & 0xff;
    memcpy(msg + 10, app, len);
    SSL_write(ssl, msg, 10 + len);
    // periodic focus heartbeat @ ~7s
    while (!atomic_load(&g_comchan_abort)) {
        sleep(7);
        SSL_write(ssl, focus, 10);
    }
    return NULL;
}
```

## Test runtime A/B

1. impl + build linux + run native-stream
2. Measure the bottom NAL ratio over 60 s with the ComChan ON against OFF
3. Si bottom_ratio passe de 49.7% → >55% : ComChan trigger CONFIRMED
4. Si aucun changement : bug = libavcodec ceiling (= NVDEC Switch path)

## Refs

- `tools/ida/out/TIER6_RE_2026-05-16.md §M4`
- Capture plaintext L27384, L28992, L33147, L33887, L683470
- `06-shadow-recon-linux/dumps/udu_classes.txt:4,12,13,49`
- `tools/ida/out/TIER3_RE_2026-05-16.md` (= `+15` unidentified)
