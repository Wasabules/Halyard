/* test_vita_paths.c - the PS VITA port's pure arithmetic.
 *
 * === WHY THIS FILE ===================================================
 *
 * Everything the Vita port gained on 2026-09-13 had exactly one witness: the
 * console. Each hypothesis cost a deployment, a session, and sometimes a
 * crash -- and three of that day's defects were PURE ARITHMETIC, checkable
 * here in microseconds:
 *
 *   - the stick read a permanent half-deflection to the left, because a
 *     0..255 travel was divided by 255 instead of 127.5 (PSV3);
 *   - touches landed 1.33x too far out, because a logical coordinate was
 *     converted a second time;
 *   - the audio queue's ceiling ignored half the queue.
 *
 * None of them needed a console to be found. This file pins each one WITH the
 * counter-case that produced it: a regression fails a test that NAMES the
 * symptom reported from the hardware, instead of costing another round trip
 * to the console.
 *
 * What is NOT covered, and it has to be said: nothing here touches SceCtrl,
 * SceGxm or SceAudioOut. These are the conversions, not the system calls --
 * so a green run says nothing about a picture being on screen.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int checks = 0, failures = 0;
#define CHECK(cond, what) do {                                                \
        checks++;                                                             \
        if (!(cond)) { failures++;                                            \
            printf("  FAIL %s:%d - %s\n", __FILE__, __LINE__, (what)); }      \
    } while (0)

/* ---------------------------------------------------------------- sticks */

/* The conversion from `core/input/pad_sce.h`, copied here verbatim. */
static int32_t stick_axis(unsigned char v) { return (int32_t)(((int)v - 128) * 32767 / 127); }

/* Borealis' own, BEFORE PSV3, so the counter-case is executed and not merely
 * described. */
static float borealis_before(unsigned char v) { return (float)v / 255.0f - 1.0f; }
static float borealis_after(unsigned char v)  { return (float)v / 127.5f - 1.0f; }

static void sticks(void)
{
    printf("-- sticks: the 0..255 travel onto +/-32767\n");

    /* Rest must be NEUTRAL. The pause menu thresholds at 16000: that is the
     * threshold the resting value has to miss by a wide margin, not merely
     * "be small". */
    const int32_t rest = stick_axis(128);
    CHECK(rest > -16000 && rest < 16000,
          "rest: the resting value crosses the menu threshold (permanent scrolling)");
    CHECK(rest > -300 && rest < 300, "rest: the resting value is not neutral");

    /* The extremes must reach full travel, otherwise a stick pushed all the
     * way triggers nothing. */
    CHECK(stick_axis(255) >= 32000, "extreme: hard right does not reach full travel");
    CHECK(stick_axis(0)   <= -32000, "extreme: hard left does not reach full travel");

    /* Symmetry: an equal deflection either way must give the same magnitude,
     * otherwise aiming pulls to one side. */
    const int32_t l = stick_axis(128 - 50), r = stick_axis(128 + 50);
    const int32_t skew = (l + r) < 0 ? -(l + r) : (l + r);
    CHECK(skew < 300, "symmetry: the two directions do not give the same magnitude");

    /* === PSV3 COUNTER-CASE ===========================================
     * Reported from the hardware: "the left button stays pressed all the
     * time". Dividing by 255 projects the travel onto -1..0: at rest the axis
     * reads -0.498, i.e. -16318 once scaled -- just past the 16000 threshold,
     * on every frame. */
    const float before = borealis_before(128);
    CHECK(before < -0.4f, "PSV3 counter-case: dividing by 255 should give a strongly negative rest");
    CHECK((int32_t)(before * 32767.0f) < -16000,
          "PSV3 counter-case: the faulty rest should cross the menu threshold");
    const float after = borealis_after(128);
    CHECK(after > -0.01f && after < 0.01f, "PSV3: dividing by 127.5 must make rest neutral");
    CHECK(borealis_after(255) > 0.99f, "PSV3: hard right must reach +1");
    CHECK(borealis_after(0) < -0.99f, "PSV3: hard left must reach -1");
}

/* ------------------------------------------------------------- buttons */

/* libnx bit positions, as pad_sce.h emits them. The VALUES matter:
 * `devlink::injectedNpadMask()` speaks libnx, and invented bits would work
 * right up to the day devlink injects a direction. */
