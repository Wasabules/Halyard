---
name: ⚠️ Native blocked — DELETE clients invalide nos propres JWT (401 SSE)
description: 2026-05-09 ~01:30 — `clean_my_zombies` running AFTER `proximus_create_main_client` deletes the client we have just created → main_jwt and launcher_jwt become invalid → the SSE returns 401 → :11011 is never bound by the server.
type: project
---
## TL;DR

A critical bug of 2026-05-09: we DELETE our own freshly created clients through `clean_my_zombies` or `FORCE_CLEAR`, which invalidates the associated JWTs server-side. The `/N/stream` and `/N/status` SSEs return **401 Unauthorized**, and the `:port_base+11` port is never bound.

Found through run 13, where both SSEs fail simultaneously with `[sse] perform exit rc=0 http_status=401`.

**Why** : ordre actuel dans `connecting_activity.cpp::runConnectionFlow()` :
1. step 4: `launcher_proximus_credentials` → fetches the JWT
2. step 5: `proximus_create_main_client` + `proximus_create_launcher_client` → POST /N/clients (= creates our clients 358193-`<device-id>`-main and -launcher)
3. step 7 (if SHADOW_NATIVE=1): `ctrl_rest_clean_my_zombies(device_uuid)` or `clean_all_clients` → **DELETES those very clients** we just created
4. SSE start → 401, because the JWTs are tied to deleted client_ids
5. ctrl_session_run :11011 → ECONNREFUSED car serveur n'a aucun client actif

**How to apply (= the fix)**: reorder the sequence:
1. fetch the JWT
2. CLEAN UP the device-id's zombies (= before creating)
3. POST /N/clients → our new client is ALONE with that device id
4. SSE / ctrl_session → JWT tojours valides

A safer alternative: have `clean_my_zombies` skip our new client_id (= passed as a parameter).

## Components affected

- `activity/connecting_activity.cpp:444+` — the `if (use_native)` block must do the cleanup BEFORE step 5 create_*_client, not after
- `streaming/ctrl_rest.c::ctrl_rest_clean_my_zombies` — accept an `exclude_client_id` parameter so it does not DELETE the current one

## État du code 2026-05-09 ~01h30

- ✅ port_base = vm.port + 7000 (the formula confirmed on 2 VMs)
- ✅ A JWT.instance rewrite of the proximus URLs
- ✅ The extended LD_PRELOAD hook (connect/send/recv/write/read)
- ✅ ctrl_session_glue + h264_decoder_feed_annexb + sufp reassembly
- ✅ proximus_sse_start_ex (/stream + /status) implemented
- ⚠️ clean_my_zombies invalide nos propres JWT
- ❌ TCP `:port_base+11` never bound, so the bootstrap fails

## Run 7 (= the only live-stream success)

- VM `self-belt-02deb0e7` instance=2 port=2000 base=9000
- No zombie cleanup on that particular run
- 938 frames YUV 1920x1080 @ ~27 fps en 35s, 100% chacha20 decrypt OK
- The cause of no visual stream: SUFP was not wired at the time → glitches (= now fixed through sufp.c)

## Resulting action items

- N9 (this task) — fix ordre cleanup avant create
- M3 (relaunched after the fix) — re-test SUFP live + verify the picture is clean
- (long term) — do not hammer the connect retry, just 2-3 attempts at most
