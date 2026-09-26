/* ui::lockgrid - the lock screen's grid arithmetic, with no drawing and no
 * input. Serves both grids: the pattern's 3x3 and the PIN pad's 3x4.
 *
 * The grids are addressed by three input paths that share nothing - a D-pad, a
 * finger, a mouse - and each of them has its own way of being off by one. So
 * the arithmetic is here, alone, and tests/test_lock_grid.c checks it; the view
 * is left with nothing but colours.
 *
 * === WHY THE D-PAD PATH IS THE PRIMARY ONE ===
 *
 * A docked Switch has NO touchscreen. A lock that could only be opened with a
 * finger would simply not exist for anyone playing on a television - and the
 * lock would then be impossible to open, which is the one failure a lock must
 * never have. The stick and the D-pad address the grid on every platform, in
 * both modes; the finger and the mouse are the convenience on top.
 *
 * Cells are numbered in reading order, which for the 3x3 is also Android's:
 *
 *      0 1 2
 *      3 4 5
 *      6 7 8
 */
#ifndef UI_LOCK_GRID_H
#define UI_LOCK_GRID_H

#include <stdbool.h>

/* Moves a cursor. `dx`/`dy` are -1, 0 or +1.
 *
 * The edges STOP rather than wrap. Wrapping reads as a slip of the thumb - you
 * pressed left and the cursor jumped to the far side - and on a screen where
 * each move can append a node to a secret, a surprise move is a wrong pattern
 * you have to start over.
 *
 * Blocked on one axis, it still moves on the other: stopping both would make a
 * corner a trap you can only leave with a straight press. */
static inline int ui_lock_grid_move(int cell, int dx, int dy, int cols, int rows)
{
    int r, c;
    if (cols < 1 || rows < 1) return 0;
    if (cell < 0 || cell >= cols * rows) return 0;
    r = cell / cols;
    c = cell % cols;
    c += dx;
    r += dy;
    if (c < 0) c = 0;
    if (c > cols - 1) c = cols - 1;
    if (r < 0) r = 0;
    if (r > rows - 1) r = rows - 1;
    return r * cols + c;
}

/* The centre of cell `n` inside the area (x, y, w, h).
 *
 * The centres sit at the middle of each cell - for three columns that is 1/6,
 * 3/6, 5/6 of the width, NOT 0, 1/2, 1. Placed on the edges, half of each outer
 * target would fall outside the area and be unreachable. */
static inline void ui_lock_grid_center(float x, float y, float w, float h,
                                       int cols, int rows, int n,
                                       float *cx, float *cy)
{
    if (cols < 1 || rows < 1 || n < 0 || n >= cols * rows) {
        if (cx) *cx = x;
        if (cy) *cy = y;
        return;
    }
    if (cx) *cx = x + (w / (float)cols) * ((float)(n % cols) + 0.5f);
    if (cy) *cy = y + (h / (float)rows) * ((float)(n / cols) + 0.5f);
}

/* Which cell a point falls on, or -1 for none.
 *
 * `radius_frac` is the catch radius as a fraction of the SMALLER cell side. It
 * is deliberately below 0.5: at 0.5 the targets touch, every point belongs to
 * some cell, and a finger sliding from one node to its neighbour latches the
 * neighbour before reaching it - the drawn line then lags behind the finger and
 * the pattern gains nodes nobody aimed at. Leaving a gap makes "between two
 * nodes" a real answer.
 *
 * The target is ROUND. A square one catches its corners, which are exactly the
 * points furthest from the node being aimed at, and that is how a diagonal drag
 * picks up a node it never passed over. */
static inline int ui_lock_grid_hit(float x, float y, float w, float h,
                                   int cols, int rows,
                                   float px, float py, float radius_frac)
{
    float cw, ch, r;
    int n;
    if (cols < 1 || rows < 1 || w <= 0.0f || h <= 0.0f) return -1;
    cw = w / (float)cols;
    ch = h / (float)rows;
    r  = (cw < ch ? cw : ch) * radius_frac;
    for (n = 0; n < cols * rows; n++) {
        float cx, cy, ddx, ddy;
        ui_lock_grid_center(x, y, w, h, cols, rows, n, &cx, &cy);
        ddx = px - cx;
        ddy = py - cy;
        if (ddx * ddx + ddy * ddy <= r * r) return n;
    }
    return -1;
}

/* Appends `node` to a tap sequence, ignoring one already in it.
 *
 * This is the rule a DRAG needs: the finger stays on a node for many frames and
 * each of those frames asks the same question. A caller that appended blindly
 * would turn one deliberate touch into nine identical entries. Coming back over
 * an already-drawn node changes nothing either, which is what lets the line
 * cross itself - the same rule Android applies. */
static inline int ui_lock_grid_append(unsigned char *seq, int len, int cap, int node)
{
    int i;
    if (!seq || node < 0 || node > 8 || len >= cap) return len;
    for (i = 0; i < len; i++) if (seq[i] == (unsigned char)node) return len;
    seq[len] = (unsigned char)node;
    return len + 1;
}

#endif /* UI_LOCK_GRID_H */
