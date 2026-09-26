# Advanced

Everything on this page is optional. Halyard is meant to be used from its
screens; what follows is for when you want to impose a value, read what went
wrong, or help someone diagnose a problem.

## The data directory

Halyard keeps everything it writes in one folder, created on first launch:

| Console | Folder |
|---|---|
| Nintendo Switch | `/switch/halyard/` on the SD card |
| PS Vita / PS TV | `ux0:data/halyard/` |
| Linux (development build) | `/tmp/halyard/` |
| Windows (development build) | `halyard-data\` beside the program |

What you may find there:

| File | What it is |
|---|---|
| `settings.txt` | what the settings screen saved |
| `refresh_token` | your session with Shadow; Settings › Account › Sign out deletes it. **Obfuscated, not encrypted** — anyone with the card can recover it — unless an app lock is set, which seals it |
| `device.uuid` | an identifier for this console, generated once |
| `env.txt` | the overrides described below — you create it |
| `applock.txt` | the app lock, when you set one (Settings › Security) |
| `halyard.log` | the log of the current session |
| `halyard.1.log`, `halyard.2.log`, … | the logs of the previous sessions, most recent first (three kept) |
| `hwaccel.disabled` | written after repeated hardware-decoder failures, so the next launches decode in software. Delete it to try the hardware decoder again |
| `logsink.txt` | development only: mirrors the log to a computer (see below) |
| `autotest.txt` | development only: chains sessions with nobody present |

The packages themselves never touch this folder: updating Halyard keeps your
settings and your session.

## Overriding a setting with `env.txt`

A console gives an application no environment variables, so Halyard reads them
from a file instead. Create `env.txt` in the data directory, one `KEY=VALUE` per
line, `#` for comments:

```
# env.txt
SHADOW_BITRATE_MBPS=25
SHADOW_JOURNAL_NIVEAU=3
```

Only keys starting with `SHADOW_` are read — the file comes off a memory card and
has no business changing anything else. Each value is read back after it is
applied, and a mismatch is written to the log.

> **`env.txt` wins over the settings screen.** That is on purpose: a test must be
> able to impose a value without going through the menus. The settings screen
> marks every setting that `env.txt` is overriding, so the priority is visible.
> **If what you see disagrees with what a setting says, read `env.txt` first.**

A few keys worth knowing; the settings screen covers the same ground for
everyday use:

| Key | Effect |
|---|---|
| `SHADOW_BITRATE_MBPS` | the maximum video bitrate asked of the server, in Mb/s. The server treats it as a ceiling, never as a target |
| `SHADOW_FPS` | the frame rate asked of the server. It is only sent when the session opens |
| `SHADOW_VIDEO_NET_TCP=1` | video over TCP instead of UDP. Loses nothing, but turns off the server's adaptive bitrate |
| `SHADOW_HWACCEL=0` | software video decoding. Much heavier; for diagnosis only |
| `SHADOW_JOURNAL_NIVEAU` | how much the log says: `0` errors only … `2` milestones (the default) … `4` everything, per packet |
| `SHADOW_RUMBLE_SWAP=1` | swaps the two rumble motors |

There are about 260 such switches: every change of behaviour in Halyard ships
with one that reverts it, so a comparison needs no new build. Their names and
what each one is for are documented next to the code that reads them.

## The log

`halyard.log` in the data directory says what happened, in order, with a time
and a category on every line:

```
[12.345] I/video    decoder ready, 1280x720
```

The letter is the severity (`E` error, `W` warning, `I` information, `D` detail,
`T` trace). How much is written is set in **Settings › Advanced**, or with
`SHADOW_JOURNAL_NIVEAU` in `env.txt`, which wins. The previous sessions stay
beside it as `halyard.1.log` and so on, most recent first. The log viewer in
Settings › Advanced reads them on the console itself.

> **Read a log before you share it.** It carries session material — which is why
> the keys and tokens in it are masked, but a log can still say more than you
> want to publish. Quote the lines around the problem rather than attaching the
> whole file, and never share a packet capture. See [SECURITY.md](../../SECURITY.md).

## When the app crashes

- **Switch**: Atmosphère writes a report under `/atmosphere/crash_reports/`.
- **PS Vita**: a `psp2core-*.psp2dmp` file appears in `ux0:data/`.

Both are useful in a bug report, together with the version from Settings ›
About.

## Development tools

Two files turn Halyard into something a computer can drive. They are meant for
development, and they are why the app can be tested without a person holding
the console:

- **`logsink.txt`** holds `host:port`. At launch the app connects to that
  address and mirrors its log there, and the same connection lets the computer
  take screenshots and press buttons (`tools/client/devlink.py`). The first time
  a computer connects, **the console asks you whether to allow it**, and can
  remember the answer. No file, no connection.
- **`autotest.txt`** (`runs=10 duration=30 pause=5`) chains sessions on its own,
  which is how a problem that only happens sometimes gets measured.

Both are described for developers in [docs/BUILD.md](../BUILD.md).
