---
name: project_L15_frame_ceiling_vm_mode
description: The delivered frame rate does not follow the requested one — but the VM's screen mode is NOT the cause (hypothesis refuted the same day).
metadata:
  type: project
---

**This note first asserted the opposite of what it now says.** It is kept and
corrected rather than deleted: the error and its refutation are the useful
content.

Measurements, same VM, same handheld Wi-Fi link, animated content:

    120 fps requested -> 43.0 received (max 44.4)  at 5.7 Mbps
    120 fps requested -> 68.9 received (max 73.6)
     60 fps requested -> 60.0 received             at 8.2 Mbps

**HYPOTHESIS REFUTED**: "the VM's screen mode (2560x1440 @ 59.94, first in the
mode list) caps delivery at 60 fps". One session returned 68.9 on average and
73.6 at peak **with that very mode announced**. The first entry in the list is not
a delivery cap, and the annotation "1st = current accepted" must not be read as
one.

**What the error cost**: I had concluded from ONE pair of sessions (43 against 60)
that requesting 120 delivered less than requesting 60, and recommended going back
to 60. Two sessions at 120 in fact return 43 and 69 — the difference comes from
the content or the link. It is exactly the trap documented here: three samples
minimum, a single session's variance is enormous. See
[[project_iter_2026-05-06_bandwidth_caps]] and
[[feedback_verifier_les_mesures_grep]].

Still true: `kNotifyResolution` writes only `{ width, height }` while the server's
modes carry a third `fps fixed32` field that is never written, and the message is
off by default because the official client does not emit it. An RE lead, but it no
longer explains the observation.

`[L15]` now compares requested against received every 5 s in the log.
