---
name: The consent-freshness rebuild = detection yes, prevention no
description: Rebuilding libjuice with DISABLE_CONSENT_FRESHNESS=OFF, tested 2026-05-03 — libjuice detects ICE FAILED at t~130 s but the network freeze persists at exactly t=115 s. The root cause is not RFC 7675, probably a missing DC keepalive.
type: project
---
**Test 2026-05-03 23:33** after rebuilding libjuice with `DISABLE_CONSENT_FRESHNESS=OFF` (the RFC 7675 default):

- The freeze persists at **exactly t=115 s** (rtp_v=10468 frozen, frames=3462)
- The ICE state 4→5 FAILED arrives at ~t=130 s (15 s after the freeze, not the standard 30 s)
- libjuice spams `Send while ICE is not connected` = good detection
- HTTPS REST polling active during the session: 21+ `/8/status`, 14+ `/8/stream`, 18+ `/8/forward` (proximus_trace.log)

**The RFC 7675 hypothesis is ELIMINATED**: if the server applied the RFC strictly, the Indications would never have worked. Yet 11 earlier sessions stayed connected for 115 s with consent OFF. The server accepts the Indications. The freeze's cause is elsewhere.

**The application-level HTTP hypotheses are ELIMINATED**: the `/status`/`/stream`/`/forward` polling is done correctly. Not the cause.

**Why**: it is useful to keep consent freshness ON for fast ICE detection. But the root cause of the t=115 s freeze is still to be found.

**How to apply** :
- Keep `DISABLE_CONSENT_FRESHNESS=OFF` in `library/libjuice/build_switch/CMakeCache.txt` (already applied)
- Do NOT re-test these hypotheses: RFC 7675 consent, /status polling, /stream SSE, /forward keepalive — all eliminated by this session
- **A strong new lead**: a client→server keepalive on a DataChannel that we do not send. The browser sends a 128 B `GamepadPingInputV4Model` at 0.5 Hz on the `shadow-controller` DC (KB §4.0, session9 capture = 23 pings in 52 s). If the server expects that ping and receives nothing, it cuts at a ~115 s timeout.
- An immediate action: capture a long browser session (>3 min idle) through the improved extension to see EVERY DC send between t=0-150 s, especially on shadow-controller. A periodic pattern → imitate it on the Switch side.

**Sources** : test session 2026-05-03 23:33-23:39, logs `/tmp/switch-logs-20260503_233942/`, ligne 4667 webrtc.log = ICE FAILED.
