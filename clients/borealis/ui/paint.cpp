/* ui::paint - see paint.hpp for the two structural choices (no blur, light edge
 * on top only). This file holds nothing but free, stateless functions: nothing
 * to destroy, hence nothing that can be used after being freed.
 */
#include "paint.hpp"

#include <borealis/core/assets.hpp>   /* BRLS_RESOURCES: romfs:/, app0:resources/, ... */
extern "C" {
#include "../../../core/services/journal.h"
}
#include <string>

#include <vector>

#include <cstdlib>

#include "scales.hpp"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <cmath>

namespace ui {
namespace paint {

namespace {

/* Fade a tint towards its own transparency. Writing nvgRGBA(0,0,0,0) would work
 * here (both nanovg backends in this project premultiply the gradient stops
 * before interpolating them), but naming the tint keeps the gradient readable
 * and survives a backend that does not premultiply. */
NVGcolor faded(NVGcolor c) { return nvgTransRGBA(c, 0); }

/* Integer mix (a splitmix32 variant). Used by the grain: we want FROZEN noise,
 * identical from one frame to the next. A rand() would make the grain sparkle,
 * which draws the eye exactly where it should not go. */
unsigned int mix32(unsigned int x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float unitFloat(unsigned int h) { return (float)(h & 0xffffffu) / (float)0xffffffu; }

/* Grain: a few hundred barely visible dots, all in ONE path hence ONE fill. It
 * breaks up the flatness of the translucent fill, which without it looks "flat"
 * where real glass has substance.
 *
 * The number of dots is CAPPED: the cost must be the same for a full-screen
 * panel and for a card, otherwise the first edge-to-edge window would tank the
 * frame rate. The dots are placed relative to the panel's origin and the seed is
 * fixed: the grain is welded to the panel and does not crawl when the panel
 * animates. */
void grain(NVGcontext *vg, float x, float y, float w, float h, float radius)
{
    const float ins = radius;                      /* stay clear of the rounded corners */
    const float gw  = w - ins * 2.0f;
    const float gh  = h - ins * 2.0f;
    if (gw <= 2.0f || gh <= 2.0f) return;

    int n = (int)(gw * gh / 2600.0f);
    if (n > 160) n = 160;
    if (n <= 0) return;

    nvgBeginPath(vg);
    for (int i = 0; i < n; i++) {
        const unsigned int hx = mix32((unsigned int)i * 2u + 1u);
        const unsigned int hy = mix32((unsigned int)i * 2u + 2u);
        const float px = x + ins + unitFloat(hx) * gw;
        const float py = y + ins + unitFloat(hy) * gh;
        const float s  = 1.0f + unitFloat(mix32(hx ^ hy)) * 1.4f;
        nvgRect(vg, px, py, s, s);
    }
    nvgFillColor(vg, glassGrain);
    nvgFill(vg);
}

/* The top-edge reflection. It is drawn as a complete OUTLINE of the rounded
 * shape, but painted with a vertical gradient that dies out very fast: the top
 * and the crown of both corners catch the light, the sides do not. A plain
 * horizontal line would have been shorter to write, but it stops dead where the
 * corners begin and then reads as a bar laid on top. */
void topHighlight(NVGcontext *vg, float x, float y, float w, float h, float radius,
                  NVGcolor tint)
{
    const float falloff = (h * 0.35f < 70.0f) ? h * 0.35f : 70.0f;
    NVGpaint p = nvgLinearGradient(vg, x, y, x, y + falloff, tint, faded(tint));
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, radius);
    nvgStrokeWidth(vg, 1.4f);
    nvgStrokePaint(vg, p);
    nvgStroke(vg);
}

/* Drop shadow: this is what puts the surface ABOVE the background. Without it a
 * translucent panel reads as a stain in the gradient, not as an object.
 * The hole in the middle keeps us from painting under the panel, which is
 * translucent and would let the shadow show through. */
void dropShadow(NVGcontext *vg, float x, float y, float w, float h, float radius,
                float offset, float blur, NVGcolor tint)
{
    NVGpaint p = nvgBoxGradient(vg, x, y + offset, w, h, radius + 2.0f, blur,
                                tint, faded(tint));
    nvgBeginPath(vg);
    nvgRect(vg, x - blur - 4.0f, y - blur - 4.0f,
                w + (blur + 4.0f) * 2.0f, h + (blur + 4.0f) * 2.0f + offset);
    nvgRoundedRect(vg, x, y, w, h, radius);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, p);
    nvgFill(vg);
}

}  // namespace

/* === GRADIENTS DITHERED AT THE SOURCE (2026-08-27) ===
 *
 * An 8-bit-per-channel gradient SHOWS ITS BANDS as soon as it is long and low in
 * contrast. The background runs from (16,26,54) to (4,6,14): twelve levels of
 * red spread over 720 pixels. Measured: 707 lines out of 719 identical to the
 * one above, in steps of 60 lines. That is exactly the staircase we were seeing,
 * and no amount of precision in computing the gradient changes it - it is the
 * DISPLAY that lacks precision.
 *
 * === WHY OVERLAYING A DITHER PATTERN DOES NOT WORK ===
 *
 * Dithering only works if it is applied BEFORE quantisation. A pattern laid over
 * the gradient arrives AFTER: NanoVG has computed the colour, the GPU has
 * written it in 8 bits, the step is already carved in. All you can do then is
 * mask it. Three attempts proved it, each one measured: white noise (positive
 * mean, shifts the gradient without breaking a single boundary), a Bayer matrix
 * (alternating line means -> stripes of +/-1 level on every other line),
 * zero-mean noise (attenuates, does not dissolve, and becomes a visible texture
 * as soon as you raise the amplitude).
 *
 * === WHAT WE DO INSTEAD ===
 *
 * The gradient is computed in FLOATING POINT then quantised with ERROR
 * DIFFUSION: a pixel's rounding error is carried over to the next one, so that
 * the local mean follows the ideal curve exactly. The result is uploaded once as
 * a texture and simply blitted - zero cost at runtime.
 *
 * Measured on the background's red channel (the worst case): lines identical to
 * the one above, and the longest run of them.
 *
 *      no dithering          707/719   run 60
 *      error diffusion        81/719   run  4      <- what we do
 *
 * The deviation from the ideal gradient drops to 0.23 levels: the tints are
 * INTACT, it is the quantisation that disappears, not the nuance. */
namespace {

/* Width of the tile. Diffusion runs along a COLUMN; several columns with
 * different phases shift horizontally the point where each step flips, and it is
 * that shift which makes the hard line vanish.
 * Longest run of flat lines measured against tile width:
 * 16 -> 8, 32 -> 4, 64 -> 4, 128 -> 3. Past 64 you pay memory for a gain you
 * cannot see. */
const int DITHER_W = 64;
const int DITHER_H = 720;   /* screen height: the background is then 1:1 */

int gradientTexture(NVGcontext *vg, NVGcolor top, NVGcolor bottom)
{
    unsigned char *px = (unsigned char *)malloc((size_t)DITHER_W * DITHER_H * 4);
    if (!px) return -1;

    for (int c = 0; c < 4; c++) {          /* alpha included: the glass has one */
        const float a = top.rgba[c]    * 255.0f;
        const float b = bottom.rgba[c] * 255.0f;
        for (int x = 0; x < DITHER_W; x++) {
            /* Each column starts with a DIFFERENT error. Without that offset
             * they all flip at the same pixel and the step stays a hard line:
             * a run of 21 flat lines with equal phases, against 4 with shifted
             * ones. */
            float err = (float)(mix32((unsigned int)x * 2654435761u + 12345u)
                                % 1000u) / 1000.0f - 0.5f;
            for (int y = 0; y < DITHER_H; y++) {
                const float ideal = a + (b - a) * (float)y / (float)(DITHER_H - 1);
                const float v = ideal + err;
                int q = (int)(v + 0.5f);
                if (q < 0) q = 0; else if (q > 255) q = 255;
                err = v - (float)q;
                px[((size_t)y * DITHER_W + x) * 4 + c] = (unsigned char)q;
            }
        }
    }

    /* NO NVG_IMAGE_NEAREST here - the exact opposite of the overlaid dither
     * pattern we just removed, where it was essential. Linear filtering AVERAGES
     * neighbouring texels; since error diffusion guarantees that the local mean
     * follows the ideal curve, interpolating moves us CLOSER to the exact
     * gradient instead of spoiling it. That is what makes the texture usable at
     * any size (a 300 px thumbnail, a 1080 px background). */
    const int img = nvgCreateImageRGBA(vg, DITHER_W, DITHER_H,
                                       NVG_IMAGE_REPEATX | NVG_IMAGE_REPEATY, px);
    free(px);
    return img;
}

/* The application only has three vertical gradients (background, card, focused
 * card). So the cache is tiny and BOUNDED: once full we fall back on NanoVG's
 * native gradient rather than allocating without end. */
struct Gradient { NVGcolor top, bottom; int img; };
Gradient g_gradients[12];   /* cards (3) + glass + the three wave veils */
int      g_gradient_count = 0;

bool sameColor(NVGcolor a, NVGcolor b)
{
    for (int i = 0; i < 4; i++)
        if (a.rgba[i] != b.rgba[i]) return false;
    return true;
}

}  // namespace

NVGpaint degradeVertical(NVGcontext *vg, float x, float y, float h,
                         NVGcolor top, NVGcolor bottom)
{
    if (h <= 0.0f) h = 1.0f;

    /* Two identical tints: no gradient, hence no band to dissolve. Giving it a
     * texture would be a cache entry for nothing. */
    if (sameColor(top, bottom))
        return nvgLinearGradient(vg, x, y, x, y + h, top, bottom);

    int img = -1;
    for (int i = 0; i < g_gradient_count; i++)
        if (sameColor(g_gradients[i].top, top) &&
            sameColor(g_gradients[i].bottom, bottom)) { img = g_gradients[i].img; break; }

    if (img < 0 && g_gradient_count < (int)(sizeof g_gradients / sizeof g_gradients[0])) {
        img = gradientTexture(vg, top, bottom);
        if (img >= 0) {
            g_gradients[g_gradient_count].top    = top;
            g_gradients[g_gradient_count].bottom = bottom;
            g_gradients[g_gradient_count].img    = img;
            g_gradient_count++;
        }
    }
    if (img < 0)   /* allocation refused or cache full: native gradient */
        return nvgLinearGradient(vg, x, y, x, y + h, top, bottom);

    /* The tile REPEATS horizontally at its native size and stretches over the
     * whole requested height. Stretching vertically is harmless (see the linear
     * filtering above); stretching horizontally would smear the phases into each
     * other, hence the repeat. */
    return nvgImagePattern(vg, x, y, (float)DITHER_W, h, 0.0f, img, 1.0f);
}


/* --------------------------------------------------------- backgrounds */

/* === The WHOLE background in a single dithered texture (2026-08-27) ===
 *
 * The vertical gradient was already dithered at the source, but the radial glow
 * was painted ON TOP of it as a raw NanoVG gradient: it was therefore
 * re-quantised, and since it peaks in the middle, that is where it put the bands
 * back.
 *
 * Measured on the capture, longest run of identical lines at equal measurement
 * width: edges 7, centre 12. The map of the defect followed the map of the glow
 * exactly - that was no coincidence.
 *
 * So we compose gradient AND glow into ONE image, dithered as a single whole by
 * Floyd-Steinberg error diffusion with a serpentine scan (the error is spread
 * over the four neighbours, alternately rightwards then leftwards so as not to
 * create a directional drift). Simulating the same measurement protocol: longest
 * run 6 with the glow left undithered, 3 with everything dithered.
 *
 * One fill instead of two, as a bonus. */
static int g_bg_img = -1;
static int g_bg_w = 0, g_bg_h = 0;

static int backgroundTexture(NVGcontext *vg, int W, int H)
{
    if (W < 2 || H < 2) return -1;
    unsigned char *px   = (unsigned char *)malloc((size_t)W * H * 4);
    float         *cur  = (float *)calloc((size_t)W + 2, sizeof(float));
    float         *next = (float *)calloc((size_t)W + 2, sizeof(float));
    if (!px || !cur || !next) { free(px); free(cur); free(next); return -1; }

    /* Same constants as the original drawing code - changing them here without
     * changing them there would make the background diverge from its fallback. */
    const float r     = (float)W * 0.8f;
    const float cx    = (float)W * 0.5f;
    const float cy    = -(float)H * 0.20f;
    const float inr   = r * 0.08f;
    const float span  = r - inr;
    const float glow_a = topGlow.rgba[3];

    for (int c = 0; c < 3; c++) {
        const float tb = nightBlueTop.rgba[c]    * 255.0f;
        const float bb = nightBlueBottom.rgba[c] * 255.0f;
        const float lg = topGlow.rgba[c]         * 255.0f;
        memset(cur, 0, ((size_t)W + 2) * sizeof(float));
        for (int y = 0; y < H; y++) {
            memset(next, 0, ((size_t)W + 2) * sizeof(float));
            const float base = tb + (bb - tb) * (float)y / (float)(H - 1);
            const int   even = (y % 2) == 0;
            const int   dx   = even ? 1 : -1;
            for (int i = 0; i < W; i++) {
                const int   x   = even ? i : (W - 1 - i);
                const float ddx = (float)x - cx;
                const float ddy = (float)y - cy;
                /* NanoVG interpolates LINEARLY from the inner radius to the
                 * outer one: we reproduce its formula, otherwise the texture
                 * would not look like the fallback. */
                float t = (sqrtf(ddx * ddx + ddy * ddy) - inr) / span;
                if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
                const float a     = glow_a * (1.0f - t);
                const float ideal = lg * a + base * (1.0f - a);

                const float v = ideal + cur[x + 1];
                int q = (int)(v + 0.5f);
                if (q < 0) q = 0; else if (q > 255) q = 255;
                const float e = v - (float)q;
                px[((size_t)y * W + x) * 4 + c] = (unsigned char)q;

                /* Bounds: x+1+dx ranges from 0 to W+1, hence the two guard slots
                 * on either side of the W useful columns. */
                cur[x + 1 + dx] += e * (7.0f / 16.0f);
                next[x + 1 - dx] += e * (3.0f / 16.0f);
                next[x + 1]      += e * (5.0f / 16.0f);
                next[x + 1 + dx] += e * (1.0f / 16.0f);
            }
            float *tmp = cur; cur = next; next = tmp;
        }
    }
    for (size_t i = 0; i < (size_t)W * (size_t)H; i++) px[i * 4 + 3] = 255;

    const int img = nvgCreateImageRGBA(vg, W, H, 0, px);
    free(px); free(cur); free(next);
    return img;
}

/* === BRAND-1 2026-09-26 - THE APPLICATION'S OWN BACKGROUND ============
 *
 * resources/img/background.jpg (background_vita.jpg on the PS Vita), derived
 * from the masters in
 * clients/borealis/branding/ by tools/brand-assets.py. Loaded once, drawn
 * "cover" (scaled to fill, centre cropped, never stretched). If the file is
 * missing or unreadable, the procedural gradient below is drawn exactly as
 * before. SHADOW_UI_BACKGROUND=0 forces the gradient. */
static int g_photo = -2;                   /* -2 not tried yet, -1 unavailable */
static int g_photo_w = 0, g_photo_h = 0;

static bool drawPhotoBackground(NVGcontext *vg, float x, float y, float w, float h)
{
    if (g_photo == -2) {
        g_photo = -1;
        const char *e = getenv("SHADOW_UI_BACKGROUND");
        if (!e || atoi(e) != 0) {
            /* The PS Vita has its own variant, framed for its 960x544 screen;
             * every other target, or a Vita package without it, takes the
             * general one. */
            std::string path;
#if defined(__vita__) || defined(__psp2__)
            path = std::string(BRLS_RESOURCES) + "img/background_vita.jpg";
            g_photo = nvgCreateImage(vg, path.c_str(), 0);
            if (g_photo <= 0)
#endif
            {
                path = std::string(BRLS_RESOURCES) + "img/background.jpg";
                g_photo = nvgCreateImage(vg, path.c_str(), 0);
            }
            if (g_photo > 0) nvgImageSize(vg, g_photo, &g_photo_w, &g_photo_h);
            if (g_photo <= 0 || g_photo_w <= 0 || g_photo_h <= 0) g_photo = -1;
            if (g_photo > 0)
                JOURNAL_INFO_(JOURNAL_CAT_UI, "[BRAND-1] fond : %s (%dx%d)", path.c_str(),
                              g_photo_w, g_photo_h);
            else
                JOURNAL_INFO_(JOURNAL_CAT_UI, "[BRAND-1] fond %s illisible - degrade "
                              "procedural", path.c_str());
        }
    }
    if (g_photo < 0) return false;
    const float scale = fmaxf(w / (float)g_photo_w, h / (float)g_photo_h);
    const float dw = (float)g_photo_w * scale, dh = (float)g_photo_h * scale;
    nvgBeginPath(vg);
    nvgRect(vg, x, y, w, h);
    nvgFillPaint(vg, nvgImagePattern(vg, x + (w - dw) * 0.5f, y + (h - dh) * 0.5f,
                                     dw, dh, 0.0f, g_photo, 1.0f));
    nvgFill(vg);
    return true;
}

void shadowBg(NVGcontext *vg, float x, float y, float w, float h)
{
    if (!vg || w <= 0.0f || h <= 0.0f) return;
    if (drawPhotoBackground(vg, x, y, w, h)) return;

    const int W = (int)(w + 0.5f), H = (int)(h + 0.5f);
    /* Upper bound: a per-screen texture costs W*H*4 bytes (3.7 MB at 720p). Past
     * any plausible resolution we prefer the fallback to the risk of allocating
     * an absurd image from a nonsensical size. */
    if (W >= 2 && H >= 2 && W <= 1920 && H <= 1200) {
        if (g_bg_img < 0 || W != g_bg_w || H != g_bg_h) {
            if (g_bg_img >= 0) nvgDeleteImage(vg, g_bg_img);
            g_bg_img = backgroundTexture(vg, W, H);
            g_bg_w   = W;
            g_bg_h   = H;
        }
        if (g_bg_img >= 0) {
            nvgBeginPath(vg);
            nvgRect(vg, x, y, w, h);
            nvgFillPaint(vg, nvgImagePattern(vg, x, y, w, h, 0.0f, g_bg_img, 1.0f));
            nvgFill(vg);
            return;
        }
    }

    /* === FALLBACK: the original drawing code, in two fills ===
     * Taken if the allocation fails or if the size falls outside the domain. It
     * gives the same image, banding included - a background that bands beats a
     * black screen.
     *
     * The glow is centred ABOVE the frame: we do not want to see its centre, only
     * its falloff. It does double duty - it gives the background its relief, and
     * it is the light source that the panels' top edge reflects; moving one
     * without the other breaks the coherence.
     *
     * IT COVERS THE FULL HEIGHT. It used to be limited to `h * 0.66`, which cut a
     * HARD edge two thirds of the way down the screen - a plainly visible
     * horizontal line, reported as "the background stops in the middle". */
    NVGpaint bg = degradeVertical(vg, x, y, h, nightBlueTop, nightBlueBottom);
    nvgBeginPath(vg);
    nvgRect(vg, x, y, w, h);
    nvgFillPaint(vg, bg);
    nvgFill(vg);

    const float r = w * 0.8f;
    NVGpaint glow = nvgRadialGradient(vg, x + w * 0.5f, y - h * 0.20f,
                                      r * 0.08f, r, topGlow, faded(topGlow));
    nvgBeginPath(vg);
    nvgRect(vg, x, y, w, h);
    nvgFillPaint(vg, glow);
    nvgFill(vg);
}

/* === Background waves, XMB style (2026-08-27, 4th version) ===
 *
 * The history, because each version failed for a DIFFERENT reason:
 *
 *   1. FILLED AREAS under the curve: invisible at low alpha, washed out at high
 *      alpha. Unhooked (f9aa370) - and the function stayed ORPHANED, so that no
 *      alpha tweak had any effect at all any more.
 *   2. THREE concentric STROKES: a thick stroke does not fade out, it is a band
 *      with hard edges. Three bands = three edges, read as "two lines with a
 *      rising front between them".
 *   3. A VEIL as a polygon filled with a gradient ANCHORED IN SCREEN Y. The
 *      gradient does not follow the curve: the bottom of the polygon therefore
 *      landed somewhere the gradient had not died out yet, which left an EDGE
 *      shaped like the wave, offset downwards. One per wave, undulating -
 *      reported as "loads of tiny waves under the three big ones". Veiling one
 *      side only also created a step at the crest, read as "a shadow above and a
 *      halo below".
 *
 * 4th version: EVERYTHING FOLLOWS THE CURVE. Not one screen-anchored gradient is
 * left in this function - that was the source of the defect, not its settings.
 *
 *   - the CORE: twelve concentric strokes of decreasing width, at equal
 *     intensity. Their sum is a ramp, not a step; the jump from one stroke to
 *     the next is about one display level, so invisible. It is SYMMETRIC, which
 *     removes the step at the crest.
 *   - the AURA: fourteen abutting bands stroked along the curve SHIFTED
 *     DOWNWARDS, of decreasing intensity. They hug the wave exactly, so no edge
 *     can appear anywhere.
 *   - the FILAMENT: a thin, sharp stroke, the XMB signature.
 *
 * SHAPE: two waves only, heavily stretched (a single undulation across the
 * width) and almost parallel, with the harmonic cut to the bare minimum. Three
 * waves at 2.5 cycles made a pattern; two waves at 1 cycle make a sheet that
 * breathes. */
void backgroundWaves(NVGcontext *vg, float x, float y, float w, float h, double t)
{
    if (!vg || w <= 0.0f || h <= 0.0f) return;

    struct Wave {
        float height;                        /* rest position, fraction of h */
        float amp,  cycles,  speed;          /* main undulation */
        float amp2, cycles2, speed2;         /* harmonic, just enough to keep the
                                              * two waves out of step */
        float core;                          /* cumulative alpha of the core */
        float aura;                          /* alpha of the aura, below the crest */
    };
    /* ALMOST PARALLEL: same cycle count to within 15%, close but distinct
     * speeds. They drift slowly against each other, converge, separate - that
     * slow beat is what gives the "stretched bubble", not the undulation
     * itself. */
    static const Wave WAVES[] = {
        { 0.520f, 0.078f, 1.00f,  0.038f, 0.010f, 2.30f, -0.021f, 30.0f, 20.0f },
        { 0.635f, 0.070f, 1.15f,  0.030f, 0.008f, 1.70f,  0.017f, 25.0f, 16.0f },
    };
    /* Light blue, not white: on a night-blue background, pure white reads as a
     * display glitch rather than as light. */
    const float TR = 140.0f / 255.0f, TG = 190.0f / 255.0f, TB = 1.0f;

    /* === WHAT THIS COSTS, MEASURED, AND WHY IT IS NOW SCALED =============
     *
     * At full detail this draws 2 x (12 x 96 + 14 x 64) = **4096 stroked
     * segments per frame**, every one with round joins and caps. nanovg
     * tessellates strokes on the CPU, so that is tens of thousands of vertices
     * computed per frame - fine on a desktop, ruinous on a handheld.
     *
     * Measured on PS Vita, 2026-09-14, by splitting the frame four ways:
     * `attente-gpu 0.0 | dessin 193.6 | flush 2.1 | echange 0.1`. The GPU was
     * idle; the whole 196 ms frame was this function, on a settings page
     * showing THREE rows. Nine other hypotheses were eliminated first - our own
     * per-frame code, a leak, stack depth, text volume, the font, 4x MSAA, the
     * activity stack, vsync, the swap - and every one cost a deploy.
     *
     * The layer counts are what cost, not the segment counts: the core stacks
     * twelve strokes to fake a glow, and the aura fourteen. On a screen this
     * size four and five are visually near-identical and eight times cheaper.
     *
     * `SHADOW_UI_WAVES` overrides the detail (0 = no waves at all, 1..100 =
     * percent of full detail) so it can be judged by eye rather than argued
     * about. */
    static int g_detail = -1;
    if (g_detail < 0) {
        const char *e = getenv("SHADOW_UI_WAVES");
#if defined(__vita__) || defined(__psp2__)
        g_detail = e ? atoi(e) : 35;
#else
        g_detail = e ? atoi(e) : 100;
#endif
        if (g_detail < 0)   g_detail = 0;
        if (g_detail > 100) g_detail = 100;
    }
    if (g_detail == 0) return;
    const auto scaled = [](int full, int pct, int floor_) {
        const int v = full * pct / 100;
        return v < floor_ ? floor_ : v;
    };
    const int   N_CORE   = scaled(96, g_detail, 16);  /* a thin stroke shows its corners */
    const int   N_AURA   = scaled(64, g_detail, 12);  /* the aura is soft: fewer segments will do */
    const int   P_CORE   = scaled(12, g_detail, 3);   /* layers in the core */
    const int   P_AURA   = scaled(14, g_detail, 3);   /* bands in the aura */

    nvgSave(vg);
    nvgIntersectScissor(vg, x, y, w, h);
    nvgLineJoin(vg, NVG_ROUND);
    nvgLineCap(vg, NVG_ROUND);

    for (const Wave &o : WAVES) {
        const float base = y + h * o.height;
        /* Wrap BEFORE narrowing to float: `t` is a monotonic clock that can be
         * hundreds of thousands of seconds, and a float only holds it to about a
         * radian by then. The motion would turn jerky after a few days of
         * uptime - never reproduced in testing. */
        const float ph1 = (float)(fmod(t * (double)o.speed,  1.0) * 6.2831853);
        const float ph2 = (float)(fmod(t * (double)o.speed2, 1.0) * 6.2831853);

        /* The path OVERSHOOTS by 24 px on each side: otherwise the stroke's
         * round caps would land exactly on the scissor, and you would see the
         * wave start and end at the screen edge. */
        const float x0 = x - 24.0f, span = w + 48.0f;
        #define WAVE_Y(u) (base \
            + h * o.amp  * sinf((u) * 6.2831853f * o.cycles  + ph1) \
            + h * o.amp2 * sinf((u) * 6.2831853f * o.cycles2 + ph2))
        #define WAVE_PATH(nseg, shift) do {                                    \
            nvgBeginPath(vg);                                                  \
            for (int i_ = 0; i_ <= (nseg); i_++) {                             \
                const float u_ = (float)i_ / (float)(nseg);                    \
                const float px_ = x0 + u_ * span;                              \
                const float py_ = WAVE_Y(u_) + (shift);                        \
                if (i_ == 0) nvgMoveTo(vg, px_, py_);                          \
                else         nvgLineTo(vg, px_, py_);                          \
            }                                                                  \
        } while (0)

