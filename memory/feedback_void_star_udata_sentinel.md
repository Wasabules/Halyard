---
name: feedback_void_star_udata_sentinel
description: A udata passed as a void* is never checked by the compiler — extracting a function can slip &ptr in there instead of ptr
metadata:
  type: feedback
---

By extracting `session_open_media` out of `ctrl_session_run`, `ctx` went from
from a VALUE to a POINTER. The call kept `&ctx`. Since the parameter is a `void *`,
the compiler said nothing. The callback received the address of a dead stack
slot, and the `if (!ctx->p)` safeguard was powerless: the pointer was not
null, it was wrong.

**Why:** `void *` cancels all type checking. A function extraction that
turning a value variable into a pointer silently breaks every `&x`
that referenced it. And the defect can sleep for a long time: here the `:base+20`
channel carries only key-frame retransmissions, so the callback only fired after
99 s — the crash looked tied to a user action.

**How to apply:** after any function extraction, re-read EVERY `&variable`
of the moved region. And for any context passed as a `void *`, place a
sentinel (`magic`) checked at the top of the callback: one comparison, and the error
becomes a log line instead of a crash.

A corollary on method: resolving a crash report against an `.elf`
REBUILT identically to the binary that was running — an `.elf` rebuilt from
shifts the offsets. And when `addr2line` leaves any doubt, disassemble around the
PC tranche. Voir KB.md §3.32.
