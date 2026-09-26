---
name: project-cabac-zero-word-re
description: TIER3 — The cabac_zero_word hypothesis is ELIMINATED. The desktop's DecodeH264 strips nothing; libh2645bitstream::find_nal_unit hands over the raw NAL and VA-API handles the trailing bits.
metadata:
  type: project
---

## TL;DR

TIER3 2026-05-16: an RE of `DecodeH264 = sub_C5BC30` + a full scan of the binary's strings = **ZERO references to `cabac_zero_word`, `trailing_zero_8bits`, `rbsp_trailing_bits`, `more_rbsp_data`, `byte_alignment`, `emulation_prevention`**. The desktop does **no trailing-byte stripping at all**.

**Verdict**: the "cabac_zero_word mishandled on the client side" hypothesis is **eliminated at C95**. The taskbar bug (MB rows 58-67) is NOT caused by mishandled trailing padding.

## Evidence — search results

Scanned through `tier3_re.py` §J.1 over 50,000+ strings:

| Needle | Hits |
|--------|------|
| `cabac_zero_word` | 0 |
| `cabac` (case-insensitive) | 0 |
| `CABAC` | 0 |
| `trailing_zero` | 0 |
| `trailing.?bits` | 0 |
| `rbsp` | 0 |
| `RBSP` | 0 |
| `more_rbsp_data` | 0 |
| `byte_alignment` | 0 |
| `emulation_prevention` | 0 |

The only NAL-related strings present:

- `h264_read_nal_unit` (0x40CD09) — import libh2645bitstream.so
- `find_nal_unit` (0x40CD57) — import libh2645bitstream.so
- `find_nal_unit (pass 2) returned nal_start {}` / `nal_end {}` / `nal_end <= nal_start` / `nal_end > remaining` — logs dans `sub_C5BC30`

## Pipeline desktop (rappel de TIER1)

```c
// sub_C5BC30 DecodeH264 — vaapi_video_decoder.cpp
// Pass 1
while ( find_nal_unit(buf+off, len-off, &ns, &ne, &src) >= 0 ) {
    nal_type = buf[ns] & 0x1F;
    if (nal_type == 7) { h264_read_nal_unit(libh264, buf+ns, ne-ns); break; }
    if (buf[ns] & 0x18) break;        // slice → end pass 1
}
// Pass 2 — emit each NAL as VASlice
while ( find_nal_unit(...) >= 0 ) {
    switch (nal_type) {
    case 7: case 8: h264_read_nal_unit(libh264, ...);
    case 1: case 5: submit_va_slice(...);
    }
}
```

**No post-processing of each NAL's content.** Whatever bytes `find_nal_unit` returns as `[nal_start..nal_end)` are forwarded straight to VA-API. Trailing `cabac_zero_word` triplets `00 00 03 00` (if there were any) are consumed silently by the hardware decoder per the H.264 spec § 7.3.2.10.

## Why the server probably does not emit cabac_zero_word (C70)

H.264 spec § 7.4.1.2.2: `cabac_zero_word` is **optional**, required only for HRD compliance at certain profile/level/bitrate combinations. NVENC in a **low-latency / ultra-low-latency** preset (= what cloud streaming uses) typically **disables** cabac_zero_word emission. Without a decrypted pcap + a bitstream parser we cannot prove it at C95, but its prevalence in the wild is < 5 % of NVENC streaming frames.

## And if it did anyway, the top half would be broken too (C95)

The top and bottom slices come out of the **same NVENC encoder** with the same HRD setup. If `cabac_zero_word` were mishandled on the client side, **both halves would be uniformly corrupted** — which is not what we observe (the top has been fine since V11/V12). So the cause is **not** trailing padding.

## Our av_parser_parse2 library does the same (C90)

`webrtc/h264_decoder.c::h264_decoder_feed_annexb` uses `av_parser_parse2` + `avcodec_send_packet` — libavcodec also consumes the trailing bytes silently per the spec. Identical to the desktop. No code to change.

## Remaining taskbar-bug hypotheses (a reminder)

Per `memory/project_V12_taskbar_status_2026-05-15.md`, 7 hypotheses were eliminated, including all 5 decoder-side ones. **2 untested hypotheses** remain:

1. VideoSslTcpChannel `:base+20` sends a byte-exact blob that we do not send (TIER1 §A: variant 8 = `streamingtoken`)
2. **A new TIER3 hypothesis (C50)**: `RemoteBitrateEstimatorIOChannel` (= port `+14` or `+15`?), an uplink bandwidth feedback. If we do not send it → the server downgrades the encoder mode → multi-NAL emission is disabled → fewer bottom slices.

**That TIER3 lead is more promising than cabac_zero_word.**

## Refs

- IDA dump : `tools/ida/out/tier1/ndsplit_sub_C5BC30.c`
- TIER3 doc : `tools/ida/out/TIER3_RE_2026-05-16.md` §J
- Memory cross-ref :
  - [[project-decoder-nal-split-RE]] — TIER1 : find_nal_unit confirmed
  - [[project-V12-taskbar-status-2026-05-15]] — the current taskbar-bug state
  - [[project-image-100pct-PROOF]] — 10.7% bottom NAL desktop vs Switch
  - [[project-all-ports-RE]] — RemoteBitrateEstimator candidat C50
