---
name: HAR 2026-05-06 — bandwidth/quality root causes
description: An audit of the Chrome HAR of 2026-05-06 (Linux web /home/settings/network); 5 deltas against our compiled Linux client that explain the degraded quality at high bitrates
type: project
---
The capture: `$REPO/captures/capturehar.txt` (105 entries, Chrome 147 on Linux, the page /home/settings/network, 2026-05-05 22:52). No complete stream captured, but the full launcher bootstrap is there.

**5 critical deltas between the browser's HAR and our compiled Linux client:**

1. **The SDP video offer is too poor — PROBABLY THE PRIMARY CULPRIT.** We offer only `profile-level-id=42001f` (H.264 baseline 3.1, packetization-mode=1). The browser offers 11 different H.264 PTs including `42e01f`/`4d001f`/`f4001f`/`64001f` (high profile) + VP8/VP9/AV1/H265 + red/ulpfec/flexfec. The server answers `b=AS:70000` → the bitrate is available, but it picks `42001f` because that is the only one in common. Baseline = no CABAC and no B-frames → ~30-50 % less visual quality at the same bitrate.
   - The fix: extend `sdp.c sdp_build_offer` to add PT 109/114 (H.264 high@3.1 packetization-mode=1) at a minimum. libavcodec/NVDEC decode High without trouble.

2. **The browser's streaming server API was bumped /3/→/5/→/7/** between 2026-05-02 and 2026-05-05. Our `ctrl_rest.c` is frozen on /3/ BUT that module is only used by smoke_test, not in production. Production goes through `proximus.c` (an unversioned `<proximus_url>/clients` URL) so it is fine for now. To watch if Shadow deprecates /3/.

3. **Browser telemetry that we do not send**: 15× POST to `https://prod.log.frsbg01.shadow.tech:2443/client` with `{"name":"launcher.status",...}` every ~3 s. The body includes `user-uuid`, `session`, `launcher-version`, `renderer-version`. The server may be doing QoS scoring on the absence of telemetry and restricting the bitrate.

4. **`/vms/{vm_id}/capabilities` GET never called by us.** The browser does it before `/vm/start`. Probably informational; to be tested whether its absence changes `/vm/start`'s reply.

5. **The proximus-credentials body differs.** The browser: `[{"client_type":"launcher"},{"client_type":"main"}]` (2 entries, no `ports`). Us: 3 entries (+`usb`) with `ports:["controlchan","video","cursor","input"]` on the `main` entry. The `ports` array tries to unlock direct TCP mode but the user's account is legacy WebRTC (cf. the `Shadow JWT.ports pivot` note) — probably a no-op server-side, but non-conformant with the browser.

**Mandatory headers confirmed on api.eu.shadow.tech** (already in place through `append_shadow_headers`, launcher.c:270): `X-Vm-Id`, `X-Shadow-Uuid`, `X-Shadow-Agent`, `Origin`, `Referer`. The exact X-Shadow-Agent format observed: `"Linux;x86_64;Browser <UA>;Launcher 1.15.1;Client 0.13.0"`.

**Bonus**: compute.shadow.tech's `/7/clients` body uses `"type":"launcher"` (a string) with `"opaque":"<json escaped>"`. Our `ctrl_rest.c` sends `"type":%d` (numeric) — a latent bug if we ever use it in production outside smoke_test.
