---
name: project-vst-connect-msg-byte-exact
description: "TIER1 2026-05-16 — A byte-exact RE of the `:base+20` connect_msg. The header [0x41 01 00 LL HH] is confirmed at C95. The body = a vector<uint8_t> returned by Output->vtable[+144], probably = the streamingtoken (C70). The 2 small 23 B post-connect writes = a 1-byte 0x70 heartbeat sent by sub_D67430."
metadata:
  type: project
---

# VST connect_msg byte-exact RE — TIER1 2026-05-16

## TL;DR for whoever codes variant 8

```
# Wire bytes :
[0x41 0x01 0x00 LL HH] [body ~100 B]
                          ^^
                          = the streamingtoken (a C70 hypothesis)

# Then 2× an SSL_write of a single byte 0x70 (= 'p'), ~50 ms apart
```

## Header (C95 byte-exact)

Reconfirmed through Hex-Rays on sub_D733E0 + sub_D67080 + the rodata qword_12A41C8.

```
0x12A41C8 :  01 00 43 41 70 00 00 00      ; = 0x0000007041430001
0x12A41D0 :  E8 03 00 00 00 00 00 00      ; = 0x3E8 (timeout 1000ms)
```

- Low 32 bits = `0x41430001` → `hdr_int`
  - `HIBYTE(hdr_int) = 0x41 ('A')` → byte0 wire
  - `LOWORD(hdr_int) = 0x0001` LE → bytes 1..2 wire = `01 00` (subtype=1)
- The high 32 bits = `0x00000070` → stored at channel+17044, used by the heartbeat (see §3)
- qword_12A41C8 is reused by **every channel ctor** (sub_C13350/C13650/C1CC10/C1CF00/C24810/C24CA0). An identical header for Video plain/SSL + Cursor SSL + Audio SSL + Input SSL.

## Body source path (C85 verified)

```
(decompiled excerpt omitted from the public edition - the finding it supported is stated in the text)
```

An independent confirmation through **sub_C24810** (a sister channel's ctor, e.g. Cursor):
```c
strcpy(v51, "Video");                  // 5-byte stream name std::string
v50[0] = v51; v50[1] = (void *)5;
v28 = (*v27)->vtable[+48];
v28(&v49, v27, v50);                   // virtual: stream_name → body_vector
sub_C476C0(..., qword_12A41C8, ..., &v49, ...);   // = body
```
It confirms that the body is **a session token tied to the stream's type**.

## Body content candidates (C70 / C60)

| # | Hypothesis | Confidence | Test plan |
|---|---|---|---|
| 1 | streamingtoken (= JWT-like, retour `/proximus-credentials`) | **C70** | variant 8a : send raw token, ~80-130 B |
| 2 | sessionUniqueId (36 B UUID ASCII) | C60 | variant 8c |
| 3 | sessionUniqueId + auth_hash (20 B) | C60 | variant 8d : 56-byte concat |
| 4 | The full main_jwt padded to 100 B | C50 | variant 8e |
| 5 | A server-only opaque blob (= not reconstructible client-side) | C40 | impossible without a capture |

If all of them fail → patch LD_PRELOAD to hook the desktop's `SSL_write`/`SSL_read` (= 30 min) and capture the exact plaintext. Promotion to C95.

## Les 2× 23B writes post-connect (C85)

Empirically (`captures/MASTER-20260514-153936/tls_plain.log` lines 32796 and 33142): 2 socket writes of 23 B each, at t=+0.554 s and +0.612 s after connect. The 23 B sizing = 1-2 B of plaintext + 22 B of TLS record overhead.

Source: `sub_D67430` (= "send_msg variant 1") called from the VST vtable+192 trampoline:

```c
(decompiled excerpt omitted from the public edition - the finding it supported is stated in the text)
```

`vtable[+32]` on the VST vtable @ 0x12A6BB0 = `sub_DE2820` = an **SSL_write thunk** (2 callers: sub_AE9420, sub_AE9490). The byte written = `*(channel+17044) = 0x70` (= 'p').

→ **Chaque 23B socket write post-connect = 1× SSL_write(ssl, &(uint8_t){0x70}, 1)**.

## Variant 8 code (ctrl_video_tcp.c)

```c
case 8: {                                            // streamingtoken raw + heartbeat
    extern const char *g_streaming_token;
    size_t tok_len = g_streaming_token ? strlen(g_streaming_token) : 0;
    if (tok_len == 0 || tok_len > UINT16_MAX) return -ESRCH;
    uint8_t hdr[5] = { 0x41, 0x01, 0x00,
                       (uint8_t)(tok_len & 0xFF),
                       (uint8_t)((tok_len >> 8) & 0xFF) };
    wolfSSL_write(c->ssl, hdr, 5);
    wolfSSL_write(c->ssl, g_streaming_token, (int)tok_len);
    // A 1-byte 0x70 heartbeat ×2, ~50 ms apart (mimicking the sub_D67430 cycle)
    uint8_t hb = 0x70;
    wolfSSL_write(c->ssl, &hb, 1);
    usleep(50 * 1000);
    wolfSSL_write(c->ssl, &hb, 1);
    break;
}
```

If the server answers with frames → a win. If it stays idle for 30 s → try 8c/8d, then pivot to an LD_PRELOAD plaintext capture.

## Refs

- [[project-video-ssl-tcp-channel-found]] — the channel's wire format, discovered
- [[project-V12-taskbar-status-2026-05-15]] — the picture state, taskbar residue
- `tools/ida/out/TIER1_RE_2026-05-16.md` §A — the full documentation
- `tools/ida/out/tier1/caller_795F40_sub_7C4B20.c` — Hex-Rays InitVideoManager
- `tools/ida/out/tier1/sub_C24810.c` — sister ctor preuve body=stream-token
- `tools/ida/out/tier1/pc_sub_D67430.c` + `pc_sub_D6CC10.c` — heartbeat source
