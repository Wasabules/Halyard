---
name: project_channel_map_proven
description: The Shadow channel map is PROVEN — the official client names its sockets with their port in its telemetry
metadata:
  type: project
---

**Read this before any hypothesis about a channel.** Three lines of this map were
wrong and misled the project for months.

The official client NAMES each of its sockets with its port, in cleartext, in its
telemetry (`CreateAndConnectSocket`). Verified on two different port bases; the
server independently declares the same map in its announcement replies.

    upd: Video       :10010        tcp: Clipboard   :10014
    tcp: CtrlChanV2  :10011        tcp: Cursor      :10020
    upd: Input       :10012        upd: AudioOut    :10030
    upd: Gamepad     :10013        upd: AudioIn     :10032
                                   tcp: SftpClient  :10015

**What we believed, and what was false**:
- `+20` = a fallback video channel -> it is the **CURSOR**. We had been receiving
  its bitmaps all along and the `[RE11]` guard discarded them every session.
- `+30` = "cursor + audio multiplexed" -> it is **AudioOut alone**. The cursor
  packets we counted there were an artefact of `udp_cursor_pkts`, which counts
  EVERY packet on the port — that counter skewed hours of intermittency
  measurement.
- `+14` = input / ComChan -> it is the **CLIPBOARD**. Republishing anything there
  OVERWRITES the VM's clipboard.

**The channels' wire enum** (a name table read in the official binary):
`0=VIDEO 1=AUDIO 2=INPUT 3=CURSOR 4=MIC 5=CONTROLLER 6=CLIPBOARD 7=FILETRANSFER`.
It is what made it possible to decode `ctrlchanv2: Channel down: {} (sessionId={})`
and to resolve the intermittent audio
([[project_S57_audio_solved_we_spoke_too_early]]).

`ComChan` exists in the RTTI and in the official logs but **has no socket**: it is
carried by `+11` or is purely internal. Do not go looking for a port for it — that
is what D11 was doing on `+14`, hence its 0/5 sessions.

**The method that produced this result**: capture the official client one function
at a time and look for the CONTENT in cleartext (the clipboard was identified by
the copied text, the gamepad by a ×50 contrast). See
[[project_differential_captures_plan]]. KB.md §3.37.
