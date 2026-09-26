---
name: project-base15-silent-channel-RE
description: ":base+15 = SSH-2.0 + SFTPv3 (SftpClientChannel via libssh.so.4) — user-triggered Drag-and-Drop file transfer, idle in MASTER capture, zero impact on streaming / taskbar bug, safe to skip on Switch."
metadata:
  type: project
---

# `:base+15` SSH+SFTP channel — confirmed C95

## ⚠️ RECONFIRMED 2026-05-23

Independent byte-exact RE verification by `tools/ida/out/PORT_8014_8015_RESOLUTION.md`
(2026-05-23) re-validated this finding at **C95**. The conflicting TIER 7 claim
that `:base+15 = ComChan` is REFUTED — TIER 7 mismapped `ssl=0x283868a0` to
:8015 ; that pointer is actually bound to fd=322 = :8014 (ComChan). :8015 carries
**no TLS bytes at all** ; it is pure SSH-2.0-libssh_0.9.6 + SFTPv3 subsystem.

## What it is

Raw TCP+SSH-2.0 channel running the SFTPv3 subsystem. Class
**`SftpClientChannel`** (vtable @ `0x1283380`), source
`udu/deps/framework/src/common/filetransfer/src/sftp/sftp_client_channel.cpp`.
Uses **libssh.so.4** (`LIBSSH_4_5_0` symbol). Higher-level wrapper is
**`FileTransferChannel`** (`0x1282740`) which queues `FileTransferTransaction`
objects (upload/download) over an underlying `SftpClientChannel`.

## Capture-confirmed handshake

`MASTER-20260514-153936/tls_plain.log` fd=256 peer `:8015` :

- L27794 SEND 22 B = `"SSH-2.0-libssh_0.9.6\r\n"` (client banner)
- L28042 RECV 767 B = `"SSH-2.0-libssh_0.11.0\r\n"` + SSH_MSG_KEXINIT
- L28130 SEND 976 B = SSH_MSG_KEXINIT (client) — full algo list
  (`curve25519-sha256,ecdh-sha2-nistp256/384/521,dh-group18-sha512,…`)
- L28257 SEND 48 B = SSH_MSG_KEX_ECDH_INIT (32 B X25519 pubkey)
- L29889 RECV 208 B = SSH_MSG_KEX_ECDH_REPLY (ssh-ed25519 host key +
  64 B signature)
- L29992 SEND 16 B = SSH_MSG_NEWKEYS (msg type `0x15`)
- Post-NEWKEYS : encrypted SSH packets carrying SERVICE_REQUEST(ssh-userauth),
  USERAUTH (none/pubkey), CHANNEL_OPEN(session), CHANNEL_REQUEST(subsystem=sftp),
  SSH_FXP_INIT/VERSION (= SFTPv3 init)
- L1039704 SEND 36 B at t+234 s = SSH_MSG_DISCONNECT (graceful close)

Frame format = RFC 4253 SSH binary packet : `[u32_be len][u8 pad_len][payload][padding][mac]`.

## Why it was reported "silent" (M4 cross-check error)

`M4_CROSSCHECK_2026-05-16.md` §V1 wrote "complete silence on fd=256
until t=1232" — that's wrong. The 19 TCP_SEND/RECV events between
t=998.585 and t=999.05 ARE the SSH handshake + SFTP init. After
SFTP_INIT acks, the channel sits idle (no user-triggered file
transfer fired during the 234 s capture). M4 conflated "no
plaintext-decoded application bytes" with "silent" — but SSH
encrypts after NEWKEYS, so the LD_PRELOAD shim couldn't decode
post-handshake payload.

## TOFU host-key pinning hypothesis (C75)

`ssh_session_is_known_server` + `ssh_session_update_known_hosts` are
both linked. The ed25519 fingerprint surfaced in the `:base+12`
reply (per TIER6 M1) is **likely** the pinning anchor : the server
publishes the SSH host pubkey over the encrypted ctrl bus and the
SFTP TOFU code pre-populates `~/.config/shadow/known_hosts` before
the SFTP connect runs. Not byte-verified — flagged for any future
SFTP implementer.

## User-trigger events

`ShadowAppStartFileTransferEvent` / `ShadowAppCancelFileTransferEvent`
(catalogued in the giant `ShadowRenderer::Run` event variant). These
fire only on UI drag-and-drop or menu action — never observed in the
MASTER capture.

## Impact on shadow2switch project

- Taskbar / bottom-NAL image bug : **zero impact**. No video path.
- Session bring-up : **zero impact**. Server does not require this
  channel for streaming (Switch already streams without it).
- Switch port : **do not implement**. Adding libssh + curve25519 +
  ed25519 to the NRO is ~600 kB for a feature (drag-and-drop file
  transfer) Switch has no UI for. Skip entirely.
- Future "Switch pulls file from VM" feature : would need libssh
  port + a UI ; revisit if/when wanted.

## Refs

- Doc : `tools/ida/out/BASE15_SILENT_CHANNEL_RE.md`
- Capture : `06-shadow-recon-linux/captures/MASTER-20260514-153936/tls_plain.log` fd=256
- Binary : `06-shadow-recon-linux/ShadowPCDisplay` strings + `0x1283380` vtable
- Related : [[project-tier3-channel-port-catalog]], [[project-comchan-port-corrected]]
- Refuted prior claim : `tools/ida/out/M4_CROSSCHECK_2026-05-16.md` §V1 ("complete silence on fd=256") — fd=256 carried 19 SSH packets pre/post-NEWKEYS
