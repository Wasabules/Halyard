/* ui::pad - see pad_draw.hpp for why this screen exists. */

#include "pad_draw.hpp"

#include "../device_caps.h"

#include "anim.h"
#include "paint.hpp"
#include "theme.hpp"

#include <math.h>

namespace ui {
namespace pad {

/* Reference geometry, in arbitrary units. Everything is expressed in them and
 * then scaled: changing the frame size requires touching no coordinate. */
namespace {
const float REF_W = 300.0f;
/* The PlayStation art is a SLAB - one piece, no grips - so it needs a wider
 * box. Drawing it in the Nintendo 3:2 frame left a third of the height empty
 * above and below, which reads as a rendering fault rather than as a shape. */
#if SHADOW_PAD_ART == SHADOW_PAD_ART_PLAYSTATION
const float REF_H = 150.0f;
#else
const float REF_H = 200.0f;
#endif

struct Scale { float x, y, k; };

inline float px(const Scale &e, float u) { return e.x + u * e.k; }
inline float py(const Scale &e, float u) { return e.y + u * e.k; }
inline float pk(const Scale &e, float u) { return u * e.k; }

/* Colour of an element depending on whether it is pressed. An element at rest
 * stays VISIBLE (blue-grey): a gamepad where only the pressed parts showed
 * would not tell you whether it is plugged in at all. */
NVGcolor colorFor(bool active, NVGcolor tint, bool present)
{
    if (!present) return nvgRGBA(60, 64, 76, 140);   /* absent: everything fades out */
    if (active)   return tint;
    return nvgRGBA(88, 96, 116, 220);
}

void roundButton(NVGcontext *vg, const Scale &e, float ux, float uy, float ur,
                 bool active, NVGcolor tint, bool present, const char *label)
{
    const float cx = px(e, ux), cy = py(e, uy), r = pk(e, ur);

    /* A halo when pressed: that is what makes a press readable from a
     * distance, more so than the colour change alone. */
    if (active && present) {
        NVGpaint halo = nvgRadialGradient(vg, cx, cy, r, r * 2.1f,
                                          nvgRGBA(tint.r * 255, tint.g * 255,
                                                  tint.b * 255, 110),
                                          nvgRGBA(0, 0, 0, 0));
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r * 2.1f);
        nvgFillPaint(vg, halo);
        nvgFill(vg);
    }

    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r);
    nvgFillColor(vg, colorFor(active, tint, present));
    nvgFill(vg);

    if (label && *label) {
        nvgFontSize(vg, r * 1.15f);
        nvgFontFace(vg, theme::font());
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, active && present ? nvgRGBA(8, 12, 22, 255)
                                           : nvgRGBA(20, 24, 34, 210));
        nvgText(vg, cx, cy + r * 0.04f, label, nullptr);
    }
}

/* === S102 2026-08-29 - ONE COORDINATE CONVENTION, NOT TWO ===
 *
 * `roundButton` and `stick` took a CENTRE, `rectButton` and `trigger` took a
 * top-left corner. Two conventions in the same file, and nothing flagged it:
 * the positions read identically at the call site.
 *
 * The exact consequence, as it showed up in use: the d-pad and the triggers
 * were offset right and down by half their own size, hence misaligned with the
 * sticks sitting just above them. I had written EVERY position as a centre and
 * checked my distances against that assumption - the arithmetic was right, the
 * convention was not.
 *
 * Everything now takes a centre. That is the convention of the two functions
 * that already had a correct one, and the one you naturally reach for when
 * placing a control on a gamepad. */
void rectButton(NVGcontext *vg, const Scale &e, float ucx, float ucy,
                float uw, float uh, float ur,
                bool active, NVGcolor tint, bool present)
{
    const float ux = ucx - uw * 0.5f, uy = ucy - uh * 0.5f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, px(e, ux), py(e, uy), pk(e, uw), pk(e, uh), pk(e, ur));
    nvgFillColor(vg, colorFor(active, tint, present));
    nvgFill(vg);
}

/* A stick: its well, then the cap displaced by the axis. The well stays
 * visible even at rest - it is what gives the scale of the displacement,
 * without which you cannot tell a half-pushed stick from a fully pushed one. */