        /* === THE AURA, below the crest ===
         * ABUTTING bands along the shifted curve: each one hugs the wave, so the
         * falloff follows the shape instead of being cut off by a horizontal.
         * The intensity step from one band to the next is about one display
         * level - that is what makes it read as continuous. */
        {
            const float depth = h * 0.20f;
            const float step  = depth / (float)P_AURA;
            for (int j = 0; j < P_AURA; j++) {
                const float att = 1.0f - (float)j / (float)P_AURA;
                WAVE_PATH(N_AURA, (float)(j) * step + step * 0.5f);
                nvgStrokeWidth(vg, step);
                nvgStrokeColor(vg, nvgRGBAf(TR, TG, TB, o.aura / 255.0f * att));
                nvgStroke(vg);
            }
        }

        /* === THE CORE, symmetric ===
         * One single path, stroked twelve times from wide to thin. Symmetric
         * DELIBERATELY: a falloff on one side only puts a step at the crest,
         * which the eye reads as a shadow on the dark side. */
        WAVE_PATH(N_CORE, 0.0f);
        {
            const float WIDTH_MAX = h * 0.050f;
            const float WIDTH_MIN = 2.0f;
            const float a_layer = o.core / 255.0f / (float)P_CORE;
            for (int k = P_CORE - 1; k >= 0; k--) {
                const float f = (float)k / (float)(P_CORE - 1);
                nvgStrokeWidth(vg, WIDTH_MIN + (WIDTH_MAX - WIDTH_MIN) * f);
                nvgStrokeColor(vg, nvgRGBAf(TR, TG, TB, a_layer));
                nvgStroke(vg);
            }
            /* The filament: thin, sharp. It is the one the eye follows; without
             * it the wave is just haze. */
            nvgStrokeWidth(vg, 1.4f);
            nvgStrokeColor(vg, nvgRGBAf(TR, TG, TB, o.core / 255.0f * 0.42f));
            nvgStroke(vg);
        }

