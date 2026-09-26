---
name: Shadow WebRTC H.264 stream specifics
description: Quirks of the H.264 Shadow sends over WebRTC (a legacy account) — SPS parameters, multi-slice per frame, a sensitive profile-level-id
type: project
---
Found by RE of the `chrome://webrtc-internals` dump (Chrome → the Shadow web client) + the NAL log on the Switch side.

**SDP profile-level-id: MUST be `42001f`, NOT `42e01f`**

With `42e01f` (constrained baseline), Shadow sends the IDR without SPS/PPS in the bitstream → libavcodec rejects everything. With `42001f` (unconstrained baseline, what Chrome negotiates), Shadow sends SPS+PPS in every keyframe (`sps-pps-idr-in-keyframe=1`). The constraint byte (`0xe0` against `0x00`) switches Shadow from "minimal client" mode to "full client" mode.

**Encoder Shadow** :
- profile_idc=100 (HIGH, not baseline, despite what we ask for)
- level_idc=52 (5.2, capable 4K/60)
- num_ref_frames=1 (1 seule reference frame, ultra-low-latency)
- pic_order_cnt_type=2 (no B-frames)
- chroma=4:2:0
- 1280x720 fixe

**Multi-slice per picture**: Shadow typically emits 2 IDR slices per keyframe (e.g. 3634 + 4924 bytes) with the SAME RTP timestamp. The P frames also often have 2-3 slices per picture (from 18 bytes to several KB). A crucial consequence: **every NAL of a given PTS must be aggregated into ONE packet** before `avcodec_send_packet`. Otherwise libavcodec emits 1 AVFrame per slice (with only its decoded area and the rest blank) → a visible horizontal band that scrolls.

**RTCP feedback**: PLI is enough (Chrome sends 7 PLIs over a whole session of 789 frames, 0 NACK, 0 FIR). There is no need to send FIR — Shadow answers PLI just as well once the right profile-level-id has been negotiated. It throttles the bitrate if there is no regular RR (1/s).

**Why:** without those 3 things (profile-level-id=42001f, AU aggregation, PLI is enough), the decoder produces only partial frames or nothing at all. It is not obvious from the H.264 spec — Shadow has specific behaviour depending on what you announce.

**How to apply:** for M13 and any later polish, do not touch the profile-level-id or the AU aggregation strategy in `h264_decoder.c`. If you touch the SDP offer (`sdp.c`), keep `42001f`. For M14/M15, the SCTP data channel may still be missing (an m=application in the SDP that we accept but never connect to) — if Shadow waits for that to signal "client ready" and we see odd behaviour, that is the lead to dig into.
