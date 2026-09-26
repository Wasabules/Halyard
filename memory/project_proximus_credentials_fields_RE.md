---
name: project-proximus-credentials-fields-re
description: "TIER2 2026-05-16 — /proximus-credentials returns ONLY 3 JWTs. The real blobs (streamingtoken, opaque, remote, tlskey) are on /clients. `opaque` = a new C75 candidate for the VST connect body."
metadata:
  type: project
---

## Question TIER2-F

Which JSON fields are returned by /proximus-credentials that we ignore?
Where do the bytes at channel+17016..17024 (= the VST connect body) come from?

## The answer

### 1. /proximus-credentials = only the 3 JWTs — C95

Strings `proximus-credentials` / `proximus_credentials` sont **ABSENTES**
of the `ShadowPCDisplay` binary. Confirmed through a zero-hit grep on
`dumps/strings_display.txt`.

→ The endpoint is parsed by the **Electron launcher** (= `main.js` of the
launcher-asar package), NOT by the renderer. The renderer receives
just the tokens through CLI flags: `--token <main_jwt>`, `--proximus-url
<url>`, etc.

The `/proximus-credentials` response body (= what we already parse):
```json
[
  {"client_type": "launcher", "token": "<jwt>"},
  {"client_type": "main",     "token": "<jwt>"},
  {"client_type": "usb",      "token": "<jwt>"}    // if requested
]
```

**On extrait correctement** `launcher_jwt`, `main_jwt`, `usb_jwt`.
**No ignored field** here.

### 2. /clients POST = LE vrai endpoint enrichi — C95

It is `/<instance>/clients` (a POST with a `{type, opaque}` body) that returns
the metadata fields. The rodata templates were extracted from the binary
(`strings_display.txt:32642..32673`) :

```
"data": { "id": "", "opaque": "" }                       ← generic
"data": { "id": "", "streamingtoken": "",
          "streamingtoken_expiry": 0, "remote": "" }     ← type=main
"data": { "id": "", "spice_secret": "" }                 ← type=launcher
"data": { "id": "", "tlskey": "" }                       ← type=usb
"data": { "id": "", "spice_url": "" }                    ← /devices Spice
"data": { "id": "", "port": 0 }                          ← /devices XHCI
```

### 3. Mapping par client type (IDA-verified `sub_B6DAB0`)

C95 byte-exact via decompilation `tools/ida/out/tier2/prox_sub_B6DAB0.c` :

| type | id | streamingtoken | streamingtoken_expiry | end_of_session | remote | spice_secret | tlskey |
|------|----|----------------|------------------------|----------------|--------|---------------|--------|
| launcher | ✓ |   |   |   |   | **✓** |   |
| **main** | ✓ | **✓** | **✓** | **✓ NEW** | **✓** |   |   |
| usb      | ✓ |   |   |   |   |   | **✓** |

**`opaque` does NOT appear** in the response. It exists only in
the request body (= the template at 0x1299328, `{"id":"","opaque":""}` — that is
the send-side shape). The server does not echo it back.

### 4. Champs qu'on n'extrait PAS (NEW) — C95

Notre code (`demo/src/shadow/proximus.c`) extrait :
- launcher : `id`, `spice_secret` ✓
- main : `id`, `streamingtoken`, `streamingtoken_expiry` ✓
- usb : `id` ✓ (PAS `tlskey`)

**Fields manquants** :
- **`remote`** (main) : probablement `<vm-ip>:<port>` du streaming endpoint
  (= the authoritative source, an alternative to `/vm/ip`).
- **`end_of_session`** (main, NEW in TIER2): a field found through the IDA dump
  `sub_B6DAB0` line 296 `sub_B793E0(v62, v59, "end_of_session", "");`.
  Probably an epoch marker / a structured blob. See the log
  `'End of session triggered'` (L32861).
- **`tlskey`** (usb) : 64 hex chars = 32 B preshared TLS PSK pour Spice.

### 5. Le VST connect body (channel+17016..17024) — candidats revus

TIER1 §A.2 confirmed: the body = `Output->vtable[+144]()` (= a sized vector of ~100 B).

The initial TIER2 `opaque` hypothesis is **INVALIDATED** by the IDA dump
(`sub_B6DAB0` does not parse it → the server does not echo it).

Nouveaux candidats viables (cf. TIER2 doc §SUMMARY) :
1. **Variant 10** : `auth_hash[20] || streamingtoken` — C70
   - auth_hash = server-issued (Auth reply f3.4.2)
   - We already use it for the UDP register packets
   - Reuse pattern naturel
2. **Variant 11** : `end_of_session` raw bytes — C50
   - A field found this sprint
   - Format unknown (= structured/a marker, to be investigated)
3. **Variant 8** (TIER1, unchanged): the `streamingtoken` alone — C70

### 6. Recommended action

**BEFORE testing more variants**: do the LD_PRELOAD capture
the plaintext `SSL_write` on the desktop side (= TIER1 §A.5) — 30 minutes of work,
to lock it byte-exact at C95. Far more profitable than A/B variants.

## Action items

1. **Extraire `end_of_session`** depuis `last_clients_main.json` (= NEW
   TIER2 field).
2. **Extraire `remote`** field — utile comme fallback authoritative
   (= alternative `<vm-ip>:<port>`).
3. **Extract `tlskey`** from `last_clients_usb.json` (= for a future
   Spice path).
4. **BEFORE trying more variants**: an LD_PRELOAD `SSL_write` plaintext
   capture desktop pour locker VST body byte-exact (= meilleur ROI).

## Cross-ref

- [[project-vst-connect-msg-byte-exact]] — TIER1 §A
- TIER1 doc : `tools/ida/out/TIER1_RE_2026-05-16.md` §A
- TIER2 doc : `tools/ida/out/TIER2_RE_2026-05-16.md` §F + §SUMMARY
- `demo/src/shadow/proximus.c::proximus_create_main_client`
