/* test_lock_grid.c - the lock screen's 3x3 grid arithmetic
 * (clients/borealis/ui/lock_grid.h).
 *
 * The grid is addressed by three input paths that share nothing - a D-pad, a
 * finger, a mouse - and each of them has its own way of being off by one. This
 * suite is where that arithmetic is settled once, so the view is left with
 * nothing but colours.
 *
 * Each check names the symptom it prevents. Three of them matter more than the
 * rest:
 *
 *   - the edges STOP. Wrapping would read as a slip of the thumb, and on a
 *     screen where each move can append a node to a secret, a surprise move is
 *     a wrong pattern you have to start over.
 *
 *   - the catch radius leaves a GAP between targets. At half a cell the targets
 *     touch, every point belongs to some node, and a finger sliding across
 *     latches the neighbour before reaching it - the line lags behind the
 *     finger and the pattern gains nodes nobody aimed at.
 *
 *   - a node already in the sequence is ignored. A finger resting on one node
 *     asks the same question sixty times a second; appending blindly turns one
 *     deliberate touch into nine identical entries.
 */
#include "../clients/borealis/ui/lock_grid.h"

#include <stdio.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                                \
    checks++;                                                                   \
    if (!(cond)) { failures++; printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); } \
} while (0)

/* ── Le deplacement ───────────────────────────────────────────────────────── */

static void the_cursor(void)
{
    /* The grid, for reading what follows:   0 1 2
     *                                       3 4 5
     *                                       6 7 8   */
    CHECK(ui_lock_grid_move(4,  1,  0, 3, 3) == 5, "[curseur] right from the centre");
    CHECK(ui_lock_grid_move(4, -1,  0, 3, 3) == 3, "[curseur] left");
    CHECK(ui_lock_grid_move(4,  0, -1, 3, 3) == 1, "[curseur] up");
    CHECK(ui_lock_grid_move(4,  0,  1, 3, 3) == 7, "[curseur] down");
    CHECK(ui_lock_grid_move(4,  1,  1, 3, 3) == 8, "[curseur] both axes at once");

    /* THE COUNTER-CASE: the edges stop. A wrap here would jump the cursor to
     * the far side of the grid on a press the user meant as "no further". */
    CHECK(ui_lock_grid_move(2,  1,  0, 3, 3) == 2, "[bords] the right edge stops");
    CHECK(ui_lock_grid_move(0, -1,  0, 3, 3) == 0, "[bords] the left edge stops");
    CHECK(ui_lock_grid_move(1,  0, -1, 3, 3) == 1, "[bords] the top edge stops");
    CHECK(ui_lock_grid_move(7,  0,  1, 3, 3) == 7, "[bords] the bottom edge stops");
    CHECK(ui_lock_grid_move(0, -1, -1, 3, 3) == 0, "[bords] a corner stops on both axes");
    CHECK(ui_lock_grid_move(8,  1,  1, 3, 3) == 8, "[bords] and so does the opposite one");
    /* A diagonal against one edge still moves along the other: stopping both
     * would make the corner a trap you can only leave with a straight press. */
    CHECK(ui_lock_grid_move(2,  1,  1, 3, 3) == 5, "[bords] blocked on x, still moves on y");

    CHECK(ui_lock_grid_move(-1, 1, 0, 3, 3) == 0, "[bornes] a node below the grid");
    CHECK(ui_lock_grid_move(99, 1, 0, 3, 3) == 0, "[bornes] a node above it");
    CHECK(ui_lock_grid_move(4,  0, 0, 3, 3) == 4, "[curseur] no movement is no movement");
}

/* ── Les centres ──────────────────────────────────────────────────────────── */

static void the_centres(void)
{
    float cx = -1, cy = -1;

    /* At a third of the side each, the centres fall on 1/6, 3/6, 5/6 - NOT on
     * 0, 1/2, 1. Placed on the edges, half of each outer node's touch target
     * would sit outside the area and be unreachable. */
    ui_lock_grid_center(0, 0, 300, 300, 3, 3, 0, &cx, &cy);
    CHECK(cx == 50.0f && cy == 50.0f, "[centres] node 0 sits at one sixth, not in the corner");
    ui_lock_grid_center(0, 0, 300, 300, 3, 3, 4, &cx, &cy);
    CHECK(cx == 150.0f && cy == 150.0f, "[centres] node 4 is the middle");
    ui_lock_grid_center(0, 0, 300, 300, 3, 3, 8, &cx, &cy);
    CHECK(cx == 250.0f && cy == 250.0f, "[centres] node 8 is five sixths, not the far corner");
    ui_lock_grid_center(0, 0, 300, 300, 3, 3, 2, &cx, &cy);
    CHECK(cx == 250.0f && cy == 50.0f, "[centres] node 2 is top-right, in reading order");
    ui_lock_grid_center(0, 0, 300, 300, 3, 3, 6, &cx, &cy);
    CHECK(cx == 50.0f && cy == 250.0f, "[centres] node 6 is bottom-left");

    /* The offset is honoured: the grid is drawn inside a page, not at 0,0. */
    ui_lock_grid_center(100, 40, 300, 300, 3, 3, 0, &cx, &cy);
    CHECK(cx == 150.0f && cy == 90.0f, "[centres] the area's origin is added");

    ui_lock_grid_center(0, 0, 300, 300, 3, 3, 9, &cx, &cy);
    CHECK(cx == 0.0f && cy == 0.0f, "[bornes] a node outside the grid gives the origin");
}