/* === S102 2026-08-29 - THE STICK TRAIL MAKES DRIFT VISIBLE ===
 *
 * A stick at rest reporting 3 or 4 instead of 0 is invisible: the cap moves by
 * one pixel. And a worn stick does not describe a circle when you draw one -
 * it flattens, it catches. The instantaneous position tells you neither; the
 * PATH travelled tells you both at a glance.
 *
 * The ring buffer is a function-level `static`, which this repo only accepts
 * with a justification: it is not session state but DISPLAY state, bounded to
 * two lanes, with no effect on anything else, and a trail inherited from a
 * previous screen fades on its own in under a second. The alternative -
 * carrying it in `State` - would send a rendering detail travelling all the way
 * down the gamepad read path.
 *
 * One sample per frame per lane: at sixty frames the trail covers eight tenths
 * of a second, which is about how long a hand-drawn circle takes. */
/* === THE FOUR PLAYSTATION SYMBOLS ======================================
 *
 * Drawn as GEOMETRY, not as text. The obvious shortcut was to hand the existing
 * `roundButton` the UTF-8 characters this repo already has in
 * `ui/key_label.h` - and it is a trap here: those glyphs come from whatever
 * font the theme loaded, they are laid out on a text baseline rather than
 * centred on the button, and if the face has no coverage for U+25B3 the
 * tester silently draws a tofu box on the one screen whose job is to be
 * believed. Four stroked paths depend on nothing.
 *
 * WHY THE SYMBOLS ARE NOT IN THE PLAYSTATION COLOURS. Colour is already
 * carrying this screen's actual information: `tint` distinguishes what the
 * console READ from what we SENT, which is the whole comparison the tester
 * exists for. Painting the triangle green and the circle red would put four
 * fixed colours next to a colour that MEANS something, and the one you must
 * read is the one that would stop standing out. The symbols are drawn in a
 * light neutral that holds against both the resting blue-grey and the tint. */
enum Symbol { SYM_TRIANGLE, SYM_CIRCLE, SYM_CROSS, SYM_SQUARE };

void symButton(NVGcontext *vg, const Scale &e, float ux, float uy, float ur,
               bool active, NVGcolor tint, bool present, Symbol sym)
{
    /* The disc, the halo and the resting colour are the shared ones: only the
     * mark on top changes, so a press reads identically on both layouts. */
    roundButton(vg, e, ux, uy, ur, active, tint, present, nullptr);

    const float cx = px(e, ux), cy = py(e, uy), r = pk(e, ur);
    const float a = r * 0.52f;              /* half-size of the mark */

    nvgBeginPath(vg);
    switch (sym) {
        case SYM_TRIANGLE:
            /* Sat on its CENTROID, not on the bounding box: a triangle centred
             * by its box looks pushed down, which on a button reads as a
             * misalignment rather than as a triangle. */
            nvgMoveTo(vg, cx,            cy - a * 1.10f);
            nvgLineTo(vg, cx + a * 1.0f, cy + a * 0.72f);
            nvgLineTo(vg, cx - a * 1.0f, cy + a * 0.72f);
            nvgClosePath(vg);
            break;
        case SYM_CIRCLE:
            nvgCircle(vg, cx, cy, a * 0.92f);
            break;
        case SYM_CROSS:
            nvgMoveTo(vg, cx - a * 0.86f, cy - a * 0.86f);
            nvgLineTo(vg, cx + a * 0.86f, cy + a * 0.86f);
            nvgMoveTo(vg, cx + a * 0.86f, cy - a * 0.86f);
            nvgLineTo(vg, cx - a * 0.86f, cy + a * 0.86f);
            break;
        case SYM_SQUARE:
            nvgRect(vg, cx - a * 0.84f, cy - a * 0.84f, a * 1.68f, a * 1.68f);
            break;
    }
    nvgStrokeColor(vg, present ? nvgRGBA(226, 232, 246, 235)
                               : nvgRGBA(120, 126, 140, 150));
    nvgStrokeWidth(vg, r * 0.155f);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgStroke(vg);
}

