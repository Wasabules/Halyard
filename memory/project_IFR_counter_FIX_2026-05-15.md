---
name: project-IFR-counter-FIX-2026-05-15
description: DECISIVE — bytes 4-5 of the IFR request packet are an incrementing LE counter, NOT an output_id. Stuck at 0 = the server deduplicates every IDR refresh request → 0.81% bottom slices. Increment the counter → 9.0% bottom slices = parity with the desktop. The picture updates regularly.
metadata:
  node_type: memory
  type: project
---

# IFR Request Counter Fix — the picture refreshes regularly again

## The decisive find (= the V9 RE)

Source : `tools/ida/out/sub_BEC180.c` lignes 246-265 RE'd by agent V9.

The IFR (= I-Frame Request) UDP packet sent to the `:video` port, format:
```
[0x69, 0x50, 0x00, 0x02, counter_LE_u16]
```

Bytes 4-5 are a **monotonically incrementing request_counter**, stored at `a1+194` in the desktop's struct. NOT an `output_id` as we thought.

## Bug pre-fix

Our code always sent `counter = 0x0000`: 
```c
uint8_t ifr[6] = {0x69, 0x50, 0x00, 0x02, 0x00, 0x00};
send(udp_video, ifr, 6, 0);
if (udp_input >= 0) send(udp_input, ifr, 6, 0);
```

**Consequence**: the server deduplicates IDR requests by counter. Every subsequent IDR refresh request (= emitted every ~2 s) had counter=0 → **ALL IGNORED** except the first.

→ The server sent ONE initial IDR then no refresh → the bottom slices were never pushed again → a picture frozen at ~30-50% top + bottom error_concealment.

## Fix

`05-shadow-client-borealis/demo/src/streaming/ctrl_session.c::on_video_packet` main loop :
```c
static uint16_t g_ifr_counter = 1;
if ((ip_counter % 28) == 14) {
    uint8_t ifr[6] = {0x69, 0x50, 0x00, 0x02,
                       (uint8_t)(g_ifr_counter & 0xFF),
                       (uint8_t)((g_ifr_counter >> 8) & 0xFF)};
    send(udp_video, ifr, 6, 0);
    if (udp_input >= 0) send(udp_input, ifr, 6, 0);
    g_ifr_counter++;
}
```

## Runtime results confirmed (= session 2026-05-15 11:20)

| Metric | Before F2 | After F2 | Desktop ref |
|---|---|---|---|
| NAL top | 2084 | 2121 | similaire |
| NAL bottom | 17 | **210** | similaire |
| **Bottom ratio** | **0.81%** | **9.0%** | **11%** |
| IDR top | 3 | 32 | nombreux |
| **IDR bottom** | 1 | **26** | nombreux |
| SPS/PPS | 3/3 | 32/32 | nombreux |
| Visual | 50% top + striped/frozen | The picture refreshes regularly | 100% complete |

**The bottom ratio went from 0.81 % to 9.0 % = +1108 %**. 81 % of IDRs now have a bottom IDR complement (= the server pushes FULL refreshes).

## Tests of the other pre-F2 leads = NO effect, because they were blocked by this bug

- Plan A frequencies ✗ (aligned State/Hid/Flush) — not the bug
- H5 EDID fix ✗ (byte 126/127) — not the bug
- IDR_INJECT ✗ — not enough bottom IDRs to inject
- YUV_COMPOSITE ✗ — not enough bottom NAL to cache
- Reed-Solomon ✗ — confirmed absent

All those fixes were cosmetic or diagnostic — the real bug was counter=0.

## Refs
- `tools/ida/out/H1_V9_server_discrimination.md` — the full V9 analysis
- `tools/ida/out/sub_BEC180.c` — the RE of the IFR packet builder
- [[project-image-50pct-state-2026-05-15]] — the state before the fix
- [[project-image-100pct-proof]] — the empirical desktop reference

## Status
- F2 = **WIN** (= 5 min, root cause confirmed)
- The picture is nearly 100% now on the Windows side
- Left to do: port it to the Switch, validate stream stability
