/* plane_copy - the decoded planes into a destination with a different stride.
 *
 * === QT1 2026-10-03 — WHY THIS IS A MODULE AND NOT SIX LINES ===============
 *
 * The decoder's `linesize` is aligned for SIMD; the destination's
 * `bytesPerLine` is whatever the surface chose. They differ, so a single
 * `memcpy` of a whole plane SHEARS the picture — each row lands a few bytes
 * further along than the one before, and the result is a diagonal smear that
 * looks like a decoder fault.
 *
 * That is the kind of defect this repository keeps paying for: plausible,
 * silent, and blamed on the wrong layer. It is also arithmetic, which means it
 * can be tested without a GPU, a window or an account — so it is, by
 * `tests/test_qt_planes.cpp`.
 *
 * Pure: no Qt, no allocation, no state. Deliberately free of `QVideoFrame` so
 * the test needs no QApplication; the caller maps the frame and passes the
 * pointers.
 *
 * === WHAT IT REFUSES ========================================================
 *
 * A destination stride NARROWER than the row it is given. Copying
 * `min(src, dst)` bytes would produce a cropped picture that looks almost
 * right, which is worse than a visible refusal: at 1080p a sixteen-byte
 * shortfall is eight pixels of the right-hand edge, every frame, and nobody
 * would file that. So it says no and the caller drops the frame.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace halyard {

/* The shape of one plane: how many rows, and how many bytes of each row carry
 * picture (which is NOT the stride - the stride includes the padding). */
struct PlaneShape {
    int rows;
    int bytes_per_row;
};

/* YUV420P: three planes, chroma at half width and half height.
 * NV12: two planes, the second interleaving U and V - so it is half the height
 * but the FULL width in bytes, which is the detail a reader gets wrong. */
inline PlaneShape planeShapeYuv420p(int width, int height, int plane)
{
    if (plane == 0) return PlaneShape{height, width};
    return PlaneShape{height / 2, width / 2};
}

inline PlaneShape planeShapeNv12(int width, int height, int plane)
{
    if (plane == 0) return PlaneShape{height, width};
    return PlaneShape{height / 2, width};
}

/* Copies `shape.rows` rows of `shape.bytes_per_row` bytes from `src` to `dst`,
 * each advancing by its own stride.
 *
 * Returns false and copies NOTHING when an argument is impossible or when the
 * destination row cannot hold the source row - see "what it refuses". */
inline bool copyPlane(uint8_t *dst, int dst_stride,
                      const uint8_t *src, int src_stride,
                      PlaneShape shape)
{
    if (!dst || !src) return false;
    if (shape.rows <= 0 || shape.bytes_per_row <= 0) return false;
    if (dst_stride <= 0 || src_stride <= 0) return false;
    /* A stride shorter than the picture row is a destination that cannot hold
     * it. Refused rather than cropped. */
    if (dst_stride < shape.bytes_per_row) return false;
    if (src_stride < shape.bytes_per_row) return false;

    for (int r = 0; r < shape.rows; r++) {
        std::memcpy(dst + (std::size_t)r * (std::size_t)dst_stride,
                    src + (std::size_t)r * (std::size_t)src_stride,
                    (std::size_t)shape.bytes_per_row);
    }
    return true;
}

/* True when the strides are equal AND there is no padding, i.e. the whole
 * plane is one contiguous block and a single memcpy would be correct.
 *
 * Exposed so a caller can take the fast path knowingly, and so the test can
 * assert that the common 1080p case is NOT contiguous - which is the reason
 * this module exists at all. */
inline bool planeIsContiguous(int dst_stride, int src_stride, PlaneShape shape)
{
    return dst_stride == src_stride && dst_stride == shape.bytes_per_row;
}

}  // namespace halyard