        #undef WAVE_PATH
        #undef WAVE_Y
    }

    nvgRestore(vg);
}

void glassPanel(NVGcontext *vg, float x, float y, float w, float h, float radius)
{
    if (!vg || w <= 0.0f || h <= 0.0f) return;

    dropShadow(vg, x, y, w, h, radius, 8.0f, 22.0f, shadowColor);

    NVGpaint body = degradeVertical(vg, x, y, h, glassFill, glassFillBottom);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, radius);
    nvgFillPaint(vg, body);
    nvgFill(vg);

    grain(vg, x, y, w, h, radius);

    /* Quiet border first, reflection second: the reflection has to pass OVER the
     * border along the top edge, otherwise the border's grey dulls it exactly
     * where it carries the whole effect. */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, radius);
    nvgStrokeWidth(vg, 1.0f);
    nvgStrokeColor(vg, glassBorder);
    nvgStroke(vg);

    topHighlight(vg, x, y, w, h, radius, glassHighlight);
}

void card(NVGcontext *vg, float x, float y, float w, float h,
           bool focused, bool active, bool led, double t)
{
    if (!vg || w <= 0.0f || h <= 0.0f) return;

    const float radius = CARD_RADIUS;

    /* Halo: light, and only on the focused element. It is the one place in the
     * interface where we spend light; if there is any elsewhere, you can no
     * longer tell where the cursor is. */
    if (focused)
        dropShadow(vg, x, y, w, h, radius, 0.0f, 18.0f, accentHalo);
    else
        dropShadow(vg, x, y, w, h, radius, 4.0f, 12.0f, nvgRGBA(0, 0, 0, 90));

    NVGcolor top, bottom;
    if (focused)     { top = cardBgFocusTop; bottom = cardBgFocusBottom; }
    else if (active) { top = cardBgTop;      bottom = cardBgBottom;      }
    else             { top = cardBgMuted;    bottom = cardBgMuted;       }

    NVGpaint body = degradeVertical(vg, x, y, h, top, bottom);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, radius);
    nvgFillPaint(vg, body);
    nvgFill(vg);

    /* No grain on cards: a list shows ten of them at once, so ten extra fills
     * per frame for an effect you cannot see at that size. Grain is a
     * large-surface effect.
     *
     * The gradient, on the other hand, is dithered AT THE SOURCE (see
     * degradeVertical): ever since the thumbnails became 400 pixels tall it is
     * long enough to show its bands, exactly like the background. */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, radius);
    nvgStrokeWidth(vg, focused ? 1.8f : 1.0f);
    nvgStrokeColor(vg, focused ? accentVif : cardBorder);
    nvgStroke(vg);

    /* The glass reflection does not stack with the accent: on the focused card
     * the edge is already painted bright blue, and a white reflection on top
     * only dirties the colour that carries the information. */
    if (!focused)
        topHighlight(vg, x, y, w, h, radius, glassHighlight);

    /* === Moving sheen on the FOCUSED card (2026-08-27) ===
     * A light diagonal band sweeping slowly across. It is what makes the card
     * read as a GLASS surface rather than a coloured rectangle - a fixed
     * reflection reads as just one more gradient.
     *
     * On the focused card only, for two reasons: a whole list of moving sheens
     * would be a shimmer, and motion is here the last thing that sets the chosen
     * element apart. It is VERY low in contrast (alpha 26): it should be felt
     * without being looked at. */
    if (focused)
        lightSheen(vg, x, y, w, h, radius,
                   (float)(fmod(t, (double)duration::AMBIANCE_LENTE)
                           / (double)duration::AMBIANCE_LENTE),
                   alpha::SUBLIMINAL);

    /* Status indicator: a DOT in the top-right corner, not a bar stuck to the
     * left edge - on a tall thumbnail that bar became a long stripe that read as
     * a display defect rather than as a state. A round dot, where one looks for
     * a status, says the same thing without drawing the eye.
     *
     * The ON/OFF state does NOT depend on focus: otherwise moving the cursor
     * would look like it was changing the machines' state.
     *
     * S78: and it is only drawn if the caller has a state to show. See paint.hpp
     * - on a settings row this dot was either redundant with the switch, or grey
     * forever, hence a false "unavailable". */
    const float pr = 6.0f;
    if (led && w > pr * 4.0f && h > pr * 4.0f) {
        const float px2 = x + w - 18.0f, py2 = y + 18.0f;
        /* A dark ring behind it: without it the dot gets lost against a light
         * thumbnail, and that is what happened - you could not make it out. */
        nvgBeginPath(vg);
        nvgCircle(vg, px2, py2, pr + 2.5f);
        nvgFillColor(vg, nvgRGBA(6, 10, 20, 170));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, px2, py2, pr);
        nvgFillColor(vg, active ? ledActive : ledOff);
        nvgFill(vg);
    }
}

