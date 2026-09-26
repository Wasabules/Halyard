---
name: project-channel-announcements-8msgs-RE
description: "2026-05-23 — 8 channel announcements byte-exact DECODED. Mapping outer fN → channel type identified (f1=Video, f2=Audio, f3=Input, f4=Cursor, f5=Micro, f6=Controller, f7=Clipboard, f8=FileTransfer). CI_BODY_5..12 confirmed byte-exact desktop. Multi-NAL trigger NOT in this layer — candidate = RegisterSession_StreamingProtocol.protoType (= SSUFP vs SUFP, C70 hypothesis untested)."
metadata:
  type: project
---

## TL;DR
- KB.md §3.8 upgraded: **[C70] TODO RE → [C95] DECODED** for the 8 channel
  announcements desktop send post-RegisterSession sur :base+11.
- Cross-referenced IDA Pro RTTI strings + a diff against the MASTER capture tls_plain.log
  byte by byte = each CI_BODY_5..12 in `ctrl_msgs.c` is **byte-exact with the desktop**.
- **Multi-NAL trigger NOT identified** in this layer (= the H2 RE9/RE10 verdict
  stand). Holy-grail candidate = `RegisterSession_StreamingProtocol.protoType=SSUFP`
  (= a field we do not send), a C70 hypothesis, untested live.

## Mapping channels → outer field number

Each of the 8 `Message{f1=seq, f2=Request{f8=ChannelInfo{...}}, ...}` carries
a single channel registration. Outer fN within ChannelInfo = channel type :

| seq | size | outer fN | channel       | inner content (= RegisterSession_<Type> proto) |
|-----|------|----------|---------------|------------------------------------------------|
| 5   | 119B | f1       | **Video**     | codec_config{H.264,profile=1}, resolution{1920×1080@143.85fps}, max_bitrate=163742208 |
| 6   | 103B | f4       | **Cursor**    | f4{f2=4, f3=1, f4=1} |
| 7   | 103B | f3       | **Input**     | f4{f1=4, f4=1}, empty resolution |
| 8   | 107B | f2       | **Audio**     | f4{f4=1}, sample_rate=48000, bit_depth=16, channels=1 |
| 9   | 99B  | f6       | **Controller**| f4{f4=1} (minimal) |
| 10  | 101B | f7       | **Clipboard** | f4{f3=1, f4=1} |
| 11  | 107B | f5       | **Micro**     | identical Audio (48k/16/1) |
| 12  | 103B | f8       | **FileTransfer**| f4{f1=5 (=SFTP), f3=1, f4=1} |

**Source** :
- Capture `06-shadow-recon-linux/captures/MASTER-20260514-153936/tls_plain.log`
  lines 20759, 21805, 21822, 21839, 21857, 21874, 21891, 22953.
- IDA Pro RTTI mangled strings @ 0x131A3A0..0x131A6E0 (= 11 RegisterSession_<Type>
  proto types).
- IDA dumps `tools/ida/out/channels/FINAL_<channel>_serB_*.c` (= 8 per-channel
  proto serializers identified via descriptor table 0x131C000-0x131D400 with
  type_info ptr at slot +112).

## Diff vs current code

`05-shadow-client-borealis/demo/src/streaming/ctrl_msgs.c:533-560` CI_BODY_5..12
sont **byte-exact desktop** (verified line-by-line vs capture). No code change
needed for wire fidelity.

## Multi-NAL trigger hypotheses (= holy grail residual)

Eliminated (C0) :
- 5 bool fields in RegisterSession_Video (f4..f6, f8, f10) — RE9/RE10 tested all
  combos live A/B 2026-05-18, none triggered multi-NAL.

Candidates non-tested :
- **C70 best candidate** : `RegisterSession_StreamingProtocol.protoType` enum
  value. Desktop may send `SSUFP=4` (= Secure SUFP) instead of `SUFP=5`. Our
  current code doesn't send this field at all → server defaults to legacy
  mono-NAL. cf TIER1_RE §B.3.
- C60 alternative : Capabilities ctrl-msg 7 capability strings advertise (=
  `display_management, gamepads, network_notifications, dynamic_bitrate,
  multiscreen, streaming_profile, audio_out_codec`). cf H2_H3 §IMPL.H3.

## Impact projet Switch
- **No code change required** for the Switch port — the bootstrap is byte-exact
  preserved.
- **R&D follow-up** (= code recipe dans `tools/ida/out/CHANNEL_ANNOUNCEMENTS_RE.md` §5.2)
  : ajouter `ctrl_build_streaming_protocol_announcement(seq, proto_type)` pour
  tester l'override protoType. Si SSUFP active multi-NAL chunks server-side,
  that is the holy grail.

## Cross-refs
- [[project-RE9-RE10-field-semantics-2026-05-18]] — bool tests verdict
- [[project-ctrl-oneof-enum-VERIFIED]] — 26 oneof cases du Request proto
- [[project-native-bootstrap-VALIDATED]] — full bootstrap sequence
- [[project-image-100pct-PROOF]] — 10.7% bottom NAL desktop reference
- [[project-PARITY-RAW-FIX-2026-05-15]] — V11 lifts bottom NAL ratio 0.81% → 49.7%
- KB.md §3.8 (= this entry upgrades it from C70 to C95)

## Refs
- Doc complet : `tools/ida/out/CHANNEL_ANNOUNCEMENTS_RE.md`
- Capture proof : `06-shadow-recon-linux/captures/MASTER-20260514-153936/tls_plain.log`
  lines 20759..22960
- Scripts IDA : `tools/ida/channels_re{1,2,3,4}.py`
- Dumps IDA : `tools/ida/out/channels/`
- Code wire-exact : `05-shadow-client-borealis/demo/src/streaming/ctrl_msgs.c::CI_BODY_5..12` (lignes 533-571)
