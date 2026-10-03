/* test_qr_matrix - the QR module grid the Qt login screen paints (QR1).
 *
 * The bug this is written against: libqrencode packs flags into each byte of
 * `qr->data` - bit 0 is the module colour and the upper bits say which pattern
 * it belongs to - so a byte that is non-zero does NOT mean a dark module. Copy
 * it without masking and every module comes out dark: a solid black square
 * that looks like a rendering fault rather than like a wrong QR code.
 *
 * The finder patterns are checked because they prove the grid is a real QR and
 * is the right way round. They are fixed by the standard: a 7x7 square in
 * three corners, dark ring, light ring inside it, dark 3x3 core. Nothing else
 * in the output can be asserted by hand - the data modules depend on the mask
 * pattern the encoder chose.
 */
#include <stdio.h>
#include <string.h>

#include "../core/services/qr_helper.h"

static int checks = 0, failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL %s\n", what); }
}

/* The 7x7 finder at (ox, oy): ring dark, next ring light, 3x3 core dark. */
static int finder_ok(const unsigned char *m, int w, int ox, int oy)
{
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 7; x++) {
            const int edge = (x == 0 || y == 0 || x == 6 || y == 6);
            const int ring = (!edge && (x == 1 || y == 1 || x == 5 || y == 5));
            const int want = edge ? 1 : (ring ? 0 : 1);
            if (m[(oy + y) * w + (ox + x)] != want) return 0;
        }
    return 1;
}

int main(void)
{
    unsigned char m[177 * 177];
    int w = 0;

    printf("== QR module grid (QR1 2026-10-03) ==\n");

    /* --- refusals -------------------------------------------------------- */
    w = 123;
    ok(!qr_matrix(NULL, m, sizeof m, &w), "NULL text is refused");
    ok(w == 0, "a refused call sets the width to 0");
    ok(!qr_matrix("https://shadow.tech/device", NULL, 100, &w),
       "NULL buffer is refused");
    /* Half a QR is not a smaller QR: it must refuse, not truncate. */
    w = 123;
    ok(!qr_matrix("https://shadow.tech/device", m, 16, &w),
       "a buffer too small is refused, not truncated");
    ok(w == 0, "and the width is 0, not the size it would have been");
    ok(!qr_matrix("x", m, 0, &w), "a zero capacity is refused");

    /* --- a real code ----------------------------------------------------- */
    memset(m, 0xAA, sizeof m);
    const int got = qr_matrix("https://shadow.tech/device", m, sizeof m, &w);
    ok(got, "a URL encodes");
    if (!got) { printf("%d checks, %d failure(s)\n", checks, failures); return 1; }

    /* Every version is 21 + 4k modules a side, and never even. */
    ok(w >= 21 && w <= 177, "the width is a legal QR size");
    ok((w - 21) % 4 == 0, "the width is 21 + 4k");

    /* THE masking check: the flag bits must be gone. Without the mask the
     * values are 2, 3, 0xc1 and so on, and 0xAA would survive untouched in
     * any cell the loop failed to write. */
    int clean = 1, dark = 0, light = 0;
    for (int i = 0; i < w * w; i++) {
        if (m[i] > 1) { clean = 0; break; }
        if (m[i]) dark++; else light++;
    }
    ok(clean, "every module is 0 or 1 - the flag bits are masked off");
    ok(dark > 0 && light > 0, "the grid is not uniformly one colour");
    /* A QR is roughly balanced by construction; anything past 80% one way
     * means the values are not module colours at all. */
    ok(dark * 100 / (w * w) > 20 && dark * 100 / (w * w) < 80,
       "the dark/light split is plausible for a QR");

    /* --- structure: the three finder patterns ---------------------------- */
    ok(finder_ok(m, w, 0, 0), "finder pattern, top left");
    ok(finder_ok(m, w, w - 7, 0), "finder pattern, top right");
    ok(finder_ok(m, w, 0, w - 7), "finder pattern, bottom left");
    /* And NOT in the fourth corner - that is what tells a reader the
     * orientation, and a grid that had one there would be transposed. */
    ok(!finder_ok(m, w, w - 7, w - 7), "no finder pattern bottom right");

    /* The quiet zone is the caller's to add; the grid must start at the
     * finder, or the painter would add a second margin and the code would
     * scan at the wrong module size. */
    ok(m[0] == 1, "the grid starts at the finder, with no quiet zone");

    /* --- determinism ----------------------------------------------------- */
    {
        unsigned char again[177 * 177];
        int w2 = 0;
        ok(qr_matrix("https://shadow.tech/device", again, sizeof again, &w2),
           "the same text encodes again");
        ok(w2 == w, "to the same width");
        ok(memcmp(m, again, (size_t)w * w) == 0, "and to the same modules");
    }

    /* A longer text needs a bigger code - a width that never moved would mean
     * the version was being ignored. */
    {
        int wbig = 0;
        const char *lng = "https://shadow.tech/device?user_code=ABCD-EFGH"
                          "&state=0123456789abcdef0123456789abcdef";
        ok(qr_matrix(lng, m, sizeof m, &wbig), "a long URL encodes");
        ok(wbig >= w, "a longer text does not give a smaller code");
    }

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
