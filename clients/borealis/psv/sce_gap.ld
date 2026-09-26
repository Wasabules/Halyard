/* sce_gap.ld - guarantee the room vita-elf-create needs after the code segment.
 *
 * vita-elf-create writes ~3.4 KB of SCE data between the end of segment 0 (the
 * code) and the start of segment 1 (the data). The vitasdk's default script
 * starts the data on a 64 KB boundary (DATA_SEGMENT_ALIGN(..., 0x10000)), so
 * the room is whatever `0x10000 - (end & 0xFFFF)` happens to be: 444 bytes
 * once, 2 084 on a fresh vitasdk on 2026-09-26 - and a short gap makes
 * vita-elf-create segfault after a message with no "error:" in it.
 *
 * This section is inserted LAST in the code segment. When the gap would be
 * under 8 KB it adds a FIXED 0x2100 bytes, which carries the end past the
 * boundary and leaves at least 0x10000 - 0x2100 (57 KB); otherwise it is one
 * byte. The pad is a constant on purpose: a size computed from the gap itself
 * (gap + 16) made ld chase its own tail ("VMA wraps around address space").
 * `ABSOLUTE` because `.` inside a section is an offset from its start.
 * tools/vita-check-gap.py still checks the result after the link. */
SECTIONS
{
  .sce_gap_pad : {
    BYTE(0)
    . = . + ((((0x10000 - (ABSOLUTE(.) & 0xFFFF)) & 0xFFFF) < 0x2000) ? 0x2100 : 0);
  }
}
INSERT AFTER .eh_frame;
