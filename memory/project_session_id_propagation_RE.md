---
name: project-session-id-propagation-re
description: "TIER2 2026-05-16 — Two distinct IDs: sessionUniqueId (a client-minted UUIDv4, Auth f5 only) and vm_session_id (the server's /vm/ip). The VST connect body IS NOT sessionUniqueId."
metadata:
  type: project
---

## Question TIER2-G

Where is sessionUniqueId / session.id generated? How does it propagate to the
channels? Is it sent in the `:base+20` connect_msg? In what format?

## The answer — C90

### 1. Deux IDs distincts (CRITIQUE)

| Name (binaire) | Source | Type | Stockage | Envoi |
|----------------|--------|------|----------|-------|
| **`sessionUniqueId`** | **client-mint UUID v4** | ASCII 36 chars | local var | **Auth proto f5 only** |
| `connectionUniqueId` | client-mint UUID v4 | ASCII 36 chars | local var | Auth proto f6 |
| `session_id` / `sessionId` | **server `/vm/ip` returns as `vm_session_id`** | UUID-like string | `ConnectedClient` field | logs + metrics + CLI |
| `vm_session_id` | server `/vm/ip` | UUID string | `VmConnectionInfo` | metrics + CLI `--session-id` |

### 2. sessionUniqueId est CLIENT-GENERATED — C95

Confirmed by 2 sources:
- `strings_display.txt:27985` : `Not session_id found. A dev one has been
  generated:` — a launcher log for when the JWT does not contain a session id; it
  mint localement.
- Our `main_test.c:1185-1186` mirrors the pattern:
  ```c
  char session_id[37], connection_id[37];
  gen_uuid_v4(session_id);
  gen_uuid_v4(connection_id);
  ```

→ **sessionUniqueId is NOT a binding with the server.** It is an id
internal to the client, for correlating logs/metrics.

### 3. vm_session_id (server) — C95

The real server-issued id comes from the `/<instance>/vms/{id}/ip` response, in
the `vm_session_id` field. The schema observed (the web-client HAR):
```
{ip, port, offset, slot_number, alias, proximus_url, messaging_url,
 provider, vm_session_id}
```

Our code extracts it in `launcher.c:468`:
```c
out->vm_session_id = jstrdup(root, "vm_session_id");
```

### 4. Propagation vm_session_id (server-issued) — C95

The `vm_session_id` is used by the desktop:
1. **CLI launcher → renderer** : `--session-id <vm_session_id>`
2. **Embedded dans metrics** : `StreamingMetricSink` ajoute
   `vm_session_id` on every metric event (L31423).
3. **Logged in the "Channel down" event**: `sessionId={}` (L33013).
4. **Sent in the SSE `/forward` body**: it lets the server dispatch to the
   bon VM-pool slot.
5. **Derived for the client_id**: the format `<vm_session_id>-<client_type>`
   (= e.g. `<user-id-A>-...-main`).

### 5. On our Switch — the current state

`vm_session_id` is used ONLY for the legacy WebRTC path:
- `webrtc.c::session_id`
- `dc_session.cpp:637` (vm_sid pour signaling)
- `dc_shadow_cli.cpp:741` (sessionId dans signaling)

**The native ctrl bootstrap path (`ctrl_session.c`) NEVER sees it.**
That is probably fine (= the server identifies through the JWT + opaque), but worth noting
si on ajoute un nouveau channel type.

### 6. Format — C95

- **sessionUniqueId / connectionUniqueId (client-mint)** : UUID v4 ASCII
  36 chars avec dashes (`xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx`).
- **vm_session_id (server)**: a 36-char ASCII UUID, sometimes prefixed
  pool-id.

**Texte length-prefixed proto string**, PAS binaire 16 bytes.

### 7. Implication pour VST connect body (TIER1 §A)

The TIER1 candidate 8c hypothesis = "sessionUniqueId (a 36 B UUID)" → **DOWNGRADED
to C30**.

Reason: sessionUniqueId is CLIENT-MINTED, so the server CANNOT use it
for binding (it only learns it through Auth f5, i.e. after the VST
handshake). It is logically impossible for the VST connect message's body
to be an ID the client generates itself — the server would have no way
to validate it.

→ The VST connect body must be a **server-issued blob**:
- streamingtoken (server-issued JWT) — C70
- **opaque (server-echoed blob)** — C75 NEW (cf. TIER2 §F)
- vm_session_id (server-issued UUID) — C40 (trop court)

### 8. Channels other than ctrl+11 that consume a UUID — NONE

Recherche grep : aucun channel (`:base+10` video, `:base+12` audio,
`:base+13` input, `:base+20` VST, `:base+30` cursor) receives neither
neither sessionUniqueId nor vm_session_id in the payload. The binding is done through:
- The 20 B auth hash (= carried in the Auth reply, used in the UDP register)
- JWT bearer (= TLS layer)
- Per-client `opaque` (= probablement le VST connect body)

## Action items

- **No change needed** to the sessionUniqueId generation
  (notre `gen_uuid_v4` mirror desktop).
- If the variant-8c tests (sessionUniqueId as the VST body) are still to be done:
  **strike them off the TODO**. Pointless.
- **Focus variant 9 = opaque** (cf. TIER2-F).

## Cross-ref

- [[project-vst-connect-msg-byte-exact]] — TIER1 §A
- [[project-overnight-re-2026-05-03]] — RE cross-platform JWT/CLI
- TIER2 doc : `tools/ida/out/TIER2_RE_2026-05-16.md` §G
- `demo/src/shadow/launcher.c:468` (= extraction vm_session_id)
- `demo/src/main_test.c:1186` (= gen client sessionUniqueId)