/* --- Button hints --------------------------------------------------------
 *
 * Dimensions shared by both functions: separating them would let them drift, and
 * a few pixels of disagreement between the measurement and the drawing is enough
 * to push the last hint off the screen. */
namespace {
const float HINT_R      = 12.0f;   /* radius of the round pill */
const float HINT_GAP    = 7.0f;    /* pill -> label */
const float HINT_PAD    = 7.0f;    /* K21: side padding of the wide keycap */
const float HINT_FONT   = type::SECONDARY;   /* follows the scale */

/* K21 2026-09-12 - the pill's width, which is no longer a constant.
 *
 * It carried a single glyph until the footer started naming KEYS as well as
 * buttons (see `ui/key_label.h`): "Enter" and "L/R" do not fit inside a 24 px
 * circle, and drawn there they spilled over both edges into the neighbouring
 * hints. So the shape follows the name - and the DECISION is made by measuring
 * the text, not by counting characters, because "↑" is three bytes and one
 * glyph while "F1" is two of each.
 *
 * Both functions below call this one. That is the point: the measurement and
 * the drawing disagreeing by a few pixels is exactly what pushes the last hint
 * off the screen. */
float pillWidth(NVGcontext *vg, const char *button)
{
    if (!button || !*button) return HINT_R * 2.0f;
    nvgFontSize(vg, HINT_FONT - 2.0f);
    const float need = textWidth(vg, button) + HINT_PAD * 2.0f;
    return need > HINT_R * 2.0f ? need : HINT_R * 2.0f;
}
}

