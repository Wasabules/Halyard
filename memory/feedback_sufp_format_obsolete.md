---
name: The SUFP wire format is obsolete on the current `:13010` video stream
description: 2026-05-09 ~02:00 — Our SUFP parser (`[u8 type][u8 sub][u16 chunk_idx][u16 max_chunks][u32 frame_id]`) no longer matches the current wire on :port_base+10 video. The window of 256 saturates at 8200+ evictions/30 s, 0 frames decoded.
type: feedback
---
## Le bug

Our SUFP parser (sufp.c::sufp_feed) reads `frame_id` at offset 6 as a u32 LE. On the current video stream's wire (:13010 / :11010 / :13010 depending on the VM), the parsed frame_ids jump by +1.3 M/sec (= absurd), which saturates any reassembly window.

For the VM tested:
- video=2041 packets en 5s
- frame_ids observed: 395673223 → 402253931 (+6.5M) in 5 s
- No complete frame → decoded=0
- 8200+ evictions en 30s (= ~273 evictions/sec)

## Why

The SUFP wire format we documented in `06-shadow-recon-linux/ENCRYPTION_AND_FRAMING.md §2` was based on a Ghidra RE of the Linux binary. The server has probably changed format since (= a rolling protocol update), OR the format is only valid for non-video subchannels (= the cursor on :port_base+30 for instance, to be validated).

## How to apply

- **Do NOT use SUFP for video** on the current wire. The legacy mode (= per-packet chacha20 + per-PTS reassembly through the VideoFrame timestamp) runs at ~28-32 fps with degraded quality (= multi-slice out of order).
- To fix the visual quality (= "a clean first line + the rest blurry", reported by the user on 2026-05-09), you need either:
  1. to RE the real current SUFP wire format (= dump the first 16 bytes of fresh UDP video and compare against the spec)
  2. to implement another reordering (= sort by a header byte inside the decrypted chacha20 payload)
  3. to request RegisterSession with less_fragmentation=true (= if the server supports it)
- The SUFP code is left in place for the cursor (= for when we implement it) but not enabled for video.

## Refs

- `streaming/ctrl_session.c::on_video_packet` — the legacy mode, not SUFP, for video
- `streaming/sufp.c` — code SUFP intact, just unused pour video
- Cross-ref : `project_native_GUI_streaming_STABLE.md`, `project_shadow_h264_quirks.md` (= multi-slice par picture issue)

## Iterations tested on 2026-05-09 (all failed on quality)

| Run | Approach | Result |
|---|---|---|
| 17 | Header 6B (= bytes 0-5 type/sub/idx/max), concat puis decrypt | 0 decrypt OK / 47 fail |
| 18 | Idem mais chunk_idx 0-indexed | 0 / 47 fail |
| 19 | Per-packet decrypt (header 6B), concat plain | 0 / 474 fail |
| 20 | Header 11B + per-packet decrypt | 1 frame total |
| 21 | Cas A 1-chunk decrypt direct + Cas B multi-chunk concat (header 11B) | 0 / 31 |
| 22 | Revert legacy `byte 10 == 0x01` filter | 202 frames/5s comme run 15 |

Conclusion: none of the formal hypotheses matches the current wire. The legacy filter `byte 10 == 0x01` captures **a subset** of the packets and allows a direct decrypt, but that fraction only gives partial slices of frames → a degraded picture.

## Plan pour next session RE

1. Add to `on_video_packet` a dump of the first 32 bytes of the first 10 packets received (= through `webrtc_log("video pkt %d: %.32s", ...)`)
2. Launch shadow-client native + capture the first packets of OUR wire
3. Compare byte by byte against:
   - The `13 00 ...` format from the v1 log (= the desktop app on another VM)
   - The content of the last byte (= perhaps a length suffix)
4. Test the chunk_idx against packet_idx hypotheses, and understand why the `byte 10 == 0x01` filter only captures some of them
5. Once the format is understood: implement correct reassembly with concatenated fragments → a single chacha20 wire → decrypt → a complete frame

## RE completed 2026-05-09 ~03:00

Avec dump full packet + decrypt python validation :

- **Header SUFP video = 11 bytes** : `[byte 0=type 0x13][byte 1=subchan][bytes 2-3=chunk_idx u16 LE][bytes 4-5=max_chunks u16 LE][bytes 6-9=4B variants][byte 10=flag 0/1]`
- **The payload format for byte 10 == 0x01**: `[ct N B][nonce 12B][tag 16B]` = a self-contained chacha20 wire, decodable per packet
- **Payload format for byte 10 == 0x00**: UNKNOWN — probably a different structure (= an intermediate chunk of a multi-packet frame OR FEC redundancy). Concatenation attempts failed.
- **VideoFrame header (post-decrypt)**: `[byte 0=0x02][bytes 1-4=ts u32 LE 90 kHz][byte 5=a flag bitmask: 0x00 normal, 0x01/0x06 keyframe][bytes 6-? = a variable-length ext header][NAL Annex-B start codes]`
- **Fix applied (run 26+)**: scan for the first `00 00 00 01` from byte 6 to find the NAL start instead of assuming 6/19 B. It gives visual artefacts (= the start of the picture visible) instead of a complete blur.

## État image runs 22-28

| Run | Approche | Decoded | Visual |
|---|---|---|---|
| 22 | Legacy (skip 11B + byte10==0x01 + assume hdr_len=6 ou 19) | ~30 fps | "1ère ligne propre + flou" |
| 26 | Same + an Annex-B `00 00 00 01` scan to find the NAL start | ~30 fps | "Visible artefacts, the beginning of a picture" ✓ |
| 27 | Concatenate every chunk then decrypt | 1 frame only | Waiting for video |
| 28 | Revert run 26 | (idem 26) | (idem 26) |

For a fully SHARP picture: decode the format of the byte10==0x00 packets. A hypothesis to test: Reed-Solomon FEC, OR another wire format (= plain ciphertext fragments to concatenate with the LAST byte10==0x01 chunk to form the complete wire). It needs a Ghidra RE of the shadow-prod binary or brute-force hypothesis testing.
