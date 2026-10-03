# Changelog

What changes between releases, for someone who uses Halyard. The protocol
findings behind these changes live in [`KB.md`](KB.md); the build and packaging
detail in the commit history.

A release is a `vX.Y.Z` tag, and the release workflow refuses a tag that has no
section here. Versions follow [Semantic Versioning](https://semver.org/); before
1.0.0, a minor version may change behaviour.

## [Unreleased]

A desktop client, and what came out of reading the replies we were already
receiving.

### New — the Qt desktop client

- **A desktop client for Linux, Windows and macOS.** Sign in, pick a machine,
  stream it, with the picture, the sound and your keyboard, mouse and gamepad
  going back the other way.
- **A file manager**, two panes like an FTP client, with a transfer panel that
  says which files are running, which failed and why, and a progress bar for
  each.
- **An in-stream overlay** on a key you choose: volume, equaliser, which
  metrics show in the corner, a screenshot, and a HUD whose blocks you can
  arrange.
- **Metrics** matching the console client's, graph for graph.
- **An Account window**: your plan, how much of the monthly allowance is
  spent, how long a session may last, and what your account is allowed to do.
- **Sign-in by QR code** — scan it with a phone instead of retyping eight
  characters.
- **Configurable keyboard shortcuts**, with a warning when two commands share
  one.
- **A light/dark/automatic theme**, and animations that honour the system's
  "reduce animations" setting.

### Fixed

- **Sending a file to the machine failed every time.** The client treated a
  partial write as an error, and the SSH library splits every write larger
  than its packet size — so every upload above 64 KiB reported a failure it
  had not had.
- **Signing out now tells Shadow.** It used to delete the saved session from
  this computer and leave it valid on the server, so a copy of the file kept
  working.
- **The client no longer leaves registrations behind on your machine.** Every
  session registered two clients with the VM and deleted neither.
- **A machine under maintenance says so** instead of accepting a connection
  that fails halfway through.
- **The data centre shown is the machine's own**, not the account's.

### Known

- The Switch and PS Vita clients are unchanged in this release.
- The desktop client streams one screen. Shadow allows more on some machines;
  the client does not ask for them yet.

## [0.1.0] - 2026-09-26

The first public release.

### What it does

- **Streams your Shadow cloud PC to a Nintendo Switch or a PS Vita**: sign in
  with your Shadow account, pick your machine, play. Picture, sound, and your
  inputs going back the other way.
- **Hardware video decoding on both consoles** — H.264 and HEVC on Switch,
  H.264 on Vita.
- **Audio in Opus or FLAC**, whichever the server grants, with a 5-band
  equaliser and per-mode profiles.
- **Gamepad, mouse and keyboard reach the VM**: the sticks or the touchscreen
  drive a mouse, and there is an on-screen keyboard. Rumble and gyroscope aiming
  on Switch.
- **The VM's own mouse pointer** is drawn.
- **It measures itself**: latency by stage, packet loss and the link's estimated
  capacity, in the pause menu while you play.
- In-app testers for the network, the pad and the mouse; an optional lock on
  opening the app.

### Known limitations

- The VM's **clipboard** and **microphone** are not served.
- On PS Vita: **H.264 only**, no rumble, no gyroscope — hardware limits, not
  pending work. The Vita port has far fewer hours of play than the Switch one.
- Above about **25 Mb/s** the picture breaks up in play over Wi-Fi: cap the
  bitrate in the pause menu.
- Wi-Fi stalls of a few hundred milliseconds show as short freezes; a wired link
  helps more than any setting.

### Coming from a build named `shadow-client`

The data folder moved to `/switch/halyard/` (Switch) and `ux0:data/halyard/`
(Vita). Move the old folder's contents there, or sign in again — see
[`docs/INSTALL.md`](docs/INSTALL.md#coming-from-shadow-client).

[0.1.0]: https://github.com/Wasabules/halyard/releases/tag/v0.1.0