float hintWidth(NVGcontext *vg, const char *button, const char *label)
{
    if (!vg) return 0.0f;
    nvgSave(vg);
    nvgFontFace(vg, theme::font());
    const float wp = pillWidth(vg, button);
    nvgFontSize(vg, HINT_FONT);
    const float wl = (label && *label) ? textWidth(vg, label) : 0.0f;
    nvgRestore(vg);
    return wp + (wl > 0.0f ? HINT_GAP + wl : 0.0f);
}

float hintButton(NVGcontext *vg, float x, float y, const char *button,
                 const char *label)
{
    if (!vg) return 0.0f;

    nvgSave(vg);
    nvgFontFace(vg, theme::font());

    const float wp = pillWidth(vg, button);
    const float cx = x + wp * 0.5f;

    nvgBeginPath(vg);
    if (wp <= HINT_R * 2.0f + 0.01f) {
        nvgCircle(vg, cx, y, HINT_R);
    } else {
        /* K21 - a KEY, so a key's silhouette: a rounded rectangle, not the
         * capsule `badge` uses for a status and not the circle a console button
         * uses. Three kinds of information, three shapes - the radius is small
         * on purpose, because at HINT_R it would round back into a capsule and
         * read as a status pill. */
        nvgRoundedRect(vg, x, y - HINT_R, wp, HINT_R * 2.0f, 4.0f);
    }
    nvgFillColor(vg, nvgRGBA(236, 240, 250, 30));
    nvgFill(vg);
    nvgStrokeWidth(vg, 1.2f);
    nvgStrokeColor(vg, nvgRGBA(206, 218, 240, 130));
    nvgStroke(vg);

    if (button && *button) {
        /* The letter is a little smaller than the label: at the same size it
         * would look bigger, an isolated glyph filling its whole box. */
        nvgFontSize(vg, HINT_FONT - 2.0f);
        nvgFillColor(vg, nvgRGBA(232, 238, 250, 235));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(vg, cx, y + 0.5f, button, nullptr);
    }

    float w = wp;
    if (label && *label) {
        nvgFontSize(vg, HINT_FONT);
        nvgFillColor(vg, theme::hint);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(vg, x + w + HINT_GAP, y, label, nullptr);
        w += HINT_GAP + textWidth(vg, label);
    }
    nvgRestore(vg);
    return w;
}

