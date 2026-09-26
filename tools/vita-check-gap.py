#!/usr/bin/env python3
"""vita-check-gap.py - prove the SCE metadata will fit, before it is written.

`vita-elf-create` appends 3 368 bytes of SCE data into the gap between the end
of the code segment and the start of the data segment. When the gap is too
small it prints its complaint and SEGFAULTS - and its message contains no
"error:", which is how three deployments in a row shipped the previous build
while every step reported success.

The gap is `alignment - (code size mod alignment)`, so it moves with every
change to the binary's size. That makes it a dice roll, and a dice roll is only
acceptable when losing is LOUD. This runs right after the link and fails the
build with the two numbers that matter.

Usage: vita-check-gap.py <elf> [required-bytes]
"""
import re, subprocess, sys

REQUIRED = 3368


def main() -> int:
    elf = sys.argv[1]
    need = int(sys.argv[2]) if len(sys.argv) > 2 else REQUIRED
    try:
        out = subprocess.run(["arm-vita-eabi-readelf", "-lW", elf],
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        # A check that cannot run must say so rather than pass silently - a
        # green line nobody produced is worse than no line.
        print(f"vita-check-gap: cannot read {elf}: {e}", file=sys.stderr)
        return 1

    segs = []
    for line in out.splitlines():
        m = re.match(r"\s+LOAD\s+(0x\S+)\s+(0x\S+)\s+(0x\S+)\s+(0x\S+)\s+(0x\S+)", line)
        if m:
            segs.append((int(m.group(2), 16), int(m.group(5), 16)))
    if len(segs) < 2:
        print(f"vita-check-gap: expected two LOAD segments in {elf}, found {len(segs)}",
              file=sys.stderr)
        return 1

    gap = segs[1][0] - (segs[0][0] + segs[0][1])
    if gap < need:
        print(f"vita-check-gap: only {gap} bytes between the code and data segments; "
              f"vita-elf-create needs {need}.", file=sys.stderr)
        print("It would print a message with no \"error:\" in it and then segfault, "
              "leaving the PREVIOUS .self in place.", file=sys.stderr)
        print("The gap is alignment - (code size mod alignment): change the binary's "
              "size, or raise the segment alignment in CMakeLists.txt.", file=sys.stderr)
        return 1
    print(f"vita-check-gap: {gap} bytes of SCE headroom (need {need})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
