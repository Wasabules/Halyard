---
name: project-decoder-nal-split-RE
description: "TIER1 2026-05-16 — An RE of the desktop's H.264 NAL splitter. sub_C5BC30 (DecodeH264, vaapi_video_decoder.cpp) calls libh2645bitstream::find_nal_unit() in a 2-pass loop to split every multi-NAL chunk into separate NALs before submitting them to VA-API. Our Switch client needs no extra logic: libavcodec's av_parser_parse2 does the same automatically."
metadata:
  type: project
---

# Desktop NAL splitter RE — TIER1 2026-05-16

## TL;DR

The desktop client **does not depend** on the server to split the NALs. It accepts
multi-NAL buffers and uses `libh2645bitstream::find_nal_unit()` in a
2-pass loop to enumerate every NAL before submitting to VA-API. Our Switch
client uses `libavcodec av_parser_parse2()`, which does the same in
internal — **no code change required** on the splitter side.

## Splitter principal — sub_C5BC30 (C95)

```
sub_C5BC30  "DecodeH264"   __file__ = vaapi_video_decoder.cpp
   Single caller : sub_C61810  (= decode_one_frame entry)
   Imports : .h264_read_nal_unit (0x41F600)
              .find_nal_unit     (0x422BE0)
              .vaCreateBuffer / .vaMapBuffer / .vaBeginPicture / ...
```

Boucle 2-pass :

```c
// Pass 1 — detect the SPS/PPS/AUD/SEI headers
for ( i = 0; ; i += v190 ) {
    int v189 = 0, v190 = 0, src = -1;
    if ( find_nal_unit(buf + v8, len - v8, &v189, &v190, &src) < 0 || v189 < 0 ) break;
    uint8_t *nal_ptr = buf + i + v189;
    int nal_type = *nal_ptr & 0x1F;
    if ( nal_type == 7 )      h264_read_nal_unit(SPS), break;
    if ( (nal_ptr[0] & 0x18) != 0 && nal_type != 9 ) break;
    if ( (nal_ptr[0] & 0x18) == 0 ) {
        if ( !nal_type ) { log("Corrupted H264 NAL type unspecified"); abort; }
        if ( nal_type != 6 ) break;
    }
}

// Pass 2 — emit every slice/PPS to VA-API
while ( 1 ) {
    int v189 = 0, v190 = 0, v194 = -1;
    if ( find_nal_unit(buf, len, &v189, &v190, &v194) < 0 ) goto err;
    if ( !v189 ) break;
    switch ( nal_type ) {
    case 7: read_nal_unit(SPS); break;       // update SPS
    case 8: read_nal_unit(PPS); break;       // update PPS
    case 1: case 5:                          // P/IDR slice → submit VA-API
        vaCreateBuffer(VASliceDataBufferType);
        vaMapBuffer; copy slice bytes; vaUnmapBuffer;
        h264_read_nal_unit(&parsed_slice_header);
        ...
    }
}
```

## Library used — libh2645bitstream.so (C95)

Imports rodata strings @ 0x40CD09..0x40CD65 :
- `h264_read_nal_unit`
- `h264_new`
- `h264_free`
- `find_nal_unit`
- `hevc_read_nal_unit`
- `hevc_new`
- `hevc_is_idr`
- `hevc_is_irap`

`libh2645bitstream` is an external library (= probably github.com/cisco-open/h264bitstream + an HEVC fork). It parses the annex-B bytes and returns NAL boundaries through `find_nal_unit(buf, len, &nal_start, &nal_end, &nal_unit_type)`.

## HEVC twin — sub_C5D6F0 (C95)

The same patterns, but using `hevc_read_nal_unit`. Strings:
- "Corrupted stream, HEVC NAL unit type unspecified {}"
- "Corrupted stream, HEVC NAL unit type unspecified {} (2nd pass)"
- "read_nal_unit failed (SPS)"
- "read_nal_unit failed (PPS)"

So the desktop supports both H.264 AND HEVC, but our stream is confirmed H.264.

## Implication pour Switch client (C80)

Our `h264_decoder_feed_annexb(dec, buf, len)` uses libavcodec's internal parser, which also scans for the `00 00 00 01` start codes and splits the NALs automatically. **If we receive a multi-NAL chunk, libavcodec will decode it correctly** — there is no need to add our own `find_nal_unit_simple`.

Verification: `webrtc/h264_decoder.c::h264_decoder_feed_annexb` uses `av_parser_parse2()` in default mode (PARSER_FLAG_COMPLETE_FRAMES=0), which handles multi-NAL.

## If we had to write a splitter by hand

```c
// Find next NAL boundary by start code 00 00 00 01 or 00 00 01
static int find_nal_unit_simple(const uint8_t *buf, size_t len,
                                 size_t *nal_start, size_t *nal_end) {
    size_t i = 0;
    while (i + 3 < len) {
        if (buf[i]==0 && buf[i+1]==0 && buf[i+2]==1) { *nal_start = i+3; goto found; }
        if (buf[i]==0 && buf[i+1]==0 && buf[i+2]==0 && buf[i+3]==1) { *nal_start = i+4; goto found; }
        ++i;
    }
    return -1;
found:
    i = *nal_start;
    while (i + 3 < len) {
        if (buf[i]==0 && buf[i+1]==0 && (buf[i+2]==1 || (buf[i+2]==0 && buf[i+3]==1))) {
            *nal_end = i;
            return 0;
        }
        ++i;
    }
    *nal_end = len;
    return 0;
}
```

## Risk: a NAL crossing a chunk boundary (C80 — mitigated)

If the server packs multi-NAL and one NAL straddles 2 SUFP chunks, our concatenation buffer `g_display_buf_append` recomposes them. Then `h264_decoder_feed_annexb` (= through av_parser_parse2) scans the start codes across the complete buffer → no truncation.

## Verdict

**The image bug is NOT in the Switch's NAL splitter.** The splitter is fine (= libavcodec does it automatically). The bug is **upstream** — either:
1. **B**: the server does not pack multi-NAL for us (= a missing feature flag in RegisterSession_Video)
2. **A**: we are not opening the `:base+20` channel correctly (= we are missing real retransmit frames)

Next session's focus = resolve A (variant 8 = the streamingtoken) or capture the plaintext for B (= map the boolean field that triggers multi-NAL).

## Refs

- [[project-image-100pct-proof]] — preuve empirique multi-NAL gap
- [[project-shadow-h264-quirks]] — multi-slice aggregation libavcodec
- `tools/ida/out/TIER1_RE_2026-05-16.md` §C
- `tools/ida/out/tier1/ndsplit_sub_C5BC30.c` — H.264 splitter Hex-Rays (1208 L)
- `tools/ida/out/tier1/ndsplit_sub_C5D6F0.c` — HEVC splitter (jumeau)
- `tools/ida/out/tier1/ndsplit_caller_sub_C61810.c` — decode_one_frame entry
- `tools/ida/out/tier1/NAL_SPLIT_HUNT.md` — strings hints + xrefs find_nal_unit
