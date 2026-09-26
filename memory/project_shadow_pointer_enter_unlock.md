---
name: Shadow input echo — PointerEnter unblocks everything
description: The 136 B "PointerEnter" is what unblocks Shadow's echo. Without it, total silence even with a clean Connect+Hello+SCTP.
type: project
---
Found at the end of M15 stage C (2026-05-02). For Shadow to start echoing our inputs:

**The mandatory init sequence on shadow-input (PPID=53)**:
1. Connect, 96 B — verbatim, with the wire+40 timestamp patched to `Date.now()`
2. Hello, 128 B — verbatim, timestamp patched
3. **PointerEnter 136 B** — verbatim, with the timestamp patched. **Critical.** Without that 3rd message, Shadow acks the DCEP but ignores everything at the application level.

**The chronology validated by a browser capture (the chrome-ext extension)**:
- SEND[0] = 96b Connect (t=0)
- SEND[1] = 128b Hello (t=+5ms)
- SEND[2..8] = 144b mouse-moves (t=+6..+9ms)
- SEND[9] = a 136 B PointerEnter (t=+1370 ms — the first time the mouse enters the canvas)
- RECV[0] arrives at t=+1405 ms = **35 ms after the 136 B** → that is what unblocks the echo

**Switch confirmation (the logs after a push)**:
```
shadow_input: PointerEnter sent x3 (136 bytes wire, rc=164) ts-patched
sctp: DATA tsn=... stream=1 ppid=53 payload=104   ← 1er echo input
sctp: DATA tsn=... stream=3 ppid=53 payload=1160  ← cursor bitmap part 1
sctp: DATA tsn=... stream=3 ppid=53 payload=1160  ← part 2
sctp: DATA tsn=... stream=3 ppid=53 payload=1160  ← part 3
sctp: DATA tsn=... stream=3 ppid=53 payload=752   ← part 4 (total 4232b = bitmap curseur)
```

**Why:** without this finding (and without the browser capture through the chrome-ext extension that reveals that unique 3rd message), we would have spent weeks investigating the SCTP/DTLS/SDP layers, which were already working.

**How to apply:**
- Always send those 3 messages in the order Connect→Hello→PointerEnter as soon as the DCEP_OPEN on shadow-input is ACKed.
- Bytes 122-123 and 124-125 of the PointerEnter = the absolute cursor position X,Y (uint16 LE) at the moment of the PointerEnter. For the final M15, to be regenerated according to the current stream resolution (the centre = w/2, h/2).
- The timestamp patch + the ×3 redundancy + the seq counter are all still needed for subsequent mouse-moves to be processed.
