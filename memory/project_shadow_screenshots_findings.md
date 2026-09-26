---
name: Shadow Linux app — settings UI ↔ code parallel
description: Mapping the Linux Shadow app's screenshots against the binary RE — it reveals the TCP/UDP toggle, shadowusb for the gamepad, and so on.
type: project
---
Source : `$REPO/screenshotlinuxapp/` (8 screenshots, 2026-05-02). Sections couvertes : General / Video & Display / Audio / Controllers & USB / Network.

## Key findings

### 1. Le toggle "Streaming preferences"
**`Network > Streaming preferences = "Prefer speed (UDP)" | "Prefer reliability"`**
- THAT is the selector visible in the UI, choosing between raw TCP+UDP (UDP) and TCP-only ("reliability")
- On this account, "Prefer speed (UDP)" is active → the pcap shows UDP at slot×1000+offset
- Under "Prefer reliability" → everything would go over TCP (the binary's `*TcpChannel` classes become active)
- **No third QUIC option** in the UI → confirming that QUIC is dormant code

### 2. Gamepad via daemon SÉPARÉ : shadowusb
**`Controllers & USB`** :
- "0/4 controllers connected — Plug in your controller and press on any button"
- "Use my USB devices on my Shadow — sudo apt install shadowusb"
- A separate Linux daemon that forwards physical USB devices to the VM
- Consistent with the captured JSON: `[{"client_type":"launcher"},{"client_type":"main"},{"client_type":"usb"}]`
- The `client_type: "usb"` = the shadowusb registration (separate from the main streaming)

**A major implication**: for the Switch homebrew, porting the gamepad is not about implementing `Shadow::Protocols::Controller::Client::*` (the FlatBuffer GamepadChannel) **natively**. It is **USB-over-network forwarding** where the local client acts as a virtual USB hub.

The revised plan for the gamepad on the Switch:
- Or RE shadowusb (the apt daemon) to understand the USB-forwarding protocol
- Or implement the native `GamepadChannel` if the server accepts it (gated server-side, not granted for this account)

### 3. Codecs et options runtime
- An HEVC/H.265 toggle exists (against H.264 by default)
- Color Enhancement 4:4:4 = full chroma sampling
- A limited frame rate, up to 240 fps (bold!)
- Software decoding fallback toggle (`videoFallbackSoftware` string)
- Renderer : Default/Vaapi/FFmpeg (3 backends Linux)
- Decoder : Default/OpenGL/VaX11 (3 backends Linux GPU)
- Audio quality : Regular (Opus) / High (Flac lossless `FlacAudioDecoder`)

On the Switch we have NVDEC fixed to H.264 and Opus audio, so we follow the defaults.

### 4. Quick Menu (overlay)
**`General > Enable Quick Menu`** : "interface over the streaming to control your experience and monitor streaming stats. Disable to fix performances issues on low-end configurations."
→ the equivalent of our in-stream Switch menu (Continue/Stats/Quit). Our current implementation is consistent.

### 5. Bitrate config
Network > Bitrate: a 1-50 Mbps slider, 20 by default. "up to 8 GB per hour".
The backend = the `setBitrate` event observed in the rtti. The Switch can hardcode 5-10 Mbps to stay sensible.

## How to apply

For the Switch port:
1. **Keep UDP mode** = "Prefer speed"; it matches what we have already documented
2. **Skip QUIC** = dormant code, not reachable on the user's server side
3. **The gamepad is complex**: not a simple protocol to reproduce (USB-over-network through the shadowusb daemon). Plan B for M15+ on Switch: implement the native `GamepadChannel` and test whether the server accepts it with a different account. Plan C: Joy-Con → an emulated USB HID through the usb channel (hard on Switch HOS, which has no easy USB passthrough)
4. **H.264 + Opus are enough** for the initial port