void stick(NVGcontext *vg, const Scale &e, float ucx, float ucy, float ur,
           float ax, float ay, bool click, NVGcolor tint, bool present,
           int lane)
{
    const float cx = px(e, ucx), cy = py(e, ucy), r = pk(e, ur);

    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r);
    nvgFillColor(vg, nvgRGBA(14, 18, 28, 200));
    nvgFill(vg);
    nvgStrokeColor(vg, nvgRGBA(96, 108, 132, 90));
    nvgStrokeWidth(vg, pk(e, 0.8f));
    nvgStroke(vg);

    /* The trail. Clamped BEFORE being recorded, with the same guards as the
     * position: a NaN in the ring would stay there for a whole second. */
    {
        /* === S111 2026-08-29 - ONE RING PER (GAMEPAD, STICK), NOT PER STICK ===
         *
         * The index used to designate the stick - left or right - and nothing
         * else. But the pause menu draws TWO gamepads side by side, "read" and
         * "sent": their left sticks wrote into the same ring, every other
         * position coming from a different one. The stroke therefore joined two
         * distinct points alternately, which draws a LINE between them instead
         * of the path travelled - reported verbatim as "a vertical line that
         * does not start from the centre".
         *
         * Four rings, and the caller says which gamepad it is drawing. */
        static const int N = 48;
        static float tx[4][48] = {{0}}, ty[4][48] = {{0}};
        static int   head[4] = {0, 0, 0, 0};
        const int v = (lane >= 0 && lane < 4) ? lane : 0;
        float sx = ax, sy = ay;
        if (isnan(sx)) sx = 0.0f;
        if (isnan(sy)) sy = 0.0f;
        if (sx < -1.0f) sx = -1.0f;
        if (sx > 1.0f)  sx = 1.0f;
        if (sy < -1.0f) sy = -1.0f;
        if (sy > 1.0f)  sy = 1.0f;
        tx[v][head[v]] = sx; ty[v][head[v]] = sy;
        head[v] = (head[v] + 1) % N;

        if (present) {
            /* Segment by segment rather than one polyline: that is what lets
             * the stroke FADE towards the past. A single polyline has only one
             * colour, and you would no longer know which way the thumb was
             * going. */
            for (int i = 1; i < N; i++) {
                const int a0 = (head[v] + i - 1) % N;
                const int a1 = (head[v] + i) % N;
                const float f = (float)i / (float)N;      /* 0 = the oldest */
                nvgBeginPath(vg);
                nvgMoveTo(vg, cx + tx[v][a0] * r * 0.62f,
                              cy - ty[v][a0] * r * 0.62f);
                nvgLineTo(vg, cx + tx[v][a1] * r * 0.62f,
                              cy - ty[v][a1] * r * 0.62f);
                /* The cast happens AFTER the multiplication. Written
                 * `(unsigned char)tint.r * 255`, it first truncated a component
                 * between 0 and 1 to an integer - so 0 - before multiplying:
                 * the trail lost all its red, silently. */
                nvgStrokeColor(vg, nvgRGBA((unsigned char)(tint.r * 255.0f),
                                           (unsigned char)(tint.g * 255.0f),
                                           (unsigned char)(tint.b * 255.0f),
                                           (unsigned char)(f * 150.0f)));
                nvgStrokeWidth(vg, pk(e, 1.2f));
                nvgStroke(vg);
            }
        }
    }

    /* The axes come from the network or from a driver: clamp them before
     * turning them into a position, otherwise an out-of-range value draws the
     * cap outside the frame. */
    /* NaN is tested FIRST: it fails both `< -1` and `> 1`, so clamping by
     * comparisons alone lets it through intact - and the cap then vanishes from
     * the drawing without a word. Same trap as in nav.h and anim.h. */
    if (isnan(ax)) ax = 0.0f;
    if (isnan(ay)) ay = 0.0f;
    if (ax < -1.0f) ax = -1.0f;
    if (ax >  1.0f) ax =  1.0f;
    if (ay < -1.0f) ay = -1.0f;
    if (ay >  1.0f) ay =  1.0f;

    const float travel = r * 0.52f;
    /* Positive `ay` = upwards; the screen's Y grows downwards, hence the sign. */
    const float hx = cx + ax * travel;
    const float hy = cy - ay * travel;

    /* Trace of the displacement: a line from the centre to the cap. It makes
     * DRIFT visible - a stick that does not return to centre - which a cap
     * sitting almost dead centre does not show on its own. */
    if (fabsf(ax) > 0.02f || fabsf(ay) > 0.02f) {
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, cy);
        nvgLineTo(vg, hx, hy);
        nvgStrokeColor(vg, nvgRGBA(tint.r * 255, tint.g * 255, tint.b * 255, 150));
        nvgStrokeWidth(vg, pk(e, 1.2f));
        nvgStroke(vg);
    }

    nvgBeginPath(vg);
    nvgCircle(vg, hx, hy, r * 0.56f);
    nvgFillColor(vg, colorFor(click, tint, present));
    nvgFill(vg);
}
/* Analog triggers. Guarded with the layout that draws them: the PS Vita has
 * no ZL/ZR, and an unused static here is a -Wunused-function warning on the
 * one build that cannot afford noise in its output. */
