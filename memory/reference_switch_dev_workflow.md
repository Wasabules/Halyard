---
name: Switch dev workflow
description: Build/push/test/logs for shadow2switch on Switch through USB MTP. Not nxlink, not a direct cp. See also the switch-dev skill.
type: reference
---
# Switch dev workflow (shadow2switch)

For EVERY code iteration on the Switch. **Do not try nxlink** (unreliable). Always USB MTP through `gio`.

## Variables

```bash
PROJECT=$REPO/05-shadow-client-borealis
BUILD=$PROJECT/build_switch
MTP="/run/user/$(id -u)/gvfs/mtp:host=Nintendo_Nintendo_Switch_XTJ10221245951"
SWITCH_NRO="$MTP/SD Card/switch/shadow-client.nro"
LOGS_DIR="$MTP/SD Card/switch/shadow-client"
```

## The standard method: FTP through `ftpd.nro` + `tools/switch-sync.sh`

```bash
# On the Switch: launch ftpd.nro from hbmenu (port 5000)
# Sur PC :
tools/switch-sync.sh ping
tools/switch-sync.sh push       # build NRO → Switch
tools/switch-sync.sh logs       # → /tmp/switch-logs-YYYYMMDD_HHMMSS/
```

Override : `SWITCH_IP=...` `SWITCH_PORT=...`. Default 192.168.1.17:5000.

**Advantages**: no need to physically unplug and replug, it is fast, and it is scriptable. **sys-ftpd-light (a sysmodule) is INSTALLED and runs as a daemon** — nothing to launch, nothing to quit, reachable while a homebrew is running. The old standalone `ftpd.nro`, which had to be relaunched after every test, is no longer used. Authentication is MANDATORY (`tools/switch-ftp.env`, sourced by `switch-sync.sh`). Errors: **450** = the app is running and locking the .nro; **430** = credentials missing — and if you see 430 while testing with a bare `curl`, it is YOUR call that lacks them.

## Logs : rotation 2-fichiers

The app does `rename(webrtc.log, webrtc_prev.log)` then `fopen(webrtc.log, "w")` at start-up. Stop looking for the end of a 12 MB log: `webrtc.log` = the current session (~100 KB), `webrtc_prev.log` = the previous one.
The start marker: `=== webrtc.log new session YYYY-MM-DD HH:MM:SS ===`.

## Build

```bash
cd $BUILD && make shadow-client.nro    # cible explicite (pas juste `make -j4`)
md5sum shadow-client.nro
```

## Push (jamais cp direct → "Operation not supported")

```bash
gio remove "$SWITCH_NRO"                          # supprimer d'abord
gio copy shadow-client.nro "$SWITCH_NRO"
gio info "$SWITCH_NRO" | grep size                # check it
```

## Running it (= to be asked of the user)

1. Unplug
2. Lance shadow-client dans hbmenu
3. Test scenario
4. Quitte proprement (B → hbmenu) — sinon webrtc.log pas flush
5. Rebranche USB
6. Confirme

## Logs

```bash
gio copy "$LOGS_DIR/borealis.log" /tmp/borealis.log
gio copy "$LOGS_DIR/webrtc.log"   /tmp/webrtc.log
```

- `borealis.log` : par-session, ~kB, brls::Logger
- `webrtc.log`: append-only, megabytes, ulog → **the last lines** = the current session
- A direct shell `cp` of webrtc.log → "Input/output error". `gio copy` works.
- Autres : `last_clients_*.json`, `last_vm_ip_response.txt`, `proximus_trace.log`, `curl_trace.log`

## Pourquoi

- An nxlink ping often fails (the Switch is not in hbmenu, or the network).
- MTP overwrite via cp shell → "Operation not supported" silently.
- A gvfs IO error on large files through cp → gio copy gets through.
- The user must be physically present on every round (unplug / launch / plug back in).

The associated skill: `~/.claude/skills/switch-dev/SKILL.md`.
