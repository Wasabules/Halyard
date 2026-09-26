---
name: project-server-push-messages-RE
description: TIER7 T2 ⭐⭐⭐ BREAKTHROUGH — the server pushes a 109 B kRequestFlush every ~200 ms, and the desktop answers a 92 B kFlush in <50 ms. Our Switch has NO ctrl recv loop after the bootstrap = we ignore all of it. The first C75+ actionable finding since ComChan.
metadata:
  type: project
---

# TIER7 T2 — Server-initiated push messages ⭐⭐⭐

> **Mission**: identify the `SSL_READ`s with no preceding `SSL_WRITE` <100 ms
> (= spontaneous server pushes) on the ctrl `:base+11`. Decompile the handler
> desktop. Find the client emissions triggered by a push we do not send.
>
> **🎯 BREAKTHROUGH** : serveur push **109B kRequestFlush** auquel desktop
> answers **systematically** with a **92 B kFlush in under 50 ms**. Our Switch
> has **no ctrl recv loop** after the bootstrap → we never ACK that push.
> The code recipe is ready, ~30 LoC, to be tested live.

## Server push inventory (96 pushes / 235s capture MASTER)

| len | count | cadence p50 | type |
|-----|-------|-------------|------|
| 4 | 48 | 844ms | wire header `00 01 00 NN` (= 2B framing prefix) |
| 147 | 9 | 6299ms | NotifyVideoCommand stats (= TIER6 M1.4) |
| 146/148/144/140 | 18 | varied | NotifyVideoCommand stats variants |
| 139 | 6 | 1404ms | NotifyVideoCommand stats variant |
| **109** | **3** | **197ms** | **🎯 NEW kRequestFlush server push** |
| 343/522/121/125/119/129 | 13 | bootstrap-only | Channel announcement replies |

## 109B kRequestFlush byte-exact

```
00 01 00 6d              ← header type=1 len=109
08 ae 01                 ← f1 = 174 (= seq)
1a 0a                    ← f3 sub(10) = Reply
   32 08                 ← f6 sub(8) = Reply.kRequestFlush ⭐
      0a 02 08 01        ← f1 sub(2) = {f1=1}
      12 00 1a 00        ← f2, f3 empty
22 19 ... (client meta echo)
2a 41 ... (OCapture echo)
```

**Reply oneof case 6** = a new type, undocumented before TIER7.

## Desktop response — 92B kFlush en 38ms

```
00 01 00 58              ← header type=1 len=92
08 af 01                 ← f1 = 175 (= seq+1)
12 02 3a 00              ← Request.f7 sub(0) = empty kFlush
22 41 ... (client meta)
2a 0c ... (OCapture)
```

= byte-exact `ctrl_build_request_flush(buf, len, 175)` already defined in
`ctrl_msgs.c:350-372`. **Everything is ready** except the reactive trigger.

## The Switch's gap: no ctrl recv loop

```bash
grep -n "ctrl_tcp_recv_cleartext" ctrl_session.c
  1539, 1551, 1567, 1591, 1674  # = bootstrap only
```

**ZERO** recv call in the main `ctrl_session_run()` loop. We send
heartbeats/Flush blind. Every server push (= 96 pushes / 235 s) is
ignored.

## Code recipe — ~30 LoC

```c
/* in the run loop, after the heartbeat send */
uint8_t rbuf[4096]; size_t rlen = 0;
while (ctrl_tcp_recv_cleartext_nonblock(tcp, rbuf, sizeof(rbuf), &rlen)) {
    if (rlen < 4) continue;
    uint16_t blen = (rbuf[2]<<8) | rbuf[3];
    const uint8_t *body = rbuf + 4;
    /* find `1a XX 32 YY` = Reply.f3.f6 = kRequestFlush */
    for (size_t i = 0; i + 4 < blen; i++) {
        if (body[i]==0x1a && body[i+2]==0x32) {
            uint8_t fbuf[64];
            int fn = ctrl_build_request_flush(fbuf, sizeof(fbuf), hb_seq++);
            if (fn>0) ctrl_tcp_send_cleartext(tcp, fbuf, (size_t)fn);
            break;
        }
    }
}
```

## Test plan A/B

1. Build `shadow-test-cli --mode=native-stream` baseline (= no auto-ack)
2. Apply patch, build again
3. Run `tools/auto-test.sh` 3× both modes, 60s each
4. Compare NAL stats : parity/data ratio, bottom NAL %, IDR count

**If the parity ratio drops (= the server releases more data chunks) or the IDR rate falls,
the kRequestFlush ACK is the taskbar trigger** ⇒ a ⭐⭐⭐⭐ writeup.

## Refs

- 109B push hex : capture L662711
- 92B response : capture L662719
- Script analyzer : `tools/ida/tier7_server_push.py`
- `ctrl_build_request_flush` existant : `ctrl_msgs.c:350-372`
- Run loop sans recv : `ctrl_session.c:2120-2170`
- Doc : `tools/ida/out/TIER7_RE_2026-05-16.md` §T2

## Cross-refs

- [[project-ctrl-oneof-enum-VERIFIED]] (= mapping field# → oneof case)
- [[project-image-progressive-breakthrough]] (= notre Request_Flush 1Hz)
- [[project-PARITY-RAW-FIX-2026-05-15]] (= 49.7% bottom NAL baseline)
- [[project-all-server-replies-RE]] (= TIER6 M1 push inventory)
