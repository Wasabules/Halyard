/* power_profile - see power_profile.h.
 *
 * === WHAT EACH CONSOLE KEEPS TO ITSELF ================================
 *
 * PS VITA, measured 2026-09-13: an application starts at 333 MHz with its GPU
 * at **41 MHz**. The CPU knows 444 and the GPU 222; games ask for them, we
 * never did. The crypto bench confirmed it in the exact ratio of the clocks
 * (333/444 = 0.75): 1024-bit modexp 5.9 -> 4.4 ms, P-256 point 12.9 -> 9.6,
 * 4096-bit modexp 74 -> 55. The whole path -- decode, TLS, audio -- benefits.
 *
 * And the radio sleeps between two beacons. `scePowerSetUsingWireless` tells
 * the system the application is using the network. It was the only actionable
 * lead on the jitter: the RTT jittered from 15 to 67 ms while OUR send cadence
 * is steady to within 1-3 ms, so the delay was on the path. Measured after:
 * 11-13 ms.
 *
 * NINTENDO SWITCH: the question was asked and the answer is NO -- the
 * equivalent lever (`appletSetCpuBoostMode`) throttles the GPU in order to
 * raise the CPU, which is exactly backwards for a client that decodes and
 * renders on the GPU. See the Switch branch: it changes nothing and says why.
 *
 * The values are READ back before and after, and logged. A frequency we
 * believe we set and did not is exactly the phantom measurement this repo has
 * already paid for.
 */
#include "power_profile.h"
#include "journal.h"

#define plog(...) JOURNAL_INFO_(JOURNAL_CAT_SYSTEM, __VA_ARGS__)

#if defined(__vita__) || defined(__psp2__)

#include <psp2/power.h>
#include <psp2/display.h>
#include <time.h>

void shadow_power_profile_apply(void)
{
    const int arm0 = scePowerGetArmClockFrequency();
    const int bus0 = scePowerGetBusClockFrequency();
    const int gpu0 = scePowerGetGpuClockFrequency();

    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    plog("[PWR] horloges : arm %d -> %d MHz, bus %d -> %d, gpu %d -> %d",
         arm0, scePowerGetArmClockFrequency(), bus0, scePowerGetBusClockFrequency(),
         gpu0, scePowerGetGpuClockFrequency());

    /* The panel and our clock, put against each other. `[L5] video/cadence`
     * read 17.4 ms for a panel supposed to run at 60 Hz, with a FLAT
     * distribution: either the panel is not at 60, or the clock drifts. Asking
     * for the rate and then timing 60 vblanks settles it -- and if the clock
     * reads crooked, EVERY measurement taken with it carries that bias. */
    {
        float fps = 0.0f;
        sceDisplayGetRefreshRate(&fps);
        struct timespec a0, a1;
        clock_gettime(CLOCK_MONOTONIC, &a0);
        for (int i = 0; i < 60; i++) sceDisplayWaitVblankStart();
        clock_gettime(CLOCK_MONOTONIC, &a1);
        const double ms = ((double)(a1.tv_sec - a0.tv_sec) * 1000.0
                           + (double)(a1.tv_nsec - a0.tv_nsec) / 1e6) / 60.0;
        plog("[PWR] dalle : %.2f Hz annonces (%.2f ms) | 60 vblancs chronometres "
             "= %.2f ms chacun -> horloge %+.1f %%",
             (double)fps, fps > 0 ? 1000.0 / (double)fps : 0.0, ms,
             fps > 0 ? (ms * (double)fps / 1000.0 - 1.0) * 100.0 : 0.0);
    }

    const int wrc = scePowerSetUsingWireless(1);
    plog("[PWR] radio maintenue active : rc=0x%08x, etat=%d",
         wrc, scePowerGetUsingWireless());
}

/* === LAT-V1 2026-09-26 - ARE THE CLOCKS STILL WHERE WE PUT THEM? ======
 *
 * An unattended Vita session drew at 60 Hz for its first ~15 s of stream and
 * at 46-48 Hz after, in every run, unimodal around 21-22 ms: 16.7 x 444/333.
 * The clocks are set once at start-up and were never read again. This says,
 * with numbers, whether the system moved them. */
void shadow_power_profile_check(void)
{
    static int last_arm = -1, last_bus = -1, last_gpu = -1;
    const int arm = scePowerGetArmClockFrequency();
    const int bus = scePowerGetBusClockFrequency();
    const int gpu = scePowerGetGpuClockFrequency();
    if (arm != last_arm || bus != last_bus || gpu != last_gpu) {
        plog("[PWR] horloges relues : arm %d MHz, bus %d, gpu %d%s", arm, bus, gpu,
             (arm < 444 || gpu < 222) ? " -- EN DESSOUS de ce que apply() a regle" : "");
        last_arm = arm; last_bus = bus; last_gpu = gpu;
    }
}

#elif defined(__SWITCH__)

#include <switch.h>

void shadow_power_profile_apply(void)
{
    /* === WE RAISE NOTHING HERE, AND THAT IS THE FINDING ================
     *
     * The question was worth asking: on PS Vita, asking for its real clocks
     * returned a third of the machine's power, and nothing in this repo called
     * the Switch equivalent. But the equivalent is not one -- libnx's
     * `switch/services/apm.h` says so in its own words:
     *
     *     ApmCpuBoostMode_FastLoad = 1
     *     /// Boost CPU. Additionally, throttle GPU to minimum.
     *
     * FastLoad is built for a loading screen: a CPU spike paid for by crushing
     * the GPU. A video-stream client decodes and renders ON the GPU -- the
     * trade is exactly the wrong way round, and applying it would have been a
     * regression that no measurement in this repo would have caught before
     * somebody saw it on screen.
     *
     * So the Switch is not throttled the way the Vita was: HOS picks its own
     * clocks for a sustained load, and its default mode IS the one a stream
     * wants. There is nothing to ask for.
     *
     * What stays useful is SAYING which regime the console is in: the
     * performance configuration distinguishes docked from handheld, and two
     * measurements taken on either side do not compare. The line costs one call
     * at startup and stops incomparable figures being put side by side. */
    u32 perf = 0;
    const Result rc = appletGetCurrentPerformanceConfiguration(&perf);
    if (R_SUCCEEDED(rc))
        plog("[PWR] configuration de performance : 0x%08x, %s (mode processeur "
             "laisse par defaut : FastLoad ecraserait le GPU)",
             (unsigned)perf,
             appletGetOperationMode() == AppletOperationMode_Console
                 ? "dock" : "portable");
    else
        plog("[PWR] configuration de performance illisible rc=0x%08x", (unsigned)rc);
}

/* The Switch's clocks are not ours to move (see above): nothing to check. */
void shadow_power_profile_check(void) { }

#else

void shadow_power_profile_apply(void) { }
void shadow_power_profile_check(void) { }

#endif