#if SHADOW_PAD_ART != SHADOW_PAD_ART_PLAYSTATION

/* An analog trigger: a gauge that fills up. A binary button would not tell you
 * whether the travel is complete - which is exactly what we want to check on
 * ZL/ZR. */
/* S102 - centre, like everything else in this file. */
void trigger(NVGcontext *vg, const Scale &e, float ucx, float ucy,
             float uw, float uh, float value, NVGcolor tint, bool present)
{
    const float ux = ucx - uw * 0.5f, uy = ucy - uh * 0.5f;
    if (isnan(value)) value = 0.0f;   /* before the comparisons: see `stick` */
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;

    const float x = px(e, ux), y = py(e, uy);
    const float w = pk(e, uw), h = pk(e, uh);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, pk(e, 1.5f));
    nvgFillColor(vg, nvgRGBA(14, 18, 28, 200));
    nvgFill(vg);

    if (value > 0.001f && present) {
        /* The gauge fills from the BOTTOM: that is the direction you push the
         * trigger, and the analogy saves having to explain it. */
        const float hh = h * value;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y + h - hh, w, hh, pk(e, 1.5f));
        nvgFillColor(vg, tint);
        nvgFill(vg);
    }
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, pk(e, 1.5f));
    nvgStrokeColor(vg, nvgRGBA(96, 108, 132, 80));
    nvgStrokeWidth(vg, pk(e, 0.6f));
    nvgStroke(vg);
}
#endif
}  // namespace

float aspect() { return REF_W / REF_H; }

