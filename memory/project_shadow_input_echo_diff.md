---
name: Shadow input echo — diff multi-session findings
description: The result of diffing 2 Chrome sessions for M15 stage C. The 96 B "Connect" is NOT a handshake and the order is free.
type: project
---
The diff `tools/capture_diff.py captures/session1.json captures/session2.json` (captured 2026-05-02):

**1. The 96 B "Connect" is byte-static between sessions (modulo the 8 B timestamp)**:
- Seuls offsets 40-47 varient (timestamp Unix ms LE).
- No session fingerprint, no hidden UUID. Verbatim is enough.

**2. The message order on the browser side is FREE, not a handshake**:
- session1: SEND sizes = [144, 120, 136, 136, 136, 144, **96**, 144, ...] → 96b en idx 6
- session2: SEND sizes = [144, **96**, 128, 144, ...] → 96b en idx 1
- session1 NEVER sent a 128 B "Hello" (n=0). So the 128 B one is OPTIONAL.
- **Conclusion**: 96 B and 128 B are not a required Connect/Hello. They are just events among others (focus/blur/idle/ping?).

**3. Shadow ECHOES every input back into the browser**:
- session1: shadow-input SEND=30 RECV=30 (104b chacun)
- session2: shadow-input SEND=30 RECV=30 (104b chacun)
- Each 104 B RECV = a different vtable from the SENDs (table_inline=32, fields=5 but offsets {6,12,20,7,8}).
- Session-variant bytes: 50-51 (uint16 — a session epoch?) constant for one session; bytes 40, 48-49, 96-99 = a counter incrementing per RECV.

**4. The Switch receives NO PPID=53 on stream=1**:
- `grep "ppid=53" /tmp/webrtc.log` → 0 hits.
- Seulement DCEP control (PPID=50) revient.
- So Shadow accepts our OPEN and ACKs it, but ignores our application messages.

**Why:** our hypothesis "Connect must be sent first" was wrong. Shadow's silence comes from something else.

**How to apply:** Pour M15 stage C suivant :
1. Try sending a 144 B mouse-move DIRECTLY, with no Connect/Hello (the way session2 does).
2. Check the DCEP_OPEN parameters (priority/maxRetx/protocol) byte by byte against what the browser sends, through Wireshark + tools/extract_dtls_sctp.sh.
3. Check that the timestamp sent falls inside the window Shadow accepts (the Switch's RTC can be off) — patch_timestamp() is currently disabled.
4. Re-capture v2 with `capture_hook.js` (with ts_ms) to reconstruct the SEND/RECV timing and confirm whether it is synchronous.
