# Fonts

**`font.ttf` — [Inter](https://rsms.me/inter/), SIL Open Font License 1.1**, text
of the licence in `LICENSE-Inter.txt`. Loaded by Borealis as `USER_FONT_PATH`,
which it tries before anything else, so no vendored code needed changing.

Covers Latin (95/95 basic, 191/192 extended) and **Cyrillic (248/256)** — so the
`en-US`, `fr` and `ru` catalogues render fully — plus `U+26A0`, the warning sign
the French catalogue uses.

## What used to be here, and why it is gone (2026-09-13)

Two fonts extracted from console firmware, which the `.vpk` redistributed:

- `switch_font.ttf` — family `nintendo_udsg-r_std_003`, "Version 004 (Nintendo
  Co., Ltd.)", the **Morisawa UD Shin Go** typeface as licensed to Nintendo;
- `switch_icons.ttf` — `NintendoExt003`, © Nintendo 2016, the button glyphs.

The second was **not replaced, it was simply removed**, and the reason is worth
recording: nothing renders it. A sweep of every source file in `core/`,
`clients/` and the compiled part of Borealis found **zero** private-use
codepoints (U+E000–U+F8FF). Our own UI spells button names out rather than
drawing glyphs — see `clients/borealis/ui/pad_label.hpp`. It was 180 KB of
someone else's copyright shipped in both packages for a fallback font nobody
ever drew a character from.

## The gap this leaves, stated rather than hidden

Inter has no CJK, so the **`zh-Hans` catalogue will show missing glyphs on PS
Vita and on desktop**. It is fine on Switch, where Borealis loads the system's
own shared fonts, CJK included.

Bundling a CJK face would fix it and cost 10–20 MB in a package that is
currently 7.7 MB — not a trade worth making for the Vita. Borealis already
provides the escape hatch: it loads `font/font.ttf` first, so anyone who needs
CJK can drop their own font in at that path and it takes precedence.