void draw(NVGcontext *vg, float x, float y, float w, float h,
          const State &e, NVGcolor tint, double t, int gamepad)
{
    /* S111 - which gamepad are we drawing? Two can be shown side by side, and
     * their trails must stay distinct. */
    const int base = (gamepad == 1) ? 2 : 0;
    (void)t;   /* the press state is read directly; no animation here */

    /* Fit inside the frame WITHOUT distorting: the smaller of the two scales
     * wins, and the rest is centred. A stretched gamepad would be harder to
     * read than a smaller one. */
    const float k = (w / REF_W < h / REF_H) ? w / REF_W : h / REF_H;
    Scale sc;
    sc.k = k;
    sc.x = x + (w - REF_W * k) * 0.5f;
    sc.y = y + (h - REF_H * k) * 0.5f;

    /* === S101 2026-08-29 - THE GAMEPAD FINALLY USES THE SPACE IT IS GIVEN ===
     *
     * The body was only 96 units tall inside a reference frame offering 200:
     * half the allotted area stayed empty, and the gamepad looked tiny in the
     * middle of a whole screen. The buttons, meanwhile, kept their original
     * radius - hence A/B/X/Y spilling out of a body that had become too narrow
     * for them.
     *
     * And by separating the sticks from the buttons (S100) I opened a thirty
     * unit gap between the left stick and the d-pad, where a Switch has about
     * fifteen. Fixing an overlap by pushing the pieces as far apart as possible
     * trades one defect for another.
     *
     * So the layout is redone whole, like a real console: a wide body, a SCREEN
     * at the centre that genuinely occupies the middle, and on either side the
     * controls grouped as they are on the hardware. The distances are computed,
     * not eyeballed.
     *
     *   body         x  20..280   y  45..165
     *   screen       x  95..205   y  60..150
     *   left         x  38..76    (stick y 62..94, d-pad y 110..146)
     *   right        x 217..269   (buttons y 52..104, stick y 112..144)
     */

#if SHADOW_PAD_ART == SHADOW_PAD_ART_PLAYSTATION
    /* === THE PS VITA, AND WHAT IT DELIBERATELY DOES NOT DRAW ============
     *
     * Reference frame 300 x 150. A Vita is ONE SLAB: no grips, so the two
     * overflowing circles of the Nintendo body are gone rather than reused -
     * they are what makes that drawing read as a controller-with-handles.
     *
     * FOUR CONTROLS ARE ABSENT AND ARE NOT DRAWN AT ALL: ZL, ZR and the two
     * stick clicks. `pad_test.cpp` already spelled out why, and then relied on
     * an `absent[]` array that was never written - so on hardware those four
     * sat there permanently dark, which on THIS screen means "read nothing",
     * i.e. exactly the failure the tester is built to detect. A control the
     * machine does not have must be missing from the picture, not shown
     * unlit; that is also why the PS button is not drawn, since nothing
     * reports it here and a decorative button that never lights would be read
     * as a defect. (On a PlayStation TV a DualShock does provide all six, and
     * `pad_test.cpp` still READS them - what this layout drops is the
     * handheld's picture, not the input path.)
     *
     *   body      x  15..285   y  30..140
     *   screen    x  88..212   y  42..128
     *   left      x  34..70    (d-pad y 44..80, stick y 94..122)
     *   right     x 222..274   (buttons y 36..88, stick y 94..122)
     */

    /* --- Body ------------------------------------------------------------ */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, px(sc, 15), py(sc, 30), pk(sc, 270), pk(sc, 110), pk(sc, 30));
    nvgFillColor(vg, nvgRGBA(26, 32, 48, 235));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, px(sc, 15), py(sc, 30), pk(sc, 270), pk(sc, 110), pk(sc, 30));
    nvgStrokeColor(vg, nvgRGBA(104, 124, 168, 60));
    nvgStrokeWidth(vg, pk(sc, 1.0f));
    nvgStroke(vg);

    /* --- The screen -------------------------------------------------------
     * Before the controls, so that a drifting position lands on the screen
     * instead of hiding a button - the same rule as the other layout. */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, px(sc, 88), py(sc, 42), pk(sc, 124), pk(sc, 86), pk(sc, 5));
    nvgFillColor(vg, nvgRGBA(10, 14, 24, 255));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, px(sc, 88), py(sc, 42), pk(sc, 124), pk(sc, 86), pk(sc, 5));
    nvgStrokeColor(vg, nvgRGBA(104, 124, 168, 80));
    nvgStrokeWidth(vg, pk(sc, 1.0f));
    nvgStroke(vg);

    /* --- L and R, the only shoulders this console has --------------------- */
    rectButton(vg, sc,  52, 22, 40, 11, 5, e.pressed[L], tint, e.present);
    rectButton(vg, sc, 248, 22, 40, 11, 5, e.pressed[R], tint, e.present);

    /* --- D-pad, ABOVE the left stick -------------------------------------
     * This is the arrangement that distinguishes a Vita from the other
     * layout at a glance: the two thumb rows sit d-pad over stick on the left
     * and buttons over stick on the right, where the Nintendo drawing puts
     * the stick above the d-pad and below the buttons. */
    rectButton(vg, sc,  39, 62, 11, 11, 3, e.pressed[LEFT],  tint, e.present);
    rectButton(vg, sc,  65, 62, 11, 11, 3, e.pressed[RIGHT], tint, e.present);
    rectButton(vg, sc,  52, 49, 11, 11, 3, e.pressed[UP],    tint, e.present);
    rectButton(vg, sc,  52, 75, 11, 11, 3, e.pressed[DOWN],  tint, e.present);

    /* --- The four symbols -------------------------------------------------
     * Placed by SYMBOL and mapped to the indices `pad_test.cpp` fills, which
     * follow Borealis: CROSS lands on A, CIRCLE on B, SQUARE on X, TRIANGLE
     * on Y. Writing the positions in symbol order and letting the index
     * follow is what keeps the picture honest if that mapping ever moves -
     * the alternative, placing by index, would silently draw the wrong
     * symbol lit. */
    symButton(vg, sc, 248, 45, 9, e.pressed[Y], tint, e.present, SYM_TRIANGLE);
    symButton(vg, sc, 265, 62, 9, e.pressed[B], tint, e.present, SYM_CIRCLE);
    symButton(vg, sc, 248, 79, 9, e.pressed[A], tint, e.present, SYM_CROSS);
    symButton(vg, sc, 231, 62, 9, e.pressed[X], tint, e.present, SYM_SQUARE);

    /* --- The two sticks, on the bottom row -------------------------------- */
    stick(vg, sc,  52, 108, 14, e.lx, e.ly, false, tint, e.present, base + 0);
    stick(vg, sc, 248, 108, 14, e.rx, e.ry, false, tint, e.present, base + 1);

    /* --- SELECT and START, bottom right ----------------------------------
     * Two small pills where the hardware has them, captioned underneath: at
     * this size a label inside would be a smear, and unlabelled they are the
     * two controls a reader is least able to guess. */
    rectButton(vg, sc, 230, 132, 15, 7, 3, e.pressed[MINUS], tint, e.present);
    rectButton(vg, sc, 258, 132, 15, 7, 3, e.pressed[PLUS],  tint, e.present);
    nvgFontSize(vg, pk(sc, 6.5f));
    nvgFontFace(vg, theme::font());
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, nvgRGBA(140, 152, 180, 200));
    nvgText(vg, px(sc, 230), py(sc, 124), "SELECT", nullptr);
    nvgText(vg, px(sc, 258), py(sc, 124), "START", nullptr);

