---
name: project_S35_udp_buffer_hos
description: On Switch the UDP receive buffer is capped at 42240 B by libnx's init configuration — N56's 4 MB SO_RCVBUF has no effect
metadata:
  type: project
---

Mesure console (2026-08-25) : `udp_register: :10010 SO_RCVBUF set=4194304 actual=42240`,
on **every** session. 42,240 = `0xA500` = libnx's default `udp_rx_buf_size`
(documente « typically 0xA500 » dans `switch/runtime/devices/socket.h`).

**The N56 fix (a 4 MB buffer) has no effect on the Switch.** The driver
`bsdsockets` refuses to exceed what its init configuration reserved. Borealis
sets that configuration in its `userAppInit`
(`library/borealis/.../switch_wrapper.c`): it starts from
`socketGetDefaultInitConfig()` and only raises `num_bsd_sessions` and
`sb_efficiency` — **not** the buffer sizes.

The measured consequence: **0.46 % UDP loss on average** across 9 sessions, which
triggers a key frame every ~500 ms through G26. This is NOT G5/G37's autofocus
(which described IDRs on phantom errors at `perdus=0`): here the loss is real, so
treat the cause and not the symptom.

Correctif : `shadow/sockets_compat.c`, branche `__SWITCH__` — on referme le
driver and reopen it with a 1 MB receive buffer at the very start of `main`,
before any network I/O. Borealis is not modified (it is a vendored library).
Falls back to the original configuration if HOS refuses; `SHADOW_UDP_RXBUF=0` disables it.

**VALIDATED by an A/B on console**: UDP loss 0.460 % -> 0.111 % (divided by 4.1),
packets received +38 %, frames per session +10 %, 8/8 with a picture.

**Reflex**: on HOS, `setsockopt` cannot exceed the socket driver's configuration —
always check the value READ BACK, never assume the request
abouti. Voir KB.md §3.30. Lie a [[project_N56_udp_rcvbuf_FIX]].