/* ── La zone d'accroche ───────────────────────────────────────────────────── */

static void the_hit_test(void)
{
    CHECK(ui_lock_grid_hit(0, 0, 300, 300, 3, 3,  50,  50, 0.35f) == 0, "[accroche] dead on node 0");
    CHECK(ui_lock_grid_hit(0, 0, 300, 300, 3, 3, 150, 150, 0.35f) == 4, "[accroche] dead on the centre");
    CHECK(ui_lock_grid_hit(0, 0, 300, 300, 3, 3, 250, 250, 0.35f) == 8, "[accroche] dead on node 8");

    /* THE COUNTER-CASE. Exactly halfway between two nodes must be NOBODY. With
     * touching targets a finger sliding from 0 to 1 latches 1 while it is still
     * over the gap, the drawn line lags behind the finger, and the pattern
     * gains nodes the user did not aim at. */
    CHECK(ui_lock_grid_hit(0, 0, 300, 300, 3, 3, 100, 50, 0.35f) == -1,
          "[accroche] halfway between two nodes is neither");
    CHECK(ui_lock_grid_hit(0, 0, 300, 300, 3, 3, 100, 100, 0.35f) == -1,
          "[accroche] and the diagonal gap likewise");

    /* Just inside and just outside the radius: 0.35 of a 100-wide cell is 35. */
    CHECK(ui_lock_grid_hit(0, 0, 300, 300, 3, 3, 50 + 34, 50, 0.35f) == 0, "[accroche] just inside catches");
    CHECK(ui_lock_grid_hit(0, 0, 300, 300, 3, 3, 50 + 36, 50, 0.35f) == -1, "[accroche] just outside does not");

    /* Round and not square. The corner of a square target is the point
     * FURTHEST from the node aimed at, and catching it is how a diagonal drag
     * picks up a node it never passed over. The radius is 35, so a diagonal
     * offset is inside the circle only out to 35/sqrt(2) = 24.7 - while a
     * square of half-side 35 would catch everything out to (35, 35). */
    CHECK(ui_lock_grid_hit(0, 0, 300, 300, 3, 3, 50 + 24, 50 + 24, 0.35f) == 0,
          "[accroche] a diagonal offset inside the circle catches");
    CHECK(ui_lock_grid_hit(0, 0, 300, 300, 3, 3, 50 + 30, 50 + 30, 0.35f) == -1,
          "[accroche] the square's corner does NOT - the target is round");

    CHECK(ui_lock_grid_hit(0, 0, 0, 0, 3, 3,  50,  50, 0.35f) == -1, "[bornes] a zero-sized area");
    CHECK(ui_lock_grid_hit(0, 0, 300, 300, 3, 3, 999, 999, 0.35f) == -1, "[bornes] far outside");
    CHECK(ui_lock_grid_hit(0, 0, 300, 300, 3, 3, -50, -50, 0.35f) == -1, "[bornes] before the origin");
    /* A radius of zero still catches the exact centre, and nothing else: the
     * degenerate case must not become "catches everything". */
    CHECK(ui_lock_grid_hit(0, 0, 300, 300, 3, 3,  50,  50, 0.0f) == 0,  "[bornes] a zero radius catches the centre");
    CHECK(ui_lock_grid_hit(0, 0, 300, 300, 3, 3,  51,  50, 0.0f) == -1, "[bornes] and nothing beside it");
}

/* ── L'ajout au geste ─────────────────────────────────────────────────────── */

