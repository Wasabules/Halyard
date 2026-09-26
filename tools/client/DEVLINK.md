# devlink — driving the UI remotely

Checking a visual change used to cost one human gesture: launch the app, look at
the screen, describe what you see. `tools/devlink.py` removes that gesture — it
captures the screen as a PNG, presses buttons and reads the current state, so an
assistant can loop on the UI on its own. It is a minimal equivalent of ADB.

## Which way the connection goes (the part that surprises people)

**The app connects to us.** It reads `logsink.txt` from the SD card, opens an
outbound socket to this machine, dumps its log into it and reads its commands from
it (`core/services/journal.c`). So `devlink.py` is a **server**: it listens, waits
for the connection, then talks. Nothing listens on the console side; there is
nothing to reach from outside.

Three consequences worth remembering:

1. **The app tries to connect ONLY ONCE, at start-up.** If nobody is listening at
   that moment, there will be no channel for the whole session. So: start `listen`
   **before** the app.
2. No `logsink.txt` = no channel, zero cost. That is also the only protection:
   this channel can drive the app, and it only arms itself with a file you dropped
   there yourself.
3. Only one process can hold the port. So `listen` acts as a **relay** for the
   other calls (see below).

## Arming the channel (once per SD card)

```bash
tools/switch-logsink.sh setup      # drops logsink.txt (address:port) through ftpd
```

The script writes THIS machine's address as the console sees it. Redo it if the
machine's IP address changes. `ftpd.nro` is only needed for that drop.

## Commands

```bash
tools/devlink.py listen [journal.log]   # listen, print the log, act as a relay
tools/devlink.py shot [file.png]        # capture the screen
tools/devlink.py state                  # describe the current screen (one line)
tools/devlink.py btn a                  # a b x y l r zl zr plus minus
tools/devlink.py nav down               # up down left right
tools/devlink.py tap 640 360            # touch at a screen position
tools/devlink.py cmd "ping"             # a raw line (quit, ping, autotest N D P...)
tools/devlink.py quit                   # close the app (the .nro is locked while it runs)
tools/devlink.py relaunch [path]        # close AND load a .nro in its place (default: the one running)
tools/devlink.py script plan.txt        # play a sequence of commands
tools/devlink.py --autotest             # check the tool itself, offline
```

Useful options: `--port` (default 9999, or `LOGSINK_PORT`), `--wait` (seconds to
wait for the connection, default 60), `--timeout` (wait for a reply), `--pace`
(pause between two commands of a script), `--verbose` (also print the log received
during the command).

`stdout` carries only the **result** — the PNG's path, the state text — so it can
be consumed directly by a script. Everything else goes to `stderr`.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | the app answered `err`, or the capture was invalid |
| 2 | an argument refused **here**, before anything was sent |
| 3 | no app connected within the deadline |
| 4 | the port is already taken by something else |

### Script file

One command per line; `#` comments; `attends <seconds>` makes a local pause;
`shot <file>` names the capture. **The whole plan is validated before the first
send**: a half-played plan leaves the UI in an unknown state, which is worse than
an immediate refusal.

**Traps observed on a Windows desktop, 2026-09-11**:

- **`attends` lines placed AT THE HEAD of a plan are played BEFORE the port is
  opened.** The app only attempts its connection once, at launch: a plan that
  starts with `attends 44` misses it for certain, then waits 120 s for nothing.
  Put a real command (`state`) on the first line.
- **`--wait`, `--timeout` and `--port` are GLOBAL options**: they go BEFORE the
  subcommand (`devlink.py --wait 120 --timeout 30 script plan.txt`). After it,
  argparse refuses them (code 2).
- **The first `state` arrives at the app's first log line**, well before its first
  frame: allow `--timeout 30`.