void badge(NVGcontext *vg, float x, float y, const char *text, NVGcolor tint)
{
    if (!vg || !text || !*text) return;

    nvgSave(vg);
    nvgFontFace(vg, theme::font());
    nvgFontSize(vg, BADGE_FONT);

    const float w = textWidth(vg, text) + 20.0f;
    const float h = BADGE_HEIGHT;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, h * 0.5f);
    nvgFillColor(vg, nvgTransRGBA(tint, 38));
    nvgFill(vg);
    nvgStrokeWidth(vg, 1.0f);
    nvgStrokeColor(vg, nvgTransRGBA(tint, 120));
    nvgStroke(vg);

    /* Text at full tint over a heavily diluted background of the same tint: the
     * pill reads as a single colour, whoever the caller is. */
    nvgFillColor(vg, tint);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, x + 10.0f, y + h * 0.5f, text, nullptr);
    nvgRestore(vg);
}

float badgeWidth(NVGcontext *vg, const char *text)
{
    if (!vg || !text || !*text) return 0.0f;
    nvgSave(vg);
    nvgFontFace(vg, theme::font());
    nvgFontSize(vg, BADGE_FONT);
    const float w = textWidth(vg, text) + 20.0f;
    nvgRestore(vg);
    return w;
}

/* ---------------------------------------------------------------- text */

/* === Overlong text - the ONLY implementation in the repo ===
 * Do not copy this anywhere else. This code once lived twice, here and in
 * overlay_menu.cpp; the pause menu was brought back onto this version on
 * 2026-08-27, precisely because two copies of the same rule drift apart - you
 * fix a UTF-8 cutting bug on one side and not on the other.
 *
 * The original symptom: labels and descriptions overflowed their row, the text
 * carried on over the value on the right and then out of the frame. */

float textWidth(NVGcontext *vg, const char *txt)
{
    float b[4];
    nvgTextBounds(vg, 0.0f, 0.0f, txt, nullptr, b);
    return b[2] - b[0];
}

