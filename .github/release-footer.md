
---

### Downloads

| File | For | How |
|---|---|---|
| `halyard.nro` | Nintendo Switch (Atmosphère) | copy to `/switch/` on the SD card |
| `halyard.vpk` | PS Vita / PS TV (HENkaku, Ensō) | install with VitaShell |
| `corresponding-source.tar.gz` | anyone | the GPL-3.0-or-later Corresponding Source of the two packages |

The source bundle is this repository at the tag, every library at the exact
commit it was built from, the FFmpeg 7.1.5 release tarball with the nvtegra
patch for the Switch decoder, our other patches and the build scripts. It has
no git history, so it carries a `VERSION` file that CMake reads instead of the
tag.

Install guide: [docs/INSTALL.md](https://github.com/Wasabules/halyard/blob/main/docs/INSTALL.md).
Coming from a build named `shadow-client`: move its data folder, see the guide.

These packages are built by CI. A green build means they link and package — it
does **not** mean a session was played on hardware.

**Unofficial.** Not affiliated with, endorsed by, or sponsored by Shadow,
Nintendo, or Sony Interactive Entertainment. You need your own Shadow
subscription, and using an unofficial client may breach Shadow's terms of
service — read [docs/LEGAL.md](https://github.com/Wasabules/halyard/blob/main/docs/LEGAL.md)
before installing.