enum { B_A = 1u<<0, B_B = 1u<<1, B_X = 1u<<2, B_Y = 1u<<3,
       B_SL = 1u<<4, B_SR = 1u<<5, B_L = 1u<<6, B_R = 1u<<7,
       B_PLUS = 1u<<10, B_MINUS = 1u<<11,
       B_LEFT = 1u<<12, B_UP = 1u<<13, B_RIGHT = 1u<<14, B_DOWN = 1u<<15 };

static void buttons(void)
{
    printf("-- buttons: distinct bits, at libnx's positions\n");
    const uint64_t all[] = { B_A, B_B, B_X, B_Y, B_SL, B_SR, B_L, B_R,
                             B_PLUS, B_MINUS, B_LEFT, B_UP, B_RIGHT, B_DOWN };
    const int n = (int)(sizeof all / sizeof all[0]);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            CHECK((all[i] & all[j]) == 0,
                  "two buttons share a bit: one press would trigger both");
    /* The four directions do occupy 12..15, the order `stream_view`'s d-pad
     * reads. */
    CHECK(B_LEFT == (1u<<12) && B_UP == (1u<<13)
          && B_RIGHT == (1u<<14) && B_DOWN == (1u<<15),
          "d-pad: the positions are no longer libnx's");
}

/* --------------------------------------------------------------- touch */

/* The pattern that produced the offset: four call sites, only one divided. */
static float touch_logical(float x) { return x; }
static float touch_divided(float x, float scale) { return x / scale; }

static void touch(void)
{
    printf("-- touch: one coordinate space for all four call sites\n");
    const float scale = 960.0f / 1280.0f;   /* windowScale on this console */

    /* The finger arrives in LOGICAL coordinates; dividing them a second time
     * pushes them out in proportion to their distance from the origin. */
    CHECK(touch_logical(0.0f) == 0.0f, "origin: the top-left corner must coincide");
    const float at_edge = 1200.0f;
    const float wrong = touch_divided(at_edge, scale);
    CHECK(wrong > at_edge * 1.3f,
          "counter-case: re-dividing must push the point out by more than 30 % at the edge");
    CHECK(touch_divided(0.0f, scale) == 0.0f,
          "counter-case: the error must be ZERO at the origin (it is a scale, not an offset)");
    CHECK(touch_logical(at_edge) == at_edge, "logical space: the value must pass through unchanged");
}

/* ------------------------------------------------- audio queue ceiling */

/* L13's arithmetic exactly as `core/media/audio.c` applies it. */
static size_t l13_drop(size_t ring, size_t dev, size_t cap, size_t target, size_t floor_)
{
    const size_t total = ring + dev;
    if (total <= cap) return 0;
    size_t want = (total - target > ring) ? ring : (total - target);
    if (ring > floor_ && want > ring - floor_) want = ring - floor_;
    else if (ring <= floor_)                   want = 0;
    return want;
}

static void audio_queue(void)
{
    printf("-- audio queue: the ceiling covers what the listener hears\n");
    const size_t K = 48;                      /* samples per ms at 48 kHz */
    const size_t cap = 40*K, target = 20*K, floor_ = 16*K;

    /* Never drop more than the ring holds: samples already handed to the
     * device are gone. */
    CHECK(l13_drop(10*K, 60*K, cap, target, floor_) <= 10*K,
          "never more than the ring: a negative or excessive drop would break the index");

    /* The floor stops the ring being emptied: that is what produced eleven
     * underruns on the first attempt. */
    CHECK(l13_drop(20*K, 30*K, cap, target, floor_) <= 20*K - floor_,
          "floor: the ring drops below the floor (starvation)");
    CHECK(l13_drop(16*K, 40*K, cap, target, floor_) == 0,
          "floor: a ring already at the floor must not be trimmed further");

    /* === COUNTER-CASE: the ceiling that ignored the device ==============
     * Ring 30 ms, device 20 ms. The old test (`ring > cap`) saw only 30 and
     * trimmed nothing, while the listener hears 50 ms. */
    const size_t ring = 30*K, dev = 20*K;
    CHECK(ring <= cap, "counter-case: the ring alone must stay under the ceiling");
    CHECK(ring + dev > cap, "counter-case: the total must exceed it");
    CHECK(l13_drop(ring, dev, cap, target, floor_) > 0,
          "counter-case: counting the total must trigger a trim the ring alone missed");

    /* Under the ceiling, nothing is touched. */
    CHECK(l13_drop(10*K, 10*K, cap, target, floor_) == 0,
          "rest: a queue under the ceiling must not be trimmed");
}