namespace {

/* Advance by ONE UTF-8 character. Cutting in the middle of a codepoint would
 * print a replacement character instead of an accented letter or an arrow - and
 * the interface is full of both. */
size_t nextUtf8(const std::string &s, size_t i)
{
    if (i >= s.size()) return s.size();
    size_t j = i + 1;
    while (j < s.size() && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80) j++;
    return j;
}

/* Where the text sits in its scrolling cycle. The pauses at both ends are what
 * make it readable: without them the start of the text is never still long
 * enough to be read. */
float scrollOffset(float overflow, double t)
{
    const double PAUSE = 1.2;    /* s at each end     */
    const double SPEED = 45.0;   /* pixels per second */
    const double travel = overflow / SPEED;
    const double cycle  = PAUSE + travel + PAUSE;
    double u = std::fmod(t, cycle);
    if (u < PAUSE)          return 0.0f;
    if (u < PAUSE + travel) return static_cast<float>((u - PAUSE) * SPEED);
    return overflow;
}

}  // namespace

/* === UI7 2026-09-14 - THIS WAS QUADRATIC, AND IT COST THE WHOLE UI =====
 *
 * The loop this replaces grew a prefix ONE CHARACTER AT A TIME and measured the
 * WHOLE prefix on every step. For a description of n characters that is n calls
 * to `nvgTextBounds`, each shaping an ever longer string: O(n^2) glyph work,
 * per item, PER FRAME.
 *
 * What it cost, measured on a PS Vita rather than estimated: the settings
 * screen's Video section - seven rows whose descriptions run to 155 characters
 * - drew at 196 to 287 ms per frame, i.e. 3 to 5 images per second, with the
 * GPU completely idle. Sections whose descriptions are twenty characters long
 * were fluid, which is exactly the signature of a squared cost and is what the
 * report ("the Video section is slow, the others are not") described.
 *
 * Finding it took eleven wrong hypotheses, every one eliminated by measurement:
 * our own per-frame code, a leak, activity-stack depth, the number of rows, the
 * font, 4x MSAA, the whole stack being redrawn, vsync, the buffer swap, a GPU
 * stall, and the animated background. The instrument that settled it split one
 * frame four ways and then split our own list draw five more.
 *
 * `nvgTextGlyphPositions` gives every glyph's x position in ONE shaping pass,
 * so the cut point is then a linear scan over an array. Same output - the same
 * character is chosen - for O(n) instead of O(n^2).
 */
std::string truncated(NVGcontext *vg, const std::string &txt, float maxw)
{
    static const char *ELL = "\u2026";
    const float wEll = textWidth(vg, ELL);
    if (maxw <= wEll) return std::string();
    if (txt.empty()) return std::string(ELL);

    /* THE BUFFER IS SIZED FROM THE STRING, and that is the whole point of this
     * second attempt. The first one used a fixed 256-glyph buffer and fell back
     * to the quadratic walk beyond it - which optimised every easy case and
     * left the pathological one exactly as slow as before. `vsync_desc` is 317
     * characters in French, so the single worst row on the page took the slow
     * path, and the console showed it: putting the cursor ON that row made the
     * page fluid, because a focused row scrolls instead of being truncated.
     *
     * A cap that the worst case escapes is not an optimisation, it is a
     * measurement waiting to disappoint. There is no cap now: one glyph is at
     * most one byte, so the byte length bounds the count.
     *
     * A stack buffer covers everything this UI shows; the vector is there so a
     * long string is still O(n) rather than falling off a cliff. */
    NVGglyphPosition stack_pos[192];
    std::vector<NVGglyphPosition> heap_pos;
    NVGglyphPosition *pos = stack_pos;
    int cap = (int)(sizeof stack_pos / sizeof stack_pos[0]);
    if (txt.size() > (size_t)cap) {
        heap_pos.resize(txt.size());
        pos = heap_pos.data();
        cap = (int)txt.size();
    }

    /* One shaping pass gives every glyph's x range; the cut is then a scan. */
    const int n = nvgTextGlyphPositions(vg, 0.0f, 0.0f, txt.c_str(), nullptr, pos, cap);
    if (n <= 0) return std::string(ELL);

    size_t cut = 0;
    for (int k = 0; k < n; k++) {
        /* `maxx` is where the glyph ENDS: the first one that overruns the
         * budget is the first that must go. */
        if (pos[k].maxx + wEll > maxw) break;
        cut = (size_t)(pos[k].str - txt.c_str()) + 1;
        /* Never cut inside a codepoint: an accented letter or an arrow would
         * print as a replacement character. That defect is why the old walk
         * advanced one UTF-8 character at a time, and it must not come back
         * through the fast path. */
        while (cut < txt.size()
               && (static_cast<unsigned char>(txt[cut]) & 0xC0) == 0x80) cut++;
    }
    return txt.substr(0, cut) + ELL;
}

void clampedText(NVGcontext *vg, float x, float y, float maxw,
                float clipY, float clipH,
                const std::string &txt, bool scroll, double t)
{
    if (!vg || txt.empty() || maxw <= 1.0f) return;

    const float w = textWidth(vg, txt.c_str());
    if (w <= maxw) { nvgText(vg, x, y, txt.c_str(), nullptr); return; }

    if (!scroll) {
        const std::string cut = truncated(vg, txt, maxw);
        if (!cut.empty()) nvgText(vg, x, y, cut.c_str(), nullptr);
        return;
    }

    nvgSave(vg);
    /* `nvgIntersectScissor` and not `nvgScissor`: a scrolling list has already
     * set its own scissor, and REPLACING it would let the last row's text spill
     * out of the list instead of being clipped by it. With no outer scissor the
     * two are equivalent, which is why the pause menu - which sets none - saw no
     * regression. */
    nvgIntersectScissor(vg, x, clipY, maxw, clipH);
    nvgText(vg, x - scrollOffset(w - maxw, t), y, txt.c_str(), nullptr);
    nvgRestore(vg);
}

/* ------------------------------------------------------------- waiting */

