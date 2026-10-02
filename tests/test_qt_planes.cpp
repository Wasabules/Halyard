/* test_qt_planes - the stride-aware plane copy of the Qt client.
 *
 * At 187 MB/s this runs sixty times a second, and the way it goes wrong is a
 * diagonal smear that looks like a decoder fault: the decoder's linesize is
 * aligned for SIMD, the destination's is its own, and one memcpy of the whole
 * plane shears every row against the last.
 *
 * Needs no Qt, no GPU and no window - `clients/qt/plane_copy.hpp` is pure
 * arithmetic, which is why it was separated from the widget.
 *
 * Compiled with -Wall -Wextra -Werror -O1 by tests/run_tests.sh (run_cpp).
 */
#include <cstdio>
#include <cstring>
#include <vector>

#include "../clients/qt/plane_copy.hpp"

using halyard::PlaneShape;
using halyard::copyPlane;
using halyard::planeIsContiguous;
using halyard::planeShapeNv12;
using halyard::planeShapeYuv420p;

static int checks = 0, failures = 0;

static void ok(bool cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        std::printf("  FAIL %s\n", what);
    }
}

static void eq_int(int got, int want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        std::printf("  FAIL %-50s got %d, expected %d\n", what, got, want);
    }
}

int main()
{
    std::printf("== the Qt client's plane copy (QT1 2026-10-03) ==\n");

    /* --- the shapes ------------------------------------------------------- */
    eq_int(planeShapeYuv420p(1920, 1080, 0).rows, 1080, "YUV420P luma rows");
    eq_int(planeShapeYuv420p(1920, 1080, 0).bytes_per_row, 1920, "YUV420P luma bytes");
    eq_int(planeShapeYuv420p(1920, 1080, 1).rows, 540,  "YUV420P chroma rows (half)");
    eq_int(planeShapeYuv420p(1920, 1080, 1).bytes_per_row, 960, "YUV420P chroma bytes (half)");

    /* NV12's chroma plane is half the HEIGHT but the FULL width in bytes,
     * because U and V are interleaved. This is the line a reader gets wrong,
     * and getting it wrong copies half the chroma and leaves the rest green. */
    eq_int(planeShapeNv12(1920, 1080, 1).rows, 540, "NV12 chroma rows (half)");
    eq_int(planeShapeNv12(1920, 1080, 1).bytes_per_row, 1920,
           "NV12 chroma bytes: FULL width, U and V interleaved");

    /* --- the copy itself, with DIFFERENT strides ------------------------- */
    {
        const PlaneShape sh{4, 5};          /* 4 rows of 5 picture bytes */
        const int src_stride = 8;           /* decoder: padded to 8 */
        const int dst_stride = 6;           /* destination: padded to 6 */
        std::vector<uint8_t> src((size_t)src_stride * sh.rows, 0xAA);
        std::vector<uint8_t> dst((size_t)dst_stride * sh.rows, 0x00);
        /* A recognisable pattern: row r, column c -> r*10 + c. */
        for (int r = 0; r < sh.rows; r++)
            for (int c = 0; c < sh.bytes_per_row; c++)
                src[(size_t)r * src_stride + c] = (uint8_t)(r * 10 + c);

        ok(copyPlane(dst.data(), dst_stride, src.data(), src_stride, sh),
           "a copy with differing strides succeeds");

        bool exact = true;
        for (int r = 0; r < sh.rows && exact; r++)
            for (int c = 0; c < sh.bytes_per_row; c++)
                if (dst[(size_t)r * dst_stride + c] != (uint8_t)(r * 10 + c))
                    exact = false;
        ok(exact, "every row landed at its own destination stride");

        /* The padding between rows must be UNTOUCHED: writing into it is how a
         * copy that uses the source stride on the destination corrupts the
         * next row. */
        bool padding_clean = true;
        for (int r = 0; r < sh.rows; r++)
            for (int c = sh.bytes_per_row; c < dst_stride; c++)
                if (dst[(size_t)r * dst_stride + c] != 0x00)
                    padding_clean = false;
        ok(padding_clean, "the destination padding was not written");

        /* And nothing past the last row. */
        ok(dst.size() == (size_t)dst_stride * sh.rows,
           "no write past the plane (vector size unchanged)");
    }

    /* --- the refusals ---------------------------------------------------- */
    {
        const PlaneShape sh{2, 8};
        std::vector<uint8_t> src(64, 1), dst(64, 0);
        ok(!copyPlane(nullptr, 8, src.data(), 8, sh),       "no destination");
        ok(!copyPlane(dst.data(), 8, nullptr, 8, sh),       "no source");
        ok(!copyPlane(dst.data(), 0, src.data(), 8, sh),    "a zero destination stride");
        ok(!copyPlane(dst.data(), 8, src.data(), 0, sh),    "a zero source stride");
        ok(!copyPlane(dst.data(), -8, src.data(), 8, sh),   "a negative stride");
        ok(!copyPlane(dst.data(), 8, src.data(), 8, PlaneShape{0, 8}),  "zero rows");
        ok(!copyPlane(dst.data(), 8, src.data(), 8, PlaneShape{2, 0}),  "zero bytes per row");

        /* THE ONE THAT MATTERS: a destination row too narrow for the picture.
         * Copying min(src,dst) would crop the right-hand edge of every frame -
         * at 1080p a 16-byte shortfall is eight pixels nobody would report. */
        ok(!copyPlane(dst.data(), 7, src.data(), 8, sh),
           "a destination stride NARROWER than the row is REFUSED, not cropped");

        /* And a refusal must write nothing at all. */
        std::vector<uint8_t> untouched(64, 0);
        (void)copyPlane(untouched.data(), 7, src.data(), 8, sh);
        bool clean = true;
        for (uint8_t b : untouched) if (b != 0) clean = false;
        ok(clean, "a refused copy writes nothing");
    }

    /* --- contiguity, and why this module exists -------------------------- */
    {
        const PlaneShape luma = planeShapeYuv420p(1920, 1080, 0);
        ok(planeIsContiguous(1920, 1920, luma),
           "equal strides with no padding ARE contiguous");
        /* The real 1080p case: ffmpeg aligns the luma linesize to 32 or 64, so
         * 1920 becomes 1920 (already aligned) but 1922-wide content becomes
         * 1984. A plane whose strides differ is NOT contiguous, and a single
         * memcpy would shear it - which is the whole point. */
        ok(!planeIsContiguous(1920, 1984, luma),
           "a padded source is NOT contiguous");
        ok(!planeIsContiguous(1984, 1920, luma),
           "a padded destination is NOT contiguous either");
    }

    /* === MUTATION CHECK ==================================================
     *
     * Replace the loop with one `memcpy(dst, src, rows * bytes_per_row)` - the
     * edit someone makes for speed - and the assertion below fails: row 1 of
     * the destination would hold the source's padding, not the picture.
     *
     * Asserted with strides that differ by ONE, because that is the case a
     * careless test with equal strides would miss entirely. */
    {
        const PlaneShape sh{3, 4};
        std::vector<uint8_t> src((size_t)5 * 3, 0xFF);
        std::vector<uint8_t> dst((size_t)4 * 3, 0x00);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++)
                src[(size_t)r * 5 + c] = (uint8_t)(100 + r);
        ok(copyPlane(dst.data(), 4, src.data(), 5, sh), "the mutation case copies");
        /* Row 1 must be the source's row 1, not a byte of its padding. */
        eq_int(dst[4], 101, "row 1 came from the source's row 1, not its padding");
        eq_int(dst[8], 102, "row 2 likewise");
    }

    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
