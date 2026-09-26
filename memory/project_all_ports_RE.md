---
name: project-all-ports-re
description: TIER3 — Full Channel class inventory + port-offset table (47 classes, +14/+15/+32 still unidentified, RemoteBitrateEstimator candidat)
metadata:
  type: project
---

## TL;DR

TIER3 2026-05-16 : exhaustive scan of `ShadowPCDisplay` Channel classes via RTTI/typeinfo strings (`tools/ida/out/tier3/INDEX_TIER3.md` §H.3-§H.4). **47 distinct `*Channel*` classes** catalogued, mapped to Tcp/Udp/SslTcp/SslUdp variants per stream type.

**No new port offset hardcoded in the binary**. Channel ctors take port as `unsigned __int16` argument (e.g. `sub_C13350(VideoTcpChannel)::a4`). Port assignment is done by the **Electron launcher** which gets the table from `/clients` POST response and forwards via CLI flags.

## Known port table (C95)

| Offset | Proto | Class | Purpose |
|--------|-------|-------|---------|
| `+10` | UDP | `VideoUdpChannel` | SUFP video chacha20-poly1305 |
| `+11` | TCP+TLS | `SslCtrlChanV2Channel` | Ctrl proto (length-delim) |
| `+12` | UDP | `AudioOutUdpChannel` | DTLS-SRTP Opus |
| `+13` | UDP | `InputSslUdpFlatBuffersChannel` | Input chacha20 flatbuffers |
| `+14` | TCP | **?** (observed in capture, no class match) | candidate : `ComChanChannel` / `RemoteBitrateEstimatorIOChannel` |
| `+15` | TCP | **?** | candidate : `RemoteBitrateEstimatorIOChannel` |
| `+20` | TCP+TLS | `VideoSslTcpChannel` | STFP IDR retransmit / keepalive |
| `+30` | UDP | `CursorUdpChannel` | Cursor chacha20 |
| `+32` | UDP | **?** (observed in capture) | candidate : `AudioInUdpChannel` (mic upstream) |

## Channel class full inventory (C95)

From `INDEX_TIER3.md` :

- **Video**     : `VideoTcpChannel`, `VideoSslTcpChannel`, `VideoUdpChannel`, `VideoChannel`
- **Audio In**  : `AudioInTcpChannel`, `AudioInUdpChannel`, `AudioInChannel`
- **Audio Out** : `AudioOutTcpChannel`, `AudioOutSslTcpChannel`, `AudioOutUdpChannel`, `AudioOutChannel`
- **Cursor**    : `CursorTcpChannel`, `CursorSslTcpChannel`, `CursorUdpChannel`, `CursorChannel`
- **Input**     : `InputTcpChannel`, `InputSslChannel`, `InputUdpChannel`, `InputSslTcpFlatBuffersChannel`, `InputSslUdpFlatBuffersChannel`, `InputTcpFlatBuffersChannel`, `InputChannel`, `InputFlatBuffersChannel`
- **Clipboard** : `TcpClipboardChannel`, `SslClipboardChannel`, `ClipboardChannel`, `NetworkIOChannel`
- **FileTrans** : `FileTransferChannel`, `SftpClientChannel`
- **Gamepad**   : `GamepadChannel` (via spice-html5 USB-redir, **not direct TCP/UDP**)
- **ComChan**   : `ComChanChannel`
- **VmProxy**   : `VmProxyHttpChannel` (REST)
- **Ctrl**      : `SslCtrlChanV2Channel`
- **Bitrate**   : ⭐ `RemoteBitrateEstimatorIOChannel` (0x12A9D20) — **new finding**
- **Base IO**   : `TcpIOChannel`, `UdpIOChannel`, `SslTcpIOChannel`, `SslUdpIOChannel`, `SslIOChannel`, `StfpSslTcpIOChannel`, `SftpTcpIOChannel`, `SufpUdpIOChannel`, `MessageIOChannel`, `PausableIOChannel`, `PipeIOChannel`, `PingableChannel`, `TimeoutUdpIOChannel`, `CallableChannel`, `CancelableChannel`, `HttpChannel`, `CurlHttpChannel`

## Why no port arithmetic in this binary

Streaming channel ctors like `VideoTcpChannel::sub_C13350` :

```c
(decompiled excerpt omitted from the public edition - the finding it supported is stated in the text)
```

= port is passed in by caller chain ultimately from launcher CLI (cf. `project_overnight_re_2026-05-03.md`). The renderer never computes `base + N`.

## Unexpected finding

`RemoteBitrateEstimatorIOChannel` exists as a real class (RTTI 0x12A9D20). **If desktop opens this channel** and our Switch client doesn't, the server may downgrade to a "low-bandwidth client" encoder profile that doesn't pack multi-NAL chunks. Could explain why server emits 0.81% bottom slice on Switch vs 11% on desktop. **Candidate test** : LD_PRELOAD capture filtered on `:base+14` and `:base+15` to see uplink payload (= probably JSON or protobuf `{bandwidth: X kbps, loss: Y%, jitter: Z ms}`).

## Refs

- IDA dumps : `tools/ida/out/tier3/INDEX_TIER3.md`, `INDEX_TIER3_v2.md`, `INDEX_TIER3_v3.md`
- Source script : `tools/ida/tier3_re.py` + `tier3_re2.py` + `tier3_re3.py`
- Memory cross-ref :
  - [[project-native-port-base-discovery]] — port_base varies by VM
  - [[project-overnight-re-2026-05-03]] — launcher → renderer CLI flags
  - [[project-shadow-real-protocol-confirmed]] — UDP/TCP slot×1000+offset
  - [[project-image-50pct-state-2026-05-15]] — server discriminates Switch vs desktop
- Doc : `tools/ida/out/TIER3_RE_2026-05-16.md`

## Action items

- Probe `:base+14`, `:base+15` via LD_PRELOAD with extended port filter
- If `RemoteBitrateEstimatorIOChannel` traffic observed → RE its proto + emit equivalent from Switch
- Update Switch `ctrl_session.c` port-probe to include +14/+15 as future-candidates (currently only +11)
