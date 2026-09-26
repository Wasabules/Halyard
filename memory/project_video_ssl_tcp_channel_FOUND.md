---
name: project-video-ssl-tcp-channel-found
description: "DECISIVE 2026-05-14 — The bottom slice arrives through VideoSslTcpChannel on port_base+20 (TLS+TCP), NOT UDP. Our code does not listen on that port → the bottom of the picture is always green."
metadata: 
  node_type: memory
  type: project
---

## The finding

An LD_PRELOAD capture, `/tmp/shadow_tls.log`, of the Shadow desktop (pid=21720, t=1778709224..) reveals:

- Le desktop ouvre `CONNECT fd=296 peer=<VM public IPv6> type=TCP`
- Suivi d'un handshake TLS (1er WRITE = `16 03 01 01 56 01 00 01 52 03 03 ...` = ClientHello classique)
- Then 38 TCP_WRITE/TCP_READ sections of sizes 23, 127, 4196, etc.
- Classe RE'd dans `ShadowPCDisplay` strings : `18VideoSslTcpChannel`, `15VideoTcpChannel`, `13VideoTcpFrame`
- Source path : `udu/deps/framework/src/common/video/src/video_ssl_tcp_channel.cpp`
- Events : `kVideoChannelTcpConnectSuccess` / `kVideoChannelTcpConnectionFailed`

## ⚠️ UPDATE 2026-05-14 ~01h25

**:11020 IS NOT the video's bottom slice.** The RE agent confirms:
- It is `VideoSslTcpChannel`, but it serves as a **TCP fallback / on-demand IDR retransmit**
- Observed throughput = 6.5 kbps (against 1 Mbps on `:11010` UDP) → far too low for the bottom video
- Format = STFP `tcp_frame.cpp` `[0x12 type][seq][u16le len][00 00][u32 id]` mostly zero payload (= keepalive)
- Our N24 probe confirms it: auth `41 01 00 14 00 [hash]` + a 'd' trigger → the server sends NOTHING (= no spontaneous push)

**The bottom slice probably travels on :11010 UDP** but our code filters it out / misses it. Our `flag10 & 1` filter skips 80 % of the packets (= Reed-Solomon parity). Perhaps:
- the bottom slice is encoded inside the parity chunks and needs RS decoding to reconstruct
- or it uses a subchannel range we do not handle
- or it is NOT sent at all by the server (= a native 1920×540 picture, with the SPS saying 1920×1080 by configuration)

→ for the rest see [[project-video-decode-38pct-solved]] and task N25.

## ~~Conclusion (initiale, OBSOLÈTE)~~

~~**The bottom slice of the 1920×1080 frame arrives over TCP on port_base+20**~~, not over UDP. Our client only connects to:
- `:9010` (port_base+10) UDP video → only top slices received → the decoder's bottom stays green (= a YUV placeholder)
- :9011 (port_base+11) TCP control = SslCtrlChanV2 (= bootstrap auth)

**Why**: the TCP/UDP split maps onto the SubChannels: UDP = primary (lossy), TCP = secondary (reliable, bottom + redundancy).

**How to apply**: implement a `ctrl_video_tcp_open()` that:
1. Connects over TLS on `port_base + 20`
2. Sends Capabilities (= probably the same format as SslCtrlChanV2)
3. Envoie Auth avec `streamingtoken` + `sessionUniqueId`
4. Parse `VideoTcpFrame` (= probablement length-prefixed H.264 NAL avec timestamp)
5. Feed the NALs into the same `g_display_buf` cross-subchan reassembly

Cf. [[project-native-port-base-discovery]] : table des ports
- +10 = UDP video primary
- +11 = TCP control (SslCtrlChanV2)
- +12 = UDP audio
- +13 = UDP input (+ TCP input via :11013 secondary?)
- +20 = **TCP video secondary (VideoSslTcpChannel) = BOTTOM SLICE** ⭐
- +30 = UDP cursor (+ TCP cursor via :11030)

Also confirmed: `CursorSslTcpChannel`, `InputSslTcpFlatBuffersChannel` and `TcpClipboardChannel` exist → a generic multi-channel SSL TCP architecture.