static void the_append(void)
{
    unsigned char seq[9];
    int n = 0;

    /* THE COUNTER-CASE. A finger resting on one node asks this question on
     * every frame. Appending blindly would make one deliberate touch fill the
     * whole sequence with the same node. */
    n = ui_lock_grid_append(seq, n, 9, 4);
    n = ui_lock_grid_append(seq, n, 9, 4);
    n = ui_lock_grid_append(seq, n, 9, 4);
    CHECK(n == 1, "[geste] a motionless finger appends once, not once per frame");
    CHECK(seq[0] == 4, "[geste] and it is the node it rested on");

    n = ui_lock_grid_append(seq, n, 9, 0);
    CHECK(n == 2 && seq[1] == 0, "[geste] a new node is appended");
    /* Coming back over a node already drawn does not append it again - that is
     * the same rule Android applies, and what lets a line cross itself. */
    n = ui_lock_grid_append(seq, n, 9, 4);
    CHECK(n == 2, "[geste] passing back over a visited node changes nothing");

    { int k;
      for (k = 0; k < 9; k++) n = ui_lock_grid_append(seq, n, 9, k);
      CHECK(n == 9, "[geste] the full grid fits"); }
    n = ui_lock_grid_append(seq, n, 9, 3);
    CHECK(n == 9, "[bornes] and nothing is written past it");

    n = 0;
    CHECK(ui_lock_grid_append(seq, n, 9, -1) == 0, "[bornes] a node below the grid is refused");
    CHECK(ui_lock_grid_append(seq, n, 9,  9) == 0, "[bornes] and above it");
    CHECK(ui_lock_grid_append(NULL, n, 9, 4) == 0, "[bornes] NULL");
    CHECK(ui_lock_grid_append(seq, 9, 9, 4) == 9, "[bornes] a full sequence is returned unchanged");
}

/* ── Le pave PIN : trois colonnes, quatre rangees ─────────────────────────── */

static void the_keypad(void)
{
    float cx = -1, cy = -1;

    /* The PIN pad is 3x4 and uses the SAME arithmetic. It is here because a
     * grid helper written for a square and then handed a rectangle is a classic
     * way to get cells that drift as you go down the page.
     *
     *      0 1 2      1 2 3
     *      3 4 5      4 5 6
     *      6 7 8  =   7 8 9
     *      9 10 11    del 0 ok   */
    CHECK(ui_lock_grid_move(1, 0, 1, 3, 4) == 4,  "[pave] down moves a full row");
    CHECK(ui_lock_grid_move(9, 0, 1, 3, 4) == 9,  "[pave] the bottom row stops");
    CHECK(ui_lock_grid_move(11, 1, 0, 3, 4) == 11, "[pave] the right edge stops");
    CHECK(ui_lock_grid_move(11, 0, -1, 3, 4) == 8, "[pave] up from the last cell");
    /* The cell count follows the shape, not a square: cell 9 exists on a 3x4
     * and does not on a 3x3. */
    CHECK(ui_lock_grid_move(9, 0, 0, 3, 3) == 0,  "[pave] cell 9 is outside a 3x3");
    CHECK(ui_lock_grid_move(9, 0, 0, 3, 4) == 9,  "[pave] and inside a 3x4");

    /* Non-square cells: 300 wide over 3 columns is 100, 400 tall over 4 rows is
     * 100 as well - so the centres stay on halves. */
    ui_lock_grid_center(0, 0, 300, 400, 3, 4, 0, &cx, &cy);
    CHECK(cx == 50.0f && cy == 50.0f, "[pave] the first cell's centre");
    ui_lock_grid_center(0, 0, 300, 400, 3, 4, 11, &cx, &cy);
    CHECK(cx == 250.0f && cy == 350.0f, "[pave] the last cell's centre");
    /* Rows narrower than columns: the vertical step must follow the ROW count,
     * not the column count. Getting that wrong puts the bottom row off the
     * area, which is exactly the drift this check exists for. */
    ui_lock_grid_center(0, 0, 300, 200, 3, 4, 9, &cx, &cy);
    CHECK(cx == 50.0f && cy == 175.0f, "[pave] the vertical step follows the row count");

    /* The catch radius follows the SMALLER side, so a flat cell does not get a
     * target taller than itself. Cells here are 100 x 50, radius 0.35*50 = 17.5. */
    CHECK(ui_lock_grid_hit(0, 0, 300, 200, 3, 4, 50, 25, 0.35f) == 0,
          "[pave] dead on the first cell");
    CHECK(ui_lock_grid_hit(0, 0, 300, 200, 3, 4, 50, 25 + 17, 0.35f) == 0,
          "[pave] just inside the smaller half-side");
    CHECK(ui_lock_grid_hit(0, 0, 300, 200, 3, 4, 50, 25 + 19, 0.35f) == -1,
          "[pave] and outside it, even though the cell is wide");
}

int main(void)
{
    printf("test_lock_grid - the lock screen's grids\n");
    the_cursor();
    the_centres();
    the_hit_test();
    the_append();
    the_keypad();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