#else
    /* --- Body ------------------------------------------------------------ */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, px(sc, 20), py(sc, 45), pk(sc, 260), pk(sc, 120), pk(sc, 34));
    nvgFillColor(vg, nvgRGBA(26, 32, 48, 235));
    nvgFill(vg);
    /* The two grips, as overflowing circles: enough to recognise a gamepad at a
     * glance, and far simpler than an exact outline. */
    nvgBeginPath(vg);
    nvgCircle(vg, px(sc, 57), py(sc, 150), pk(sc, 44));
    nvgCircle(vg, px(sc, 243), py(sc, 150), pk(sc, 44));
    nvgFillColor(vg, nvgRGBA(26, 32, 48, 235));
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, px(sc, 20), py(sc, 45), pk(sc, 260), pk(sc, 120), pk(sc, 34));
    nvgStrokeColor(vg, nvgRGBA(104, 124, 168, 60));
    nvgStrokeWidth(vg, pk(sc, 1.0f));
    nvgStroke(vg);

    /* --- The screen, at the centre --------------------------------------
     * Drawn BEFORE the controls: if a position ever drifts, the overlap shows
     * up on the screen instead of hiding a button. */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, px(sc, 95), py(sc, 60), pk(sc, 110), pk(sc, 90), pk(sc, 6));
    nvgFillColor(vg, nvgRGBA(10, 14, 24, 255));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, px(sc, 95), py(sc, 60), pk(sc, 110), pk(sc, 90), pk(sc, 6));
    nvgStrokeColor(vg, nvgRGBA(104, 124, 168, 80));
    nvgStrokeWidth(vg, pk(sc, 1.0f));
    nvgStroke(vg);

    /* --- Shoulders and triggers (above the body) ------------------------- */
    rectButton(vg, sc,  57, 36, 38, 12, 5, e.pressed[L], tint, e.present);
    rectButton(vg, sc, 243, 36, 38, 12, 5, e.pressed[R], tint, e.present);
    trigger(vg, sc,  57, 18, 38, 16, e.zl, tint, e.present);
    trigger(vg, sc, 243, 18, 38, 16, e.zr, tint, e.present);

    /* --- Right-hand buttons (Nintendo layout: A on the right) ------------ */
    roundButton(vg, sc, 243,  61, 9, e.pressed[X], tint, e.present, "X");
    roundButton(vg, sc, 260,  78, 9, e.pressed[A], tint, e.present, "A");
    roundButton(vg, sc, 226,  78, 9, e.pressed[Y], tint, e.present, "Y");
    roundButton(vg, sc, 243,  95, 9, e.pressed[B], tint, e.present, "B");

    /* --- D-pad ----------------------------------------------------------- */
    rectButton(vg, sc,  44, 128, 11, 11, 3, e.pressed[LEFT],  tint, e.present);
    rectButton(vg, sc,  70, 128, 11, 11, 3, e.pressed[RIGHT], tint, e.present);
    rectButton(vg, sc,  57, 115, 11, 11, 3, e.pressed[UP],    tint, e.present);
    rectButton(vg, sc,  57, 141, 11, 11, 3, e.pressed[DOWN],  tint, e.present);

    /* --- Sticks -----------------------------------------------------------
     * Fifteen units separate the left stick from the d-pad, seven the B button
     * from the right stick: that is the spacing of a real gamepad, not the one
     * you get by pushing the pieces as far apart as they will go. */
    stick(vg, sc,  57,  78, 16, e.lx, e.ly, e.pressed[STICK_L], tint, e.present, base + 0);
    stick(vg, sc, 243, 128, 16, e.rx, e.ry, e.pressed[STICK_R], tint, e.present, base + 1);

    /* --- Minus / Plus, on either side of the screen ---------------------- */
    roundButton(vg, sc,  85, 55, 6, e.pressed[MINUS], tint, e.present, "-");
    roundButton(vg, sc, 215, 55, 6, e.pressed[PLUS],  tint, e.present, "+");
#endif  /* SHADOW_PAD_ART */
}

}  // namespace pad
}  // namespace ui