void spinner(NVGcontext *vg, float cx, float cy, float r, double t)
{
    if (!vg || r <= 1.0f) return;

    const float thick = (r * 0.22f < 2.0f) ? 2.0f : r * 0.22f;
    const float rr = r - thick * 0.5f;

    /* Background ring: gives the widget a stable footprint. Without it the comet
     * spins in the void and the eye does not know where to look. */
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, rr);
    nvgStrokeWidth(vg, thick);
    nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 26));
    nvgStroke(vg);

    /* CONSTANT speed, deliberately: a busy indicator also says "the display
     * thread is still running". A varying speed would make a real hang
     * indistinguishable from an animation. */
    const float a0 = (float)std::fmod(t * 2.6, 6.283185307179586);
    const float a1 = a0 + 4.4f;   /* ~250 deg: long enough to read the direction */

    /* The gradient runs from the tail (transparent) to the head (opaque) along
     * the arc's chord - that is what gives the comet without having to chop the
     * arc into segments, each of which would be one more draw call. */
    const float qx = cx + std::cos(a0) * rr, qy = cy + std::sin(a0) * rr;
    const float hx = cx + std::cos(a1) * rr, hy = cy + std::sin(a1) * rr;
    NVGpaint comet = nvgLinearGradient(vg, qx, qy, hx, hy, faded(accentVif), accentVif);

    nvgBeginPath(vg);
    nvgArc(vg, cx, cy, rr, a0, a1, NVG_CW);
    nvgStrokeWidth(vg, thick);
    nvgLineCap(vg, NVG_ROUND);
    nvgStrokePaint(vg, comet);
    nvgStroke(vg);
}

/* === The sheen is DITHERED too (2026-08-27) ===
 *
 * It used to be two raw alpha gradients sliding across. Its ramp climbs to alpha
 * 18 over nearly 400 pixels: ONE STEP EVERY 22 PIXELS, and since it composites
 * over the card, it also re-quantises the dithered gradient underneath. Hence
 * the reported defect: "visible bands when the light peak goes past".
 *
 * Same remedy as for the background, transposed: we dither the ramp AT THE
 * SOURCE. Two things set it apart from the background texture:
 *
 *   - the sheen MOVES, so it cannot be baked into the background image. But its
 *     SHAPE does not change: we dither the shape once and translate it. That is
 *     what makes the trick applicable to an animation.
 *   - the amplitude must be BAKED INTO the texture. Passing a normalised ramp
 *     and multiplying by the alpha via nvgImagePattern would divide the dither
 *     by 255 - it would fall far below the quantisation step and do nothing at
 *     all. So the cache is keyed by the peak alpha.
 *
 * Diffusion runs along a ROW (the ramp is horizontal), with a different starting
 * error per row: without that offset every row would flip at the same column and
 * the step would stay a hard vertical line - exactly the mirror image of the
 * problem solved on the vertical gradients. */
namespace {

const int SHEEN_W = 512;   /* resolution of the ramp */
const int SHEEN_H = 64;    /* tile height, repeated vertically */

int sheenTexture(NVGcontext *vg, int peak_alpha)
{
    unsigned char *px = (unsigned char *)malloc((size_t)SHEEN_W * SHEEN_H * 4);
    if (!px) return -1;

    for (int y = 0; y < SHEEN_H; y++) {
        float err = (float)(mix32((unsigned int)y * 2246822519u + 4321u)
                            % 1000u) / 1000.0f - 0.5f;
        for (int x = 0; x < SHEEN_W; x++) {
            /* Triangle 0 -> peak -> 0, identical to the original drawing code
             * (two linear ramps meeting in the middle). We keep that profile
             * exactly: it is the one that was validated by eye. */
            const float u = (float)x / (float)(SHEEN_W - 1);
            const float tri = 1.0f - fabsf(2.0f * u - 1.0f);
            float ideal = (float)peak_alpha * tri;

            /* Both edges are forced to zero. Outside the texture NanoVG extends
             * the edge texel; if it happened to be 1 through a rounding
             * accident, that 1 would spread over the WHOLE card as a permanent
             * veil. */
            int q;
            if (x < 2 || x > SHEEN_W - 3) { q = 0; err = 0.0f; }
            else {
                const float v = ideal + err;
                q = (int)(v + 0.5f);
                if (q < 0) q = 0; else if (q > 255) q = 255;
                err = v - (float)q;
            }
            unsigned char *p = px + ((size_t)y * SHEEN_W + x) * 4;
            p[0] = p[1] = p[2] = 255;          /* white: only the alpha varies */
            p[3] = (unsigned char)q;
        }
    }

    /* REPEATY only. Horizontally it MUST NOT repeat, or the sheen would reappear
     * next to itself; the edge is extended with zero, so the outside is
     * transparent, which is what we want. */
    const int img = nvgCreateImageRGBA(vg, SHEEN_W, SHEEN_H, NVG_IMAGE_REPEATY, px);
    free(px);
    return img;
}

struct Sheen { int alpha; int img; };
Sheen g_sheens[4];
int   g_sheen_count = 0;

}  // namespace

void lightSheen(NVGcontext *vg, float x, float y, float w, float h,
                float radius, float phase, int peak_alpha)
{
    if (!vg || w <= 0.0f || h <= 0.0f) return;
    if (phase < 0.0f) phase = 0.0f;
    if (phase > 1.0f) phase = 1.0f;
    if (peak_alpha <= 0) return;
    if (peak_alpha > 255) peak_alpha = 255;

    /* Wider than the shape: any narrower and the transition is too short, and it
     * reads as a line crossing the card instead of a sheet of light. */
    const float bw = w * 2.20f;
    /* From "entirely off to the left" to "entirely off to the right": its
     * appearance and disappearance are never seen, only the pass. */
    const float bx = x - bw + phase * (w + bw);

    int img = -1;
    for (int i = 0; i < g_sheen_count; i++)
        if (g_sheens[i].alpha == peak_alpha) { img = g_sheens[i].img; break; }
    if (img < 0 && g_sheen_count < (int)(sizeof g_sheens / sizeof g_sheens[0])) {
        img = sheenTexture(vg, peak_alpha);
        if (img >= 0) {
            g_sheens[g_sheen_count].alpha = peak_alpha;
            g_sheens[g_sheen_count].img   = img;
            g_sheen_count++;
        }
    }

    nvgSave(vg);
    nvgIntersectScissor(vg, x, y, w, h);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, radius);

    if (img >= 0) {
        nvgFillPaint(vg, nvgImagePattern(vg, bx, y, bw, (float)SHEEN_H,
                                         0.0f, img, 1.0f));
        nvgFill(vg);
    } else {
        /* Fallback: the two original ramps. They show their bands, but a sheen
         * that bands beats a card with no reflection at all. */
        const float peak = bx + bw * 0.5f;
        const NVGcolor light = nvgRGBA(255, 255, 255, (unsigned char)peak_alpha);
        const NVGcolor none  = nvgRGBA(255, 255, 255, 0);
        nvgFillPaint(vg, nvgLinearGradient(vg, bx, y, peak, y, none, light));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, w, h, radius);
        nvgFillPaint(vg, nvgLinearGradient(vg, peak, y, bx + bw, y, light, none));
        nvgFill(vg);
    }

    nvgRestore(vg);
}

}  // namespace paint
}  // namespace ui