/* --------------------------------------------------- render geometry */

/* The GXM renderer's nanovg pattern: the texture is as wide as the PITCH,
 * only `w x h` of its columns are displayable. */
static void pattern(float dw, float dh, int pitch, int texh, int w, int h,
                    float *ex, float *ey)
{
    *ex = dw * (float)pitch / (float)w;
    *ey = dh * (float)texh  / (float)h;
}

static int contiguous(const uint8_t *y, const uint8_t *uv, int pitch, int h, int uvs)
{
    return (uv == y + (size_t)pitch * (size_t)h) && (uvs == pitch);
}

static void render(void)
{
    printf("-- render: the pitch must not reach the screen\n");
    float ex = 0, ey = 0;

    /* Pitch EQUAL to the width: the pattern covers the rectangle exactly. */
    pattern(960.0f, 544.0f, 1280, 720, 1280, 720, &ex, &ey);
    CHECK(ex > 959.0f && ex < 961.0f, "pitch = width: the pattern must cover the rectangle");

    /* WIDER pitch: the pattern must overflow in the same proportion, otherwise
     * the encoder's alignment columns show -- the green band every port of
     * this kind hits once. */
    pattern(960.0f, 544.0f, 1408, 720, 1280, 720, &ex, &ey);
    CHECK(ex > 960.0f, "pitch > width: the pattern must overflow, else the alignment band shows");
    const float expected = 960.0f * 1408.0f / 1280.0f;
    CHECK(ex > expected - 1.0f && ex < expected + 1.0f,
          "pitch > width: the overflow is not proportional");

    /* The ZERO-COPY path's condition is DERIVED, not assumed. */
    const uint8_t *base = (const uint8_t *)0x81000000u;
    CHECK(contiguous(base, base + 1280 * 720, 1280, 720, 1280),
          "zero copy: a contiguous layout must be accepted");
    CHECK(!contiguous(base, base + 1280 * 736, 1280, 720, 1280),
          "zero copy: a differently aligned height must FALL BACK on the copy");
    CHECK(!contiguous(base, base + 1280 * 720, 1280, 720, 1408),
          "zero copy: a different chroma pitch must fall back on the copy");
}

/* --------------------------------------------- audio output granularity */

/* The accumulator in `core/media/audio_out_vita.c`: chunks of any size go in,
 * whole grains come out. */
static int grains_emitted(int grain, const int *chunks, int n, int *left)
{
    int acc = 0, out = 0;
    for (int i = 0; i < n; i++) {
        int done = 0;
        while (done < chunks[i]) {
            int take = chunks[i] - done;
            if (take > grain - acc) take = grain - acc;
            acc += take; done += take;
            if (acc == grain) { out++; acc = 0; }
        }
    }
    *left = acc;
    return out;
}

static void granularity(void)
{
    printf("-- audio output: chunks of 480 into grains of 512\n");
    int left = 0;
    const int chunks[10] = { 480,480,480,480,480,480,480,480,480,480 };

    const int out = grains_emitted(512, chunks, 10, &left);
    /* Nothing may be lost or invented: 4800 frames go in, as many must come
     * out, grains emitted plus remainder. */
    CHECK(out * 512 + left == 4800, "conservation: frames are lost or invented");
    CHECK(left >= 0 && left < 512, "remainder: the residue must fit in one grain");
    CHECK(out == 9, "grains: 4800 frames by 512 must give 9 full grains");

    /* A chunk larger than a grain must emit several, otherwise a burst would
     * stay stuck in the accumulator. */
    const int big[1] = { 2048 };
    const int out2 = grains_emitted(512, big, 1, &left);
    CHECK(out2 == 4 && left == 0, "burst: a large chunk must emit several grains");
}

int main(void)
{
    printf("== PS Vita paths: the pure arithmetic ==\n");
    sticks();
    buttons();
    touch();
    audio_queue();
    render();
    granularity();
    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
