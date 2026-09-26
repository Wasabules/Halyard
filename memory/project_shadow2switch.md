---
name: shadow2switch project goal
description: Long-term goal of the $REPO project — get Shadow PC (cloud gaming) usable on a Nintendo Switch via homebrew
type: project
---
The user's end goal: run **Shadow PC** (Shadow.tech's cloud-gaming service) on their **Nintendo Switch** through homebrew. The Switch already has **Atmosphère** installed.

Several approaches are to be tested:
1. Through the **Shadow web page** (shadow.tech) opened with the Switch's **web applet** (libnx `webCommonConfig`).
2. **Moonlight-NX + Sunshine** installed inside the Shadow Windows VM (the most realistic path for real quality).
3. Mapping the Switch **gamepad** to Shadow inputs (Joy-Con/Pro Controller through libnx HID, already on the homebrew side).
4. Possibly a native port of a Shadow client (very unlikely — the official client is closed-source).

**Why:** a personal project, a multi-method exploration — no deadline, no service-quality commitment. The goal is learning Switch homebrew development AND getting Shadow to stream.

**How to apply:**
- Working dir: `$REPO` (created empty on 2026-04-29).
- Always start by validating the ground (toolchain, hello-world, nxlink) before attacking Shadow.
- When "Shadow PC" is said without context: it means the Shadow.tech service, **not** a Sega Shadow or anything else.
- The current choice (2026-04-30): **Route A — a full native Switch port**; the user wants complete understanding.
- The Shadow APK recon is finished (v3.33.2). The wire format = **Protocol Buffers + msquic** (QUIC). ALPN = `shadow`. OIDC Device Grant auth through Hydra. Full documentation in `03-shadow-recon/PROTOCOL.md` + `WIRE_FORMAT.md`.
- Client_id OAuth prod extrait : `7aa43568-5704-492e-8faf-147e8e68b5a3`. Scope : `openid email vm_access api profile offline`.
- Remaining blocking unknowns for a native port: (1) the protobuf field numbers, (2) the binary headers of the Video/Audio frames, (3) per-channel crypto, (4) the capabilities bitfield. Estimated at 2-4 more weeks of RE.
- **Ghidra 12.0.4 installed on 2026-04-30**: `/opt/ghidra/`, launched through `ghidra` or `/opt/ghidra/ghidraRun`. Java 21 configured (the default). The RE projects live under `$REPO/03-shadow-recon/ghidra-projects/`.
- For the RE: import `arm64/lib/armeabi-v7a/libAwesomeClientProject.so` (13.7 MB, ARMv7), the analysis takes 5-15 min. Priority targets: `ControlProto::*::_InternalParse` for the field names and `_InternalSerialize` for the tags; `CtrlChanV2Manager::Authenticate` for the capabilities bitfield; `EnableEncryption` + `CryptoFactory` for the key derivation.
