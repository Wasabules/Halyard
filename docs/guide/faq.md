# FAQ and troubleshooting

## Questions people ask

### Do I need a Shadow subscription?

Yes. Halyard is a client: it signs in as you, with your own account, to your own
machine. Without a subscription it has nothing to connect to.

### Is it safe for my account?

You sign in through Shadow's own login page, and Halyard circumvents nothing — no
authentication, no payment, no DRM. But using an unofficial client may still
breach Shadow's terms of service, and your account is what is at risk. Read the
[legal notes](../LEGAL.md) before you install.

### Is it made by Shadow, Nintendo or Sony?

No. Halyard is an independent, unofficial project, and none of those companies
has anything to do with it. Their names appear only to say which service it talks
to and which hardware it runs on.

### Which consoles does it run on?

A Nintendo Switch running Atmosphère, and a PS Vita or PS TV running HENkaku or
h-encore. What differs between them is listed in
[Controls](controls.md) and on the [home page](../../README.md#what-works-per-console).

### Is there a PC, Mac or web version?

Not as a product. Halyard also builds on Linux and Windows, but those builds are
for developing and testing it: the interface is made for a gamepad and a console
screen. On a computer, Shadow's own apps are the better choice.

A web version is not possible as Halyard is built: it talks to your machine over
raw network connections that a web page is not allowed to open.

### Does it cost anything?

No. Halyard is free software (GPL-3.0-or-later): you can read, build and share it.

### How do I update it?

Download the new release and replace the file: copy the new `halyard.nro` over
the old one on the Switch, or install the new `halyard.vpk` over the old app on
the Vita. Your settings and your session are kept. See
[Install and sign in](../INSTALL.md#updating).

### How do I sign out?

Settings › Account › **Sign out**. It forgets the session and quits; the next
launch asks you to link the console again. Deleting `refresh_token` from the data
directory does the same.

### Which version am I running?

Settings › About shows the version and the build. On the Switch, the homebrew
menu shows it too.

## When something goes wrong

### "No internet access" when it starts

Check the console's own connection first. On a Switch that has been on for a long
time, the system's network service is sometimes left in a bad state: restart the
console.

### The sign-in code never gets accepted

The code is valid for a few minutes, counted down on screen. If it runs out, press
**A** to get a new one. Type it on the address shown on the console, signed in to
the Shadow account that owns the machine.

### My machine does not appear, or does not start

The list shows the machines of the account you linked. A machine that is off is
started when you pick it, which can take a minute or two. If the list is empty or
wrong, refresh it; if it stays wrong, check the machine from Shadow's own app.

### "Session already open on another device"

Shadow allows one session per machine. If you have just left a session on a
computer or another console, give it a moment to close, then try again.

### The picture breaks up while playing

Lower the maximum bitrate to about 25 Mb/s — in the pause menu, or in Settings.
Above that, over Wi-Fi, the server probes for more than the link carries and loses
data on every probe. See [Picture, sound and network](performance.md).

### Short freezes every so often

On Wi-Fi, the network stalling for a fraction of a second is the usual cause, and
no setting can prevent it. A wired connection (a Switch in its dock with an
Ethernet adapter) helps more than anything else. The performance panel shows the
loss and the round trip while you play.

### There is no sound

Check the volume in the pause menu. If the sound stops after reconnecting, note
the version from Settings › About and report it. The log says which audio format
the server granted and whether it is being decoded.

### The controls do not reach the game

Some games only react to a gamepad, others only to a mouse and keyboard. Check
[Controls](controls.md) for how the buttons are sent, and the settings that
change it: sending the console's gamepad to the machine, and the mouse mode for
games that capture the pointer.

### The app crashed

Relaunch it. On the Switch, Atmosphère writes a report under
`/atmosphere/crash_reports/`; on the Vita, a `psp2core-*.psp2dmp` file appears in
`ux0:data/`. Both help a bug report — see [Advanced](advanced.md#when-the-app-crashes).

## Reporting a problem

Open an issue on [GitHub](https://github.com/Wasabules/Halyard/issues): the form
asks for the console, the version and what happened. Quote a few lines of
`halyard.log` around the problem if you can, **after reading them** — never
attach a whole log or a network capture.