- **During an `attends`, devlink does not read the socket** (`time.sleep`). On a
  Windows desktop the connection was lost within seconds of pausing, even with 6 s
  pauses interleaved with `state`. Cause not established: it is not a full buffer
  on the app side (`WSAEWOULDBLOCK` is correctly recognised there as "drop the
  line", verified at the preprocessor in `journal.c`).
- **Until 2026-09-11, NO reply was recognised**: since S81 the log writes
  `[12.345] I/systeme  [devlink] ...`, and the prefix stripping only knew about the
  timestamp. Fixed in `devlink.py` (`RE_HORODATAGE`), with a counter-case in
  `--autotest`. Under Windows the autotest keeps one failure that predates that
  fix: "second taker of the port -> PortOccupe", because `SO_REUSEADDR` there lets
  two sockets take the same port.

```
# go to the 2nd machine and look at it
nav down
btn a
attends 1.5
shot /tmp/screen-vm2.png
state
```

## Iterating without touching the console (RELOAD-1, 2026-09-12)

Until now the loop kept one irreducible human gesture: **pressing A in hbmenu**.
Everything else was automatic. `relaunch` removes it.

libnx exposes `envSetNextLoad(path, argv)`: a `.nro` launched by hbloader can name
the `.nro` to load NEXT, and hbloader launches that one on exit instead of
returning to hbmenu. It is the mechanism hbmenu uses itself. So the app can hand
over to a new version of itself.

**The trap that dictates the command's shape**: a running `.nro` is LOCKED. So the
new version cannot land on the path in use — the upload fails with FTP 450, which
is already the reason `quit` exists, since S45. Hence **two alternating names**:
you push onto the one that is not running, then ask for the handover to it.

```bash
# 1. once per session
tools/devlink.py listen /tmp/session.log     # BEFORE launching
#    ... launch halyard.nro on the console: THE ONLY HUMAN GESTURE

# 2. as many times as you like, without touching the console
(cd build_switch && make -j$(nproc) halyard.nro)
tools/switch-sync.sh relance
tools/devlink.py shot /tmp/after.png
```

`switch-sync.sh relance` keeps **no counter** to know which name is free: it tries,
and the lock answers. A counter would desynchronise on the first launch made by
hand on the console, and the error would then be "it pushes onto the locked file",
a world away from its cause.

**What this is not**: a RECOVERY mechanism. If the app crashes, nothing is left to
receive the command and you have to relaunch by hand. That is exactly where a
remote-input sysmodule (`sys-botbase`, not installed here) would be complementary,
not redundant.

**The refusals**, each stated with its reason rather than simply endured:

| Reply | Meaning |
|---|---|
| `err relaunch hors-console` | no hbloader — on a desktop there is nothing to hand over to |
| `err relaunch sans-hbloader` | on console, but launched some other way (title takeover): the slot is not read |
| `err relaunch chemin` | the path does not pass the allow-list (see below) |
| `err relaunch inconnu` | on console, but `argv[0]` said nothing usable |

On a refusal the app **stays alive**: exiting anyway would hand control back to
hbmenu, which is exactly the human gesture the command exists to remove.

**The path is the only datum on this channel that names a file to EXECUTE**, so it
is filtered by an allow-list, on both sides (`devcmd_path_ok` in
`clients/borealis/devlink/devcmd.h`, replayed in `devlink.py` so the refusal names the
fault): under `/switch/`, `.nro` suffix, characters `[A-Za-z0-9_-./]`, no `..` and
no `//` anywhere, a real name between the prefix and the suffix. `--autotest`
replays the same list of counter-cases as `tests/test_devcmd.c`: if the two
diverge, the tool would accept what the app refuses.

## One instance only: the relay

The port can only be held by one process. If `listen` is already running, the other
calls (`shot`, `btn`, …) **go through it automatically** over a local socket
(`$TMPDIR/shadow-devlink.sock`, adjustable with `--socket` or `DEVLINK_RELAIS`).
That is the normal mode: you keep a listener showing the log, and drive from
another terminal.

With no listener, the call takes the port itself, waits for the app to connect
(`--wait`), plays the command and returns.

If the port is taken by something else (`tools/switch-logsink.sh listen`, an
`nc -l`), the tool says so and exits with code 4 — it does not let an "address
already in use" through, which states neither the cause nor the remedy.

## The complete loop

```bash
# 1. one terminal keeps the listener (and acts as the relay)
tools/devlink.py listen /tmp/session.log

# 2. change some code, rebuild
cd build_switch && make -j$(nproc) halyard.nro

# 3. the old instance locks the .nro: close it through the channel
tools/devlink.py quit
tools/switch-sync.sh push          # from the repo root

# 4. LAUNCH THE APP ON THE CONSOLE — the only human gesture left
#    (the listener from step 1 accepts the new connection on its own)

# 5. navigate to the screen you want, capture, look
tools/devlink.py state
tools/devlink.py nav down
tools/devlink.py btn a
tools/devlink.py shot /tmp/before.png
```

The assistant reads `/tmp/before.png` directly: it sees what the console is
showing, with no human description in between.

## What the tool refuses to do

**A truncated capture is never written to disk.** `shot-begin` announces the
width, the height and the byte count; the tool checks that the reassembled PNG is
exactly that many bytes and does start with the PNG signature. Otherwise: an error
message, code 1, no file. A truncated PNG would still open, half grey — and you
would blame the renderer when it is bytes lost in transit. If the IHDR contradicts
the announced dimensions, the file is written (the pixels are complete) but the
discrepancy is reported: it comes from the C side.

Commands are validated **before** anything is sent: a button that does not exist,
a non-numeric or out-of-range `tap`, a command longer than `log.c`'s buffer are all
refused here, with code 2 and nothing sent.

## The trap that looks like a dead channel (Linux/Wayland, 2026-09-12)

**`shot`, `state`, `btn` and `quit` go unanswered if the window is not
composited** — hidden behind another window, minimised, on another desktop. The
compositor then stops sending its frame callbacks, `eglSwapBuffers` blocks, and
the render loop stops dead: measured on an occluded window, **1 CPU tick in
3 seconds**. And `onFrame()` is the only place a command is served, by
construction (`glReadPixels` only makes sense on the thread that holds the GL
context).

**What makes the trap expensive: the log keeps arriving.** It is written by the
session, network and audio threads, which are still running. So you watch the
mirror scroll past, conclude the channel is alive, and go looking for the failure
on the wrong side — the command is received and correctly parsed, it is only
waiting for a frame that never comes. Verified in the diagnostic: `recu 'state'
len=5 actif=1 parse=1 kind=2 handled=1`, and no reply.

**The test that settles it in one second**, before blaming anything:

```bash
P=$(pgrep -x halyard); a=$(awk '{print $14+$15}' /proc/$P/stat); sleep 3
b=$(awk '{print $14+$15}' /proc/$P/stat); echo "$((b-a)) ticks in 3 s"
```

A few hundred ticks: the loop is running, the problem is elsewhere. Zero or one:
the window is not drawing, there is nothing to fix — bring it back to the front.

On console the question does not arise: the homebrew is alone on screen and always
draws.

## Known limits

- **Launching the app remains manual.** Nothing allows starting a homebrew
  remotely; it is the loop's last human gesture.
- **One command at a time, ~0.35 s between two.** `log.c` reads 255 bytes per 100 ms
  round of its drain thread and splits on newlines: a larger batch would be cut
  mid-line, and half of it would go out as an unknown command. So the tool never
  sends two lines at once.
- **A capture pollutes the log**: ~1400 `shot-data` lines for a 720p screen. Use
  `listen journal.log` if you want to reread the log afterwards.
- **No encryption, no authentication.** A trusted local network only. The channel
  only exists if `logsink.txt` is present, and the app connects outbound: it opens
  no port.
- **The relay requires `AF_UNIX`.** Where that is missing, only direct mode (one
  call takes the port) works.
- **The content of `state` and the quality of `shot` depend on the C side**
  (`clients/borealis/devlink/devcmd.h`). If a command is not implemented there yet, the reply is
  `err <command> inconnue` and the tool exits with code 1 — that is the expected
  behaviour, not a channel failure.
- `switch-logsink.sh listen` and `devlink.py listen` fight over the same port: do
  not run both. `devlink.py listen` does everything the other one does.

## Checking the tool without a console

```bash
tools/devlink.py --autotest
```

59 offline checks, with no app and no external network: a stub imitates the app
(it connects, and answers with `log.c`'s timestamp) and the tool talks to it over a
real socket, including through the relay. Every check names its **counter-case** —
the truncated capture, the announcement that lies by one byte, the damaged base64,
`log.c`'s `[12.345]` timestamp that made every reply time out, the command sent
before the app is connected (it must **wait**, not fail), the port already taken.
So a failure tells you what you have just undone.
