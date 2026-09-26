# Installing Halyard

You need a **Shadow subscription** and a console that already runs homebrew.
Halyard does not jailbreak anything and does not get you a subscription; it is
a client for a service you pay for.

> ⚠️ **Before you install: the risk is your account.** Connecting with an
> unofficial client may breach Shadow's terms of service, and they may suspend
> or terminate your subscription for it. Nobody involved in this project can
> prevent that, appeal it for you, or compensate you — the licence disclaims all
> warranty and liability. The full position, including what this client
> deliberately does **not** do and where EU law stands on interoperability
> research, is in [`LEGAL.md`](LEGAL.md). Decide with that in hand.

## Nintendo Switch

**Requires** Atmosphère (or an equivalent CFW) and the homebrew menu.

1. Copy `halyard.nro` to `/switch/` on the SD card.
2. Put the card back, boot into CFW, open the homebrew menu.
3. Launch **Halyard**.

Launch it from the **album/homebrew menu with full memory** if you can — a
title launched through the album applet gets a smaller memory budget, and the
hardware decoder wants room. If the picture never appears and the log complains
about allocation, that is usually why.

The app creates `/switch/halyard/` on first run and keeps its settings, its
token and its logs there.

## PS Vita / PS TV

**Requires** HENkaku or h-encore, and VitaShell.

1. Copy `halyard.vpk` to the console (VitaShell's FTP, USB, or a card).
2. In VitaShell, press ✕ on the `.vpk` and confirm the install.
3. Launch **Halyard** from LiveArea.

The app creates `ux0:data/halyard/` on first run.

Two things are permanently absent on this console: **rumble** (no such hardware)
and **gyroscope aiming** (not wired to SceMotion). Video is **H.264 only** —
the Vita's decoder does not do HEVC, so the codec choice does not appear in the
settings.

## Signing in

The first launch shows a short **code** and a URL. Open that URL on a phone or a
computer, sign in to Shadow, and type the code. The console picks up the session
by itself within a few seconds — there is nothing to type on the console.

The refresh token is then kept in the data directory so you do not repeat this.
It is stored **obfuscated, not encrypted**: anyone with the SD card can recover
it. The optional app lock (Settings › Security) protects *opening the app*, not
the data beside it.

To sign out, delete `refresh_token` from the data directory.

## First session

Pick your machine from the list; it starts if it is not already running.

Then open the pause menu: **hold + (Switch) or START (Vita) for half a
second**. A short press is not the menu — it sends the gamepad's Options button
through to the VM, which is usually the game's own menu.

- **Cap the bitrate at about 25 Mb/s.** Above that the picture breaks up in
  play — the server probes beyond what the link carries and loses on every
  probe. This is the single setting most worth changing.
- Check the **link quality** page if the picture is poor. It reports loss, the
  round trip and what the path looks like it can carry.
- The **latency** page breaks the delay into eleven stages, so you can see
  whether a problem is the network or the console.

## Settings, and the file that beats them

Settings live in the app and persist in `settings.txt`. Underneath, each drives
a `SHADOW_*` variable.

A console passes no environment, so a **file** does it: `env.txt` in the data
directory, one `KEY=VALUE` per line, `#` for comments. Only keys starting with
`SHADOW_` are honoured — the file comes off a memory card and has no business
redefining `PATH`.

```
# env.txt
SHADOW_BITRATE_MBPS=25
SHADOW_JOURNAL_NIVEAU=3
```

**`env.txt` wins over the settings screen.** That is deliberate — a test must be
able to impose a value without going through menus — and the app marks
overridden settings on screen so the priority is visible. If what you measure
disagrees with what the screen says, read `env.txt` first.

Other files the app reads from the same directory:

| File | What it does |
|---|---|
| `env.txt` | the toggles above |
| `settings.txt` | what the settings screen saved |
| `refresh_token` | your session; delete to sign out |
| `logsink.txt` | `host:port` to mirror the log to a PC (dev) |
| `autotest.txt` | chains sessions unattended (dev) |
| `applock.txt` | the app lock |

## Updating

Replace the `.nro`, or reinstall the `.vpk`. Settings and your token survive —
they are in the data directory, which the package does not touch.

## Uninstalling

Delete the `.nro` (Switch) or uninstall from LiveArea (Vita), then remove the
data directory to take the token and logs with it.

## Getting help

Read `halyard.log` in the data directory first; it usually names the problem.
The [README's troubleshooting section](../README.md#when-something-goes-wrong)
covers the common ones.

**Before attaching a log to an issue, read it.** Logs carry session material.
See [`SECURITY.md`](../SECURITY.md).
