---
name: project-image-50pct-root-cause
description: "2026-05-14 — The Shadow server sends 50% of the picture's top whatever the client's configuration. Every client-side lead is exhausted. The limitation is server-side, and not negotiable with the messages we have RE'd."
metadata: 
  node_type: memory
  type: project
---

## Confirmed facts

1. **The server sends UDP video ONLY on :11010** (= 10240 packets in the 60 s desktop capture). No additional port.
2. **EVERY slice received is `first_mb=0`** (= the top half). 0 % bottom slices across 1042 reassembled NALs.
3. **The decoded picture = the TOP 50% of the desktop** (= the Steam/Edge icons are visible, the taskbar is absent).
4. **Server-side scaling = 50% of requested height**:
   - height=540 → 270 effective (= the wrong half visible)
   - height=1080 → 540 effective (= the baseline)
   - height=2160 → the server REJECTS it (= 0 video packets received)

## Pistes client-side TESTÉES et ÉCHOUÉES

| Attempt | Result |
|---|---|
| N23 IDR injection (= prepend SPS+PPS+IDR_top devant IDR_bottom) | KO — IDR bottom trop rare |
| N24 VideoSslTcpChannel :11020 | It is an STFP TCP fallback, not the bottom |
| N25 BOTTOM_INJECT with a memorised slice | KO — a frame_num mismatch broke the decoder |
| N26 RegisterSession height=540 | The server adapts the SPS but still sends 50 % |
| N26b height=2160 | Server REJETTE (= 0 video pkts) |
| N27 baseline clamp 540 | ✅ Workaround visuel (= image utilisable 1920×540) |
| N29 rG NACK packets | KO — cassait la session (392→66 pps) |
| N30 an audit of the parity chunks | KO — random bytes, no hidden NAL |
| N30b the viewport_height string | It is just a metric counter |

## ctrl messages sent (byte-exact with the desktop)

- seq=0..2 : Capabilities/Auth/Encryption (= bootstrap)
- seq=3 : Heartbeat
- seq=4 : RegisterSession (= avec width=1920 height=1080)
- seq=5..12 : 8 ChannelAnnouncements (= **seq=5 video f8{f1{f1{f4{f2=2,f4=1}},f2{w=1920,h=1080,fps=143.85f}},f7=19716864}**)
- seq=17 : VideoEncodingConfig (= bitrate=1024561, fps=142.0f)
- seq=18..28 : Heartbeats
- seq=29 : Ready (= f7 empty)
- seq=30 : DisplayReady (= f6{f1{f1=1},f2 empty})

All byte-exact with the desktop. No identifiable missing message.

## Pistes restantes (= demandent plus de captures/RE)

1. **A desktop capture while moving the mouse AT THE BOTTOM of the screen** → it may trigger a bottom-slice send
2. **A desktop capture with another account / another VM** → compare, to see whether the 50 % limit is universal
3. **A complete RE of the `RequestIFrame` vtable per output_id** → find the correct byte-exact iP packet format
4. **Decrypt OUR OWN UDP video packets (OUR chunks)** + dump ALL the plaintext bytes to see whether the bottom is HIDDEN inside some rare chunks
5. **Test with H.265 or AV1** instead of H.264 in VideoEncodingConfig → the codec may change the policy
6. **Run the LD_PRELOAD while the desktop plays a REAL GAME** → the bottom slice is probably sent if there is graphical activity at the bottom

## How to apply

When working on Shadow's image quality / resolution:
- The `1920×540` baseline (= the height/2 clamp) remains the accepted standard
- Any regression must come back to 28 fps + 4 decode errors per 30 s + 100% decrypt
- To go further, capture a desktop session with activity at the bottom of the screen (= sub-windows down there)
