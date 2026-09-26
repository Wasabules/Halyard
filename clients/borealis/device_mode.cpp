/* device_mode - see the header. */
#include "device_mode.hpp"

#ifdef __SWITCH__
#  include <switch.h>
#elif defined(__vita__) || defined(__psp2__)
#  include <psp2/kernel/sysmem.h>
#endif

namespace device {

static int g_last_strength = -1;   /* D4: -1 = never measured */

#ifdef __SWITCH__
/* AF10 2026-09-10 - file scope rather than statics inside `linkType()`:
 * `releaseServices()` has to see them. Process state, not session state. */
static bool g_nifm_tried = false, g_nifm_ready = false;
#endif

void releaseServices()
{
#ifdef __SWITCH__
    /* === AF10 2026-09-10 - THE nifm WE OPEN, WE CLOSE ===
     *
     * `linkType()` opens nifm on first use and nothing ever closed it.
     * Borealis holds its own reference (switch_wrapper.c) and gives back only
     * that one, so ours left a nifm:u session inside the loader's process on
     * every launch - the class of leak `sslExit()` was put back into main for
     * (S26). Balanced here: Borealis' reference keeps the service up for it. */
    if (g_nifm_ready) nifmExit();
    g_nifm_ready = false;
    g_nifm_tried = false;
#endif
}

bool isDocked()
{
#ifdef __SWITCH__
    return appletGetOperationMode() == AppletOperationMode_Console;
#elif defined(__vita__) || defined(__psp2__)
    /* A Vita has no dock, and the question is not meaningless here - it is
     * ANSWERED, once and for all, by the hardware: a handheld is never docked,
     * a PlayStation TV always is. Reported from a real console, 2026-09-13.
     *
     * This matters beyond a label. `docked()` drives settings the stream obeys
     * silently - the audio profile the equaliser picks (EQV1: built-in speakers
     * produce nothing below 250 Hz, a TV needs none of that correction) and
     * what the quality screen offers. Returning a flat `false` would have given
     * a PS TV, plugged into an amplifier, the handheld speaker correction.
     *
     * Resolved ONCE: the answer cannot change while the application runs, and
     * this repo has been bitten enough by session state in a function `static`
     * to say so explicitly - this one is HARDWARE state, decided before the
     * process existed. */
    static int is_tv = -1;
    if (is_tv < 0) is_tv = (sceKernelIsPSVitaTV() == 1) ? 1 : 0;
    return is_tv != 0;
#else
    return false;
#endif
}

LinkType linkType()
{
#ifdef __SWITCH__
    /* `nifm` is NOT brought up by libnx's socket setup: we do it on the first
     * question and keep it open for the session. Closing it between two queries
     * would cost an IPC round trip every time, for a piece of information that
     * only changes when someone plugs in a cable. */
    if (!g_nifm_tried) {
        g_nifm_tried = true;
        g_nifm_ready = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    }
    if (!g_nifm_ready) return LinkType::Unknown;

    NifmInternetConnectionType type = (NifmInternetConnectionType)0;
    u32 strength = 0;
    NifmInternetConnectionStatus status = (NifmInternetConnectionStatus)0;
    if (R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &status)))
        return LinkType::Unknown;
    /* D4 2026-08-28 - the signal strength was read and THROWN AWAY. It is the
     * only radio measurement we have, and the video stall (KB §3.34) is the first
     * thing it could explain. We keep it. */
    g_last_strength = (int)strength;

    switch (type) {
        case NifmInternetConnectionType_WiFi:     return LinkType::WiFi;
        case NifmInternetConnectionType_Ethernet: return LinkType::Ethernet;
        default:                                  return LinkType::Unknown;
    }
#else
    return LinkType::Unknown;
#endif
}

const char *linkLabel()
{
    switch (linkType()) {
        case LinkType::WiFi:     return "Wi-Fi";
        case LinkType::Ethernet: return "Ethernet";
        default:                 return "—";
    }
}

}  // namespace device


/* === D4 2026-08-28 - C BRIDGE FOR THE STALL DIAGNOSIS ===
 * `ctrl_session.c` is C and also links into the headless binary, which does not
 * carry this layer: hence a single function rather than an include. Returns the
 * link type (0 unknown, 1 Wi-Fi, 2 Ethernet) and fills `docked` (0/1) and
 * `strength` (-1 when not measured). Off Switch, everything is unknown. */
extern "C" int shadow_link_info(int *docked, int *strength)
{
    const device::LinkType t = device::linkType();   /* refreshes the strength */
    if (docked)   *docked   = device::isDocked() ? 1 : 0;
    if (strength) *strength = device::g_last_strength;
    switch (t) {
        case device::LinkType::WiFi:     return 1;
        case device::LinkType::Ethernet: return 2;
        default:                             return 0;
    }
}
