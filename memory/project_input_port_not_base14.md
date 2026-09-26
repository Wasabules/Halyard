---
name: project-input-port-not-base14
description: "🏆 RESOLVED 2026-08-21 — INPUT WORKING (movements, clicks, drags, keyboard). The chain: DTLS :base+12 + Connect @83=0x01 + @122 + a per-device counter at @96 + the left-button release."
metadata:
  type: project
---

**RESOLVED**: the input channel is a **DTLS over UDP `:base+12`** session, not TCP
on `:base+14`. So it was neither a content problem nor a simple port problem: it
was the **wrong transport**.

Direct proof (capture `captures_inputport_20260821_135207/`, with the hook
extended to writev/readv) — a contiguous sequence:

```
UDP_RECVMSG sockfd=366 peer=:14012 len=141
BIO_WRITE   bio=0x75cf24007760      len=141
BIO_READ    bio=0x75cf24007760      len=141
SSL_READ    ssl=0x33a60350          len=104   <- the input channel's reply
```

The SSL object `0x33a60350` carries 1833 movements of 144 B, 19 clicks of 152 B, 3
Connects of 96 B and 1212 replies of 104 B. Corollary: **`:base+12` is not the
audio**, which explains our audio channel's permanent `idle Ns (no audio
packets)`.

Corrected map: `+11` TCP ctrl, `+12` **UDP DTLS = INPUT**, `+14` TCP = ComChan
alone, `+20` TCP video.

**The second key — byte `@83` of the 96 B Connect.** With `@83=0x02` (what F2
forced) the server stays silent; with `0x01` it answers. A controlled A/B, the
same movements in both branches. Default `SHADOW_INPUT_B83=1`.

**THE INPUT WORKS** — movements, clicks, drags and keyboard are applied inside the
VM. Visual proofs (an H.264 dump of the remote screen): a drag-select from
(400,400) to (1400,750) highlights exactly that diagonal in Firefox, and **F11
switches Firefox to full screen** then back out on the next shot (reproduced 3
times).

**The full chain, every link necessary**:
1. DTLS/UDP transport on `:base+12` (not TCP on `:base+14`)
2. `@83 = 0x01` in the 96 B Connect — with `0x02` the server stays silent
3. `@122` = 1 press / 0 release
4. `@96` = a **per-device** counter (the keyboard restarts at 1)
5. **The left-button release is a 144 B MOVEMENT message with `@127 = 2`** (not a
   click message — the left button emits only one 152 B message per cycle).
   Without it the button latches, the VM stays in a permanent drag and its desktop
   freezes, which masked everything else. Proofs: the temporal split of holds
   (152 B on press, 144 B on release), a 10/10 correlation with real releases, and
   a functional A/B on a cookie banner.

**Measurement lesson**: aiming at a small button is a bad test (it fails for
reasons of focus/precision and makes you wrongly conclude that nothing gets
through); drag-select and F11 are unambiguous. And a taskbar icon being
highlighted = the *active* application under Windows 11, not a hover.

**Tooling**: `SHADOW_DUMP_H264=1` + `ffmpeg -update 1` to see the remote screen; a
click harness running in a closed loop on the position the server sends back (the
VM keeps the cursor's position between sessions).

The 42 B encrypted packet on `:base+13` has been understood and correctly emitted
since — see [[project-shadow-dual-key-crypto]].

**Key tooling**: `SHADOW_DUMP_H264=1` + `ffmpeg -update 1` gives a picture of the
remote screen — you validate an input by its real effect, not by a proxy.

The history of the reasoning that led there:

The chain of evidence (LD_PRELOAD capture 2026-08-21, VM base=14000):

1. The desktop has two application-level TLS objects on what we believed was a
   single channel: `0x3df46930` = the **ComChan** (4 writes, framing
   `[u32_be type][u32_be 0][u16_be len]`, id 4 = the title of the focused window
   on the client side) and `0x3df25730` = the **input channel** (a 96 B Connect,
   152 B clicks, 144 B keyboard, ~15 server replies of 104 B per second).
2. `fd=317` (`:14014`) carries exactly the ComChan's encrypted lengths
   (112, 32, 32, 61) → **the ComChan occupies `:base+14`**.
3. The input channel's ciphertext (133, 189, 181) is on **no** socket.
4. A live TLS scan: `:base+14` accepts only **one TLS session per Shadow
   session**. The ComChan and the input cannot coexist there — so the input is
   elsewhere.

What is **excluded** as a cause: our Connect/click/keyboard frames are identical
to the byte with the desktop's outside the dynamic fields (the counter at @40-41,
the timestamp at @48-51, the coordinates at @128-130), verified against 4
independent sessions. The `CI_BODY_7` announcement is identical too. The bootstrap
is now byte-exact ([[project-bootstrap-byte-exact-s4-s5]]).

Ports refuted: `+15` = SSH (`SSH-2.0-libssh_0.11.0`), `+16` = HTTP 403,
`+17/21/22` closed, the VM's `:443` = the 2 SSE streams. None from `base+0` to
`base+40` answers the 96 B Connect.

**Why the port was invisible**: the hook did not intercept `writev`/`readv` (the
TLS BIOs use them for scatter-gather). Added on 2026-08-21 in
`06-shadow-recon-linux/tls_hook/shadow_tls_hook.c`. A capture with that hook
should reveal the socket.

See `KB.md` §3.21. Tools: `SHADOW_SCAN_INPUT=1` (a TLS+Connect scan of the
offsets), `SHADOW_PROBE_PORTS` (a raw probe), `SHADOW_DUMP_INPUT` (our frames).
